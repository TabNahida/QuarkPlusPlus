#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace quarkpp::cli
{
struct LoginOptions
{
    unsigned timeout_seconds{300};
    std::filesystem::path qr_svg_path;
    std::string ca_bundle;
};

// Prints the QR code and progress to stderr. Only success emits JSON to stdout.
// Cancellation returns 130; protocol, expiry and save errors throw.
int run_login(const std::filesystem::path &config_path, const LoginOptions &options = {});

// CLI implementation helpers, exposed here for offline integration tests.
void save_login_cookie(const std::filesystem::path &config_path, std::string_view cookie);
[[nodiscard]] std::string render_qr_terminal(std::string_view url);
[[nodiscard]] std::string render_qr_svg(std::string_view url);
} // namespace quarkpp::cli
