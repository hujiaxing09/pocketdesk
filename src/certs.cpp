#include "certs.h"
#include "log.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <ctime>
#include <memory>

namespace pd {

namespace {

using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using CertPtr = std::unique_ptr<X509, decltype(&X509_free)>;

constexpr long kDay = 24L * 3600;
// iOS 要求服务器证书有效期不超过 825 天；这里用 397 天，与公共 CA 的上限一致
constexpr long kLeafDays = 397;
constexpr long kCaDays = 3650;
constexpr long kRenewBeforeDays = 30;

// 根证书只能签局域网私有地址和 .local 名称，私钥泄露时影响范围有限
constexpr const char *kNameConstraints =
    "critical,permitted;IP:10.0.0.0/255.0.0.0,permitted;IP:172.16.0.0/255.240.0.0,"
    "permitted;IP:192.168.0.0/255.255.0.0,permitted;DNS:local";

std::string sslError() {
	unsigned long e = ERR_get_error();
	char buf[256] = {};
	ERR_error_string_n(e, buf, sizeof(buf));
	return buf;
}

KeyPtr generateKey() { return KeyPtr(EVP_EC_gen("P-256"), EVP_PKEY_free); }

bool addExt(X509 *cert, X509 *issuer, int nid, const std::string &value) {
	X509V3_CTX ctx;
	X509V3_set_ctx_nodb(&ctx);
	X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
	X509_EXTENSION *ex = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
	if (!ex) {
		log("证书扩展 {} 生成失败: {}", OBJ_nid2sn(nid), sslError());
		return false;
	}
	X509_add_ext(cert, ex, -1);
	X509_EXTENSION_free(ex);
	return true;
}

void setRandomSerial(X509 *cert) {
	unsigned char bytes[16];
	RAND_bytes(bytes, sizeof(bytes));
	bytes[0] &= 0x7F;
	BIGNUM *bn = BN_bin2bn(bytes, sizeof(bytes), nullptr);
	BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert));
	BN_free(bn);
}

void setName(X509_NAME *name, const std::string &cn) {
	X509_NAME_add_entry_by_txt(name, "O", MBSTRING_UTF8,
	                           reinterpret_cast<const unsigned char *>("PocketDesk"), -1, -1, 0);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
	                           reinterpret_cast<const unsigned char *>(cn.c_str()), -1, -1, 0);
}

bool validDnsLabel(const std::string &s) {
	if (s.empty() || s.size() > 63 || s.front() == '-' || s.back() == '-')
		return false;
	for (char c : s)
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
			return false;
	return true;
}

bool writeKey(const std::filesystem::path &p, EVP_PKEY *key) {
	BIO *bio = BIO_new_file(reinterpret_cast<const char *>(p.u8string().c_str()), "wb");
	if (!bio)
		return false;
	bool ok = PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
	BIO_free(bio);
	return ok;
}

bool writeCert(const std::filesystem::path &p, X509 *cert) {
	BIO *bio = BIO_new_file(reinterpret_cast<const char *>(p.u8string().c_str()), "wb");
	if (!bio)
		return false;
	bool ok = PEM_write_bio_X509(bio, cert) == 1;
	BIO_free(bio);
	return ok;
}

KeyPtr readKey(const std::filesystem::path &p) {
	BIO *bio = BIO_new_file(reinterpret_cast<const char *>(p.u8string().c_str()), "rb");
	if (!bio)
		return KeyPtr(nullptr, EVP_PKEY_free);
	EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
	BIO_free(bio);
	return KeyPtr(key, EVP_PKEY_free);
}

CertPtr readCert(const std::filesystem::path &p) {
	BIO *bio = BIO_new_file(reinterpret_cast<const char *>(p.u8string().c_str()), "rb");
	if (!bio)
		return CertPtr(nullptr, X509_free);
	X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
	BIO_free(bio);
	return CertPtr(cert, X509_free);
}

bool makeCa(const std::string &caName, KeyPtr &keyOut, CertPtr &certOut) {
	KeyPtr key = generateKey();
	CertPtr cert(X509_new(), X509_free);
	if (!key || !cert)
		return false;
	X509 *c = cert.get();
	X509_set_version(c, 2);
	setRandomSerial(c);
	X509_gmtime_adj(X509_getm_notBefore(c), -kDay);
	X509_gmtime_adj(X509_getm_notAfter(c), kCaDays * kDay);
	setName(X509_get_subject_name(c), caName);
	X509_set_issuer_name(c, X509_get_subject_name(c));
	X509_set_pubkey(c, key.get());
	bool ok = addExt(c, c, NID_basic_constraints, "critical,CA:TRUE,pathlen:0") &&
	          addExt(c, c, NID_key_usage, "critical,keyCertSign,cRLSign") &&
	          addExt(c, c, NID_subject_key_identifier, "hash") &&
	          addExt(c, c, NID_name_constraints, kNameConstraints);
	if (!ok || X509_sign(c, key.get(), EVP_sha256()) <= 0) {
		log("根证书签名失败: {}", sslError());
		return false;
	}
	keyOut = std::move(key);
	certOut = std::move(cert);
	return true;
}

