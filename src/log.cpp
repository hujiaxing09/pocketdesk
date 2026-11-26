#include "log.h"

#include <chrono>
#include <cstdio>
#include <mutex>

namespace pd {

namespace {
std::mutex gLogMutex;
std::filesystem::path gDir;
FILE *gFile = nullptr;
std::string gFileDate;
constexpr auto kKeep = std::chrono::days(14);

void openForDate(const std::string &date) {
	if (gFile)
		fclose(gFile);
	gFile = nullptr;
	gFileDate = date;
	if (gDir.empty())
		return;
	auto path = gDir / ("pocketdesk-" + date + ".log");
	gFile = _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
}
} // namespace

void initLogFile(const std::filesystem::path &dir) {
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	// 删掉 14 天前的日志
	const auto cutoff = std::filesystem::file_time_type::clock::now() - kKeep;
	for (auto &e : std::filesystem::directory_iterator(dir, ec)) {
		if (e.path().extension() == ".log" && e.last_write_time(ec) < cutoff)
			std::filesystem::remove(e.path(), ec);
	}
	std::lock_guard lock(gLogMutex);
	gDir = dir;
	gFileDate.clear();
}

std::filesystem::path logDir() {
	std::lock_guard lock(gLogMutex);
	return gDir;
}

void logLine(const std::string &line) {
	auto now = std::chrono::system_clock::now();
	auto local = std::chrono::zoned_time{std::chrono::current_zone(),
	                                     std::chrono::floor<std::chrono::milliseconds>(now)};
	std::string out = std::format("{:%H:%M:%S} {}\n", local, line);
	std::string date = std::format("{:%Y%m%d}", local);
	std::lock_guard lock(gLogMutex);
	if (date != gFileDate)
		openForDate(date);
	if (gFile) {
		fwrite(out.data(), 1, out.size(), gFile);
		fflush(gFile);
	}
	fwrite(out.data(), 1, out.size(), stdout);
	fflush(stdout);
}

std::string hexhr(long code) { return std::format("0x{:08X}", static_cast<unsigned long>(code)); }

std::string utf8(const std::filesystem::path &p) {
	auto s = p.u8string();
	return std::string(s.begin(), s.end());
}

} // namespace pd
