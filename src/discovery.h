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
// 广播里带着所在的组和组记录的版本：同组电脑据此发现记录不一致、去同步；
// 不在组里的电脑显示在配对页上，可以加入它的组。广播本身不可信，只用来“提醒”。
class Discovery {
public:
	struct Self {
		std::string id, name, version;
		int httpsPort = 0, httpPort = 0;
		std::string group, groupName, rev;
	};
	struct Peer {
		std::string id, name, ip, version; // ip：收到广播的来源地址
		std::vector<std::string> ips;      // 对方报告的全部局域网地址（可能有多个网段）
		std::string group, groupName, rev; // 1.0.0 的电脑没有这些
		int httpsPort = 0, httpPort = 0;
		std::chrono::steady_clock::time_point seen;
	};

	~Discovery() { stop(); }
	bool start(const Self &self, uint16_t port);
	void stop();
	// 组记录变了：马上广播一次，让同组电脑尽快来同步
	void setGroup(const std::string &id, const std::string &name, const std::string &rev);
	// 最近 20 秒内听到过的其他电脑
	std::vector<Peer> peers();

private:
	void run();
	void announce(uintptr_t sock, const char *toIp);

	Self mSelf;
	uint16_t mPort = 0;
	std::atomic<bool> mRunning{false};
	std::atomic<bool> mAnnounceNow{false};
	std::thread mThread;
	std::mutex mMutex;
	std::map<std::string, Peer> mPeers; // 键：id + 端口（同一台电脑可以跑测试用的第二个实例）
};

} // namespace pd
