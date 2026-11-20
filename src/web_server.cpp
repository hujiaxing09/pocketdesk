#include "web_server.h"
#include "devices.h"
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
	auto noStore = [](const httplib::Request &, httplib::Response &res) {
		res.set_header("Cache-Control", "no-store");
	};

	// 手机建立连接：要带配对时拿到的令牌；本机调试不需要
	auto createSession = [&streamer, &devices](const httplib::Request &req, httplib::Response &res) {
		std::string name;
		if (isLoopback(req))
			name = "本机调试";
		else if (!devices.verify(bearer(req), name)) {
			jsonReply(res, {{"error", "unauthorized"}}, 401);
			return;
		}
		std::string id, sdp;
		if (!streamer.createSession(name, id, sdp)) {
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
		if (body.is_discarded() ||
		    !devices.pair(body.value("code", ""), body.value("name", ""), token)) {
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
	http.Get("/api/local/pair", [&devices, ips, httpPort](const httplib::Request &,
	                                                        httplib::Response &res) {
		int left = 0;
		const std::string code = devices.pairCode(left);
		json urls = json::array();
		for (auto &ip : ips) {
			std::string url = std::format("http://{}:{}/?pair={}", ip, httpPort, code);
			urls.push_back({{"ip", ip}, {"url", url}, {"svg", qrSvg(url)}});
		}
		jsonReply(res, {{"secondsLeft", left},
		                {"lastPaired", devices.lastPaired()},
		                {"urls", urls},
		                {"devices", devices.list()}});
	});
	http.Post("/api/local/devices/remove", [&devices](const httplib::Request &req,
	                                                  httplib::Response &res) {
		auto body = json::parse(req.body, nullptr, false);
		res.status = !body.is_discarded() && devices.remove(body.value("id", "")) ? 204 : 404;
	});
	// 本机调试入口：浏览器把 http://127.0.0.1 当作安全页面，不装证书也能测整条链路
	http.set_mount_point("/app", options.webDir.string());
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
