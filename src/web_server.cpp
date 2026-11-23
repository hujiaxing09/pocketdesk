#include "web_server.h"
#include "devices.h"
#include "discovery.h"
#include "log.h"
#include "streamer.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <qrcodegen.hpp>

#include <fstream>
#include <sstream>

namespace pd {

using json = nlohmann::json;

namespace {

std::string readFile(const std::filesystem::path &p) {
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

void replaceAll(std::string &s, const std::string &from, const std::string &to) {
	for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size())
		s.replace(pos, from.size(), to);
}

// 允许跨域的来源：局域网里其他 PocketDesk 页面（私有地址或 .local 名称），以及本机调试页。
// 接口本身靠令牌鉴权，这里只是不让互联网上的网页随便调用。
bool allowedOrigin(const std::string &origin) {
	std::string rest;
	if (origin.rfind("https://", 0) == 0)
		rest = origin.substr(8);
	else if (origin.rfind("http://127.0.0.1", 0) == 0 || origin.rfind("http://localhost", 0) == 0)
		return true;
	else
		return false;
	const std::string host = rest.substr(0, rest.find(':'));
	if (host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0)
		return true;
	unsigned a = 0, b = 0, c = 0, d = 0;
	if (sscanf_s(host.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
		return false;
	return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

void addCors(const httplib::Request &req, httplib::Response &res) {
	const auto origin = req.get_header_value("Origin");
	if (origin.empty() || !allowedOrigin(origin))
		return;
	res.set_header("Access-Control-Allow-Origin", origin);
	res.set_header("Vary", "Origin");
}

bool isLoopback(const httplib::Request &req) {
	return req.remote_addr == "127.0.0.1" || req.remote_addr == "::1";
}

std::string bearer(const httplib::Request &req) {
	auto h = req.get_header_value("Authorization");
	const std::string prefix = "Bearer ";
	return h.rfind(prefix, 0) == 0 ? h.substr(prefix.size()) : std::string();
}

std::string qrSvg(const std::string &text) {
	using qrcodegen::QrCode;
	const QrCode qr = QrCode::encodeText(text.c_str(), QrCode::Ecc::MEDIUM);
	const int size = qr.getSize(), border = 4;
	std::string path;
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x)
			if (qr.getModule(x, y))
				path += std::format("M{},{}h1v1h-1z", x + border, y + border);
	return std::format(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {0} {0}" )"
	                   R"(shape-rendering="crispEdges"><rect width="100%" height="100%" fill="#fff"/>)"
	                   R"(<path d="{1}" fill="#000"/></svg>)",
	                   size + border * 2, path);
}

void jsonReply(httplib::Response &res, const json &j, int status = 200) {
	res.status = status;
	res.set_content(j.dump(), "application/json");
}

} // namespace

WebServer::WebServer() = default;
WebServer::~WebServer() { stop(); }

bool WebServer::start(const Options &options, const CertFiles &certs, Streamer &streamer,
                      DeviceStore &devices) {
	const std::string caDer = certs.caDer;
	auto serveCa = [caDer](const httplib::Request &, httplib::Response &res) {
		// Safari 识别这个类型后会提示下载描述文件
		res.set_header("Content-Disposition", "attachment; filename=\"PocketDesk-CA.crt\"");
		res.set_content(caDer, "application/x-x509-ca-cert");
	};
	auto noStore = [](const httplib::Request &req, httplib::Response &res) {
		res.set_header("Cache-Control", "no-store");
		if (req.path.rfind("/api/", 0) == 0)
			addCors(req, res);
	};
	auto preflight = [](const httplib::Request &, httplib::Response &res) {
		res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
		res.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type");
		res.set_header("Access-Control-Max-Age", "600");
		res.status = 204;
	};
	// 这台电脑是谁（不需要授权）：手机用它确认连上的是哪台电脑、证书是否已经信任
	const json info = {{"id", certs.pcId},
	                   {"name", options.pcName},
	                   {"version", options.version},
	                   {"https", options.httpsPort},
	                   {"http", options.httpPort},
	                   {"ips", options.lanIps}};
	auto getInfo = [info](const httplib::Request &, httplib::Response &res) { jsonReply(res, info); };
	// 局域网里看得到的其他电脑
	Discovery *discovery = options.discovery;
	auto getPeers = [info, discovery](const httplib::Request &, httplib::Response &res) {
		json peers = json::array();
		if (discovery)
			for (auto &p : discovery->peers())
				peers.push_back({{"id", p.id},
				                 {"name", p.name},
				                 {"ip", p.ip},
				                 {"ips", p.ips},
				                 {"https", p.httpsPort},
				                 {"http", p.httpPort},
				                 {"version", p.version}});
		jsonReply(res, {{"self", info}, {"peers", peers}});
	};

	// 手机建立连接：要带配对时拿到的令牌；本机调试不需要
	auto createSession = [&streamer, &devices](const httplib::Request &req, httplib::Response &res) {
		std::string name, deviceId;
		if (isLoopback(req))
			name = "本机调试";
		else if (!devices.verify(bearer(req), req.remote_addr, name, &deviceId)) {
			jsonReply(res, {{"error", "unauthorized"}}, 401);
			return;
		}
		std::string id, sdp;
		if (!streamer.createSession(name, deviceId, id, sdp)) {
			jsonReply(res, {{"error", "创建连接失败"}}, 500);
			return;
		}
		log("[{}] {} 从 {} 请求连接", id, name, req.remote_addr);
		jsonReply(res, {{"id", id}, {"sdp", sdp}});
	};
	auto acceptAnswer = [&streamer](const httplib::Request &req, httplib::Response &res) {
		auto body = json::parse(req.body, nullptr, false);
		if (body.is_discarded() || !body.contains("id") || !body.contains("sdp")) {
			res.status = 400;
			return;
		}
		bool ok = streamer.acceptAnswer(body["id"].get<std::string>(), body["sdp"].get<std::string>());
		res.status = ok ? 204 : 404;
	};
	auto pair = [&devices](const httplib::Request &req, httplib::Response &res) {
		auto body = json::parse(req.body, nullptr, false);
		std::string token;
		if (body.is_discarded() || !devices.pair(body.value("code", ""), body.value("name", ""),
		                                         body.value("phone", ""), req.remote_addr, token)) {
			log("配对失败：{} 的配对码无效或已过期", req.remote_addr);
			jsonReply(res, {{"error", "配对码无效或已过期"}}, 403);
			return;
		}
		jsonReply(res, {{"token", token}});
	};

	// ---- HTTPS ----
	mHttps = std::make_unique<httplib::SSLServer>(certs.serverCert.string().c_str(),
	                                              certs.serverKey.string().c_str());
	if (!mHttps->is_valid()) {
		log("HTTPS 服务初始化失败（证书或私钥读取失败）");
		return false;
	}
	auto &https = *mHttps;
	https.set_post_routing_handler(noStore);
	if (!https.set_mount_point("/", options.webDir.string())) {
		log("网页目录不存在: {}", options.webDir.string());
		return false;
	}
	https.Get("/ca.crt", serveCa);
	// 设置页用它判断手机是否已经信任证书
	https.Get("/api/ping", [](const httplib::Request &, httplib::Response &res) {
		res.set_header("Access-Control-Allow-Origin", "*");
		res.status = 204;
	});
	https.Options(R"(/api/.*)", preflight);
	https.Get("/api/info", getInfo);
	https.Get("/api/peers", getPeers);
	https.Post("/api/pair", pair);
	https.Post("/api/session", createSession);
	https.Post("/api/answer", acceptAnswer);

	// ---- HTTP ----
	mHttp = std::make_unique<httplib::Server>();
	auto &http = *mHttp;
	http.set_post_routing_handler(noStore);
	const auto webDir = options.webDir;
	const std::string httpsPort = std::to_string(options.httpsPort);
	const std::string caName = certs.caName;
	http.Get("/", [webDir, httpsPort, caName](const httplib::Request &, httplib::Response &res) {
		std::string html = readFile(webDir / "setup.html");
		replaceAll(html, "{{HTTPS_PORT}}", httpsPort);
		replaceAll(html, "{{CA_NAME}}", caName);
		res.set_content(html, "text/html; charset=utf-8");
	});
	http.Get("/ca.crt", serveCa);

	// 以下只允许本机访问
	http.Get("/pair", [webDir](const httplib::Request &, httplib::Response &res) {
		res.set_content(readFile(webDir / "pair.html"), "text/html; charset=utf-8");
	});
	const auto ips = options.lanIps;
	const int httpPort = options.httpPort;
	const int httpsPortNum = options.httpsPort;
	const std::string pcId = certs.pcId, pcName = options.pcName;
	http.Get("/api/local/pair", [&devices, ips, httpPort, httpsPortNum, pcId,
	                             pcName](const httplib::Request &, httplib::Response &res) {
		int left = 0;
		std::string shortCode;
		const std::string code = devices.pairCode(left, &shortCode);
		json urls = json::array();
		for (auto &ip : ips) {
			// 相机扫：打开设置页；PocketDesk 页面里扫：读出地址、编号和配对码
			std::string url = std::format("http://{}:{}/?pair={}&id={}&https={}", ip, httpPort, code,
			                              pcId, httpsPortNum);
			urls.push_back({{"ip", ip}, {"url", url}, {"svg", qrSvg(url)}});
		}
		jsonReply(res, {{"secondsLeft", left},
		                {"shortCode", shortCode},
		                {"pcName", pcName},
		                {"lastPaired", devices.lastPaired()},
		                {"urls", urls},
		                {"devices", devices.list()}});
	});
	http.Post("/api/local/devices/remove", [&devices, &streamer](const httplib::Request &req,
	                                                             httplib::Response &res) {
		auto body = json::parse(req.body, nullptr, false);
		const std::string id = body.is_discarded() ? "" : body.value("id", "");
		const bool ok = devices.remove(id);
		if (ok)
			streamer.closeDevice(id);
		res.status = ok ? 204 : 404;
	});
	// 本机调试入口：浏览器把 http://127.0.0.1 当作安全页面，不装证书也能测整条链路（本机请求不需要授权）
	http.set_mount_point("/app", options.webDir.string());
	http.Get("/api/info", getInfo);
	http.Get("/api/peers", getPeers);
	http.Post("/api/session", createSession);
	http.Post("/api/answer", acceptAnswer);
	http.set_pre_routing_handler([](const httplib::Request &req, httplib::Response &res) {
		if (req.path == "/" || req.path == "/ca.crt" || isLoopback(req))
			return httplib::Server::HandlerResponse::Unhandled;
		res.status = 403;
		return httplib::Server::HandlerResponse::Handled;
	});

	if (!https.bind_to_port("0.0.0.0", options.httpsPort)) {
		log("HTTPS 端口 {} 被占用", options.httpsPort);
		return false;
	}
	if (!http.bind_to_port("0.0.0.0", options.httpPort)) {
		log("HTTP 端口 {} 被占用", options.httpPort);
		return false;
	}
	mHttpsThread = std::thread([this] { mHttps->listen_after_bind(); });
	mHttpThread = std::thread([this] { mHttp->listen_after_bind(); });
	return true;
}

void WebServer::stop() {
	if (mHttps)
		mHttps->stop();
	if (mHttp)
		mHttp->stop();
	if (mHttpsThread.joinable())
		mHttpsThread.join();
	if (mHttpThread.joinable())
		mHttpThread.join();
}

} // namespace pd
