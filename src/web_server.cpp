#include "web_server.h"
#include "log.h"
#include "streamer.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace pd {

using json = nlohmann::json;

static std::string readFile(const std::filesystem::path &p) {
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static void replaceAll(std::string &s, const std::string &from, const std::string &to) {
	for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size())
		s.replace(pos, from.size(), to);
}

WebServer::WebServer() = default;
WebServer::~WebServer() { stop(); }

bool WebServer::start(const Options &options, const CertFiles &certs, Streamer &streamer) {
	const std::string caDer = certs.caDer;
	auto serveCa = [caDer](const httplib::Request &, httplib::Response &res) {
		// Safari 识别这个类型后会提示下载描述文件
		res.set_header("Content-Disposition", "attachment; filename=\"PocketDesk-CA.crt\"");
		res.set_content(caDer, "application/x-x509-ca-cert");
	};
	auto noStore = [](const httplib::Request &, httplib::Response &res) {
		res.set_header("Cache-Control", "no-store");
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
	auto createSession = [&streamer](const httplib::Request &req, httplib::Response &res) {
		std::string id, sdp;
		if (!streamer.createSession(id, sdp)) {
			res.status = 500;
			res.set_content(R"({"error":"创建连接失败"})", "application/json");
			return;
		}
		log("[{}] 手机 {} 请求连接", id, req.remote_addr);
		res.set_content(json{{"id", id}, {"sdp", sdp}}.dump(), "application/json");
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
	https.Post("/api/session", createSession);
	https.Post("/api/answer", acceptAnswer);

	// ---- HTTP（首次设置）----
	mHttp = std::make_unique<httplib::Server>();
	auto &http = *mHttp;
	http.set_post_routing_handler(noStore);
	const auto setupPath = options.webDir / "setup.html";
	const std::string httpsPort = std::to_string(options.httpsPort);
	const std::string caName = certs.caName;
	http.Get("/", [setupPath, httpsPort, caName](const httplib::Request &, httplib::Response &res) {
		std::string html = readFile(setupPath);
		replaceAll(html, "{{HTTPS_PORT}}", httpsPort);
		replaceAll(html, "{{CA_NAME}}", caName);
		res.set_content(html, "text/html; charset=utf-8");
	});
	http.Get("/ca.crt", serveCa);
	// 本机调试入口：浏览器把 http://127.0.0.1 当作安全页面，不装证书也能测整条链路。
	// 只允许本机访问，局域网里的设备只能拿到设置页和证书。
	http.set_mount_point("/app", options.webDir.string());
	http.Post("/api/session", createSession);
	http.Post("/api/answer", acceptAnswer);
	http.set_pre_routing_handler([](const httplib::Request &req, httplib::Response &res) {
		if (req.path == "/" || req.path == "/ca.crt")
			return httplib::Server::HandlerResponse::Unhandled;
		if (req.remote_addr == "127.0.0.1" || req.remote_addr == "::1")
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
