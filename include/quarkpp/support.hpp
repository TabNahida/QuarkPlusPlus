#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace quarkpp
{
using json = nlohmann::json;
using HeaderList = std::vector<std::pair<std::string, std::string>>;
using QueryList = std::vector<std::pair<std::string, std::string>>;
using ProgressCallback = std::function<void(std::uint64_t, std::uint64_t)>;

class QuarkException final : public std::runtime_error
{
  public:
    explicit QuarkException(const std::string &message);
};

struct HttpResponse
{
    int status_code{};
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    // Set-Cookie is not comma-combinable; retain every response field.
    std::vector<std::string> set_cookies;
    std::string url;
    [[nodiscard]] std::string header(std::string_view key) const;
    [[nodiscard]] json json_body() const;
};

struct HashDigests
{
    std::string md5_hex;
    std::string sha1_hex;
};

// Each operation owns its connection. API calls use a complete-request deadline;
// streamed file transfers use connect and individual socket read/write timeouts.
class HttpClient
{
  public:
    explicit HttpClient(unsigned timeout_ms = 120000, std::string ca_bundle = {});
    [[nodiscard]] HttpResponse request(std::string method, const std::string &url, const QueryList &query = {},
                                       const HeaderList &headers = {}, const std::string &body = {},
                                       bool follow_redirects = true) const;
    // Receivers run synchronously, after the final response's headers. Returning
    // false aborts the connection and throws. Bodies are never buffered here.
    [[nodiscard]] HttpResponse request_stream(const std::string &url, const HeaderList &headers,
                                              const std::function<bool(const HttpResponse &)> &on_response,
                                              const std::function<bool(const char *, std::size_t)> &on_chunk) const;
    [[nodiscard]] HttpResponse upload_file_range(const std::string &url, const QueryList &query,
                                                 const HeaderList &headers, const std::filesystem::path &file_path,
                                                 std::uint64_t offset, std::uint64_t length,
                                                 const ProgressCallback &progress = {}) const;
    // Resume appends only after a matching Content-Range has been validated.
    // On transport failure, received bytes remain available for the next retry.
    [[nodiscard]] HttpResponse download_file(const std::string &url, const HeaderList &headers,
                                             const std::filesystem::path &output_path, bool resume,
                                             const ProgressCallback &progress = {},
                                             std::optional<std::uint64_t> expected_size = std::nullopt,
                                             unsigned threads = 1) const;

  private:
    unsigned timeout_ms_;
    std::string ca_bundle_;
};

[[nodiscard]] std::string trim(std::string_view value);
[[nodiscard]] std::string to_lower(std::string value);
[[nodiscard]] std::string url_encode(std::string_view value);
[[nodiscard]] std::string base64_encode(const std::string &value);
[[nodiscard]] std::string md5_base64(const std::string &value);
[[nodiscard]] std::string gmt_http_date_now();
[[nodiscard]] std::string format_bytes(std::uint64_t bytes);
[[nodiscard]] HashDigests compute_file_hashes(const std::filesystem::path &file_path);
[[nodiscard]] std::uint64_t unix_time_milliseconds_now();
[[nodiscard]] std::vector<std::string> split_remote_path(std::string_view remote_path);
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view value);
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path &value);
} // namespace quarkpp
