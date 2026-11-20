// PocketDesk：截屏 → FFmpeg 硬件 H.264 → WebRTC → 手机 Safari；手机的触控和键盘 → Windows
#include "capture.h"
#include "certs.h"
#include "cursor.h"
#include "desktop.h"
#include "devices.h"
#include "discovery.h"
#include "encoder.h"
#include "input.h"
#include "log.h"
#include "netinfo.h"
#include "service.h"
#include "streamer.h"
#include "tray.h"
#include "web_server.h"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>
#include <winsock2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>

using namespace pd;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> gRunning{true};
constexpr const char *kVersion = "0.4.3";
std::filesystem::path gDataDir; // 证书、授权记录、日志都放这里，启动时确定

enum class Mode {
	Normal,    // 普通启动（便携使用，或者从开始菜单打开）
	Service,   // --service：系统服务，由服务管理器启动
	Agent,     // --agent：由服务在用户会话里以 SYSTEM 启动的程序
	Install,   // --install：安装包调用，注册服务和防火墙
	Uninstall, // --uninstall：卸载程序调用
};

struct Args {
	Mode mode = Mode::Normal;
	HANDLE agentQuit = nullptr;   // 服务通知程序退出的事件
	HANDLE agentParent = nullptr; // 服务进程，它没了程序也退出
	int output = -1;
	std::string encoder;
	int fps = 60;
	int bitrateKbps = 12000;
	int httpsPort = 47443;
	int httpPort = 47080;
	int playoutDelayMaxMs = 0;
	std::filesystem::path webDir;
	bool listOnly = false;
	std::filesystem::path dataDir;
	std::string name;           // 显示名，默认是计算机名
	int discoveryPort = 47100;  // 局域网发现用的 UDP 端口
};

// 码率范围（kbps）
constexpr int kAutoMinKbps = 3000;
constexpr int kAutoMaxKbps = 20000;

// 手机可以改的设置；所有手机共用一份，后改的生效
struct Settings {
	std::mutex m;
	int output = -1;      // -1 = 主显示器
	int scaleHeight = 0;  // 0 = 原始分辨率，否则按这个高度缩放
	int fps = 60;
	bool autoBitrate = true;
	int fixedKbps = 12000;
	int autoKbps = 12000; // 自动模式当前的目标
	Clock::time_point lastDecrease{}, lastChange{};
	struct Counters {
		int64_t lost = 0, received = 0;
	};
	std::map<std::string, Counters> clients;
	std::atomic<bool> stateDirty{true};

	int targetKbps() {
		std::lock_guard lock(m);
		return autoBitrate ? autoKbps : fixedKbps;
	}
};

std::wstring widen(const std::string &s) {
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
	return w;
}

std::string narrow(const wchar_t *w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 1)
		WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

bool parseArgs(Args &a) {
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (int i = 1; i < argc; ++i) {
		std::string k = narrow(argv[i]);
		auto next = [&]() -> std::string { return i + 1 < argc ? narrow(argv[++i]) : ""; };
		try {
			if (k == "--list")
				a.listOnly = true;
			else if (k == "--service")
				a.mode = Mode::Service;
			else if (k == "--install")
				a.mode = Mode::Install;
			else if (k == "--uninstall")
				a.mode = Mode::Uninstall;
			else if (k == "--agent") {
				a.mode = Mode::Agent;
				a.agentQuit = reinterpret_cast<HANDLE>(uintptr_t(std::stoull(next())));
				a.agentParent = reinterpret_cast<HANDLE>(uintptr_t(std::stoull(next())));
			}
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
			else if (k == "--web" && i + 1 < argc)
				a.webDir = argv[++i];
			else if (k == "--data" && i + 1 < argc)
				a.dataDir = argv[++i];
			else if (k == "--name")
				a.name = next();
			else if (k == "--discovery-port")
				a.discoveryPort = std::stoi(next());
			else {
				log("不认识的参数 {}", k);
				return false;
			}
		} catch (const std::exception &) {
			log("参数 {} 的值无效", k);
			return false;
		}
	}
	LocalFree(argv);
	return true;
}

