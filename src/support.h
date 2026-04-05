#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace quarkpp {

using json = nlohmann::json;
using HeaderList = std::vector<std::pair<std::string, std::string>>;
using QueryList = std::vector<std::pair<std::string, std::string>>;
using ProgressCallback = std::function<void(std::uint64_t, std::uint64_t)>;

class QuarkException final : public std::runtime_error {
public:
    explicit QuarkException(const std::string& message);
};

struct HttpResponse {
    int status_code {};
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::string url;

    [[nodiscard]] std::string header(std::string_view key) const;
    [[nodiscard]] json json_body() const;
};

struct HashDigests {
    std::string md5_hex;
    std::string sha1_hex;
};

class HttpClient {
public:
    explicit HttpClient(unsigned timeout_ms = 120000);

    [[nodiscard]] HttpResponse request(std::string method,
                                       const std::string& url,
                                       const QueryList& query = {},
                                       const HeaderList& headers = {},
                                       const std::string& body = {}) const;

    [[nodiscard]] HttpResponse upload_file_range(const std::string& url,
                                                 const QueryList& query,
                                                 const HeaderList& headers,
                                                 const std::filesystem::path& file_path,
                                                 std::uint64_t offset,
                                                 std::uint64_t length,
                                                 const ProgressCallback& progress = {}) const;

    [[nodiscard]] HttpResponse download_file(const std::string& url,
                                             const HeaderList& headers,
                                             const std::filesystem::path& output_path,
                                             bool resume,
                                             const ProgressCallback& progress = {}) const;

private:
    unsigned timeout_ms_ {};
};

class ProgressBar {
public:
    explicit ProgressBar(std::string label);
    void update(std::uint64_t current, std::uint64_t total);
    void finish();

private:
    std::string label_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point last_rendered_;
    bool finished_ {false};
};

[[nodiscard]] std::string trim(std::string_view value);
[[nodiscard]] std::string to_lower(std::string value);
[[nodiscard]] std::string url_encode(std::string_view value);
[[nodiscard]] std::string base64_encode(const std::string& value);
[[nodiscard]] std::string md5_base64(const std::string& value);
[[nodiscard]] std::string gmt_http_date_now();
[[nodiscard]] std::string format_bytes(std::uint64_t bytes);
[[nodiscard]] HashDigests compute_file_hashes(const std::filesystem::path& file_path);
[[nodiscard]] std::uint64_t unix_time_milliseconds_now();
[[nodiscard]] std::vector<std::string> split_remote_path(std::string_view remote_path);

}  // namespace quarkpp
