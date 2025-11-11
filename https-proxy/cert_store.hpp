#ifndef CERT_STORE_HPP
#define CERT_STORE_HPP

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>

class cert_store {
public:
    struct cert_pair {
        std::unique_ptr<X509, decltype(&X509_free)> cert;
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key;

        cert_pair() : cert(nullptr, X509_free), key(nullptr, EVP_PKEY_free) {}
        cert_pair(X509* c, EVP_PKEY* k) : cert(c, X509_free), key(k, EVP_PKEY_free) {}
    };

    static cert_store& instance();

    // Ensure the root CA exists (load or create)
    bool ensure_ca();

    // Issue a certificate for a specific hostname
    cert_pair issue_for_host(const std::string& hostname);

private:
    cert_store();
    ~cert_store() = default;
    cert_store(const cert_store&) = delete;
    cert_store& operator=(const cert_store&) = delete;

    std::string get_ca_dir() const;
    std::string get_ca_cert_path() const;
    std::string get_ca_key_path() const;

    bool load_ca();
    bool create_ca();
    bool save_ca();
    void install_ca_to_trust_store();

    cert_pair create_cert_for_host(const std::string& hostname);

    std::unique_ptr<X509, decltype(&X509_free)> ca_cert_;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> ca_key_;
    std::unordered_map<std::string, cert_pair> cert_cache_;
    std::mutex mutex_;
    bool ca_loaded_;
};

#endif // CERT_STORE_HPP

