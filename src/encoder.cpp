#include "encoder.h"
#include "log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/hwcontext_qsv.h>
#include <libavutil/opt.h>
}

#include <chrono>
#include <cstdarg>
#include <mutex>
#include <thread>

namespace pd {

static std::string avErr(int code) {
	char buf[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, buf, sizeof(buf));
	return buf;
}

// FFmpeg 自己的警告和错误（比如“不支持这种像素格式”）默认打到 stderr，看不到；转进我们的日志
static void ffmpegLog(void *avcl, int level, const char *fmt, va_list vl) {
	if (level > AV_LOG_WARNING)
		return;
	char line[1024];
	int prefix = 1;
	av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &prefix);
	std::string s(line);
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
		s.pop_back();
	if (!s.empty())
		log("FFmpeg: {}", s);
}

static void hookFfmpegLog() {
	static std::once_flag once;
	std::call_once(once, [] {
		av_log_set_level(AV_LOG_WARNING);
		av_log_set_callback(ffmpegLog);
	});
}

std::string encoderForVendor(UINT vendorId) {
	switch (vendorId) {
	case 0x10DE:
		return "h264_nvenc";
	case 0x1002:
	case 0x1022:
		return "h264_amf";
	case 0x8086:
		return "h264_qsv";
	default:
		return "";
	}
}

bool VideoEncoder::open(ID3D11Device *device, const EncoderConfig &cfg) {
	close();
	mCfg = cfg;
	mCfg.width &= ~1;
	mCfg.height &= ~1;

	hookFfmpegLog();
	const bool qsv = mCfg.name == "h264_qsv";
	const AVCodec *codec = avcodec_find_encoder_by_name(mCfg.name.c_str());
	if (!codec) {
		log("FFmpeg 里没有编码器 {}", mCfg.name);
		return false;
	}

	mDeviceRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
	auto *devCtx = reinterpret_cast<AVHWDeviceContext *>(mDeviceRef->data);
	auto *d3d = static_cast<AVD3D11VADeviceContext *>(devCtx->hwctx);
	d3d->device = device;
	device->AddRef(); // FFmpeg 释放上下文时会 Release
	int ret = av_hwdevice_ctx_init(mDeviceRef);
	if (ret < 0) {
		log("av_hwdevice_ctx_init 失败: {}", avErr(ret));
		close();
		return false;
	}

	if (qsv) {
		// Intel：从截屏用的 D3D11 设备派生 QSV 设备，帧池建在 QSV 设备上
		ret = av_hwdevice_ctx_create_derived(&mQsvDeviceRef, AV_HWDEVICE_TYPE_QSV, mDeviceRef, 0);
		if (ret < 0) {
			log("从 D3D11 派生 QSV 设备失败: {}（显卡驱动可能太旧）", avErr(ret));
			close();
			return false;
		}
		mFramesRef = av_hwframe_ctx_alloc(mQsvDeviceRef);
		auto *frames = reinterpret_cast<AVHWFramesContext *>(mFramesRef->data);
		frames->format = AV_PIX_FMT_QSV;
		frames->sw_format = AV_PIX_FMT_NV12;
		frames->width = mCfg.width;
		frames->height = mCfg.height;
		// 只要 1 帧（同 Sunshine）：多帧时 FFmpeg 会建 NV12 纹理数组，带 RENDER_TARGET 的纹理数组
		// 很多驱动不支持（Iris Xe 上实测 E_INVALIDARG）。编码是同步的，这一帧取一次、之后每帧重复使用
		frames->initial_pool_size = 1;
		auto *qsvFrames = static_cast<AVQSVFramesContext *>(frames->hwctx);
		// 视频处理器要往里写：底下的 D3D11 纹理要带 RENDER_TARGET
		qsvFrames->frame_type = MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET;
	} else {
		mFramesRef = av_hwframe_ctx_alloc(mDeviceRef);
		auto *frames = reinterpret_cast<AVHWFramesContext *>(mFramesRef->data);
		frames->format = AV_PIX_FMT_D3D11;
		frames->sw_format = AV_PIX_FMT_NV12;
		frames->width = mCfg.width;
		frames->height = mCfg.height;
		// 0 = 按需单独建纹理；NV12 纹理数组 + RENDER_TARGET 在 NVIDIA 上会 E_INVALIDARG
		frames->initial_pool_size = 0;
		auto *d3dFrames = static_cast<AVD3D11VAFramesContext *>(frames->hwctx);
		d3dFrames->BindFlags = D3D11_BIND_RENDER_TARGET; // 视频处理器要往里写
	}
	ret = av_hwframe_ctx_init(mFramesRef);
	if (ret < 0) {
		log("av_hwframe_ctx_init 失败: {}", avErr(ret));
		close();
		return false;
	}

	mCtx = avcodec_alloc_context3(codec);
	mCtx->width = mCfg.width;
	mCtx->height = mCfg.height;
	mCtx->time_base = {1, 90000};
	mCtx->framerate = {mCfg.fps, 1};
	mCtx->pix_fmt = qsv ? AV_PIX_FMT_QSV : AV_PIX_FMT_D3D11;
	mCtx->sw_pix_fmt = AV_PIX_FMT_NV12;
	mCtx->hw_frames_ctx = av_buffer_ref(mFramesRef);
	mCtx->gop_size = mCfg.fps * 60; // 平时靠手机的关键帧请求，一分钟兜底一次
	mCtx->max_b_frames = 0;
	mCtx->refs = 1;
	mCtx->bit_rate = int64_t(mCfg.bitrateKbps) * 1000;
	mCtx->rc_max_rate = mCtx->bit_rate;
	mCtx->rc_buffer_size = int(mCtx->bit_rate / mCfg.fps); // 约一帧，和 Sunshine 一样
	mCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	mCtx->color_range = AVCOL_RANGE_MPEG;
	mCtx->color_primaries = AVCOL_PRI_BT709;
	mCtx->color_trc = AVCOL_TRC_BT709;
	mCtx->colorspace = AVCOL_SPC_BT709;

	AVDictionary *opts = nullptr;
	if (mCfg.name == "h264_nvenc") {
		av_dict_set(&opts, "preset", "p1", 0);
		av_dict_set(&opts, "tune", "ull", 0);
		av_dict_set(&opts, "zerolatency", "1", 0);
		av_dict_set(&opts, "delay", "0", 0);
		av_dict_set(&opts, "rc", "cbr", 0);
		av_dict_set(&opts, "forced-idr", "1", 0);
		av_dict_set(&opts, "profile", "baseline", 0);
	} else if (mCfg.name == "h264_amf") {
		av_dict_set(&opts, "usage", "ultralowlatency", 0);
		av_dict_set(&opts, "rc", "cbr", 0);
		av_dict_set(&opts, "quality", "speed", 0);
		av_dict_set(&opts, "profile", "constrained_baseline", 0);
		av_dict_set(&opts, "forced_idr", "1", 0);
		av_dict_set(&opts, "enforce_hrd", "1", 0);
		av_dict_set(&opts, "filler_data", "0", 0);
	} else if (mCfg.name == "h264_qsv") {
		av_dict_set(&opts, "preset", "veryfast", 0);
		av_dict_set(&opts, "async_depth", "1", 0);
		av_dict_set(&opts, "profile", "baseline", 0);
		av_dict_set(&opts, "forced_idr", "1", 0);
		av_dict_set(&opts, "look_ahead", "0", 0);
	}
	ret = avcodec_open2(mCtx, codec, &opts);
	AVDictionaryEntry *unused = nullptr;
	while ((unused = av_dict_get(opts, "", unused, AV_DICT_IGNORE_SUFFIX)))
		log("编码器没有用到参数 {}={}", unused->key, unused->value);
	av_dict_free(&opts);
	if (ret < 0) {
		log("打开编码器 {} 失败: {}", mCfg.name, avErr(ret));
		close();
		return false;
	}

	mFrame = av_frame_alloc();
	mPkt = av_packet_alloc();
	log("编码器 {} 已打开：{}x{} {}fps {}kbps", mCfg.name, mCfg.width, mCfg.height, mCfg.fps,
	    mCfg.bitrateKbps);
	return true;
}

