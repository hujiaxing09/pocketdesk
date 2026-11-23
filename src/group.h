#pragma once
#include "discovery.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace pd {

// 这台电脑的编号：第一次运行时定下来存进 pc.json，以后不变。
// 以前的版本用根证书指纹当编号（手机记着它）；加入组后根证书会换，所以单独存。
std::string loadPcId(const std::filesystem::path &dataDir);

// 电脑组：组里的电脑共用一个根证书（手机信任一次就够）和一份组记录（成员电脑、已授权的手机）。
// 每台电脑都属于一个组，单独一台就是只有自己的组。
//
// 组记录在组里自动同步：局域网广播带着组编号和记录版本，版本不同就向对方拉一份合并。
// 合并规则只有一条：每条记录带修改时间 t，取新的；删除不真删，标记 removed，
// 这样关着机的电脑开机后也会知道。
//
// 手机授权：电脑显示一次性配对码（二维码里的长码和给人输入的 6 位数字），手机用它换一个长期令牌，
// 这个令牌在组里所有电脑上都有效。电脑只保存令牌的 SHA-256。
class Group {
public:
	struct Self {
		std::string id, name;
		int https = 0, http = 0;
		std::vector<std::string> ips;
	};
	// certs/ 下的根证书由组决定：加入、退出组时会被替换或删掉，之后要重启
	Group(std::filesystem::path dataDir, Self self);

	std::string id();
	std::string name();
	std::string rev();
	// 加入、退出或被移出了组：根证书变了，要重启
	bool needRestart();

	// ---- 手机 ----
	// 当前有效的配对码；过期、用掉或输错 5 次后换新的。手机授权和别的电脑加入组都用它
	std::string pairCode(int &secondsLeft, std::string *shortCode = nullptr);
	std::string lastPaired();
	bool pairPhone(const std::string &code, const std::string &name, const std::string &phoneId,
	               const std::string &ip, std::string &token);
	bool verify(const std::string &token, const std::string &ip, std::string &name,
	            std::string *deviceId = nullptr);
	nlohmann::json phones();
	bool removePhone(const std::string &id);

	// ---- 电脑 ----
	// 组里的电脑（online 是局域网里现在看得到的）
	nlohmann::json members(const std::vector<Discovery::Peer> &online);
	// 局域网里不在本组的 PocketDesk 电脑
	nlohmann::json strangers(const std::vector<Discovery::Peer> &online);
	// 移出一台电脑；移出自己就是退出组
	bool removeMember(const std::string &id);
	// 别的电脑带着这台电脑的配对码来加入：返回组的全部内容（含根证书私钥），码不对返回 null
	nlohmann::json acceptJoin(const std::string &code, const nlohmann::json &pc);
	// 这台电脑加入 ip 那台电脑所在的组；返回错误说明，成功返回空串（之后要重启）
	std::string join(const std::string &ip, int https, const std::string &code);

	// ---- 同步 ----
	bool checkSecret(const std::string &secret);
	nlohmann::json stateFor(const std::string &pcId);
	// 向版本不同的同组电脑拉取并合并（force：不管版本，向所有在线的同组电脑拉一次）。
	// 返回刚被移除的手机，调用方要断开它们
	std::vector<std::string> sync(const std::vector<Discovery::Peer> &online, bool force);

private:
	using Clock = std::chrono::steady_clock;
	void load();
	void save();
	void fresh();
	void updateSelf();
	void reset(bool restart);
	void newCode();
	bool checkCode(const std::string &code);
	nlohmann::json pull(const Discovery::Peer &peer, const std::string &secret, const std::string &caPem);
	void merge(const nlohmann::json &st, std::vector<std::string> &removed);

	std::filesystem::path mDir, mFile;
	Self mSelf;
	std::mutex mMutex;
	nlohmann::json mState; // id、name、secret、members、devices（同步的部分）
	nlohmann::json mLocal; // 手机最近使用时间和 IP（只记在本机）
	std::string mRev;
	bool mNeedRestart = false;
	Clock::time_point mLeaveAt{};

	std::string mCode, mShortCode, mLastPaired;
	int mFailures = 0;
	Clock::time_point mCodeExpiry{};

	std::mutex mSyncMutex; // 同一时间只做一轮同步
	std::map<std::string, Clock::time_point> mLastPull;
	std::map<std::string, std::string> mPullError;
	Clock::time_point mLastForced{};
};

} // namespace pd
