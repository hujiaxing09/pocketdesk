#pragma once
#include <filesystem>
#include <format>
#include <string>

namespace pd {

// 日志同时写到控制台（如果有）和 dir 下按天分的文件，保留 14 天
void initLogFile(const std::filesystem::path &dir);
std::filesystem::path logDir();

void logLine(const std::string &line);

template <class... Args> void log(std::format_string<Args...> fmt, Args &&...args) {
	logLine(std::format(fmt, std::forward<Args>(args)...));
}

// 路径转成 UTF-8（日志、httplib、OpenSSL 都按 UTF-8 理解）；path::string() 是系统 ANSI 编码，中文目录会乱码或找不到
std::string utf8(const std::filesystem::path &p);

// HRESULT 转成 0x 开头的十六进制字符串
std::string hexhr(long code);

} // namespace pd
