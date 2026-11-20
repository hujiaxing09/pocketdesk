#pragma once
#include <string>
#include <vector>

namespace pd {

// 本机处于局域网私有地址段（10/8、172.16/12、192.168/16）的 IPv4 地址
std::vector<std::string> localLanIPv4();

struct LanInterface {
	std::string ip;
	std::string broadcast; // 本网段的广播地址
};
// 同上，附带每个地址所在网段的广播地址（局域网发现用）
std::vector<LanInterface> lanInterfaces();

// 计算机名（小写，DNS 形式）
std::string hostName();

} // namespace pd
