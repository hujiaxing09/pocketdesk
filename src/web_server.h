#pragma once
#include "certs.h"

#include <filesystem>
#include <memory>
#include <thread>

namespace httplib {
class Server;
class SSLServer;
} // namespace httplib

namespace pd {

class Streamer;

// HTTPS：页面和信令（必须 HTTPS，Safari 才允许 WebRTC）
// HTTP：只用于首次设置，下载根证书并说明怎么信任它
class WebServer {
public:
	struct Options {
		std::filesystem::path webDir;
		int httpsPort = 47443;
		int httpPort = 47080;
	};

	WebServer();
	~WebServer();
	bool start(const Options &options, const CertFiles &certs, Streamer &streamer);
	void stop();

private:
	std::unique_ptr<httplib::SSLServer> mHttps;
	std::unique_ptr<httplib::Server> mHttp;
	std::thread mHttpsThread, mHttpThread;
};

} // namespace pd
