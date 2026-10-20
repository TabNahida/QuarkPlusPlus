#pragma once

#include "quarkpp/support.hpp"

#include <chrono>
#include <memory>

namespace quarkpp
{
struct QrLoginOptions
{
    unsigned request_timeout_ms{15000};
    unsigned login_timeout_seconds{300};
    std::string ca_bundle;
    // Override for a compatible service or loopback integration tests.
    std::string cas_base{"https://uop.quark.cn"};
    std::string pan_base{"https://pan.quark.cn"};
    std::string drive_api_base{"https://drive-pc.quark.cn/1/clouddrive"};
};

struct QrLoginCode
{
    // Encode this official Quark URL locally. Never send it to a QR image service.
    std::string url;
    unsigned expires_in_seconds{};
};

enum class QrLoginStatus
{
    waiting,
    authenticated,
    expired,
    cancelled
};

struct QrLoginResult
{
    QrLoginStatus status{QrLoginStatus::waiting};
    // Populated only after ticket exchange and drive authentication succeed.
    // Treat this value as a credential: save privately, never log it.
    std::string cookie;
};

// A synchronous, single-session login client. start() replaces any previous
// session; poll() performs one status check without sleeping. No method prints.
// Call poll() about every two seconds until it returns a terminal state.
// Network/protocol errors throw QuarkException with credential-free messages.
class QrLoginClient
{
  public:
    explicit QrLoginClient(QrLoginOptions options = {});
    ~QrLoginClient();
    QrLoginClient(QrLoginClient &&) noexcept;
    QrLoginClient &operator=(QrLoginClient &&) noexcept;
    QrLoginClient(const QrLoginClient &) = delete;
    QrLoginClient &operator=(const QrLoginClient &) = delete;
    [[nodiscard]] QrLoginCode start();
    [[nodiscard]] QrLoginResult poll();
    void cancel();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace quarkpp
