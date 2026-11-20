#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace pd {

struct CertFiles {
	std::filesystem::path serverCert; // PEM，服务器证书
	std::filesystem::path serverKey;  // PEM，服务器私钥
	std::string caDer;                // 根证书 DER，给手机下载安装
	std::string caName;               // 根证书显示名
};

// 确保 dir 下有本机根证书和服务器证书。
// 根证书只生成一次；服务器证书在 IP 或主机名变化、或快过期时用根证书重签。
bool ensureCertificates(const std::filesystem::path &dir, const std::string &host,
                        const std::vector<std::string> &ips, CertFiles &out);

} // namespace pd
