#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace pd {

struct CertFiles {
	std::filesystem::path serverCert; // PEM，服务器证书
	std::filesystem::path serverKey;  // PEM，服务器私钥
	std::string caDer;                // 根证书 DER，给手机下载安装
	std::string caName;               // 根证书显示名（手机“证书信任设置”里看到的名字）
};

// 确保 dir 下有根证书和服务器证书。
// 根证书属于电脑组：组里的电脑共用一个（加入组时换成组的）；没有时用 displayName 新建一个。
// 服务器证书每台电脑各自签，在 IP 或主机名变化、或快过期时重签。
bool ensureCertificates(const std::filesystem::path &dir, const std::string &host,
                        const std::string &displayName, const std::vector<std::string> &ips,
                        CertFiles &out);

} // namespace pd
