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
class DeviceStore {
public:
	explicit DeviceStore(std::filesystem::path file);

	// 当前有效的配对码；过期或被用掉后自动换新的
	std::string pairCode(int &secondsLeft);
	// 最近一次配对成功的设备名（给电脑上的配对页显示）
	std::string lastPaired();
	bool pair(const std::string &code, const std::string &name, std::string &token);
	bool verify(const std::string &token, std::string &name);
	nlohmann::json list();
	bool remove(const std::string &id);

private:
	struct Device {
		std::string id, name, tokenHash;
		int64_t created = 0, lastSeen = 0;
	};
	void load();
	void save();
	void newCode();

	std::filesystem::path mFile;
	std::mutex mMutex;
	std::vector<Device> mDevices;
	std::string mCode;
	std::chrono::steady_clock::time_point mCodeExpiry{};
	std::string mLastPaired;
};

} // namespace pd
