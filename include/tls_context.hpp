#pragma once
#include <boost/asio/ssl.hpp>
#include <wincrypt.h>       // Windows CryptoAPI — HCERTSTORE, CertOpenSystemStoreA, etc.
#include <string>
#include <stdexcept>

// Link crypt32.lib automatically under MSVC.
// Under CMake the target_link_libraries() call in CMakeLists.txt handles this;
// the pragma is a belt-and-suspenders fallback for direct MSVC /P builds.
#pragma comment(lib, "crypt32.lib")

namespace v2 {

namespace ssl = boost::asio::ssl;

/// Creates a properly hardened TLS context for connecting to Binance.
///
/// Security properties enforced:
///   - Minimum TLS 1.2 (TLS 1.0 and 1.1 explicitly disabled)
///   - Full certificate chain verification (replaces V1 verify_none)
///   - Trusted CA certificates loaded from the Windows ROOT certificate store
///     (the same store trusted by Chrome/Edge — no bundled ca-bundle.pem needed)
///   - Hostname verification via RFC 2818 (prevents certificate substitution attacks)
///
/// Replaces streamer.cpp on_tls_init() where verify_none was set.
inline ssl::context make_tls_context() {
    ssl::context ctx(ssl::context::tlsv12_client);

    // ── Protocol Hardening ────────────────────────────────────────────────────
    // Binance requires TLS 1.2+. Disable all older protocol versions.
    ctx.set_options(
        ssl::context::default_workarounds |
        ssl::context::no_sslv2            |
        ssl::context::no_sslv3            |
        ssl::context::no_tlsv1            |   // Disable TLS 1.0
        ssl::context::no_tlsv1_1          |   // Disable TLS 1.1
        ssl::context::single_dh_use
    );

    // ── Certificate Verification Mode ─────────────────────────────────────────
    // verify_peer              : require a valid certificate chain from the server
    // verify_fail_if_no_peer_cert: reject if the server does not present a cert
    ctx.set_verify_mode(ssl::verify_peer | ssl::verify_fail_if_no_peer_cert);

    // ── Load Windows Certificate Store ────────────────────────────────────────
    // Open the Windows system "ROOT" store (trusted root CAs).
    // This is identical to the set of CAs trusted by Chrome/Edge/IE — no
    // separate ca-bundle.pem file is needed or desired on Windows.
    HCERTSTORE h_store = CertOpenSystemStoreA(0, "ROOT");
    if (!h_store) {
        throw std::runtime_error(
            "Failed to open Windows ROOT certificate store. "
            "Error: " + std::to_string(GetLastError())
        );
    }

    // Get the underlying OpenSSL X509_STORE from the Boost.Asio SSL context.
    X509_STORE* x509_store = SSL_CTX_get_cert_store(ctx.native_handle());

    // Iterate through all certificates in the Windows store and import them
    // into the OpenSSL trust store.
    PCCERT_CONTEXT p_cert_ctx = nullptr;
    int certs_loaded = 0;
    while ((p_cert_ctx = CertEnumCertificatesInStore(h_store, p_cert_ctx)) != nullptr) {
        // Windows stores certificates in DER (binary) format.
        // d2i_X509 converts DER → OpenSSL X509 object.
        const unsigned char* cert_data = p_cert_ctx->pbCertEncoded;
        X509* x509 = d2i_X509(nullptr, &cert_data, p_cert_ctx->cbCertEncoded);
        if (x509) {
            X509_STORE_add_cert(x509_store, x509);
            X509_free(x509);
            ++certs_loaded;
        }
    }
    CertCloseStore(h_store, 0);

    if (certs_loaded == 0) {
        throw std::runtime_error(
            "No certificates loaded from the Windows ROOT store. "
            "TLS connections will fail."
        );
    }

    // ── Hostname Verification ─────────────────────────────────────────────────
    // Without this, a valid certificate for example.com would pass verification
    // when connecting to stream.binance.com — a classic cert-substitution attack.
    // rfc2818_verification checks the CN and all SAN entries against the hostname.
    ctx.set_verify_callback(
        ssl::rfc2818_verification("stream.binance.com")
    );

    return ctx;
}

/// Creates a TLS context configured for a specific hostname.
/// Use this overload when connecting to the Binance testnet or other endpoints.
///
/// Example:
///   auto ctx = v2::make_tls_context("testnet.binance.vision");
inline ssl::context make_tls_context(const std::string& hostname) {
    // Re-use the full hardened setup, then override the hostname callback.
    // Note: the default overload sets "stream.binance.com"; we replace it here.
    ssl::context ctx(ssl::context::tlsv12_client);

    ctx.set_options(
        ssl::context::default_workarounds |
        ssl::context::no_sslv2            |
        ssl::context::no_sslv3            |
        ssl::context::no_tlsv1            |
        ssl::context::no_tlsv1_1          |
        ssl::context::single_dh_use
    );
    ctx.set_verify_mode(ssl::verify_peer | ssl::verify_fail_if_no_peer_cert);

    HCERTSTORE h_store = CertOpenSystemStoreA(0, "ROOT");
    if (!h_store) {
        throw std::runtime_error(
            "Failed to open Windows ROOT certificate store. "
            "Error: " + std::to_string(GetLastError())
        );
    }

    X509_STORE* x509_store = SSL_CTX_get_cert_store(ctx.native_handle());

    PCCERT_CONTEXT p_cert_ctx = nullptr;
    int certs_loaded = 0;
    while ((p_cert_ctx = CertEnumCertificatesInStore(h_store, p_cert_ctx)) != nullptr) {
        const unsigned char* cert_data = p_cert_ctx->pbCertEncoded;
        X509* x509 = d2i_X509(nullptr, &cert_data, p_cert_ctx->cbCertEncoded);
        if (x509) {
            X509_STORE_add_cert(x509_store, x509);
            X509_free(x509);
            ++certs_loaded;
        }
    }
    CertCloseStore(h_store, 0);

    if (certs_loaded == 0) {
        throw std::runtime_error(
            "No certificates loaded from the Windows ROOT store. "
            "TLS connections will fail."
        );
    }

    ctx.set_verify_callback(ssl::rfc2818_verification(hostname));
    return ctx;
}

} // namespace v2
