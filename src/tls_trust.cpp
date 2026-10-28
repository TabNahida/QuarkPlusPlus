#include "tls_trust.hpp"

#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <wincrypt.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <openssl/pem.h>

#include <memory>
#endif

namespace quarkpp
{
namespace
{
#ifdef _WIN32
struct CertificateStore
{
    HCERTSTORE value{};
    ~CertificateStore()
    {
        if (value)
            CertCloseStore(value, 0);
    }
};

struct Certificate
{
    PCCERT_CONTEXT value{};
    ~Certificate()
    {
        if (value)
            CertFreeCertificateContext(value);
    }
};

void load_native_roots(pace::tls::context &trust)
{
    bool loaded = false;
    for (const auto *name : {L"ROOT", L"CA"})
    {
        CertificateStore store{CertOpenSystemStoreW(0, name)};
        if (!store.value)
            continue;
        Certificate certificate;
        // CertEnumCertificatesInStore releases the preceding context even when
        // enumeration ends; Certificate owns the current one if loading throws.
        while ((certificate.value = CertEnumCertificatesInStore(store.value, certificate.value)))
        {
            const auto *encoded = certificate.value->pbCertEncoded;
            const auto size = certificate.value->cbCertEncoded;
            DWORD length{};
            if (!CryptBinaryToStringA(encoded, size, CRYPT_STRING_BASE64HEADER, nullptr, &length))
                throw std::runtime_error("Cannot encode a native trusted certificate");
            std::string pem(length, '\0');
            if (!CryptBinaryToStringA(encoded, size, CRYPT_STRING_BASE64HEADER, pem.data(), &length))
                throw std::runtime_error("Cannot encode a native trusted certificate");
            while (!pem.empty() && pem.back() == '\0')
                pem.pop_back();
            trust.load_verify_pem(pem);
            loaded = true;
        }
    }
    if (!loaded)
        throw std::runtime_error("The Windows certificate store contains no trusted certificates");
}
#elif defined(__APPLE__)
struct CfObject
{
    CFTypeRef value{};
    ~CfObject()
    {
        if (value)
            CFRelease(value);
    }
};

void load_native_roots(pace::tls::context &trust)
{
    CFArrayRef anchors{};
    const auto status = SecTrustCopyAnchorCertificates(&anchors);
    CfObject anchor_owner{anchors};
    if (status != errSecSuccess || !anchors)
        throw std::runtime_error("Cannot read macOS trusted certificates");
    bool loaded = false;
    for (CFIndex i = 0; i < CFArrayGetCount(anchors); ++i)
    {
        auto certificate = static_cast<SecCertificateRef>(const_cast<void *>(CFArrayGetValueAtIndex(anchors, i)));
        const auto data = SecCertificateCopyData(certificate);
        CfObject data_owner{data};
        if (!data)
            continue;
        const auto *encoded = CFDataGetBytePtr(data);
        std::unique_ptr<X509, decltype(&X509_free)> parsed(d2i_X509(nullptr, &encoded, CFDataGetLength(data)),
                                                           X509_free);
        if (!parsed)
            throw std::runtime_error("Cannot decode a macOS trusted certificate");
        std::unique_ptr<BIO, decltype(&BIO_free)> buffer(BIO_new(BIO_s_mem()), BIO_free);
        if (!buffer || PEM_write_bio_X509(buffer.get(), parsed.get()) != 1)
            throw std::runtime_error("Cannot encode a macOS trusted certificate");
        char *pem{};
        const auto size = BIO_get_mem_data(buffer.get(), &pem);
        if (size <= 0)
            throw std::runtime_error("Cannot encode a macOS trusted certificate");
        trust.load_verify_pem(std::string_view(pem, static_cast<std::size_t>(size)));
        loaded = true;
    }
    if (!loaded)
        throw std::runtime_error("The macOS certificate store contains no trusted certificates");
}
#endif
} // namespace

const pace::tls::context &default_api_trust()
{
    static const auto trust = []
    {
#if defined(_WIN32) || defined(__APPLE__)
        pace::tls::context configuration(pace::tls::role::client, pace::tls::trust_source::application);
        load_native_roots(configuration);
#else
        pace::tls::context configuration;
#endif
        configuration.set_alpn_protocols({"http/1.1"});
        return configuration;
    }();
    return trust;
}
} // namespace quarkpp
