#pragma once
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace pd {

struct EncoderConfig {
	std::string name; // h264_nvenc / h264_amf / h264_qsv
	int width = 0;
	int height = 0;
	int fps = 60;
	int bitrateKbps = 12000;
};

// 显卡厂商对应的硬件编码器（截屏和编码必须在同一块显卡上）
std::string encoderForVendor(UINT vendorId);

// FFmpeg 硬件 H.264 编码，输入是同一 D3D11 设备上的 NV12 纹理。
// 参数照 Sunshine：无 B 帧、CBR、约一帧大小的 VBV、按需 IDR。
// NVIDIA、AMD 的编码器直接收 D3D11 纹理；Intel 的 h264_qsv 只收 QSV 帧，
// 所以先从 D3D11 设备派生 QSV 设备，用 QSV 帧池，往每个 QSV 帧底下的 D3D11 纹理里画（同 Sunshine）。
class VideoEncoder {
public:
	using PacketFn = std::function<void(const uint8_t *data, size_t size, bool keyframe)>;

	~VideoEncoder() { close(); }
	bool open(ID3D11Device *device, const EncoderConfig &cfg);
	void close();
	bool isOpen() const { return mCtx != nullptr; }
	const EncoderConfig &config() const { return mCfg; }

	// 修改码率。NVENC 支持在编码过程中直接改；其他编码器返回 false，需要重开编码器
	bool setBitrate(int kbps);

	// 从编码器的纹理池里取一帧，调用方把画面画进 (tex, slice)
	bool beginFrame(ID3D11Texture2D **tex, UINT *slice);
	// 编码这一帧，输出的 Annex B 码流交给 onPacket；IDR 一定带 SPS/PPS
	bool endFrame(int64_t pts90k, bool forceKeyframe, const PacketFn &onPacket);

private:
	void handlePacket(const uint8_t *data, size_t size, const PacketFn &onPacket);

	EncoderConfig mCfg;
	AVBufferRef *mDeviceRef = nullptr;    // D3D11 设备（截屏用的那个）
	AVBufferRef *mQsvDeviceRef = nullptr; // Intel：从 D3D11 设备派生的 QSV 设备
	AVBufferRef *mFramesRef = nullptr;
	AVCodecContext *mCtx = nullptr;
	AVFrame *mFrame = nullptr;
	AVPacket *mPkt = nullptr;
	std::vector<uint8_t> mSps, mPps, mBuf;
	bool mWarnedDelay = false;
};

} // namespace pd
