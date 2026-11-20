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
	std::string pcId;                 // 这台电脑的身份：根证书 SHA-256 的前 16 位十六进制
};

// 确保 dir 下有本机根证书和服务器证书。
// 根证书只生成一次；服务器证书在 IP 或主机名变化、或快过期时用根证书重签。
// displayName 用在根证书名字里（手机“证书信任设置”里看到的就是它），默认是计算机名。
bool ensureCertificates(const std::filesystem::path &dir, const std::string &host,
                        const std::string &displayName, const std::vector<std::string> &ips,
                        CertFiles &out);

} // namespace pd