bool makeLeaf(X509 *ca, EVP_PKEY *caKey, const std::string &dnsName,
              const std::vector<std::string> &ips, KeyPtr &keyOut, CertPtr &certOut) {
	KeyPtr key = generateKey();
	CertPtr cert(X509_new(), X509_free);
	if (!key || !cert)
		return false;
	X509 *c = cert.get();
	X509_set_version(c, 2);
	setRandomSerial(c);
	X509_gmtime_adj(X509_getm_notBefore(c), -kDay);
	X509_gmtime_adj(X509_getm_notAfter(c), kLeafDays * kDay);
	setName(X509_get_subject_name(c), dnsName.empty() ? "PocketDesk" : dnsName);
	X509_set_issuer_name(c, X509_get_subject_name(ca));
	X509_set_pubkey(c, key.get());

	std::string san;
	if (!dnsName.empty())
		san = "DNS:" + dnsName;
	for (const auto &ip : ips)
		san += (san.empty() ? "IP:" : ",IP:") + ip;
	if (san.empty())
		san = "IP:127.0.0.1";

	bool ok = addExt(c, ca, NID_basic_constraints, "critical,CA:FALSE") &&
	          addExt(c, ca, NID_key_usage, "critical,digitalSignature") &&
	          addExt(c, ca, NID_ext_key_usage, "serverAuth") &&
	          addExt(c, ca, NID_subject_key_identifier, "hash") &&
	          addExt(c, ca, NID_authority_key_identifier, "keyid:always") &&
	          addExt(c, ca, NID_subject_alt_name, san);
	if (!ok || X509_sign(c, caKey, EVP_sha256()) <= 0) {
		log("服务器证书签名失败: {}", sslError());
		return false;
	}
	keyOut = std::move(key);
	certOut = std::move(cert);
	return true;
}

bool leafStillGood(X509 *leaf, EVP_PKEY *leafKey, EVP_PKEY *caKey, const std::string &dnsName,
                   const std::vector<std::string> &ips) {
	if (X509_verify(leaf, caKey) != 1)
		return false;
	if (X509_check_private_key(leaf, leafKey) != 1)
		return false;
	time_t renewAt = time(nullptr) + kRenewBeforeDays * kDay;
	if (X509_cmp_time(X509_get0_notAfter(leaf), &renewAt) <= 0)
		return false;
	if (!dnsName.empty() && X509_check_host(leaf, dnsName.c_str(), dnsName.size(), 0, nullptr) != 1)
		return false;
	for (const auto &ip : ips)
		if (X509_check_ip_asc(leaf, ip.c_str(), 0) != 1)
			return false;
	return true;
}

} // namespace

bool ensureCertificates(const std::filesystem::path &dir, const std::string &host,
                        const std::vector<std::string> &ips, CertFiles &out) {
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);

	const auto caKeyPath = dir / "ca.key";
	const auto caCertPath = dir / "ca.crt";
	out.serverCert = dir / "server.crt";
	out.serverKey = dir / "server.key";
	out.caName = "PocketDesk CA (" + host + ")";
	const std::string dnsName = validDnsLabel(host) ? host + ".local" : "";

	KeyPtr caKey = readKey(caKeyPath);
	CertPtr ca = readCert(caCertPath);
	if (!caKey || !ca || X509_check_private_key(ca.get(), caKey.get()) != 1) {
		log("生成新的根证书: {}", out.caName);
		if (!makeCa(out.caName, caKey, ca))
			return false;
		if (!writeKey(caKeyPath, caKey.get()) || !writeCert(caCertPath, ca.get())) {
			log("根证书写入失败: {}", caCertPath.string());
			return false;
		}
	}

	KeyPtr leafKey = readKey(out.serverKey);
	CertPtr leaf = readCert(out.serverCert);
	if (!leafKey || !leaf || !leafStillGood(leaf.get(), leafKey.get(), caKey.get(), dnsName, ips)) {
		log("签发新的服务器证书（{} 个 IP）", ips.size());
		if (!makeLeaf(ca.get(), caKey.get(), dnsName, ips, leafKey, leaf))
			return false;
		if (!writeKey(out.serverKey, leafKey.get()) || !writeCert(out.serverCert, leaf.get())) {
			log("服务器证书写入失败: {}", out.serverCert.string());
			return false;
		}
	}

	int len = i2d_X509(ca.get(), nullptr);
	out.caDer.resize(len);
	unsigned char *p = reinterpret_cast<unsigned char *>(out.caDer.data());
	i2d_X509(ca.get(), &p);
	return true;
}

} // namespace pd
