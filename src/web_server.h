#pragma once
#include "certs.h"

#include <filesystem>
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
class DeviceStore;
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
		std::string pcName;
		std::string version;
		Discovery *discovery = nullptr;
	};

	WebServer();
	~WebServer();
	bool start(const Options &options, const CertFiles &certs, Streamer &streamer,
	           DeviceStore &devices);
	void stop();

private:
	std::unique_ptr<httplib::SSLServer> mHttps;
	std::unique_ptr<httplib::Server> mHttp;
	std::thread mHttpsThread, mHttpThread;
};

} // namespace pd
