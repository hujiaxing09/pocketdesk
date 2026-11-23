#pragma once
#include "certs.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace httplib {
class Server;
class SSLServer;
} // namespace httplib

namespace pd {

class Streamer;
class Group;
class Discovery;

// HTTPS：页面、配对和信令（Safari 只在 HTTPS 页面里允许 WebRTC）
// HTTP：首次设置（下载根证书）；另有只允许本机访问的配对页和调试入口
class WebServer {
public:
	struct Options {
		std::filesystem::path webDir;
		int httpsPort = 47443;
		int httpPort = 47080;
		std::vector<std::string> lanIps;
		std::string pcId;
		std::string pcName;
		std::string version;
		Discovery *discovery = nullptr;
		// 马上和同组电脑同步一次（手机带着还没同步过来的令牌时用）
		std::function<void()> syncNow;
	};

	WebServer();
	~WebServer();
	bool start(const Options &options, const CertFiles &certs, Streamer &streamer, Group &group);
	void stop();

private:
	std::unique_ptr<httplib::SSLServer> mHttps;
	std::unique_ptr<httplib::Server> mHttp;
	std::thread mHttpsThread, mHttpThread;
};

} // namespace pd
