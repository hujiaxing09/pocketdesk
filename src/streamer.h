#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace rtc {
class PeerConnection;
class Track;
class DataChannel;
class RtpPacketizationConfig;
} // namespace rtc

namespace pd {

// 每部手机一个 WebRTC 连接；所有连接共享同一路编码输出。
// 信令由电脑发 offer，手机回 answer（两次 HTTPS 请求）。
class Streamer {
public:
	struct Options {
		uint16_t portBegin = 47500; // ICE 用的 UDP 端口范围，方便以后写防火墙规则
		uint16_t portEnd = 47599;
		int playoutDelayMaxMs = 0;  // 通过 playout-delay 扩展请求手机尽量不缓冲
	};

	explicit Streamer(Options options);
	~Streamer();

	bool createSession(std::string &id, std::string &offerSdp);
	bool acceptAnswer(const std::string &id, const std::string &answerSdp);

	bool hasViewers();
	// 有新连接或手机请求关键帧（PLI）时为 true，读取后清零
	bool takeKeyframeRequest() { return mKeyRequest.exchange(false); }
	int takePliCount() { return mPliCount.exchange(0); }

	void sendVideo(const uint8_t *data, size_t size, bool keyframe);
	void sendControl(const std::string &json);
	// 清理已断开或超时未连上的连接；在截屏线程里定期调用
	void sweep();

private:
	struct Session;
	Options mOptions;
	std::mutex mMutex;
	std::map<std::string, std::shared_ptr<Session>> mSessions;
	std::atomic<bool> mKeyRequest{false};
	std::atomic<int> mPliCount{0};
};

} // namespace pd
