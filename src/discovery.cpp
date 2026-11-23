#include "discovery.h"
#include "log.h"
#include "netinfo.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <nlohmann/json.hpp>

namespace pd {

namespace {
constexpr auto kAnnounceEvery = std::chrono::seconds(5);
constexpr auto kForgetAfter = std::chrono::seconds(20);
} // namespace

bool Discovery::start(const Self &self, uint16_t port) {
	mSelf = self;
	mPort = port;
	mRunning = true;
	mThread = std::thread([this] { run(); });
	return true;
}

void Discovery::setGroup(const std::string &id, const std::string &name, const std::string &rev) {
	std::lock_guard lock(mMutex);
	if (mSelf.group == id && mSelf.groupName == name && mSelf.rev == rev)
		return;
	mSelf.group = id;
	mSelf.groupName = name;
	mSelf.rev = rev;
	mAnnounceNow = true;
}

void Discovery::stop() {
	mRunning = false;
	if (mThread.joinable())
		mThread.join();
}

std::vector<Discovery::Peer> Discovery::peers() {
	std::lock_guard lock(mMutex);
	std::vector<Peer> result;
	const auto now = std::chrono::steady_clock::now();
	for (auto it = mPeers.begin(); it != mPeers.end();) {
		if (now - it->second.seen > kForgetAfter) {
			log("局域网里的电脑 {}（{}）不见了", it->second.name, it->second.ip);
			it = mPeers.erase(it);
		} else {
			result.push_back(it->second);
			++it;
		}
	}
	return result;
}

void Discovery::announce(uintptr_t sock, const char *toIp) {
	nlohmann::json j;
	{
		std::lock_guard lock(mMutex);
		j = {{"pd", "announce"},
		     {"id", mSelf.id},
		     {"name", mSelf.name},
		     {"https", mSelf.httpsPort},
		     {"http", mSelf.httpPort},
		     {"ver", mSelf.version},
		     {"group", mSelf.group},
		     {"gname", mSelf.groupName},
		     {"rev", mSelf.rev}};
	}
	j["ips"] = localLanIPv4();
	const std::string msg = j.dump();
	sockaddr_in to{};
	to.sin_family = AF_INET;
	to.sin_port = htons(mPort);
	inet_pton(AF_INET, toIp, &to.sin_addr);
	sendto(SOCKET(sock), msg.data(), int(msg.size()), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
}

void Discovery::run() {
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	BOOL on = TRUE;
	// 允许同一台电脑上的测试实例共用端口；都能收到广播
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof(on));
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&on), sizeof(on));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(mPort);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
		log("局域网发现：UDP 端口 {} 绑定失败（{}），只能手动添加电脑", mPort, WSAGetLastError());
		closesocket(s);
		WSACleanup();
		return;
	}
	log("局域网发现已启动（UDP {}），本机编号 {}", mPort, mSelf.id);

	auto lastAnnounce = std::chrono::steady_clock::time_point{};
	char buf[2048];
	while (mRunning) {
		const auto now = std::chrono::steady_clock::now();
		if (now - lastAnnounce >= kAnnounceEvery || mAnnounceNow.exchange(false)) {
			// 向每个网段的广播地址发；只发 255.255.255.255 的话 Windows 只走一块网卡
			for (auto &i : lanInterfaces())
				announce(s, i.broadcast.c_str());
			announce(s, "255.255.255.255");
			lastAnnounce = now;
		}

		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(s, &rd);
		timeval tv{0, 500 * 1000};
		if (select(0, &rd, nullptr, nullptr, &tv) <= 0)
			continue;
		sockaddr_in from{};
		int fromLen = sizeof(from);
		int n = recvfrom(s, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &fromLen);
		if (n <= 0)
			continue;
		// 广播谁都能发：字段类型不对时 value() 会抛异常，整条丢掉
		Peer p;
		try {
			auto j = nlohmann::json::parse(std::string(buf, size_t(n)), nullptr, false);
			if (j.is_discarded() || j.value("pd", "") != "announce")
				continue;
			p.id = j.value("id", "");
			p.name = j.value("name", "");
			p.version = j.value("ver", "");
			p.httpsPort = j.value("https", 0);
			p.httpPort = j.value("http", 0);
			p.group = j.value("group", "");
			p.groupName = j.value("gname", "");
			p.rev = j.value("rev", "");
			if (p.id.empty() || p.httpsPort <= 0 || (p.id == mSelf.id && p.httpsPort == mSelf.httpsPort))
				continue;
			char ip[INET_ADDRSTRLEN] = {};
			inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
			p.ip = ip;
			p.ips.push_back(ip);
			for (auto &a : j.value("ips", nlohmann::json::array()))
				if (a.is_string() && a.get<std::string>() != p.ip)
					p.ips.push_back(a.get<std::string>());
		} catch (const std::exception &) {
			continue;
		}
		p.seen = now;
		bool isNew;
		{
			std::lock_guard lock(mMutex);
			const std::string key = p.id + ":" + std::to_string(p.httpsPort);
			auto it = mPeers.find(key);
			isNew = it == mPeers.end();
			mPeers[key] = p;
		}
		if (isNew) {
			log("发现局域网里的电脑 {}（{}:{}，编号 {}，地址 {} 个）", p.name, p.ip, p.httpsPort, p.id,
			    p.ips.size());
			announce(s, p.ip.c_str()); // 马上回一声，对方不用等下一轮广播
		}
	}
	closesocket(s);
	WSACleanup();
}

} // namespace pd
