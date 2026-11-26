#include "service.h"
#include "log.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <wtsapi32.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <string>
#include <vector>

namespace pd::service {

namespace {

constexpr wchar_t kName[] = L"PocketDesk";
constexpr wchar_t kDisplayName[] = L"PocketDesk";
constexpr wchar_t kDescription[] =
    L"让局域网里的手机远程查看和操作这台电脑（包括锁屏界面）。停止后手机无法连接。";
constexpr wchar_t kFirewallRule[] = L"PocketDesk";

using Clock = std::chrono::steady_clock;

std::string narrow(const std::wstring &w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring lower(std::wstring s) {
	std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
	return s;
}

struct ScHandle {
	SC_HANDLE h = nullptr;
	ScHandle(SC_HANDLE v = nullptr) : h(v) {}
	~ScHandle() {
		if (h)
			CloseServiceHandle(h);
	}
	ScHandle(const ScHandle &) = delete;
	ScHandle &operator=(const ScHandle &) = delete;
	explicit operator bool() const { return h != nullptr; }
};

// 运行一个命令行程序（不显示窗口），返回退出码；启动失败返回 -1
int runHidden(const std::wstring &cmdline) {
	std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
	buf.push_back(0);
	STARTUPINFOW si{sizeof(si)};
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
	                    &si, &pi))
		return -1;
	CloseHandle(pi.hThread);
	WaitForSingleObject(pi.hProcess, 30000);
	DWORD code = DWORD(-1);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	return int(code);
}

std::wstring systemDir() {
	wchar_t buf[MAX_PATH];
	GetSystemDirectoryW(buf, MAX_PATH);
	return buf;
}

// ---------------- 服务进程 ----------------

SERVICE_STATUS_HANDLE gStatusHandle = nullptr;
SERVICE_STATUS gStatus{};
HANDLE gStopEvent = nullptr;
std::filesystem::path gExe;

void setState(DWORD state) {
	gStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	gStatus.dwCurrentState = state;
	gStatus.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
	gStatus.dwWin32ExitCode = NO_ERROR;
	gStatus.dwWaitHint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : 10000;
	++gStatus.dwCheckPoint;
	SetServiceStatus(gStatusHandle, &gStatus);
}

DWORD WINAPI controlHandler(DWORD control, DWORD, void *, void *) {
	switch (control) {
	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_SHUTDOWN:
		setState(SERVICE_STOP_PENDING);
		SetEvent(gStopEvent);
		return NO_ERROR;
	case SERVICE_CONTROL_INTERROGATE:
		return NO_ERROR;
	default:
		return ERROR_CALL_NOT_IMPLEMENTED;
	}
}

struct Agent {
	HANDLE process = nullptr;
	HANDLE quit = nullptr; // 通知程序退出的事件（程序继承了这个句柄）
	DWORD session = 0;
	Clock::time_point started;
};

// 在指定会话里以 SYSTEM 身份启动 pocketdesk.exe --agent（Sunshine 的做法：复制服务自己的令牌，改会话号）
bool launchAgent(Agent &agent, DWORD session) {
	HANDLE self = nullptr, token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(),
	                      TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT |
	                          TOKEN_ADJUST_SESSIONID,
	                      &self)) {
		log("[服务] 打开令牌失败（错误 {}）", GetLastError());
		return false;
	}
	const bool dup = DuplicateTokenEx(self, MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary,
	                                  &token);
	CloseHandle(self);
	if (!dup) {
		log("[服务] 复制令牌失败（错误 {}）", GetLastError());
		return false;
	}
	if (!SetTokenInformation(token, TokenSessionId, &session, sizeof(session))) {
		log("[服务] 设置会话 {} 失败（错误 {}）", session, GetLastError());
		CloseHandle(token);
		return false;
	}

	SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
	HANDLE quit = CreateEventW(&sa, TRUE, FALSE, nullptr);
	HANDLE parent = nullptr; // 程序拿着服务进程的句柄，服务意外退出时它也跟着退出
	DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent, SYNCHRONIZE, TRUE, 0);

	std::wstring cmd = L"\"" + gExe.wstring() + L"\" --agent " + std::to_wstring(uintptr_t(quit)) + L" " +
	                   std::to_wstring(uintptr_t(parent));
	std::vector<wchar_t> buf(cmd.begin(), cmd.end());
	buf.push_back(0);
	STARTUPINFOW si{sizeof(si)};
	si.lpDesktop = const_cast<wchar_t *>(L"winsta0\\default");
	PROCESS_INFORMATION pi{};
	const std::wstring dir = gExe.parent_path().wstring();
	const bool ok = CreateProcessAsUserW(token, gExe.c_str(), buf.data(), nullptr, nullptr, TRUE,
	                                     NORMAL_PRIORITY_CLASS, nullptr, dir.c_str(), &si, &pi);
	const DWORD err = GetLastError();
	CloseHandle(token);
	if (parent)
		CloseHandle(parent);
	if (!ok) {
		log("[服务] 在会话 {} 启动程序失败（错误 {}）", session, err);
		CloseHandle(quit);
		return false;
	}
	CloseHandle(pi.hThread);
	agent.process = pi.hProcess;
	agent.quit = quit;
	agent.session = session;
	agent.started = Clock::now();
	log("[服务] 已在会话 {} 启动程序（进程 {}）", session, pi.dwProcessId);
	return true;
}

