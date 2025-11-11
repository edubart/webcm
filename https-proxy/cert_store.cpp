#include "cert_store.hpp"
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/x509v3.h>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

cert_store& cert_store::instance() {
    static cert_store store;
    return store;
}

cert_store::cert_store()
    : ca_cert_(nullptr, X509_free)
    , ca_key_(nullptr, EVP_PKEY_free)
    , ca_loaded_(false) {
}

std::string cert_store::get_ca_dir() const {
    return "/etc/ssl/webcm";
}

std::string cert_store::get_ca_cert_path() const {
    return "/etc/ssl/webcm/mitm-ca.crt";
}

std::string cert_store::get_ca_key_path() const {
    return "/etc/ssl/webcm/mitm-ca.key";
}

bool cert_store::ensure_ca() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (ca_loaded_) {
        return true;
    }

    // Try to load existing CA
    bool ca_existed = load_ca();
    if (ca_existed) {
        ca_loaded_ = true;
        install_ca_to_trust_store();
        return true;
    }

    // Create new CA if loading failed
    if (create_ca() && save_ca()) {
        ca_loaded_ = true;
        install_ca_to_trust_store();
        return true;
    }

    return false;
}

bool cert_store::load_ca() {
    std::string cert_path = get_ca_cert_path();
    std::string key_path = get_ca_key_path();

    std::ifstream cert_file(cert_path, std::ios::binary);
    std::ifstream key_file(key_path, std::ios::binary);

    if (!cert_file.is_open() || !key_file.is_open()) {
        return false;
    }

    std::string cert_pem((std::istreambuf_iterator<char>(cert_file)),
                         std::istreambuf_iterator<char>());
    std::string key_pem((std::istreambuf_iterator<char>(key_file)),
                        std::istreambuf_iterator<char>());

    BIO* cert_bio = BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size()));
    BIO* key_bio = BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size()));

    if (!cert_bio || !key_bio) {
        if (cert_bio) BIO_free(cert_bio);
        if (key_bio) BIO_free(key_bio);
        return false;
    }

    X509* cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
    EVP_PKEY* key = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);

    BIO_free(cert_bio);
    BIO_free(key_bio);

    if (!cert || !key) {
        if (cert) X509_free(cert);
        if (key) EVP_PKEY_free(key);
        return false;
    }

    ca_cert_.reset(cert);
    ca_key_.reset(key);
    return true;
}

bool cert_store::create_ca() {
    // Generate CA private key
    EVP_PKEY* key = EVP_PKEY_new();
    if (!key) {
        return false;
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!ctx) {
        EVP_PKEY_free(key);
        return false;
    }

    if (EVP_PKEY_keygen_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) <= 0 ||
        EVP_PKEY_keygen(ctx, &key) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(key);
        return false;
    }
    EVP_PKEY_CTX_free(ctx);

    // Create CA certificate
    X509* cert = X509_new();
    if (!cert) {
        EVP_PKEY_free(key);
        return false;
    }

    // Set version
    X509_set_version(cert, 2);

    // Set serial number
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);

    // Set validity (10 years)
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 315360000L); // 10 years

    // Set subject and issuer
    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("US"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("WebCM"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("WebCM MITM CA"), -1, -1, 0);

    X509_set_subject_name(cert, name);
    X509_set_issuer_name(cert, name);
    X509_NAME_free(name);

    // Set public key
    X509_set_pubkey(cert, key);

    // Add basic constraints extension
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, nullptr, NID_basic_constraints,
                                               const_cast<char*>("CA:TRUE"));
    if (ext) {
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
    }

    // Add key usage extension
    ext = X509V3_EXT_conf_nid(nullptr, nullptr, NID_key_usage,
                               const_cast<char*>("keyCertSign, cRLSign"));
    if (ext) {
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
    }

    // Sign the certificate
    if (X509_sign(cert, key, EVP_sha256()) <= 0) {
        X509_free(cert);
        EVP_PKEY_free(key);
        return false;
    }

    ca_cert_.reset(cert);
    ca_key_.reset(key);
    return true;
}

