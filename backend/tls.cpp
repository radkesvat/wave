#include "tls.h"

#include <httplib.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace wave {
namespace {

namespace fs = std::filesystem;
using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

bool valid_dns_hostname(std::string_view hostname)
{
    if (hostname.empty() || hostname.size() > 253 || hostname.back() == '.') {
        return false;
    }
    for (;;) {
        const auto dot = hostname.find('.');
        const auto label = hostname.substr(0, dot);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
            label.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != label.npos) {
            return false;
        }
        if (dot == hostname.npos) {
            // Browsers treat a final numeric (or hexadecimal) label as an IPv4 address.
            return label.find_first_not_of("0123456789") != label.npos &&
                   !(label.starts_with("0x") && label.substr(2).find_first_not_of("0123456789abcdef") == label.npos);
        }
        hostname.remove_prefix(dot + 1);
    }
}

bool inside(const fs::path& path, const fs::path& directory)
{
    const auto relative = path.lexically_relative(directory);
    return !relative.empty() && *relative.begin() != "..";
}

bool in_git_checkout(fs::path path)
{
    for (;;) {
        std::error_code error;
        const auto git_dir = path / ".git";
        const auto marker = fs::status(git_dir, error);
        if (fs::is_regular_file(marker) ||
            (fs::is_directory(marker) && fs::exists(fs::symlink_status(git_dir / "HEAD", error)))) {
            return true;
        }
        if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory) {
            throw std::runtime_error("Cannot check that Wave deployment files are outside Git.");
        }
        if (path == path.root_path()) {
            return false;
        }
        path = path.parent_path();
    }
}

fs::path absolute_path(const fs::path& path)
{
    std::error_code error;
    auto absolute = fs::absolute(path, error);
    if (error) {
        throw std::runtime_error("Cannot resolve a Wave deployment path.");
    }
    return absolute;
}

fs::path resolved_path(const fs::path& absolute)
{
    std::error_code error;
    auto resolved = fs::weakly_canonical(absolute, error);
    if (error) {
        throw std::runtime_error("Cannot resolve a Wave deployment path.");
    }
    return resolved;
}

void require_private_location(const fs::path& path, const fs::path& site_dir)
{
    if (path.empty() || std::ranges::any_of(path.native(), [](unsigned char byte) {
            return byte < 0x20 || byte == 0x7f;
        })) {
        throw std::runtime_error("Supply local paths without control characters for Wave deployment files.");
    }
    const auto absolute = absolute_path(path);
    const auto lexical = absolute.lexically_normal();
    const auto resolved = resolved_path(absolute);
    const auto site = absolute_path(site_dir);
    if (inside(lexical, site.lexically_normal()) || inside(resolved, resolved_path(site)) ||
        in_git_checkout(lexical) || in_git_checkout(resolved)) {
        throw std::runtime_error("Keep Wave deployment files outside the Git checkout and the site directory.");
    }
}

void validate_settings(const TlsSettings& settings)
{
    if (!valid_dns_hostname(settings.domain)) {
        throw std::runtime_error("Use a lowercase DNS hostname without a scheme, port, wildcard, or trailing dot.");
    }
    if (!settings.certificate_chain.is_absolute() || !settings.private_key.is_absolute()) {
        throw std::runtime_error("Use absolute local paths for both TLS PEM files.");
    }
}

Bio open_pem(const fs::path& path, const char* diagnostic)
{
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error(diagnostic);
    }
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || (info.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) == 0) {
        close(fd);
        throw std::runtime_error(diagnostic);
    }
    auto* file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        throw std::runtime_error(diagnostic);
    }
    Bio pem(BIO_new_fp(file, BIO_CLOSE), BIO_free);
    if (!pem) {
        std::fclose(file);
        throw std::runtime_error(diagnostic);
    }
    return pem;
}

int reject_pem_password(char*, int, int, void*)
{
    return -1;
}

void validate_certificate_time(const X509* certificate, std::time_t now)
{
    const int start = X509_cmp_time(X509_get0_notBefore(certificate), &now);
    const int end = X509_cmp_time(X509_get0_notAfter(certificate), &now);
    if (start == 0 || end == 0) {
        throw std::runtime_error("A TLS certificate has invalid validity dates.");
    }
    if (start > 0) {
        throw std::runtime_error("A TLS certificate is not yet valid.");
    }
    if (end < 0) {
        throw std::runtime_error("A TLS certificate has expired.");
    }
}

