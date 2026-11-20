#include "log.h"

#include <chrono>
#include <cstdio>
#include <mutex>

namespace pd {

static std::mutex gLogMutex;

void logLine(const std::string &line) {
	auto now = std::chrono::system_clock::now();
	auto local = std::chrono::zoned_time{std::chrono::current_zone(),
	                                     std::chrono::floor<std::chrono::milliseconds>(now)};
	std::string out = std::format("{:%H:%M:%S} {}\n", local, line);
	std::lock_guard lock(gLogMutex);
	fwrite(out.data(), 1, out.size(), stdout);
	fflush(stdout);
}

std::string hexhr(long code) { return std::format("0x{:08X}", static_cast<unsigned long>(code)); }

} // namespace pd
