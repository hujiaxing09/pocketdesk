#include <httplib.h>

#include "group.h"
#include "log.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <fstream>
#include <sstream>

namespace pd {

using json = nlohmann::json;

namespace {

constexpr auto kCodeLifetime = std::chrono::minutes(10);
constexpr int kMaxFailures = 5;
constexpr size_t kMaxTokensPerDevice = 4;
constexpr auto kLeaveDelay = std::chrono::seconds(3); // 退出组前留给其他电脑同步的时间
constexpr auto kPullEvery = std::chrono::seconds(2);

std::string randomHex(size_t bytes) {
	std::vector<unsigned char> b(bytes);
	RAND_bytes(b.data(), int(b.size()));
	std::string s;
	for (unsigned char c : b)
		s += std::format("{:02x}", c);
	return s;
}

std::string sha256Hex(const std::string &data, size_t bytes = 32) {
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int len = 0;
	EVP_Digest(data.data(), data.size(), md, &len, EVP_sha256(), nullptr);
	std::string s;
	for (unsigned int i = 0; i < len && i < bytes; ++i)
		s += std::format("{:02x}", md[i]);
	return s;
}

int64_t nowSeconds() {
	using namespace std::chrono;
	return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// 记录被修改：时间取“现在”，并且一定比原来的新（电脑之间时钟差一点也不会丢改动）
int64_t timeOf(const json &rec) {
	return rec.is_object() && rec.contains("t") && rec["t"].is_number_integer() ? rec["t"].get<int64_t>() : 0;
}

bool isRemoved(const json &rec) {
	return rec.is_object() && rec.contains("removed") && rec["removed"].is_boolean() && rec["removed"].get<bool>();
}

// 手机在这台电脑上的最近使用记录（没有时是空对象）
json seenOf(const json &local, const std::string &k) {
	return local.contains(k) && local[k].is_object() ? local[k] : json::object();
}

// list 里键为 k 的那条记录是否已标记删除
bool removedIn(const json &list, const std::string &k) { return list.contains(k) && isRemoved(list[k]); }

void stamp(json &rec) {
	using namespace std::chrono;
	const int64_t now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
	rec["t"] = std::max(now, timeOf(rec) + 1);
}

// a 是否应该替换 b：新的赢；同一时间删除的赢；再相同就按内容比，保证每台电脑选的一样
bool newer(const json &a, const json &b) {
	const auto ta = timeOf(a), tb = timeOf(b);
	if (ta != tb)
		return ta > tb;
	if (isRemoved(a) != isRemoved(b))
		return isRemoved(a);
	return a.dump() > b.dump();
}

std::string readText(const std::filesystem::path &p) {
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

bool writeText(const std::filesystem::path &p, const std::string &text) {
	auto tmp = p;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!(f << text))
			return false;
	}
	std::error_code ec;
	std::filesystem::rename(tmp, p, ec);
	return !ec;
}

void removeFiles(const std::filesystem::path &dir, std::initializer_list<const char *> names) {
	std::error_code ec;
	for (auto n : names)
		std::filesystem::remove(dir / n, ec);
}

} // namespace

std::string loadPcId(const std::filesystem::path &dataDir) {
	const auto file = dataDir / "pc.json";
	auto j = json::parse(readText(file), nullptr, false);
	if (j.is_object() && !j.value("id", "").empty())
		return j["id"];
	// 第一次用这个版本：沿用以前的算法（根证书指纹前 8 字节），手机记着的编号不变；新装的随机生成
	std::string id;
	const std::string pem = readText(dataDir / "certs" / "ca.crt");
	if (BIO *bio = BIO_new_mem_buf(pem.data(), int(pem.size()))) {
		if (X509 *ca = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
			std::string der(size_t(i2d_X509(ca, nullptr)), '\0');
			auto *p = reinterpret_cast<unsigned char *>(der.data());
			i2d_X509(ca, &p);
			id = sha256Hex(der, 8);
			X509_free(ca);
		}
		BIO_free(bio);
	}
	if (id.empty())
		id = randomHex(8);
	std::error_code ec;
	std::filesystem::create_directories(dataDir, ec);
	writeText(file, json{{"id", id}}.dump());
	return id;
}

Group::Group(std::filesystem::path dataDir, Self self)
    : mDir(std::move(dataDir)), mFile(mDir / "group.json"), mSelf(std::move(self)) {
	load();
	newCode();
}

// ---------------- 存储 ----------------

void Group::fresh() {
	mState = {{"id", randomHex(8)},
	          {"name", mSelf.name},
	          {"secret", randomHex(32)},
	          {"members", json::object()},
	          {"devices", json::object()}};
	mLocal = json::object();
}

void Group::load() {
	auto j = json::parse(readText(mFile), nullptr, false);
	if (j.is_object() && !j.value("id", "").empty() && !j.value("secret", "").empty()) {
		mLocal = j.value("local", json::object());
		j.erase("local");
		mState = std::move(j);
	} else {
		fresh();
		// 以前的版本：授权记录在 devices.json，搬进组记录，手机不用重新授权
		const auto old = mDir / "devices.json";
		auto devs = json::parse(readText(old), nullptr, false);
		if (devs.is_array()) {
			for (auto &d : devs) {
				const std::string id = d.value("id", "");
				auto hashes = d.value("tokenHashes", json::array());
				if (d.contains("tokenHash"))
					hashes.push_back(d["tokenHash"]);
				if (id.empty() || hashes.empty())
					continue;
				mState["devices"][id] = {{"name", d.value("name", "")},
				                         {"phoneId", d.value("phoneId", "")},
				                         {"tokenHashes", hashes},
				                         {"created", d.value("created", int64_t(0))},
				                         {"t", d.value("created", int64_t(0)) * 1000},
				                         {"removed", false}};
				mLocal[id] = {{"lastSeen", d.value("lastSeen", int64_t(0))}, {"lastIp", d.value("lastIp", "")}};
			}
			std::error_code ec;
			std::filesystem::rename(old, mDir / "devices.json.bak", ec);
			log("已把 {} 部手机的授权搬进组记录", mState["devices"].size());
		}
	}
	for (auto k : {"members", "devices"})
		if (!mState[k].is_object())
			mState[k] = json::object();
	if (!mLocal.is_object())
		mLocal = json::object();
	// 上次退出组没做完（或者刚被移出）：现在就变回只有自己的组
	if (removedIn(mState["members"], mSelf.id))
		reset(false);
	updateSelf();
	save();
	size_t phones = 0, pcs = 0;
	for (auto &[k, d] : mState["devices"].items())
		phones += !isRemoved(d);
	for (auto &[k, m] : mState["members"].items())
		pcs += !isRemoved(m);
	log("电脑组 {}（{}）：{} 台电脑，{} 部手机", mState["name"].get<std::string>(), mState["id"].get<std::string>(),
	    pcs, phones);
}

void Group::save() {
	json j = mState;
	j["local"] = mLocal;
	if (!writeText(mFile, j.dump(1, '\t')))
		log("保存组记录失败: {}", mFile.string());
	mRev = sha256Hex(json{{"members", mState["members"]}, {"devices", mState["devices"]}}.dump(), 8);
}

// 自己在成员列表里的那条：名字、端口、地址有变化才改
void Group::updateSelf() {
	json rec = {{"name", mSelf.name}, {"https", mSelf.https}, {"http", mSelf.http}, {"ips", mSelf.ips},
	            {"removed", false}};
	auto &me = mState["members"][mSelf.id];
	if (me.is_object()) {
		json cur = me;
		cur.erase("t");
		if (cur == rec)
			return;
		rec["t"] = timeOf(me);
	}
	stamp(rec);
	me = rec;
}

// 变回只有自己的组：新的组编号、密钥和根证书，没有已授权的手机
void Group::reset(bool restart) {
	removeFiles(mDir / "certs", {"ca.key", "ca.crt", "server.key", "server.crt"});
	fresh();
	updateSelf();
	save();
	mLeaveAt = {};
	mNeedRestart = restart;
	log("已离开原来的组，现在是只有这台电脑的新组 {}", mState["id"].get<std::string>());
}

std::string Group::id() {
	std::lock_guard lock(mMutex);
	return mState["id"];
}

std::string Group::name() {
	std::lock_guard lock(mMutex);
	return mState["name"];
}

std::string Group::rev() {
	std::lock_guard lock(mMutex);
	return mRev;
}

bool Group::needRestart() {
	std::lock_guard lock(mMutex);
	return mNeedRestart;
}

// ---------------- 配对码 ----------------

void Group::newCode() {
	mCode = randomHex(8);
	uint32_t r = 0;
	RAND_bytes(reinterpret_cast<unsigned char *>(&r), sizeof(r));
	mShortCode = std::format("{:06}", r % 1000000);
	mFailures = 0;
	mCodeExpiry = Clock::now() + kCodeLifetime;
}

std::string Group::pairCode(int &secondsLeft, std::string *shortCode) {
	std::lock_guard lock(mMutex);
	auto now = Clock::now();
	if (now >= mCodeExpiry)
		newCode();
	secondsLeft = int(std::chrono::duration_cast<std::chrono::seconds>(mCodeExpiry - now).count());
	if (shortCode)
		*shortCode = mShortCode;
	return mCode;
}

std::string Group::lastPaired() {
	std::lock_guard lock(mMutex);
	return mLastPaired;
}

// 调用方持有 mMutex。对了就换新码（一次性）
bool Group::checkCode(const std::string &code) {
	if (Clock::now() >= mCodeExpiry)
		newCode();
	if (code.empty() || (code != mCode && code != mShortCode)) {
		// 6 位数字只有一百万种，输错 5 次就换一组，防止被猜中
		if (++mFailures >= kMaxFailures) {
			log("配对码连续输错 {} 次，已换新的配对码", mFailures);
			newCode();
		}
		return false;
	}
	newCode();
	return true;
}

// ---------------- 手机 ----------------

bool Group::pairPhone(const std::string &code, const std::string &name, const std::string &phoneId,
                      const std::string &ip, std::string &token) {
	std::lock_guard lock(mMutex);
	if (!checkCode(code))
		return false;
	const std::string displayName = name.empty() ? "手机" : name.substr(0, 60);
	const std::string pid = phoneId.substr(0, 64);
	token = randomHex(32);

	// 同一部手机再次授权（Safari 和主屏幕 App 各一次、或者清了数据重来）合并成一条：
	// 先按手机自己生成的编号认，认不出时按“同名且来自同一个 IP”认
	auto &devs = mState["devices"];
	std::string id;
	for (auto &[k, d] : devs.items())
		if (!isRemoved(d) && !pid.empty() && d.value("phoneId", "") == pid)
			id = k;
	if (id.empty())
		for (auto &[k, d] : devs.items())
			if (!isRemoved(d) && d.value("name", "") == displayName && !ip.empty() &&
			    seenOf(mLocal, k).value("lastIp", "") == ip)
				id = k;
	const bool same = !id.empty();
	if (!same)
		id = randomHex(6);

	const int64_t now = nowSeconds();
	json &d = devs[id];
	if (!same)
		d = {{"created", now}};
	auto hashes = d.value("tokenHashes", json::array());
	hashes.push_back(sha256Hex(token));
	while (hashes.size() > kMaxTokensPerDevice)
		hashes.erase(hashes.begin());
	d["tokenHashes"] = hashes;
	d["name"] = displayName;
	if (!pid.empty())
		d["phoneId"] = pid;
	d["removed"] = false;
	stamp(d);
	mLocal[id] = {{"lastSeen", now}, {"lastIp", ip}};
	mLastPaired = displayName;
	save();
	log("{}: {}（{}，{}）", same ? "同一部手机再次授权" : "新手机已授权", displayName, id, ip);
	return true;
}

bool Group::verify(const std::string &token, const std::string &ip, std::string &name,
                   std::string *deviceId) {
	if (token.empty())
		return false;
	const json hash = sha256Hex(token);
	std::lock_guard lock(mMutex);
	for (auto &[k, d] : mState["devices"].items()) {
		if (isRemoved(d))
			continue;
		const auto hashes = d.value("tokenHashes", json::array());
		if (std::find(hashes.begin(), hashes.end(), hash) == hashes.end())
			continue;
		name = d.value("name", "");
		if (deviceId)
			*deviceId = k;
		const auto seen = seenOf(mLocal, k);
		const int64_t now = nowSeconds();
		if (now - seen.value("lastSeen", int64_t(0)) > 60 || seen.value("lastIp", "") != ip) {
			mLocal[k] = {{"lastSeen", now}, {"lastIp", ip}};
			save();
		}
		return true;
	}
	return false;
}

json Group::phones() {
	std::lock_guard lock(mMutex);
	json list = json::array();
	for (auto &[k, d] : mState["devices"].items()) {
		if (isRemoved(d))
			continue;
		const auto seen = seenOf(mLocal, k);
		list.push_back({{"id", k},
		                {"name", d.value("name", "")},
		                {"ip", seen.value("lastIp", "")},
		                {"browsers", d.value("tokenHashes", json::array()).size()},
		                {"created", d.value("created", int64_t(0))},
		                {"lastSeen", seen.value("lastSeen", int64_t(0))}});
	}
	std::sort(list.begin(), list.end(), [](auto &a, auto &b) { return a["created"] < b["created"]; });
	return list;
}

bool Group::removePhone(const std::string &id) {
	std::lock_guard lock(mMutex);
	auto &devs = mState["devices"];
	if (!devs.contains(id) || isRemoved(devs[id]))
		return false;
	log("已移除手机授权: {}（{}）", devs[id].value("name", ""), id);
	json rec = {{"removed", true}, {"t", timeOf(devs[id])}};
	stamp(rec);
	devs[id] = rec;
	mLocal.erase(id);
	save();
	return true;
}

// ---------------- 电脑 ----------------

json Group::members(const std::vector<Discovery::Peer> &online) {
	std::lock_guard lock(mMutex);
	json list = json::array();
	for (auto &[k, m] : mState["members"].items()) {
		if (isRemoved(m))
			continue;
		json pc = {{"id", k},
		           {"name", m.value("name", "")},
		           {"https", m.value("https", 0)},
		           {"http", m.value("http", 0)},
		           {"ips", m.value("ips", json::array())},
		           {"self", k == mSelf.id},
		           {"online", k == mSelf.id}};
		for (auto &p : online)
			if (p.id == k && p.group == mState["id"].get<std::string>()) {
				// 现在看到的地址最准：广播的来源地址在前
				pc.update({{"name", p.name}, {"https", p.httpsPort}, {"http", p.httpPort}, {"ips", p.ips},
				           {"online", true}});
			}
		list.push_back(pc);
	}
	std::sort(list.begin(), list.end(), [](auto &a, auto &b) {
		if (a["self"] != b["self"])
			return a["self"].template get<bool>();
		return a["name"] < b["name"];
	});
	return list;
}

json Group::strangers(const std::vector<Discovery::Peer> &online) {
	std::lock_guard lock(mMutex);
	json list = json::array();
	for (auto &p : online) {
		if (p.group == mState["id"].get<std::string>())
			continue;
		list.push_back({{"id", p.id},
		                {"name", p.name},
		                {"ip", p.ip},
		                {"https", p.httpsPort},
		                {"version", p.version},
		                {"group", p.group},
		                {"groupName", p.groupName}});
	}
	return list;
}

bool Group::removeMember(const std::string &id) {
	std::lock_guard lock(mMutex);
	auto &members = mState["members"];
	if (!members.contains(id) || isRemoved(members[id]))
		return false;
	json rec = {{"removed", true}, {"t", timeOf(members[id])}};
	stamp(rec);
	if (id != mSelf.id) {
		log("把电脑 {}（{}）移出了组", members[id].value("name", ""), id);
		members[id] = rec;
		save();
		return true;
	}
	// 退出组：先把“自己已退出”记下来，等其他电脑同步过去再变回单独的组
	bool alone = true;
	for (auto &[k, m] : members.items())
		alone = alone && (k == mSelf.id || isRemoved(m));
	if (alone) {
		reset(true);
		return true;
	}
	log("退出组 {}，{} 秒后生效", mState["name"].get<std::string>(), kLeaveDelay.count());
	members[id] = rec;
	save();
	mLeaveAt = Clock::now() + kLeaveDelay;
	return true;
}

json Group::acceptJoin(const std::string &code, const json &pc) {
	std::lock_guard lock(mMutex);
	const std::string id = pc.value("id", "");
	if (id.empty() || id == mSelf.id || !checkCode(code))
		return nullptr;
	json rec = {{"name", pc.value("name", "")},
	            {"https", pc.value("https", 0)},
	            {"http", pc.value("http", 0)},
	            {"ips", pc.value("ips", json::array())},
	            {"removed", false}};
	auto &members = mState["members"];
	rec["t"] = timeOf(members.value(id, json::object()));
	stamp(rec);
	members[id] = rec;
	save();
	log("电脑 {}（{}）加入了组", rec["name"].get<std::string>(), id);
	return {{"state", mState},
	        {"caKey", readText(mDir / "certs" / "ca.key")},
	        {"caCert", readText(mDir / "certs" / "ca.crt")}};
}

std::string Group::join(const std::string &ip, int https, const std::string &code) {
	// 这时还不认识对方的证书：靠“只有看到对方屏幕的人才知道配对码”来保证加入的是对的组
	httplib::SSLClient cli(ip, https);
	cli.enable_server_certificate_verification(false);
	cli.set_connection_timeout(3);
	cli.set_read_timeout(5);
	const json body = {{"code", code},
	                   {"pc", {{"id", mSelf.id}, {"name", mSelf.name}, {"https", mSelf.https},
	                           {"http", mSelf.http}, {"ips", mSelf.ips}}}};
	auto res = cli.Post("/api/group/join", body.dump(), "application/json");
	if (!res)
		return "连不上那台电脑，确认它开着 PocketDesk";
	if (res->status == 403)
		return "数字不对或已过期，请看那台电脑上最新显示的数字";
	if (res->status == 404)
		return "那台电脑的 PocketDesk 版本太旧，请先升级它";
	if (res->status != 200)
		return std::format("那台电脑拒绝了（{}）", res->status);
	auto b = json::parse(res->body, nullptr, false);
	const auto st = b.is_object() ? b.value("state", json::object()) : json::object();
	const std::string caKey = b.is_object() ? b.value("caKey", "") : "";
	const std::string caCert = b.is_object() ? b.value("caCert", "") : "";
	if (st.value("id", "").empty() || st.value("secret", "").empty() || caKey.empty() || caCert.empty())
		return "那台电脑回复的内容不完整";

	std::lock_guard lock(mMutex);
	if (st["id"] == mState["id"])
		return "已经在这个组里了";
	const auto certs = mDir / "certs";
	removeFiles(certs, {"server.key", "server.crt"});
	if (!writeText(certs / "ca.key", caKey) || !writeText(certs / "ca.crt", caCert))
		return "保存组证书失败";
	mState = st;
	for (auto k : {"members", "devices"})
		if (!mState[k].is_object())
			mState[k] = json::object();
	mLocal = json::object();
	updateSelf();
	save();
	mNeedRestart = true;
	log("已加入组 {}（{}）", mState.value("name", ""), mState["id"].get<std::string>());
	return "";
}

// ---------------- 同步 ----------------

bool Group::checkSecret(const std::string &secret) {
	std::lock_guard lock(mMutex);
	const std::string mine = mState["secret"];
	return secret.size() == mine.size() && CRYPTO_memcmp(secret.data(), mine.data(), mine.size()) == 0;
}

json Group::stateFor(const std::string &pcId) {
	std::lock_guard lock(mMutex);
	auto &members = mState["members"];
	// 已经被移出的电脑只告诉它这一条，它收到后自己变回单独的组
	if (members.contains(pcId) && isRemoved(members[pcId]))
		return {{"id", mState["id"]}, {"members", {{pcId, members[pcId]}}}, {"devices", json::object()}};
	json st = mState;
	st.erase("secret");
	return st;
}

json Group::pull(const Discovery::Peer &peer, const std::string &secret, const std::string &caPem) {
	// 只信任组根证书签发的服务器证书，确认对方是组员才把组密钥发过去
	httplib::SSLClient cli(peer.ip, peer.httpsPort);
	cli.load_ca_cert_store(caPem.data(), caPem.size());
	cli.enable_server_certificate_verification(true);
	cli.enable_server_hostname_verification(false);
	cli.set_connection_timeout(2);
	cli.set_read_timeout(3);
	auto res = cli.Get("/api/group/state",
	                   httplib::Headers{{"Authorization", "Bearer " + secret}, {"X-PD-PC", mSelf.id}});
	std::string err;
	json st;
	if (!res)
		err = httplib::to_string(res.error());
	else if (res->status != 200)
		err = std::format("HTTP {}", res->status);
	else if (st = json::parse(res->body, nullptr, false); !st.is_object())
		err = "内容无效";
	auto &last = mPullError[peer.id];
	if (err != last) {
		if (err.empty())
			log("组同步：可以从 {}（{}）同步了", peer.name, peer.ip);
		else
			log("组同步：从 {}（{}）拉取失败：{}", peer.name, peer.ip, err);
		last = err;
	}
	return err.empty() ? st : json();
}

void Group::merge(const json &st, std::vector<std::string> &removed) {
	if (st.value("id", "") != mState["id"].get<std::string>())
		return;
	size_t changed = 0;
	for (auto kind : {"members", "devices"}) {
		auto &mine = mState[kind];
		const auto theirs = st.value(kind, json::object());
		for (auto &[k, rec] : theirs.items()) {
			if (!rec.is_object() || (mine.contains(k) && !newer(rec, mine[k])))
				continue;
			if (std::string(kind) == "devices" && isRemoved(rec) && mine.contains(k) &&
			    !isRemoved(mine[k]))
				removed.push_back(k);
			mine[k] = rec;
			++changed;
		}
	}
	if (!changed)
		return;
	for (auto &id : removed)
		mLocal.erase(id);
	save();
	log("组同步：更新了 {} 条记录", changed);
	if (removedIn(mState["members"], mSelf.id)) {
		log("这台电脑已被移出组");
		reset(true);
	}
}

std::vector<std::string> Group::sync(const std::vector<Discovery::Peer> &online, bool force) {
	std::lock_guard syncLock(mSyncMutex);
	std::vector<std::string> removed;
	std::string myId, myRev, secret, caPem;
	{
		std::lock_guard lock(mMutex);
		const auto now = Clock::now();
		if (mLeaveAt != Clock::time_point{} && now >= mLeaveAt) {
			reset(true);
			return removed;
		}
		if (mNeedRestart)
			return removed;
		// 不认识的令牌会触发强制同步；限制频率，乱发令牌也不会让电脑一直去拉
		if (force && now - mLastForced < kPullEvery)
			return removed;
		if (force)
			mLastForced = now;
		myId = mState["id"];
		myRev = mRev;
		secret = mState["secret"];
	}
	for (auto &p : online) {
		if (p.group != myId || (!force && p.rev == myRev))
			continue;
		auto &last = mLastPull[p.id];
		if (!force && Clock::now() - last < kPullEvery)
			continue;
		last = Clock::now();
		if (caPem.empty())
			caPem = readText(mDir / "certs" / "ca.crt");
		auto st = pull(p, secret, caPem);
		if (st.is_null())
			continue;
		std::lock_guard lock(mMutex);
		try {
			merge(st, removed);
		} catch (const std::exception &e) {
			log("组同步：合并 {} 的记录失败：{}", p.name, e.what());
		}
		if (mNeedRestart)
			break;
	}
	return removed;
}

} // namespace pd
