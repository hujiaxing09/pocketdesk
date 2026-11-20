#include "devices.h"
#include "log.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <fstream>

namespace pd {

namespace {

constexpr auto kCodeLifetime = std::chrono::minutes(10);
constexpr int kMaxFailures = 5;
constexpr size_t kMaxTokensPerDevice = 4;

std::string randomHex(size_t bytes) {
	std::vector<unsigned char> b(bytes);
	RAND_bytes(b.data(), int(b.size()));
	std::string s;
	for (unsigned char c : b)
		s += std::format("{:02x}", c);
	return s;
}

std::string sha256Hex(const std::string &data) {
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int len = 0;
	EVP_Digest(data.data(), data.size(), md, &len, EVP_sha256(), nullptr);
	std::string s;
	for (unsigned int i = 0; i < len; ++i)
		s += std::format("{:02x}", md[i]);
	return s;
}

int64_t nowSeconds() {
	return std::chrono::duration_cast<std::chrono::seconds>(
	           std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

} // namespace

DeviceStore::DeviceStore(std::filesystem::path file) : mFile(std::move(file)) {
	load();
	newCode();
}

void DeviceStore::load() {
	std::ifstream f(mFile);
	if (!f)
		return;
	auto j = nlohmann::json::parse(f, nullptr, false);
	if (!j.is_array())
		return;
	for (auto &d : j) {
		Device dev;
		dev.id = d.value("id", "");
		dev.name = d.value("name", "");
		dev.phoneId = d.value("phoneId", "");
		dev.lastIp = d.value("lastIp", "");
		for (auto &h : d.value("tokenHashes", nlohmann::json::array()))
			if (h.is_string())
				dev.tokenHashes.push_back(h.get<std::string>());
		if (d.contains("tokenHash")) // 旧格式只有一个令牌
			dev.tokenHashes.push_back(d.value("tokenHash", ""));
		dev.created = d.value("created", int64_t(0));
		dev.lastSeen = d.value("lastSeen", int64_t(0));
		if (!dev.id.empty() && !dev.tokenHashes.empty())
			mDevices.push_back(dev);
	}
	log("已授权的手机: {} 部", mDevices.size());
}

void DeviceStore::save() {
	nlohmann::json j = nlohmann::json::array();
	for (auto &d : mDevices)
		j.push_back({{"id", d.id},
		             {"name", d.name},
		             {"phoneId", d.phoneId},
		             {"lastIp", d.lastIp},
		             {"tokenHashes", d.tokenHashes},
		             {"created", d.created},
		             {"lastSeen", d.lastSeen}});
	auto tmp = mFile;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::trunc);
		f << j.dump(2);
	}
	std::error_code ec;
	std::filesystem::rename(tmp, mFile, ec);
	if (ec)
		log("保存设备列表失败: {}", ec.message());
}

void DeviceStore::newCode() {
	mCode = randomHex(8);
	uint32_t r = 0;
	RAND_bytes(reinterpret_cast<unsigned char *>(&r), sizeof(r));
	mShortCode = std::format("{:06}", r % 1000000);
	mFailures = 0;
	mCodeExpiry = std::chrono::steady_clock::now() + kCodeLifetime;
}

std::string DeviceStore::pairCode(int &secondsLeft, std::string *shortCode) {
	std::lock_guard lock(mMutex);
	auto now = std::chrono::steady_clock::now();
	if (now >= mCodeExpiry)
		newCode();
	secondsLeft = int(std::chrono::duration_cast<std::chrono::seconds>(mCodeExpiry - now).count());
	if (shortCode)
		*shortCode = mShortCode;
	return mCode;
}

std::string DeviceStore::lastPaired() {
	std::lock_guard lock(mMutex);
	return mLastPaired;
}

bool DeviceStore::pair(const std::string &code, const std::string &name, const std::string &phoneId,
                       const std::string &ip, std::string &token) {
	std::lock_guard lock(mMutex);
	if (std::chrono::steady_clock::now() >= mCodeExpiry)
		newCode();
	const bool ok = !code.empty() && (code == mCode || code == mShortCode);
	if (!ok) {
		// 6 位数字只有一百万种，输错 5 次就换一组，防止被猜中
		if (++mFailures >= kMaxFailures) {
			log("配对码连续输错 {} 次，已换新的配对码", mFailures);
			newCode();
		}
		return false;
	}
	newCode(); // 一次性
	const std::string displayName = name.empty() ? "手机" : name.substr(0, 60);
	const std::string pid = phoneId.substr(0, 64);
	token = randomHex(32);

	// 是不是已经授权过的同一部手机
	Device *same = nullptr;
	for (auto &d : mDevices)
		if (!pid.empty() && d.phoneId == pid)
			same = &d;
	if (!same)
		for (auto &d : mDevices)
			if (d.name == displayName && !ip.empty() && d.lastIp == ip)
				same = &d;

	const int64_t now = nowSeconds();
	if (same) {
		same->tokenHashes.push_back(sha256Hex(token));
		while (same->tokenHashes.size() > kMaxTokensPerDevice)
			same->tokenHashes.erase(same->tokenHashes.begin());
		same->name = displayName;
		same->lastIp = ip;
		same->lastSeen = now;
		if (!pid.empty())
			same->phoneId = pid;
		mLastPaired = same->name;
		save();
		log("同一部手机再次授权，合并到原记录: {}（{}，{} 个令牌）", same->name, same->id,
		    same->tokenHashes.size());
		return true;
	}
	Device d;
	d.id = randomHex(6);
	d.name = displayName;
	d.phoneId = pid;
	d.lastIp = ip;
	d.tokenHashes.push_back(sha256Hex(token));
	d.created = d.lastSeen = now;
	mDevices.push_back(d);
	mLastPaired = d.name;
	save();
	log("新手机已授权: {}（{}，{}）", d.name, d.id, ip);
	return true;
}

bool DeviceStore::verify(const std::string &token, const std::string &ip, std::string &name,
                         std::string *deviceId) {
	if (token.empty())
		return false;
	const std::string hash = sha256Hex(token);
	std::lock_guard lock(mMutex);
	for (auto &d : mDevices) {
		if (std::find(d.tokenHashes.begin(), d.tokenHashes.end(), hash) != d.tokenHashes.end()) {
			name = d.name;
			if (deviceId)
				*deviceId = d.id;
			int64_t now = nowSeconds();
			if (now - d.lastSeen > 60 || d.lastIp != ip) {
				d.lastSeen = now;
				d.lastIp = ip;
				save();
			}
			return true;
		}
	}
	return false;
}

nlohmann::json DeviceStore::list() {
	std::lock_guard lock(mMutex);
	nlohmann::json j = nlohmann::json::array();
	for (auto &d : mDevices)
		j.push_back({{"id", d.id},
		             {"name", d.name},
		             {"ip", d.lastIp},
		             {"browsers", d.tokenHashes.size()},
		             {"created", d.created},
		             {"lastSeen", d.lastSeen}});
	return j;
}

bool DeviceStore::remove(const std::string &id) {
	std::lock_guard lock(mMutex);
	auto it = std::find_if(mDevices.begin(), mDevices.end(), [&](auto &d) { return d.id == id; });
	if (it == mDevices.end())
		return false;
	log("已移除手机授权: {}（{}）", it->name, it->id);
	mDevices.erase(it);
	save();
	return true;
}

} // namespace pd