bool VideoEncoder::setBitrate(int kbps) {
	if (!mCtx)
		return false;
	if (mCfg.name != "h264_nvenc")
		return false;
	// FFmpeg 的 NVENC 在下一帧编码前发现码率变了，会调用 nvEncReconfigureEncoder
	mCfg.bitrateKbps = kbps;
	mCtx->bit_rate = int64_t(kbps) * 1000;
	mCtx->rc_max_rate = mCtx->bit_rate;
	mCtx->rc_buffer_size = int(mCtx->bit_rate / mCfg.fps);
	return true;
}

void VideoEncoder::close() {
	if (mCtx)
		avcodec_free_context(&mCtx);
	if (mFrame)
		av_frame_free(&mFrame);
	if (mPkt)
		av_packet_free(&mPkt);
	av_buffer_unref(&mFramesRef);
	av_buffer_unref(&mQsvDeviceRef);
	av_buffer_unref(&mDeviceRef);
	mSps.clear();
	mPps.clear();
}

bool VideoEncoder::beginFrame(ID3D11Texture2D **tex, UINT *slice) {
	// QSV 的帧池只有 1 帧：第一次取出来以后一直留着重复用（编码器只是加一个引用，同步编完就放开）
	const bool reuse = mQsvDeviceRef && mFrame->buf[0];
	if (!reuse)
		av_frame_unref(mFrame);
	int ret = reuse ? 0 : av_hwframe_get_buffer(mFramesRef, mFrame, 0);
	if (ret < 0) {
		log("av_hwframe_get_buffer 失败: {}", avErr(ret));
		return false;
	}
	if (mFrame->format == AV_PIX_FMT_QSV) {
		// QSV 帧：data[3] 是 mfxFrameSurface1，MemId 指向 (纹理, 下标)；
		// 带 RENDER_TARGET 的纹理是单独的，下标记成 MFX_INFINITE，实际是 0
		auto *surf = reinterpret_cast<mfxFrameSurface1 *>(mFrame->data[3]);
		auto *pair = surf ? static_cast<mfxHDLPair *>(surf->Data.MemId) : nullptr;
		if (!pair || !pair->first) {
			log("QSV 帧里没有 D3D11 纹理");
			return false;
		}
		*tex = static_cast<ID3D11Texture2D *>(pair->first);
		const auto index = reinterpret_cast<uintptr_t>(pair->second);
		*slice = index == uintptr_t(MFX_INFINITE) ? 0 : UINT(index);
		return true;
	}
	*tex = reinterpret_cast<ID3D11Texture2D *>(mFrame->data[0]);
	*slice = UINT(reinterpret_cast<intptr_t>(mFrame->data[1]));
	return true;
}

