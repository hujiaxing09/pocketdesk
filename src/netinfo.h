#pragma once
#include <string>
#include <vector>

namespace pd {

// 本机处于局域网私有地址段（10/8、172.16/12、192.168/16）的 IPv4 地址
std::vector<std::string> localLanIPv4();

// 计算机名（小写，DNS 形式）
std::string hostName();

} // namespace pd
