#include "encoder.h"
#include "log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}

#include <chrono>
#include <thread>

namespace pd {

static std::string avErr(int code) {
	char buf[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, buf, sizeof(buf));
	return buf;
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
	mCtx->pix_fmt = AV_PIX_FMT_D3D11;
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
	av_buffer_unref(&mDeviceRef);
	mSps.clear();
	mPps.clear();
}

bool VideoEncoder::beginFrame(ID3D11Texture2D **tex, UINT *slice) {
	av_frame_unref(mFrame);
	int ret = av_hwframe_get_buffer(mFramesRef, mFrame, 0);
	if (ret < 0) {
		log("av_hwframe_get_buffer 失败: {}", avErr(ret));
		return false;
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
	av_frame_unref(mFrame);
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
