#pragma once
#include <format>
#include <string>

namespace pd {

void logLine(const std::string &line);

template <class... Args> void log(std::format_string<Args...> fmt, Args &&...args) {
	logLine(std::format(fmt, std::forward<Args>(args)...));
}

// HRESULT 转成 0x 开头的十六进制字符串
std::string hexhr(long code);

} // namespace pd
