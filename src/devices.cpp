#include "devices.h"
#include "log.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <fstream>

namespace pd {

namespace {

constexpr auto kCodeLifetime = std::chrono::minutes(10);

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
		dev.tokenHash = d.value("tokenHash", "");
		dev.created = d.value("created", int64_t(0));
		dev.lastSeen = d.value("lastSeen", int64_t(0));
		if (!dev.id.empty() && !dev.tokenHash.empty())
			mDevices.push_back(dev);
	}
	log("已授权的手机: {} 部", mDevices.size());
}

void DeviceStore::save() {
	nlohmann::json j = nlohmann::json::array();
	for (auto &d : mDevices)
		j.push_back({{"id", d.id},
		             {"name", d.name},
		             {"tokenHash", d.tokenHash},
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
	mCodeExpiry = std::chrono::steady_clock::now() + kCodeLifetime;
}

std::string DeviceStore::pairCode(int &secondsLeft) {
	std::lock_guard lock(mMutex);
	auto now = std::chrono::steady_clock::now();
	if (now >= mCodeExpiry)
		newCode();
	secondsLeft = int(std::chrono::duration_cast<std::chrono::seconds>(mCodeExpiry - now).count());
	return mCode;
}

std::string DeviceStore::lastPaired() {
	std::lock_guard lock(mMutex);
	return mLastPaired;
}

bool DeviceStore::pair(const std::string &code, const std::string &name, std::string &token) {
	std::lock_guard lock(mMutex);
	if (code.empty() || code != mCode || std::chrono::steady_clock::now() >= mCodeExpiry)
		return false;
	newCode(); // 一次性
	Device d;
	d.id = randomHex(6);
	d.name = name.empty() ? "手机" : name.substr(0, 60);
	token = randomHex(32);
	d.tokenHash = sha256Hex(token);
	d.created = d.lastSeen = nowSeconds();
	mDevices.push_back(d);
	mLastPaired = d.name;
	save();
	log("新手机已授权: {}（{}）", d.name, d.id);
	return true;
}

bool DeviceStore::verify(const std::string &token, std::string &name) {
	if (token.empty())
		return false;
	const std::string hash = sha256Hex(token);
	std::lock_guard lock(mMutex);
	for (auto &d : mDevices) {
		if (d.tokenHash == hash) {
			name = d.name;
			int64_t now = nowSeconds();
			if (now - d.lastSeen > 60) {
				d.lastSeen = now;
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
		j.push_back({{"id", d.id}, {"name", d.name}, {"created", d.created}, {"lastSeen", d.lastSeen}});
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