std::filesystem::path exePath() {
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	return std::filesystem::path(buf);
}

std::filesystem::path exeDir() { return exePath().parent_path(); }

// 等本机的某个 TCP 端口能连上（服务刚启动，程序还在准备证书和端口）
bool waitForPort(int port, int ms) {
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	bool ok = false;
	for (int waited = 0; waited <= ms && !ok; waited += 300) {
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(u_short(port));
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		ok = connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
		closesocket(s);
		if (!ok)
			Sleep(300);
	}
	WSACleanup();
	return ok;
}

std::filesystem::path dataDir() { return gDataDir; }

// 以前版本的数据位置
std::filesystem::path legacyDataDir() {
	const wchar_t *local = _wgetenv(L"LOCALAPPDATA");
	return std::filesystem::path(local ? local : L".") / L"PocketDesk";
}

bool writable(const std::filesystem::path &dir) {
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	if (ec)
		return false;
	const auto probe = dir / L".write-test";
	HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	CloseHandle(h);
	return true;
}

// 把 from 里的内容搬到 to（跨盘时先复制再删），成功后删掉 from
bool moveTree(const std::filesystem::path &from, const std::filesystem::path &to, std::string &err) {
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::create_directories(to, ec);
	for (auto &e : fs::directory_iterator(from, ec)) {
		const auto dest = to / e.path().filename();
		fs::rename(e.path(), dest, ec);
		if (ec) {
			ec.clear();
			fs::copy(e.path(), dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
			if (ec) {
				err = ec.message();
				return false;
			}
		}
	}
	fs::remove_all(from, ec);
	return true;
}

// 数据目录：优先放在程序旁边的 data 文件夹（删掉程序文件夹就删干净了）；
// 程序文件夹不可写（例如装在 Program Files）时放到 %LOCALAPPDATA%\PocketDesk。
// 返回给日志看的说明。
std::string chooseDataDir(const Args &args) {
	namespace fs = std::filesystem;
	if (!args.dataDir.empty()) {
		gDataDir = args.dataDir;
		return "命令行指定";
	}
	const auto portable = exeDir() / L"data";
	if (!writable(portable)) {
		gDataDir = legacyDataDir();
		return "用户目录（程序文件夹不可写）";
	}
	gDataDir = portable;
	// 第一次用新版本：把以前放在 %LOCALAPPDATA% 里的证书、授权记录和日志搬过来，手机不用重新授权
	const auto old = legacyDataDir();
	std::error_code ec;
	if (fs::exists(old / L"certs", ec) && !fs::exists(portable / L"certs", ec)) {
		std::string err;
		if (moveTree(old, portable, err))
			return "程序文件夹（已把 " + old.string() + " 里的旧数据搬过来）";
		return "程序文件夹（搬旧数据失败：" + err + "）";
	}
	return "程序文件夹";
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

// ---- 手机发来的设置和统计（在 libdatachannel 的线程里调用）----

void applySettings(Settings &s, const json &j) {
	std::lock_guard lock(s.m);
	if (j.contains("output") && j["output"].is_number_integer())
		s.output = j["output"].get<int>();
	if (j.contains("scale") && j["scale"].is_number_integer())
		s.scaleHeight = std::clamp(j["scale"].get<int>(), 0, 4320);
	if (j.contains("fps") && j["fps"].is_number_integer())
		s.fps = j["fps"].get<int>() <= 30 ? 30 : 60;
	if (j.contains("bitrate")) {
		if (j["bitrate"].is_string() && j["bitrate"] == "auto") {
			s.autoBitrate = true;
		} else if (j["bitrate"].is_number_integer()) {
			s.autoBitrate = false;
			s.fixedKbps = std::clamp(j["bitrate"].get<int>(), 1000, 50000);
		}
	}
	s.stateDirty = true;
	log("手机修改设置: {}", j.dump());
}

// 自动码率：手机丢包率超过 2% 就降到 70%，连续 10 秒没丢包再慢慢加回去
void onClientStats(Settings &s, const std::string &id, const std::string &device, const json &j) {
	log("[{}] {} 统计 {}", id, device, j.dump());
	std::lock_guard lock(s.m);
	auto &c = s.clients[id];
	const int64_t lost = j.value("lost", int64_t(0)), received = j.value("received", int64_t(0));
	const int64_t dLost = std::max<int64_t>(0, lost - c.lost);
	const int64_t dRecv = std::max<int64_t>(0, received - c.received);
	const bool first = c.received == 0;
	c.lost = lost;
	c.received = received;
	if (first || !s.autoBitrate || dRecv + dLost == 0)
		return;
	const double loss = double(dLost) / double(dLost + dRecv);
	const auto now = Clock::now();
	if (loss > 0.02 && now - s.lastChange > std::chrono::seconds(2)) {
		int next = std::max(kAutoMinKbps, int(s.autoKbps * 0.7));
		if (next != s.autoKbps) {
			log("丢包率 {:.1f}%，码率 {} → {} kbps", loss * 100, s.autoKbps, next);
			s.autoKbps = next;
			s.lastDecrease = s.lastChange = now;
			s.stateDirty = true;
		}
	} else if (loss < 0.005 && now - s.lastDecrease > std::chrono::seconds(10) &&
	           now - s.lastChange > std::chrono::seconds(4) && s.autoKbps < kAutoMaxKbps) {
		int next = std::min(kAutoMaxKbps, int(s.autoKbps * 1.15));
		log("网络稳定，码率 {} → {} kbps", s.autoKbps, next);
		s.autoKbps = next;
		s.lastChange = now;
		s.stateDirty = true;
	}
}

// ---- 截屏、编码线程：D3D11 上下文只有它在用 ----

void captureLoop(const Args &args, Streamer &streamer, InputInjector &input, Settings &settings) {
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	const double qpcFreq = double(f.QuadPart);

	DesktopCapture capture;
	VideoEncoder encoder;
	Nv12Converter converter;
	CursorRenderer cursor;
	ComPtr<ID3D11Texture2D> composed; // 画面 + 指针
	ID3D11Device *encoderDevice = nullptr;
	uint64_t cursorShapeVersion = 0;
	int lostCount = 0;
	auto lastEncoderReopen = Clock::time_point{};
	bool noDisplay = false; // 一个显示器都没有（没接显示器的主机、显示器关着）

	bool dirty = false;
	int64_t framePresentQpc = 0;
	int64_t lastEncodeQpc = 0;

	int64_t statsStart = qpcNow();
	double cpuStart = processCpuSeconds();
	int capturedFrames = 0, encodedFrames = 0, keyframes = 0;
	size_t encodedBytes = 0;
	double encodeMsSum = 0, lagMsSum = 0;
	auto lastSweep = Clock::now();

	auto closeAll = [&] {
		encoder.close();
		converter.reset();
		cursor.reset();
		composed.Reset();
		capture.close();
		encoderDevice = nullptr;
	};

	while (gRunning) {
		input.tick();
		auto steadyNow = Clock::now();
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

		// ---- 读取设置 ----
		int wantOutput, wantScale, wantFps;
		{
			std::lock_guard lock(settings.m);
			wantOutput = settings.output >= 0 ? settings.output : args.output;
			wantScale = settings.scaleHeight;
			wantFps = settings.fps;
		}
		const int wantKbps = settings.targetKbps();
		const int64_t frameInterval = int64_t(qpcFreq / wantFps);

		// ---- 截屏设备（换显示器时重建）----
		// wantOutput 为 -1 表示主显示器
		if (capture.device() &&
		    (wantOutput >= 0 ? capture.outputIndex != wantOutput : !capture.isPrimary)) {
			log("切换到显示器 {}", wantOutput >= 0 ? std::to_string(wantOutput) : "（主）");
			closeAll();
		}
		if (!capture.device()) {
			if (!capture.open(wantOutput)) {
				if (listOutputs().empty()) {
					if (!noDisplay)
						log("没有可用的显示器（没接显示器或显示器关着），等显示器出现");
					// 新手机连上时（stateDirty）也要告诉它
					if (!noDisplay || settings.stateDirty.exchange(false))
						streamer.sendControl(json{{"type", "notice"}, {"reason", "no-display"}}.dump());
					noDisplay = true;
				}
				// 指定的显示器不存在了，退回主显示器
				std::lock_guard lock(settings.m);
				settings.output = -1;
				Sleep(1000);
				continue;
			}
			if (noDisplay)
				log("显示器出现了");
			noDisplay = false;
			input.setTarget(capture.desktopRect);
			cursor.init(capture.device(), capture.context());
			cursorShapeVersion = 0;
			lostCount = 0;
			settings.stateDirty = true;
		}
		if (!capture.hasDuplication()) {
			// 安全桌面（UAC、锁屏）期间会失败；失败太多次就整个重建（显卡或显示器可能变了）
			if (!capture.reopenDuplication()) {
				if (++lostCount > 25)
					closeAll();
				Sleep(200);
				continue;
			}
			lostCount = 0;
			settings.stateDirty = true;
		}

		// ---- 编码器（分辨率、帧率、设备变了就重建；码率尽量不重建）----
		int encW = capture.width & ~1, encH = capture.height & ~1;
		if (wantScale > 0 && wantScale < capture.height) {
			encH = wantScale & ~1;
			encW = int(double(capture.width) * encH / capture.height) & ~1;
		}
		if (encoder.isOpen()) {
			const auto &c = encoder.config();
			if (encoderDevice != capture.device() || c.width != encW || c.height != encH ||
			    c.fps != wantFps) {
				encoder.close();
				converter.reset();
			} else if (c.bitrateKbps != wantKbps) {
				if (encoder.setBitrate(wantKbps)) {
					log("码率改为 {} kbps", wantKbps);
				} else if (Clock::now() - lastEncoderReopen > std::chrono::seconds(2)) {
					encoder.close(); // 这个编码器不能在线改码率，重开（会发一个关键帧）
					converter.reset();
				}
			}
		}
		if (!encoder.isOpen()) {
			EncoderConfig cfg;
			cfg.name = args.encoder.empty() ? encoderForVendor(capture.vendorId) : args.encoder;
			cfg.width = encW;
			cfg.height = encH;
			cfg.fps = wantFps;
			cfg.bitrateKbps = wantKbps;
			if (cfg.name.empty()) {
				log("不认识的显卡厂商 0x{:04X}，请用 --encoder 指定编码器", capture.vendorId);
				Sleep(2000);
				continue;
			}
			if (!encoder.open(capture.device(), cfg) ||
			    !converter.init(capture.device(), capture.context(), capture.width, capture.height,
			                    encW, encH)) {
				encoder.close();
				Sleep(2000);
				continue;
			}
			lastEncoderReopen = Clock::now();
			encoderDevice = capture.device();
			streamer.takeKeyframeRequest();
			dirty = capture.frame() != nullptr;
			lastEncodeQpc = 0;
			settings.stateDirty = true;
		}

		// ---- 把当前状态告诉手机（显示器列表、分辨率、码率）----
		if (settings.stateDirty.exchange(false)) {
			json outputs = json::array();
			for (auto &o : listOutputs())
				outputs.push_back({{"index", o.index},
				                   {"name", o.deviceName},
				                   {"width", o.rect.right - o.rect.left},
				                   {"height", o.rect.bottom - o.rect.top},
				                   {"primary", o.primary}});
			std::lock_guard lock(settings.m);
			streamer.sendControl(json{{"type", "state"},
			                          {"outputs", outputs},
			                          {"output", capture.outputIndex},
			                          {"width", encW},
			                          {"height", encH},
			                          {"scale", settings.scaleHeight},
			                          {"fps", settings.fps},
			                          {"autoBitrate", settings.autoBitrate},
			                          {"bitrate", settings.autoBitrate ? settings.autoKbps
			                                                           : settings.fixedKbps}}
			                         .dump());
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
		bool pointerChanged = false;
		auto r = capture.acquire(timeoutMs, presentQpc, pointerChanged);
		if (r == DesktopCapture::Result::Lost)
			continue;
		if (r == DesktopCapture::Result::Frame) {
			framePresentQpc = presentQpc; // 编码的是最新一帧，延迟从它的呈现时间算起
			dirty = true;
			++capturedFrames;
		}
		const auto &ptr = capture.pointer();
		if (ptr.shapeVersion != cursorShapeVersion) {
			cursor.setShape(ptr.shapeInfo, ptr.shape);
			cursorShapeVersion = ptr.shapeVersion;
		}
		if (pointerChanged && capture.frame()) {
			if (!dirty)
				framePresentQpc = qpcNow();
			dirty = true;
		}

		const int64_t now = qpcNow();
		bool forceKey = streamer.takeKeyframeRequest();
		if (forceKey && capture.frame())
			dirty = true;
		if (dirty && now >= lastEncodeQpc + frameInterval) {
			// 有可见指针时先把画面复制一份，在副本上画指针
			ID3D11Texture2D *source = capture.frame();
			if (ptr.visible && cursor.hasShape()) {
				D3D11_TEXTURE2D_DESC fd{}, cd{};
				source->GetDesc(&fd);
				if (composed)
					composed->GetDesc(&cd);
				if (!composed || cd.Width != fd.Width || cd.Height != fd.Height) {
					fd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
					composed.Reset();
					capture.device()->CreateTexture2D(&fd, nullptr, &composed);
				}
				if (composed && cursor.compose(source, composed.Get(), ptr.pos))
					source = composed.Get();
			}
			ID3D11Texture2D *tex = nullptr;
			UINT slice = 0;
			if (encoder.beginFrame(&tex, &slice) && converter.convert(source, tex, slice)) {
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
			streamer.sendControl(json{
			    {"type", "stats"},
			    {"encoder", encoder.config().name},
			    {"width", encoder.config().width},
			    {"height", encoder.config().height},
			    {"bitrateKbps", encoder.config().bitrateKbps},
			    {"captureFps", capturedFrames / secs},
			    {"encodeFps", encodedFrames / secs},
			    {"kbps", encodedBytes * 8 / 1000.0 / secs},
			    {"keyframes", keyframes},
			    {"pli", streamer.takePliCount()},
			    {"encodeMs", encodedFrames ? encodeMsSum / encodedFrames : 0.0},
			    {"captureToSendMs", encodedFrames ? lagMsSum / encodedFrames : 0.0},
			    {"cpuPercent", (cpu - cpuStart) / secs / cores * 100.0},
			}.dump());
			statsStart = t;
			cpuStart = cpu;
			capturedFrames = encodedFrames = keyframes = 0;
			encodedBytes = 0;
			encodeMsSum = lagMsSum = 0;
		}
	}
	closeAll();
}

// 由服务启动时进程是 SYSTEM，网址和文件夹要以登录用户的身份打开
void openUrl(const std::wstring &url) { openAsUser(url); }

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	Args args;
	if (!parseArgs(args))
		return 2;
	const std::wstring pairUrl = L"http://127.0.0.1:" + std::to_wstring(args.httpPort) + L"/pair";

	// 已经装成系统服务：用户从开始菜单打开时，不再另起一份，确保服务在运行，然后打开配对页
	if (args.mode == Mode::Normal && args.dataDir.empty() && !args.listOnly && !isLocalSystem()) {
		auto st = service::stateFor(exePath());
		if (st != service::State::NotInstalled) {
			if (st == service::State::Stopped)
				service::start();
			waitForPort(args.httpPort, 15000);
			openUrl(pairUrl);
			return 0;
		}
	}

	const std::string dataNote = chooseDataDir(args);
	initLogFile(dataDir() / "logs");
	if (args.mode == Mode::Service)
		return service::run(exePath());
	if (args.mode == Mode::Install)
		return service::install(exePath(), dataDir());
	if (args.mode == Mode::Uninstall)
		return service::uninstall(exePath());
	const bool agent = args.mode == Mode::Agent;

	log("PocketDesk {} 启动{}，数据目录 {}（{}）", kVersion, agent ? "（系统服务）" : "", dataDir().string(),
	    dataNote);
	for (auto &o : listOutputs())
		log("  显示器 {}{}: {} [{},{} {}x{}] 显卡 {} (0x{:04X})", o.index, o.primary ? "（主）" : "",
		    o.deviceName, o.rect.left, o.rect.top, o.rect.right - o.rect.left,
		    o.rect.bottom - o.rect.top, o.adapterName, o.vendorId);
	if (args.listOnly)
		return 0;

	// 只允许运行一个；再次启动时打开配对页
	std::wstring mutexName = L"Local\\PocketDesk.Next";
	if (!args.dataDir.empty())
		mutexName += L"." + std::to_wstring(std::hash<std::wstring>{}(args.dataDir.wstring()));
	HANDLE singleton = CreateMutexW(nullptr, TRUE, mutexName.c_str());
	// 系统服务启动的那份是 SYSTEM 建的互斥量，普通用户打不开，会得到“拒绝访问”
	if (GetLastError() == ERROR_ALREADY_EXISTS || GetLastError() == ERROR_ACCESS_DENIED) {
		if (agent) {
			log("这个会话里已经有 PocketDesk 在运行，服务启动的这份退出");
			return 5;
		}
		log("PocketDesk 已经在运行，打开配对页");
		openUrl(pairUrl);
		return 0;
	}
	timeBeginPeriod(1);

	if (args.webDir.empty())
		args.webDir = exeDir() / "web";
	const auto host = hostName();
	const auto ips = localLanIPv4();

	CertFiles certs;
	const std::string displayName = args.name.empty() ? host : args.name;
	if (!ensureCertificates(dataDir() / "certs", host, displayName, ips, certs)) {
		log("证书准备失败");
		return 1;
	}
	DeviceStore devices(dataDir() / "devices.json");
	InputInjector input;
	Settings settings;
	settings.fps = args.fps;
	settings.autoKbps = std::clamp(args.bitrateKbps, kAutoMinKbps, kAutoMaxKbps);
	settings.fixedKbps = args.bitrateKbps;

	Streamer::Options so;
	so.playoutDelayMaxMs = args.playoutDelayMaxMs;
	Streamer *streamerPtr = nullptr;
	so.onMessage = [&](const std::string &id, const std::string &text) {
		auto j = json::parse(text, nullptr, false);
		if (j.is_discarded() || !j.is_object())
			return;
		if (j.contains("t")) {
			input.handle(j);
			return;
		}
		const std::string type = j.value("type", "");
		if (type == "settings")
			applySettings(settings, j);
		else if (type == "debug")
			log("[{}] 手机调试 {}", id, text);
		else if (type == "clientStats")
			onClientStats(settings, id, streamerPtr ? streamerPtr->deviceName(id) : "", j);
	};
	so.onControlOpen = [&](const std::string &id) {
		settings.stateDirty = true;
		// 告诉手机这份程序是不是在远程桌面会话里：远程桌面窗口最小化时 Windows 会停止绘制这个会话，
		// 画面全黑，手机据此提示原因
		if (streamerPtr)
			streamerPtr->sendControlTo(
			    id, json{{"type", "hello"}, {"remote", GetSystemMetrics(SM_REMOTESESSION) != 0}}.dump());
	};
	so.onClosed = [&](const std::string &id) {
		input.releaseAll();
		std::lock_guard lock(settings.m);
		settings.clients.erase(id);
	};
	Streamer streamer(so);
	streamerPtr = &streamer;

	WebServer web;
	WebServer::Options wo;
	wo.webDir = args.webDir;
	wo.httpsPort = args.httpsPort;
	wo.httpPort = args.httpPort;
	wo.lanIps = ips;
	wo.pcName = args.name.empty() ? host : args.name;
	wo.version = kVersion;

	Discovery discovery;
	discovery.start({certs.pcId, wo.pcName, kVersion, args.httpsPort, args.httpPort},
	                uint16_t(args.discoveryPort));
	wo.discovery = &discovery;
	if (!web.start(wo, certs, streamer, devices)) {
		// 服务启动的不弹窗（服务会过一会儿重试），看日志
		if (!agent)
			MessageBoxW(nullptr, L"PocketDesk 启动失败：端口被占用或证书无效，详见日志。", L"PocketDesk",
			            MB_ICONERROR);
		return 1;
	}
	log("证书目录: {}", (dataDir() / "certs").string());
	log("日志目录: {}", logDir().string());
	for (auto &ip : ips)
		log("手机地址: https://{}:{}/（首次设置 http://{}:{}/）", ip, args.httpsPort, ip, args.httpPort);

	std::thread worker([&] { captureLoop(args, streamer, input, settings); });

	int exitCode = 0;
	Tray tray;
	Tray::Callbacks cb;
	cb.onPair = [&] { openUrl(pairUrl); };
	cb.onOpenLogs = [] { openUrl(logDir().wstring()); };
	cb.onExit = [&] {
		log("从托盘菜单退出");
		if (agent)
			exitCode = service::kExitUserQuit; // 告诉服务也停下
	};
	tray.create(instance, cb);

	// 由服务启动时：服务要求退出、服务进程没了、或者本机 IP 变了（证书要包含新 IP，重启一次）
	std::thread agentWatch;
	if (agent) {
		agentWatch = std::thread([&] {
			std::vector<std::string> lastIps = ips;
			std::sort(lastIps.begin(), lastIps.end());
			HANDLE waits[2] = {args.agentQuit, args.agentParent};
			for (int tick = 0; gRunning; ++tick) {
				DWORD r = WaitForMultipleObjects(2, waits, FALSE, 500);
				if (r == WAIT_OBJECT_0 || r == WAIT_OBJECT_0 + 1 || r == WAIT_FAILED) {
					log("{}", r == WAIT_OBJECT_0 ? "服务要求退出" : "服务进程已经不在，退出");
					tray.quit();
					return;
				}
				if (tick % 10 == 9) {
					auto now = localLanIPv4();
					std::sort(now.begin(), now.end());
					if (now != lastIps) {
						log("本机 IP 变了，重启以更新证书和二维码");
						exitCode = service::kExitRestart;
						tray.quit();
						return;
					}
				}
			}
		});
	}
	std::wstring tip = L"PocketDesk";
	if (!ips.empty())
		tip += L" · " + widen(ips.front());
	tray.setTooltip(tip);
	if (devices.list().empty()) {
		tray.notify(L"PocketDesk 已在后台运行", L"用手机相机扫描打开的二维码即可连接。");
		openUrl(pairUrl);
	} else if (!agent) {
		// 系统服务每次开机、登录都会启动，不打扰
		tray.notify(L"PocketDesk 已在后台运行", L"点托盘图标可以给新手机扫码授权。");
	}

	// 托盘提示里显示有几部手机在看
	std::thread tipper([&] {
		int last = -1;
		while (gRunning) {
			int n = streamer.viewerCount();
			if (n != last) {
				tray.setTooltip(n ? tip + L" · " + std::to_wstring(n) + L" 部手机在看" : tip);
				last = n;
			}
			for (int i = 0; i < 10 && gRunning; ++i)
				Sleep(100);
		}
	});

	tray.run();

	log("正在退出");
	gRunning = false;
	worker.join();
	tipper.join();
	if (agentWatch.joinable())
		agentWatch.join();
	input.releaseAll();
	web.stop();
	discovery.stop();
	timeEndPeriod(1);
	ReleaseMutex(singleton);
	CloseHandle(singleton);
	log("已退出（退出码 {}）", exitCode);
	return exitCode;
}