bool VideoEncoder::endFrame(int64_t pts90k, bool forceKeyframe, const PacketFn &onPacket) {
	mFrame->pts = pts90k;
	if (forceKeyframe) {
		mFrame->pict_type = AV_PICTURE_TYPE_I;
		mFrame->flags |= AV_FRAME_FLAG_KEY;
	} else {
		mFrame->pict_type = AV_PICTURE_TYPE_NONE;
		mFrame->flags &= ~AV_FRAME_FLAG_KEY;
	}
	int ret = avcodec_send_frame(mCtx, mFrame);
	if (!mQsvDeviceRef)
		av_frame_unref(mFrame); // QSV 的那一帧留着下次用
	if (ret < 0 && ret != AVERROR(EAGAIN)) {
		log("avcodec_send_frame 失败: {}", avErr(ret));
		return false;
	}

	// 低延迟设置下编码器应当立即出包；有些编码器是异步的，最多等 40ms
	using clock = std::chrono::steady_clock;
	auto deadline = clock::now() + std::chrono::milliseconds(40);
	bool got = false;
	for (;;) {
		ret = avcodec_receive_packet(mCtx, mPkt);
		if (ret == 0) {
			handlePacket(mPkt->data, size_t(mPkt->size), onPacket);
			av_packet_unref(mPkt);
			got = true;
			continue;
		}
		if (ret == AVERROR(EAGAIN)) {
			if (got)
				break;
			if (clock::now() > deadline) {
				if (!mWarnedDelay) {
					log("编码器 {} 没有立即出包（有缓冲延迟）", mCfg.name);
					mWarnedDelay = true;
				}
				break;
			}
			std::this_thread::sleep_for(std::chrono::microseconds(500));
			continue;
		}
		log("avcodec_receive_packet 失败: {}", avErr(ret));
		return false;
	}
	return true;
}

// 遍历 Annex B 码流里的 NAL 单元（不含起始码）
template <class F> static void forEachNal(const uint8_t *d, size_t n, F &&f) {
	size_t i = 0, nalStart = SIZE_MAX;
	while (i + 3 <= n) {
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
			size_t scStart = (i > 0 && d[i - 1] == 0) ? i - 1 : i;
			if (nalStart != SIZE_MAX)
				f(d + nalStart, scStart - nalStart);
			nalStart = i + 3;
			i += 3;
		} else {
			++i;
		}
	}
	if (nalStart != SIZE_MAX && nalStart < n)
		f(d + nalStart, n - nalStart);
}

void VideoEncoder::handlePacket(const uint8_t *data, size_t size, const PacketFn &onPacket) {
	bool hasSps = false, hasIdr = false;
	forEachNal(data, size, [&](const uint8_t *nal, size_t len) {
		if (len == 0)
			return;
		int type = nal[0] & 0x1F;
		if (type == 7) {
			mSps.assign(nal, nal + len);
			hasSps = true;
		} else if (type == 8) {
			mPps.assign(nal, nal + len);
		} else if (type == 5) {
			hasIdr = true;
		}
	});
	// Safari 要求每个 IDR 前都有 SPS/PPS；编码器没带时补上
	if (hasIdr && !hasSps && !mSps.empty() && !mPps.empty()) {
		static const uint8_t sc[4] = {0, 0, 0, 1};
		mBuf.clear();
		mBuf.insert(mBuf.end(), sc, sc + 4);
		mBuf.insert(mBuf.end(), mSps.begin(), mSps.end());
		mBuf.insert(mBuf.end(), sc, sc + 4);
		mBuf.insert(mBuf.end(), mPps.begin(), mPps.end());
		mBuf.insert(mBuf.end(), data, data + size);
		onPacket(mBuf.data(), mBuf.size(), true);
		return;
	}
	onPacket(data, size, hasIdr);
}

} // namespace pd
