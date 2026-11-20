#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace pd {

// 手机授权：电脑显示一次性配对码（放在二维码里），手机用它换一个长期令牌，
// 以后每次连接都带上令牌。电脑只保存令牌的 SHA-256。
// 同一部手机再次授权（Safari 和主屏幕 App 各授权一次、或手机删掉电脑后重加）时合并成一条记录：
// 先按手机自己生成的编号（phoneId）认，认不出时按“同名且来自同一个 IP”认。
// 一条记录最多保留最近 4 个令牌（每个浏览器环境一个）。
class DeviceStore {
public:
	explicit DeviceStore(std::filesystem::path file);

	// 当前有效的配对码（二维码里的长码，和给人输入的 6 位数字）；过期、用掉或输错 5 次后换新的
	std::string pairCode(int &secondsLeft, std::string *shortCode = nullptr);
	// 最近一次配对成功的设备名（给电脑上的配对页显示）
	std::string lastPaired();
	bool pair(const std::string &code, const std::string &name, const std::string &phoneId,
	          const std::string &ip, std::string &token);
	bool verify(const std::string &token, const std::string &ip, std::string &name,
	            std::string *deviceId = nullptr);
	nlohmann::json list();
	bool remove(const std::string &id);

private:
	struct Device {
		std::string id, name, phoneId, lastIp;
		std::vector<std::string> tokenHashes; // 最新的在后面
		int64_t created = 0, lastSeen = 0;
	};
	void load();
	void save();
	void newCode();

	std::filesystem::path mFile;
	std::mutex mMutex;
	std::vector<Device> mDevices;
	std::string mCode;
	std::string mShortCode;
	int mFailures = 0;
	std::chrono::steady_clock::time_point mCodeExpiry{};
	std::string mLastPaired;
};

} // namespace pd