void stopAgent(Agent &agent) {
	if (!agent.process)
		return;
	SetEvent(agent.quit);
	if (WaitForSingleObject(agent.process, 5000) == WAIT_TIMEOUT) {
		log("[服务] 程序 5 秒内没有退出，强制结束");
		TerminateProcess(agent.process, 1);
		WaitForSingleObject(agent.process, 2000);
	}
	CloseHandle(agent.process);
	CloseHandle(agent.quit);
	agent = Agent{};
}

// 选在哪个会话里运行：
//   有人在电脑前登录着（控制台会话处于活动状态）→ 控制台会话；
//   否则有人用远程桌面连着 → 远程桌面的会话（它在用户眼里就是“这台电脑的桌面”，
//     没接显示器的主机也只有这时才有画面）；
//   都没有 → 控制台会话（登录界面、锁屏）。
DWORD pickSession(bool &remote) {
	const DWORD console = WTSGetActiveConsoleSessionId();
	remote = false;
	WTS_SESSION_INFOW *list = nullptr;
	DWORD count = 0;
	DWORD rdp = 0xFFFFFFFF;
	bool consoleActive = false;
	if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &list, &count)) {
		for (DWORD i = 0; i < count; ++i) {
			if (list[i].State != WTSActive || list[i].SessionId == 0)
				continue;
			if (list[i].SessionId == console)
				consoleActive = true;
			else if (rdp == 0xFFFFFFFF)
				rdp = list[i].SessionId;
		}
		WTSFreeMemory(list);
	}
	if (consoleActive || rdp == 0xFFFFFFFF)
		return console;
	remote = true;
	return rdp;
}

void supervise() {
	Agent agent;
	int failures = 0;
	auto nextLaunch = Clock::now();
	for (;;) {
		bool remote = false;
		const DWORD session = pickSession(remote);
		if (agent.process && agent.session != session) {
			log("[服务] 要用的会话从 {} 变成 {}{}，换到新会话里运行", agent.session, session,
			    remote ? "（远程桌面）" : "");
			stopAgent(agent);
			nextLaunch = Clock::now();
		}
		if (!agent.process && session != 0xFFFFFFFF && Clock::now() >= nextLaunch) {
			if (!launchAgent(agent, session))
				nextLaunch = Clock::now() + std::chrono::seconds(5);
		}
		HANDLE waits[2] = {gStopEvent, agent.process};
		const DWORD r = WaitForMultipleObjects(agent.process ? 2 : 1, waits, FALSE, 1000);
		if (r == WAIT_OBJECT_0)
			break;
		if (agent.process && r == WAIT_OBJECT_0 + 1) {
			DWORD code = 0;
			GetExitCodeProcess(agent.process, &code);
			const auto uptime = Clock::now() - agent.started;
			CloseHandle(agent.process);
			CloseHandle(agent.quit);
			agent = Agent{};
			if (code == DWORD(kExitUserQuit)) {
				log("[服务] 用户在托盘里退出了 PocketDesk，服务停止（下次开机或再打开 PocketDesk 时启动）");
				break;
			}
			if (code == DWORD(kExitRestart)) {
				log("[服务] 程序要求重启");
				failures = 0;
				nextLaunch = Clock::now();
				continue;
			}
			// 启动后很快就退出的，等待时间逐次加倍，最长 1 分钟，避免反复弹出错误
			failures = uptime < std::chrono::seconds(30) ? failures + 1 : 1;
			const auto wait = std::chrono::seconds(std::min(60, 2 << std::min(failures - 1, 5)));
			log("[服务] 程序退出（退出码 {}），{} 秒后重新启动", code, wait.count());
			nextLaunch = Clock::now() + wait;
		}
	}
	stopAgent(agent);
}

