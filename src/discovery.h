#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pd {

// 局域网发现：每台电脑定时在本网段广播“我在这里”（UDP），同时收听别的电脑的广播。
// 电脑之间只是互相看得见，不互相信任；手机通过任意一台电脑拿到列表，再自己去连接和授权。
class Discovery {
public:
	struct Self {
		std::string id, name, version;
		int httpsPort = 0, httpPort = 0;
	};
	struct Peer {
		std::string id, name, ip, version; // ip：收到广播的来源地址
		std::vector<std::string> ips;      // 对方报告的全部局域网地址（可能有多个网段）
		int httpsPort = 0, httpPort = 0;
		std::chrono::steady_clock::time_point seen;
	};

	~Discovery() { stop(); }
	bool start(const Self &self, uint16_t port);
	void stop();
	// 最近 20 秒内听到过的其他电脑
	std::vector<Peer> peers();

private:
	void run();
	void announce(uintptr_t sock, const char *toIp);

	Self mSelf;
	uint16_t mPort = 0;
	std::atomic<bool> mRunning{false};
	std::thread mThread;
	std::mutex mMutex;
	std::map<std::string, Peer> mPeers; // 键：id + 端口（同一台电脑可以跑测试用的第二个实例）
};

} // namespace pd