void load_certificate_chain(SSL_CTX* context, const TlsSettings& settings)
{
    const auto pem = open_pem(settings.certificate_chain, "Cannot read the TLS certificate chain PEM.");
    Certificate leaf(PEM_read_bio_X509_AUX(pem.get(), nullptr, reject_pem_password, nullptr), X509_free);
    if (!leaf) {
        throw std::runtime_error("Cannot load the TLS certificate chain PEM.");
    }
    const auto now = std::time(nullptr);
    if (now == static_cast<std::time_t>(-1)) {
        throw std::runtime_error("Cannot read the clock to check TLS certificate validity.");
    }
    validate_certificate_time(leaf.get(), now);
    const int matches = X509_check_host(leaf.get(), settings.domain.c_str(), settings.domain.size(),
                                       X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS,
                                       nullptr);
    if (matches != 1) {
        throw std::runtime_error("The TLS certificate does not cover the configured hostname.");
    }
    if (X509_check_purpose(leaf.get(), X509_PURPOSE_SSL_SERVER, 0) != 1 ||
        SSL_CTX_use_certificate(context, leaf.get()) != 1) {
        throw std::runtime_error("The leaf certificate cannot be used for TLS server authentication.");
    }

    // Check the supplied chain in order. Clients determine trust using their own roots.
    auto issued = std::move(leaf);
    for (;;) {
        ERR_clear_error();
        Certificate issuer(PEM_read_bio_X509(pem.get(), nullptr, reject_pem_password, nullptr), X509_free);
        if (!issuer) {
            const auto error = ERR_peek_last_error();
            if (BIO_eof(pem.get()) && ERR_GET_LIB(error) == ERR_LIB_PEM &&
                ERR_GET_REASON(error) == PEM_R_NO_START_LINE) {
                ERR_clear_error();
                return;
            }
            throw std::runtime_error("Cannot load the TLS certificate chain PEM.");
        }
        validate_certificate_time(issuer.get(), now);
        auto* issuer_key = X509_get0_pubkey(issuer.get());
        if (!issuer_key || X509_check_ca(issuer.get()) == 0 || X509_check_issued(issuer.get(), issued.get()) != X509_V_OK ||
            X509_verify(issued.get(), issuer_key) != 1 || SSL_CTX_add1_chain_cert(context, issuer.get()) != 1) {
            throw std::runtime_error("Supply the TLS certificate chain in issuer order, with the leaf first.");
        }
        issued = std::move(issuer);
    }
}

} // namespace

std::unique_ptr<httplib::Server> make_tls_server(const TlsSettings& settings, const fs::path& site_dir, int port)
{
    if (port == 0) {
        throw std::runtime_error("Set a fixed listen.port in wave-db.json for HTTPS.");
    }
    validate_settings(settings);
    require_private_location(settings.certificate_chain, site_dir);
    require_private_location(settings.private_key, site_dir);
    auto server = std::make_unique<httplib::SSLServer>([](httplib::tls::ctx_t context) {
        return SSL_CTX_set_min_proto_version(static_cast<SSL_CTX*>(context), TLS1_2_VERSION) == 1;
    });
    if (!server->is_valid()) {
        throw std::runtime_error("Cannot initialize the native HTTPS listener.");
    }
    auto* context = static_cast<SSL_CTX*>(server->tls_context());
    load_certificate_chain(context, settings);
    const auto pem = open_pem(settings.private_key, "Cannot read the TLS private key PEM.");
    const Key key(PEM_read_bio_PrivateKey(pem.get(), nullptr, reject_pem_password, nullptr), EVP_PKEY_free);
    if (!key) {
        throw std::runtime_error("Cannot load the TLS private key. Supply an unencrypted PEM.");
    }
    if (X509_check_private_key(SSL_CTX_get0_certificate(context), key.get()) != 1) {
        throw std::runtime_error("The TLS private key does not match the leaf certificate.");
    }
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> key_context(
        EVP_PKEY_CTX_new_from_pkey(nullptr, key.get(), nullptr), EVP_PKEY_CTX_free);
    if (!key_context || EVP_PKEY_check(key_context.get()) != 1 ||
        SSL_CTX_use_PrivateKey(context, key.get()) != 1 || SSL_CTX_check_private_key(context) != 1) {
        throw std::runtime_error("Cannot use the supplied TLS private key.");
    }
    return server;
}

} // namespace wave
