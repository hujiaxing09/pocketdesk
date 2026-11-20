#include "netinfo.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <algorithm>
#include <cctype>

namespace pd {

static bool isPrivate(uint32_t hostOrder) {
	uint8_t a = hostOrder >> 24, b = (hostOrder >> 16) & 0xFF;
	return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

std::vector<LanInterface> lanInterfaces() {
	std::vector<LanInterface> result;
	ULONG size = 32 * 1024;
	std::vector<unsigned char> buf(size);
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
	                                 reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()), &size);
	if (ret == ERROR_BUFFER_OVERFLOW) {
		buf.resize(size);
		ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
		                           reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()), &size);
	}
	if (ret != NO_ERROR)
		return result;
	for (auto *a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()); a; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;
		for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
			auto *sin = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
			if (sin->sin_family != AF_INET)
				continue;
			const uint32_t host = ntohl(sin->sin_addr.s_addr);
			if (!isPrivate(host))
				continue;
			const UINT8 prefix = std::min<UINT8>(u->OnLinkPrefixLength, 32);
			const uint32_t mask = prefix ? 0xFFFFFFFFu << (32 - prefix) : 0;
			in_addr bc{};
			bc.s_addr = htonl(host | ~mask);
			char text[INET_ADDRSTRLEN] = {}, bcText[INET_ADDRSTRLEN] = {};
			inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text));
			inet_ntop(AF_INET, &bc, bcText, sizeof(bcText));
			if (std::none_of(result.begin(), result.end(), [&](auto &i) { return i.ip == text; }))
				result.push_back({text, bcText});
		}
	}
	return result;
}

std::vector<std::string> localLanIPv4() {
	std::vector<std::string> ips;
	for (auto &i : lanInterfaces())
		ips.push_back(i.ip);
	return ips;
}

std::string hostName() {
	char name[256] = {};
	DWORD len = sizeof(name);
	if (!GetComputerNameExA(ComputerNameDnsHostname, name, &len))
		return "pocketdesk";
	std::string s(name, len);
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return s;
}

} // namespace pd