void WINAPI serviceMain(DWORD, LPWSTR *) {
	gStatusHandle = RegisterServiceCtrlHandlerExW(kName, controlHandler, nullptr);
	if (!gStatusHandle)
		return;
	gStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	setState(SERVICE_RUNNING);
	log("[服务] 已启动");
	supervise();
	log("[服务] 已停止");
	setState(SERVICE_STOPPED);
}

// ---------------- 安装 ----------------

bool applyAcl(const std::filesystem::path &path, const wchar_t *sddl) {
	PSECURITY_DESCRIPTOR sd = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr))
		return false;
	BOOL present = FALSE, defaulted = FALSE;
	PACL dacl = nullptr;
	GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
	// SetNamedSecurityInfo 会把可继承的权限同步到已有的子文件和子文件夹
	DWORD err = SetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT,
	                                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
	                                  nullptr, nullptr, dacl, nullptr);
	LocalFree(sd);
	if (err != ERROR_SUCCESS)
		log("设置 {} 的权限失败（错误 {}）", utf8(path), err);
	return err == ERROR_SUCCESS;
}

// 程序以 SYSTEM 运行，所以普通用户不能改程序文件夹（否则换掉 exe 就能拿到 SYSTEM 权限）；
// data 里有根证书私钥和授权记录，只有管理员和 SYSTEM 能读；日志允许普通用户看。
void secureFolders(const std::filesystem::path &appDir, const std::filesystem::path &dataDir) {
	// SY、BA 完全控制；BU（普通用户）读取和运行
	applyAcl(appDir, L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)");
	std::error_code ec;
	std::filesystem::create_directories(dataDir / L"logs", ec);
	applyAcl(dataDir, L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
	applyAcl(dataDir / L"logs", L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)");
}

void removeFirewallRule() {
	runHidden(L"\"" + systemDir() + L"\\netsh.exe\" advfirewall firewall delete rule name=\"" + kFirewallRule +
	          L"\"");
}

bool addFirewallRule(const std::filesystem::path &exe) {
	removeFirewallRule();
	// 按程序放行（HTTPS、HTTP、局域网发现、WebRTC 的端口都在里面），公用网络也放行：
	// 很多家庭网络被 Windows 识别成“公用”
	const int code = runHidden(L"\"" + systemDir() + L"\\netsh.exe\" advfirewall firewall add rule name=\"" +
	                           kFirewallRule + L"\" dir=in action=allow program=\"" + exe.wstring() +
	                           L"\" enable=yes profile=any description=\"PocketDesk 手机远程控制\"");
	if (code != 0)
		log("添加防火墙规则失败（netsh 退出码 {}）", code);
	return code == 0;
}

bool waitForState(SC_HANDLE svc, DWORD want, int ms) {
	SERVICE_STATUS st{};
	for (int waited = 0; waited <= ms; waited += 200) {
		if (!QueryServiceStatus(svc, &st))
			return false;
		if (st.dwCurrentState == want)
			return true;
		Sleep(200);
	}
	return false;
}

void stopService(SC_HANDLE svc) {
	SERVICE_STATUS st{};
	if (QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED) {
		ControlService(svc, SERVICE_CONTROL_STOP, &st);
		if (!waitForState(svc, SERVICE_STOPPED, 15000))
			log("服务 15 秒内没有停下");
	}
}

// 结束从这个 exe 启动的其他 PocketDesk 进程（升级、卸载时文件被占用）
void killOtherInstances(const std::filesystem::path &exe) {
	const std::wstring want = lower(exe.wstring());
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return;
	PROCESSENTRY32W pe{sizeof(pe)};
	for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
		if (pe.th32ProcessID == GetCurrentProcessId())
			continue;
		HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
		                       pe.th32ProcessID);
		if (!p)
			continue;
		wchar_t path[MAX_PATH];
		DWORD len = MAX_PATH;
		if (QueryFullProcessImageNameW(p, 0, path, &len) && lower(path) == want) {
			log("结束正在运行的 PocketDesk（进程 {}）", pe.th32ProcessID);
			TerminateProcess(p, 1);
			WaitForSingleObject(p, 3000);
		}
		CloseHandle(p);
	}
	CloseHandle(snap);
}

} // namespace

int run(const std::filesystem::path &exe) {
	gExe = exe;
	SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t *>(kName), serviceMain}, {nullptr, nullptr}};
	if (!StartServiceCtrlDispatcherW(table)) {
		log("--service 只能由服务管理器启动（错误 {}）", GetLastError());
		return 1;
	}
	return 0;
}

