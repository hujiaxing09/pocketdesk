// PocketDesk 阶段 0：截屏 → FFmpeg 硬件 H.264 → WebRTC → 手机 Safari
#include "capture.h"
#include "certs.h"
#include "encoder.h"
#include "log.h"
#include "netinfo.h"
#include "streamer.h"
#include "web_server.h"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

using namespace pd;

namespace {

std::atomic<bool> gRunning{true};

BOOL WINAPI onConsoleCtrl(DWORD) {
	gRunning = false;
	return TRUE;
}

struct Args {
	int output = -1;
	std::string encoder;
	int fps = 60;
	int bitrateKbps = 12000;
	int httpsPort = 47443;
	int httpPort = 47080;
	int playoutDelayMaxMs = 0;
	std::filesystem::path webDir;
	bool listOnly = false;
};

void printUsage() {
	log("用法: pocketdesk [--list] [--output N] [--encoder h264_nvenc|h264_amf|h264_qsv]");
	log("                 [--fps 60] [--bitrate 12000] [--https-port 47443] [--http-port 47080]");
	log("                 [--playout-delay-max 0] [--web DIR]");
}

bool parseArgs(int argc, wchar_t **argv, Args &a) {
	auto narrow = [](const wchar_t *w) {
		std::string s;
		for (; *w; ++w)
			s += char(*w);
		return s;
	};
	for (int i = 1; i < argc; ++i) {
		std::string k = narrow(argv[i]);
		auto next = [&]() -> std::string { return i + 1 < argc ? narrow(argv[++i]) : ""; };
		if (k == "--list")
			a.listOnly = true;
		else if (k == "--output")
			a.output = std::stoi(next());
		else if (k == "--encoder")
			a.encoder = next();
		else if (k == "--fps")
			a.fps = std::stoi(next());
		else if (k == "--bitrate")
			a.bitrateKbps = std::stoi(next());
		else if (k == "--https-port")
			a.httpsPort = std::stoi(next());
		else if (k == "--http-port")
			a.httpPort = std::stoi(next());
		else if (k == "--playout-delay-max")
			a.playoutDelayMaxMs = std::stoi(next());
		else if (k == "--web")
			a.webDir = argv[++i];
		else {
			printUsage();
			return false;
		}
	}
	return true;
}

std::filesystem::path exeDir() {
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	return std::filesystem::path(buf).parent_path();
}

std::filesystem::path dataDir() {
	const wchar_t *local = _wgetenv(L"LOCALAPPDATA");
	return std::filesystem::path(local ? local : L".") / L"PocketDesk";
}

int64_t qpcNow() {
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

double processCpuSeconds() {
	FILETIME c, e, k, u;
	GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
	auto toSec = [](FILETIME f) {
		return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7;
	};
	return toSec(k) + toSec(u);
}

// 截屏和编码都在这个线程里，D3D11 上下文只有它在用
void captureLoop(const Args &args, Streamer &streamer) {
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	const double qpcFreq = double(f.QuadPart);
	const int64_t frameInterval = int64_t(qpcFreq / args.fps);

	DesktopCapture capture;
	VideoEncoder encoder;
	Nv12Converter converter;
	ID3D11Device *encoderDevice = nullptr;
	int lostCount = 0;

	bool dirty = false;
	int64_t framePresentQpc = 0;
	int64_t lastEncodeQpc = 0;

	// 每秒统计
	int64_t statsStart = qpcNow();
	double cpuStart = processCpuSeconds();
	int capturedFrames = 0, encodedFrames = 0, keyframes = 0;
	size_t encodedBytes = 0;
	double encodeMsSum = 0, lagMsSum = 0;
	auto lastSweep = std::chrono::steady_clock::now();

	while (gRunning) {
		auto steadyNow = std::chrono::steady_clock::now();
		if (steadyNow - lastSweep > std::chrono::seconds(1)) {
			streamer.sweep();
			lastSweep = steadyNow;
		}

		if (!streamer.hasViewers()) {
			if (encoder.isOpen()) {
				log("没有手机在看，关闭编码器");
				encoder.close();
				converter.reset();
			}
			Sleep(50);
			continue;
		}

		// ---- 截屏设备 ----
		if (!capture.device()) {
			if (!capture.open(args.output)) {
				Sleep(1000);
				continue;
			}
			lostCount = 0;
		}
		if (!capture.hasDuplication()) {
			// 安全桌面（UAC、锁屏）期间会失败；失败太多次就整个重建（显卡或显示器可能变了）
			if (!capture.reopenDuplication()) {
				if (++lostCount > 25) {
					capture.close();
					encoder.close();
					converter.reset();
				}
				Sleep(200);
				continue;
			}
			lostCount = 0;
		}

		// ---- 编码器（分辨率或设备变了就重建）----
		if (encoder.isOpen() &&
		    (encoderDevice != capture.device() || encoder.config().width != (capture.width & ~1) ||
		     encoder.config().height != (capture.height & ~1))) {
			encoder.close();
			converter.reset();
		}
		if (!encoder.isOpen()) {
			EncoderConfig cfg;
			cfg.name = args.encoder.empty() ? encoderForVendor(capture.vendorId) : args.encoder;
			cfg.width = capture.width;
			cfg.height = capture.height;
			cfg.fps = args.fps;
			cfg.bitrateKbps = args.bitrateKbps;
			if (cfg.name.empty()) {
				log("不认识的显卡厂商 0x{:04X}，请用 --encoder 指定编码器", capture.vendorId);
				Sleep(2000);
				continue;
			}
			if (!encoder.open(capture.device(), cfg) ||
			    !converter.init(capture.device(), capture.context(), capture.width, capture.height,
			                    encoder.config().width, encoder.config().height)) {
				encoder.close();
				Sleep(2000);
				continue;
			}
			encoderDevice = capture.device();
			streamer.takeKeyframeRequest();
			dirty = capture.frame() != nullptr;
			lastEncodeQpc = 0;
		}

		// ---- 取画面：最多等到允许编码下一帧的时刻 ----
		const int64_t now0 = qpcNow();
		const int64_t allowedAt = lastEncodeQpc + frameInterval;
		UINT timeoutMs = 0;
		if (now0 < allowedAt)
			timeoutMs = UINT((allowedAt - now0) * 1000 / int64_t(qpcFreq));
		else if (!dirty)
			timeoutMs = 30; // 画面没变时等下一次变化（也顺便及时响应关键帧请求）
		int64_t presentQpc = 0;
		auto r = capture.acquire(timeoutMs, presentQpc);
		if (r == DesktopCapture::Result::Lost)
			continue;
		if (r == DesktopCapture::Result::Frame) {
			framePresentQpc = presentQpc; // 编码的是最新一帧，延迟从它的呈现时间算起
			dirty = true;
			++capturedFrames;
		}

		const int64_t now = qpcNow();
		bool forceKey = streamer.takeKeyframeRequest();
		if (forceKey && capture.frame())
			dirty = true;
		if (dirty && now >= lastEncodeQpc + frameInterval) {
			ID3D11Texture2D *tex = nullptr;
			UINT slice = 0;
			if (encoder.beginFrame(&tex, &slice) && converter.convert(capture.frame(), tex, slice)) {
				int64_t pts = int64_t(double(now) * 90000.0 / qpcFreq);
				encoder.endFrame(pts, forceKey, [&](const uint8_t *data, size_t size, bool key) {
					streamer.sendVideo(data, size, key);
					encodedBytes += size;
					keyframes += key ? 1 : 0;
				});
				const int64_t done = qpcNow();
				++encodedFrames;
				encodeMsSum += double(done - now) * 1000.0 / qpcFreq;
				if (framePresentQpc > 0)
					lagMsSum += double(done - framePresentQpc) * 1000.0 / qpcFreq;
			} else if (forceKey) {
				streamer.takeKeyframeRequest();
			}
			dirty = false;
			framePresentQpc = 0;
			lastEncodeQpc = now;
		}

		// ---- 每秒把统计发给手机 ----
		const int64_t t = qpcNow();
		if (double(t - statsStart) / qpcFreq >= 1.0) {
			const double secs = double(t - statsStart) / qpcFreq;
			const double cpu = processCpuSeconds();
			unsigned cores = std::max(1u, std::thread::hardware_concurrency());
			nlohmann::json j = {
			    {"type", "stats"},
			    {"encoder", encoder.config().name},
			    {"adapter", capture.adapterName},
			    {"width", encoder.config().width},
			    {"height", encoder.config().height},
			    {"captureFps", capturedFrames / secs},
			    {"encodeFps", encodedFrames / secs},
			    {"kbps", encodedBytes * 8 / 1000.0 / secs},
			    {"keyframes", keyframes},
			    {"pli", streamer.takePliCount()},
			    {"encodeMs", encodedFrames ? encodeMsSum / encodedFrames : 0.0},
			    {"captureToSendMs", encodedFrames ? lagMsSum / encodedFrames : 0.0},
			    {"cpuPercent", (cpu - cpuStart) / secs / cores * 100.0},
			};
			streamer.sendControl(j.dump());
			statsStart = t;
			cpuStart = cpu;
			capturedFrames = encodedFrames = keyframes = 0;
			encodedBytes = 0;
			encodeMsSum = lagMsSum = 0;
		}
	}
	encoder.close();
	converter.reset();
	capture.close();
}

} // namespace

int wmain(int argc, wchar_t **argv) {
	SetConsoleOutputCP(CP_UTF8);
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
	timeBeginPeriod(1);

	Args args;
	if (!parseArgs(argc, argv, args))
		return 2;

	log("PocketDesk（阶段 0）");
	for (auto &o : listOutputs())
		log("  显示器 {}{}: {} [{} {}x{}] 显卡 {} (0x{:04X})", o.index, o.primary ? "（主）" : "",
		    o.deviceName, o.rect.left, o.rect.right - o.rect.left, o.rect.bottom - o.rect.top,
		    o.adapterName, o.vendorId);
	if (args.listOnly)
		return 0;

	if (args.webDir.empty())
		args.webDir = exeDir() / "web";
	const auto host = hostName();
	const auto ips = localLanIPv4();

	CertFiles certs;
	if (!ensureCertificates(dataDir() / "certs", host, ips, certs)) {
		log("证书准备失败");
		return 1;
	}

	Streamer::Options so;
	so.playoutDelayMaxMs = args.playoutDelayMaxMs;
	Streamer streamer(so);

	WebServer web;
	WebServer::Options wo;
	wo.webDir = args.webDir;
	wo.httpsPort = args.httpsPort;
	wo.httpPort = args.httpPort;
	if (!web.start(wo, certs, streamer))
		return 1;

	log("证书目录: {}", (dataDir() / "certs").string());
	for (auto &ip : ips) {
		log("首次设置（安装证书）: http://{}:{}/", ip, args.httpPort);
		log("打开画面:             https://{}:{}/", ip, args.httpsPort);
	}
	log("按 Ctrl+C 退出");

	captureLoop(args, streamer);

	log("正在退出");
	web.stop();
	timeEndPeriod(1);
	return 0;
}