bool cert_store::save_ca() {
    if (!ca_cert_ || !ca_key_) {
        return false;
    }

    std::string ca_dir = get_ca_dir();
    std::string cert_path = get_ca_cert_path();
    std::string key_path = get_ca_key_path();

    // Create directory if it doesn't exist (create parent directories if needed)
    struct stat st;
    if (stat(ca_dir.c_str(), &st) != 0) {
        // Try to create parent directories first
        std::string parent_dir = ca_dir;
        size_t pos = parent_dir.find_last_of('/');
        if (pos != std::string::npos && pos > 0) {
            parent_dir = parent_dir.substr(0, pos);
            struct stat parent_st;
            if (stat(parent_dir.c_str(), &parent_st) != 0) {
                // Try to create parent directory
                mkdir(parent_dir.c_str(), 0755);
            }
        }
        // Now create the target directory
        if (mkdir(ca_dir.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
    }

    // Write certificate
    BIO* cert_bio = BIO_new_file(cert_path.c_str(), "w");
    if (!cert_bio) {
        return false;
    }
    bool cert_ok = PEM_write_bio_X509(cert_bio, ca_cert_.get()) > 0;
    BIO_free(cert_bio);
    if (!cert_ok) {
        return false;
    }

    // Write private key
    BIO* key_bio = BIO_new_file(key_path.c_str(), "w");
    if (!key_bio) {
        return false;
    }
    bool key_ok = PEM_write_bio_PrivateKey(key_bio, ca_key_.get(), nullptr, nullptr, 0, nullptr, nullptr) > 0;
    BIO_free(key_bio);
    if (!key_ok) {
        return false;
    }

    return true;
}

void cert_store::install_ca_to_trust_store() {
    if (!ca_cert_) {
        return;
    }

    std::string cert_path = get_ca_cert_path();
    std::string trust_store_path = "/usr/local/share/ca-certificates/webcm-mitm-ca.crt";
    std::string cert_pem_path = "/etc/ssl/cert.pem";

    // Check if already installed and up to date
    struct stat trust_stat, cert_stat, pem_stat;
    bool need_update = true;
    if (stat(trust_store_path.c_str(), &trust_stat) == 0 &&
        stat(cert_path.c_str(), &cert_stat) == 0 &&
        stat(cert_pem_path.c_str(), &pem_stat) == 0 &&
        trust_stat.st_mtime >= cert_stat.st_mtime &&
        pem_stat.st_mtime >= cert_stat.st_mtime) {
        // Already installed and up to date
        need_update = false;
    }

    if (!need_update) {
        return;
    }

    // Copy certificate to trust store
    std::ifstream cert_file(cert_path, std::ios::binary);
    if (!cert_file.is_open()) {
        return;
    }

    // Copy to /usr/local/share/ca-certificates/ (for update-ca-certificates compatibility)
    std::ofstream trust_file(trust_store_path, std::ios::binary);
    if (!trust_file.is_open()) {
        cert_file.close();
        return;
    }

    // Read certificate content once
    std::string cert_content((std::istreambuf_iterator<char>(cert_file)),
                             std::istreambuf_iterator<char>());
    cert_file.close();

    // Write to trust store
    trust_file << cert_content;
    trust_file.close();

    // Copy directly to cert.pem (since we only have one CA)
    std::ofstream pem_file(cert_pem_path, std::ios::binary);
    if (pem_file.is_open()) {
        pem_file << cert_content;
        pem_file.close();
    }

    // Also update /etc/ssl/certs/ directory (create symlink for compatibility)
    std::string certs_dir = "/etc/ssl/certs";
    struct stat dir_stat;
    if (stat(certs_dir.c_str(), &dir_stat) == 0) {
        std::string cert_link = certs_dir + "/webcm-mitm-ca.crt";
        unlink(cert_link.c_str()); // Remove existing link if any
        symlink(trust_store_path.c_str(), cert_link.c_str());
    }
}

cert_store::cert_pair cert_store::issue_for_host(const std::string& hostname) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check cache first
    auto it = cert_cache_.find(hostname);
    if (it != cert_cache_.end()) {
        // Return a copy (we need to duplicate the cert/key for OpenSSL)
        X509* cert_dup = X509_dup(it->second.cert.get());
        EVP_PKEY* key = it->second.key.get();
        if (cert_dup && key && EVP_PKEY_up_ref(key)) {
            return cert_pair(cert_dup, key);
        }
        if (cert_dup) X509_free(cert_dup);
    }

    // Ensure CA is loaded
    if (!ca_loaded_ && !ensure_ca()) {
        return cert_pair();
    }

    // Create new certificate
    cert_pair pair = create_cert_for_host(hostname);
    if (pair.cert && pair.key) {
        // Cache it (increment ref count for key, duplicate cert)
        X509* cert_dup = X509_dup(pair.cert.get());
        EVP_PKEY* key = pair.key.get();
        if (cert_dup && key && EVP_PKEY_up_ref(key)) {
            cert_cache_[hostname] = cert_pair(cert_dup, key);
        } else {
            if (cert_dup) X509_free(cert_dup);
        }
    }

    return pair;
}

cert_store::cert_pair cert_store::create_cert_for_host(const std::string& hostname) {
    if (!ca_cert_ || !ca_key_) {
        return cert_pair();
    }

    // Generate leaf private key
    EVP_PKEY* key = EVP_PKEY_new();
    if (!key) {
        return cert_pair();
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!ctx) {
        EVP_PKEY_free(key);
        return cert_pair();
    }

    if (EVP_PKEY_keygen_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) <= 0 ||
        EVP_PKEY_keygen(ctx, &key) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(key);
        return cert_pair();
    }
    EVP_PKEY_CTX_free(ctx);

    // Create certificate
    X509* cert = X509_new();
    if (!cert) {
        EVP_PKEY_free(key);
        return cert_pair();
    }

    // Set version
    X509_set_version(cert, 2);

    // Set serial number (random)
    unsigned char serial_bytes[20];
    if (RAND_bytes(serial_bytes, sizeof(serial_bytes)) <= 0) {
        X509_free(cert);
        EVP_PKEY_free(key);
        return cert_pair();
    }
    BIGNUM* bn = BN_bin2bn(serial_bytes, sizeof(serial_bytes), nullptr);
    if (!bn) {
        X509_free(cert);
        EVP_PKEY_free(key);
        return cert_pair();
    }
    ASN1_INTEGER* serial = ASN1_INTEGER_new();
    if (!serial) {
        BN_free(bn);
        X509_free(cert);
        EVP_PKEY_free(key);
        return cert_pair();
    }
    BN_to_ASN1_INTEGER(bn, serial);
    BN_free(bn);
    X509_set_serialNumber(cert, serial);
    ASN1_INTEGER_free(serial);

    // Set validity (1 year)
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 31536000L); // 1 year

    // Set subject
    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("US"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("WebCM"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(hostname.c_str()), -1, -1, 0);
    X509_set_subject_name(cert, name);
    X509_NAME_free(name);

    // Set issuer (CA)
    X509_set_issuer_name(cert, X509_get_subject_name(ca_cert_.get()));

    // Set public key
    X509_set_pubkey(cert, key);

    // Add subject alternative name extension
    STACK_OF(GENERAL_NAME)* san_list = sk_GENERAL_NAME_new_null();
    if (san_list) {
        GENERAL_NAME* san = GENERAL_NAME_new();
        if (san) {
            ASN1_STRING* asn1_str = ASN1_STRING_new();
            if (asn1_str) {
                ASN1_STRING_set(asn1_str, hostname.data(), static_cast<int>(hostname.length()));
                san->type = GEN_DNS;
                san->d.dNSName = asn1_str;
                sk_GENERAL_NAME_push(san_list, san);
            } else {
                GENERAL_NAME_free(san);
            }
        }

        X509_EXTENSION* ext = X509V3_EXT_i2d(NID_subject_alt_name, 0, san_list);
        if (ext) {
            X509_add_ext(cert, ext, -1);
            X509_EXTENSION_free(ext);
        }
        sk_GENERAL_NAME_pop_free(san_list, GENERAL_NAME_free);
    }

    // Sign the certificate with CA
    if (X509_sign(cert, ca_key_.get(), EVP_sha256()) <= 0) {
        X509_free(cert);
        EVP_PKEY_free(key);
        return cert_pair();
    }

    return cert_pair(cert, key);
}