int install(const std::filesystem::path &exe, const std::filesystem::path &dataDir) {
	log("安装系统服务：{}", utf8(exe));
	ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
	if (!scm) {
		log("打开服务管理器失败（错误 {}），需要管理员权限", GetLastError());
		return 1;
	}
	killOtherInstances(exe);
	secureFolders(exe.parent_path(), dataDir);
	addFirewallRule(exe);

	const std::wstring binPath = L"\"" + exe.wstring() + L"\" --service";
	ScHandle svc(OpenServiceW(scm.h, kName, SERVICE_ALL_ACCESS));
	if (svc) {
		stopService(svc.h);
		if (!ChangeServiceConfigW(svc.h, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
		                          binPath.c_str(), nullptr, nullptr, nullptr, L"LocalSystem", nullptr,
		                          kDisplayName)) {
			log("更新服务配置失败（错误 {}）", GetLastError());
			return 1;
		}
	} else {
		svc.h = CreateServiceW(scm.h, kName, kDisplayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
		                       SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, binPath.c_str(), nullptr, nullptr,
		                       nullptr, nullptr, nullptr);
		if (!svc) {
			log("创建服务失败（错误 {}）", GetLastError());
			return 1;
		}
	}
	SERVICE_DESCRIPTIONW desc{const_cast<wchar_t *>(kDescription)};
	ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_DESCRIPTION, &desc);
	// 服务进程本身意外退出时，5 秒后自动重启
	SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 30000}};
	SERVICE_FAILURE_ACTIONSW fa{};
	fa.dwResetPeriod = 24 * 3600;
	fa.cActions = 3;
	fa.lpsaActions = actions;
	ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
	// 在默认权限上加一条：已登录的用户（IU）可以启动服务，这样在托盘里退出后，从开始菜单打开 PocketDesk 就能恢复
	PSECURITY_DESCRIPTOR sd = nullptr;
	if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
	        L"D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWRPLOCRRC;;;IU)"
	        L"(A;;CCLCSWLOCRRC;;;SU)",
	        SDDL_REVISION_1, &sd, nullptr)) {
		SetServiceObjectSecurity(svc.h, DACL_SECURITY_INFORMATION, sd);
		LocalFree(sd);
	}
	if (!StartServiceW(svc.h, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
		log("启动服务失败（错误 {}）", GetLastError());
		return 1;
	}
	waitForState(svc.h, SERVICE_RUNNING, 10000);
	log("系统服务已安装并启动，防火墙已放行");
	return 0;
}

int uninstall(const std::filesystem::path &exe) {
	log("卸载系统服务");
	ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
	if (scm) {
		ScHandle svc(OpenServiceW(scm.h, kName, SERVICE_ALL_ACCESS));
		if (svc) {
			stopService(svc.h);
			if (!DeleteService(svc.h))
				log("删除服务失败（错误 {}）", GetLastError());
		}
	} else {
		log("打开服务管理器失败（错误 {}），需要管理员权限", GetLastError());
	}
	killOtherInstances(exe);
	removeFirewallRule();
	log("系统服务和防火墙规则已删除");
	return scm ? 0 : 1;
}

State stateFor(const std::filesystem::path &exe) {
	ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
	if (!scm)
		return State::NotInstalled;
	ScHandle svc(OpenServiceW(scm.h, kName, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS));
	if (!svc)
		return State::NotInstalled;
	DWORD needed = 0;
	QueryServiceConfigW(svc.h, nullptr, 0, &needed);
	std::vector<BYTE> buf(needed);
	auto *cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buf.data());
	if (!needed || !QueryServiceConfigW(svc.h, cfg, needed, &needed) || !cfg->lpBinaryPathName)
		return State::NotInstalled;
	if (lower(cfg->lpBinaryPathName).find(lower(exe.wstring())) == std::wstring::npos)
		return State::NotInstalled;
	SERVICE_STATUS st{};
	if (!QueryServiceStatus(svc.h, &st))
		return State::NotInstalled;
	if (st.dwCurrentState == SERVICE_RUNNING)
		return State::Running;
	if (st.dwCurrentState == SERVICE_STOPPED)
		return State::Stopped;
	return State::Pending;
}

bool start() {
	ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
	if (!scm)
		return false;
	ScHandle svc(OpenServiceW(scm.h, kName, SERVICE_START | SERVICE_QUERY_STATUS));
	if (!svc)
		return false;
	if (!StartServiceW(svc.h, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
		return false;
	return waitForState(svc.h, SERVICE_RUNNING, 10000);
}

} // namespace pd::service
