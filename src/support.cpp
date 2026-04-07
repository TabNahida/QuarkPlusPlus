#include "support.h"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <ranges>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace quarkpp
{

namespace
{

struct CurlGlobalInit
{
    CurlGlobalInit()
    {
        const auto code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK)
        {
            throw QuarkException(std::string("curl_global_init 失败: ") + curl_easy_strerror(code));
        }
    }

    ~CurlGlobalInit()
    {
        curl_global_cleanup();
    }
};

struct CurlDeleter
{
    void operator()(CURL *handle) const noexcept
    {
        if (handle != nullptr)
        {
            curl_easy_cleanup(handle);
        }
    }
};

struct CurlSlistDeleter
{
    void operator()(curl_slist *list) const noexcept
    {
        if (list != nullptr)
        {
            curl_slist_free_all(list);
        }
    }
};

using UniqueCurl = std::unique_ptr<CURL, CurlDeleter>;
using UniqueCurlSlist = std::unique_ptr<curl_slist, CurlSlistDeleter>;

struct UploadState
{
    std::ifstream file;
    std::uint64_t remaining{};
    std::uint64_t transferred{};
    ProgressCallback progress;
};

struct DownloadState
{
    std::ofstream *stream{nullptr};
    std::uint64_t base_offset{};
    ProgressCallback progress;
};

[[nodiscard]] CurlGlobalInit &curl_global_state()
{
    static CurlGlobalInit instance;
    return instance;
}

[[nodiscard]] std::string append_query(const std::string &url, const QueryList &query)
{
    if (query.empty())
    {
        return url;
    }

    std::ostringstream builder;
    builder << url;
    builder << (url.contains('?') ? '&' : '?');

    bool first = true;
    for (const auto &[key, value] : query)
    {
        if (!first)
        {
            builder << '&';
        }
        first = false;
        builder << url_encode(key) << '=' << url_encode(value);
    }
    return builder.str();
}

[[nodiscard]] UniqueCurl make_curl()
{
    static_cast<void>(curl_global_state());
    UniqueCurl handle(curl_easy_init());
    if (!handle)
    {
        throw QuarkException("curl_easy_init 失败");
    }
    return handle;
}

[[nodiscard]] UniqueCurlSlist make_headers(const HeaderList &headers)
{
    curl_slist *list = nullptr;
    for (const auto &[key, value] : headers)
    {
        const auto header = key + ": " + value;
        list = curl_slist_append(list, header.c_str());
        if (list == nullptr)
        {
            throw QuarkException("构建请求头失败");
        }
    }
    return UniqueCurlSlist(list);
}

void set_common_curl_options(CURL *curl, unsigned timeout_ms, HttpResponse &response)
{
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION,
        +[](char *ptr, std::size_t size, std::size_t nmemb, void *userdata) -> std::size_t {
            auto *body = static_cast<std::string *>(userdata);
            body->append(ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(
        curl, CURLOPT_HEADERFUNCTION,
        +[](char *buffer, std::size_t size, std::size_t nitems, void *userdata) -> std::size_t {
            const std::size_t total = size * nitems;
            auto *response_ptr = static_cast<HttpResponse *>(userdata);
            std::string_view line(buffer, total);
            const auto separator = line.find(':');
            if (separator != std::string_view::npos)
            {
                auto key = to_lower(trim(line.substr(0, separator)));
                auto value = trim(line.substr(separator + 1));
                if (!key.empty())
                {
                    response_ptr->headers[key] = value;
                }
            }
            return total;
        });
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);
}

void finalize_response(CURL *curl, HttpResponse &response)
{
    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    response.status_code = static_cast<int>(status_code);

    char *effective_url = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    if (effective_url != nullptr)
    {
        response.url = effective_url;
    }
}

void perform_checked(CURL *curl, const std::string &action, HttpResponse &response)
{
    char error_buffer[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);

    const auto code = curl_easy_perform(curl);
    if (code != CURLE_OK)
    {
        std::string message = action + "失败: " + curl_easy_strerror(code);
        if (error_buffer[0] != '\0')
        {
            message += " | ";
            message += error_buffer;
        }
        throw QuarkException(message);
    }

    finalize_response(curl, response);
}

[[nodiscard]] std::string digest_to_hex(const std::filesystem::path &file_path, const EVP_MD *algorithm)
{
    std::ifstream input(file_path, std::ios::binary);
    if (!input)
    {
        throw QuarkException("无法打开文件用于哈希计算: " + path_to_utf8(file_path));
    }

    using EvpCtx = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    EvpCtx ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!ctx)
    {
        throw QuarkException("创建 OpenSSL 哈希上下文失败");
    }
    if (EVP_DigestInit_ex(ctx.get(), algorithm, nullptr) != 1)
    {
        throw QuarkException("初始化 OpenSSL 哈希失败");
    }

    std::vector<char> buffer(8 * 1024 * 1024);
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = input.gcount();
        if (read <= 0)
        {
            break;
        }
        if (EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<std::size_t>(read)) != 1)
        {
            throw QuarkException("更新 OpenSSL 哈希失败");
        }
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned digest_length = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &digest_length) != 1)
    {
        throw QuarkException("完成 OpenSSL 哈希失败");
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned index = 0; index < digest_length; ++index)
    {
        output << std::setw(2) << static_cast<int>(digest[index]);
    }
    return output.str();
}

[[nodiscard]] std::string base64_from_bytes(const unsigned char *data, std::size_t size)
{
    const auto encoded_size = 4 * ((size + 2) / 3);
    std::string encoded(encoded_size, '\0');
    const auto written =
        EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()), data, static_cast<int>(size));
    if (written < 0)
    {
        throw QuarkException("OpenSSL Base64 编码失败");
    }
    encoded.resize(static_cast<std::size_t>(written));
    return encoded;
}

std::size_t read_upload_callback(char *buffer, std::size_t size, std::size_t nmemb, void *userdata)
{
    auto *state = static_cast<UploadState *>(userdata);
    if (state->remaining == 0)
    {
        return 0;
    }

    const auto capacity = static_cast<std::uint64_t>(size * nmemb);
    const auto to_read = static_cast<std::size_t>(std::min<std::uint64_t>(state->remaining, capacity));
    state->file.read(buffer, static_cast<std::streamsize>(to_read));
    const auto read = static_cast<std::size_t>(state->file.gcount());
    state->remaining -= read;
    state->transferred += read;
    return read;
}

int xfer_info_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    auto *state = static_cast<UploadState *>(clientp);
    if (state != nullptr && state->progress)
    {
        const auto total = ultotal > 0 ? static_cast<std::uint64_t>(ultotal) : state->transferred;
        state->progress(static_cast<std::uint64_t>(ulnow), total);
    }
    return 0;
}

std::size_t write_download_callback(char *ptr, std::size_t size, std::size_t nmemb, void *userdata)
{
    auto *state = static_cast<DownloadState *>(userdata);
    const auto bytes = size * nmemb;
    state->stream->write(ptr, static_cast<std::streamsize>(bytes));
    return state->stream->good() ? bytes : 0;
}

int download_progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t)
{
    auto *state = static_cast<DownloadState *>(clientp);
    if (state != nullptr && state->progress)
    {
        const auto current = state->base_offset + static_cast<std::uint64_t>(std::max<curl_off_t>(0, dlnow));
        const auto total = dltotal > 0 ? state->base_offset + static_cast<std::uint64_t>(dltotal) : 0;
        state->progress(current, total);
    }
    return 0;
}

#if defined(_WIN32)
[[nodiscard]] std::string wide_to_utf8(std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }

    const auto size =
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        throw QuarkException("宽字符转 UTF-8 失败");
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr,
                        nullptr);
    return result;
}

[[nodiscard]] std::wstring utf8_to_wide(std::string_view value)
{
    if (value.empty())
    {
        return {};
    }

    const auto size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0)
    {
        throw QuarkException("UTF-8 转宽字符失败");
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
#endif

} // namespace

QuarkException::QuarkException(const std::string &message) : std::runtime_error(message)
{
}

std::filesystem::path path_from_utf8(std::string_view value)
{
#if defined(_WIN32)
    return std::filesystem::path(utf8_to_wide(value));
#else
    return std::filesystem::path(std::string(value));
#endif
}

std::string path_to_utf8(const std::filesystem::path &value)
{
#if defined(_WIN32)
    return wide_to_utf8(value.native());
#else
    return value.string();
#endif
}

std::string HttpResponse::header(std::string_view key) const
{
    const auto it = headers.find(to_lower(std::string(key)));
    return it == headers.end() ? std::string() : it->second;
}

json HttpResponse::json_body() const
{
    try
    {
        return json::parse(body);
    }
    catch (const std::exception &error)
    {
        throw QuarkException("响应不是合法 JSON: " + std::string(error.what()));
    }
}

HttpClient::HttpClient(unsigned timeout_ms) : timeout_ms_(timeout_ms)
{
    static_cast<void>(curl_global_state());
}

HttpResponse HttpClient::request(std::string method, const std::string &url, const QueryList &query,
                                 const HeaderList &headers, const std::string &body) const
{
    HttpResponse response;
    const auto full_url = append_query(url, query);
    auto curl = make_curl();
    auto header_list = make_headers(headers);

    curl_easy_setopt(curl.get(), CURLOPT_URL, full_url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, header_list.get());
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
    set_common_curl_options(curl.get(), timeout_ms_, response);

    if (!body.empty())
    {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    else if (method == "POST")
    {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(0));
    }

    perform_checked(curl.get(), "HTTP 请求", response);
    return response;
}

HttpResponse HttpClient::upload_file_range(const std::string &url, const QueryList &query, const HeaderList &headers,
                                           const std::filesystem::path &file_path, std::uint64_t offset,
                                           std::uint64_t length, const ProgressCallback &progress) const
{
    UploadState state;
    state.file.open(file_path, std::ios::binary);
    if (!state.file)
    {
        throw QuarkException("无法打开上传文件: " + path_to_utf8(file_path));
    }
    state.file.seekg(static_cast<std::streamoff>(offset));
    state.remaining = length;
    state.transferred = 0;
    state.progress = progress;

    HttpResponse response;
    const auto full_url = append_query(url, query);
    auto curl = make_curl();
    auto header_list = make_headers(headers);

    curl_easy_setopt(curl.get(), CURLOPT_URL, full_url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, header_list.get());
    curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(length));
    curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, &read_upload_callback);
    curl_easy_setopt(curl.get(), CURLOPT_READDATA, &state);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, &xfer_info_callback);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    set_common_curl_options(curl.get(), timeout_ms_, response);

    perform_checked(curl.get(), "分片上传", response);
    if (progress)
    {
        progress(length, length);
    }
    return response;
}

HttpResponse HttpClient::download_file(const std::string &url, const HeaderList &headers,
                                       const std::filesystem::path &output_path, bool resume,
                                       const ProgressCallback &progress) const
{
    std::filesystem::create_directories(output_path.parent_path());

    std::uint64_t base_offset = 0;
    if (resume && std::filesystem::exists(output_path) && std::filesystem::is_regular_file(output_path))
    {
        base_offset = std::filesystem::file_size(output_path);
    }

    std::ofstream file(output_path, std::ios::binary | (base_offset > 0 ? std::ios::app : std::ios::trunc));
    if (!file)
    {
        throw QuarkException("无法打开下载输出文件: " + path_to_utf8(output_path));
    }

    DownloadState state{
        .stream = &file,
        .base_offset = base_offset,
        .progress = progress,
    };

    auto request_headers = headers;
    if (base_offset > 0)
    {
        request_headers.emplace_back("Range", "bytes=" + std::to_string(base_offset) + "-");
    }

    HttpResponse response;
    auto curl = make_curl();
    auto header_list = make_headers(request_headers);

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, header_list.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &write_download_callback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(
        curl.get(), CURLOPT_HEADERFUNCTION,
        +[](char *buffer, std::size_t size, std::size_t nitems, void *userdata) -> std::size_t {
            const std::size_t total = size * nitems;
            auto *response_ptr = static_cast<HttpResponse *>(userdata);
            std::string_view line(buffer, total);
            const auto separator = line.find(':');
            if (separator != std::string_view::npos)
            {
                auto key = to_lower(trim(line.substr(0, separator)));
                auto value = trim(line.substr(separator + 1));
                if (!key.empty())
                {
                    response_ptr->headers[key] = value;
                }
            }
            return total;
        });
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, &download_progress_callback);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms_));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms_));
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");

    perform_checked(curl.get(), "文件下载", response);
    return response;
}

ProgressBar::ProgressBar(std::string label)
    : label_(std::move(label)), started_(std::chrono::steady_clock::now()), last_rendered_(started_)
{
}

void ProgressBar::update(std::uint64_t current, std::uint64_t total)
{
    if (finished_)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (current < total && now - last_rendered_ < std::chrono::milliseconds(100))
    {
        return;
    }
    last_rendered_ = now;

    const auto elapsed = std::chrono::duration<double>(now - started_).count();
    const auto speed = elapsed <= 0.0 ? 0.0 : static_cast<double>(current) / elapsed;

    std::ostringstream line;
    line << '\r' << label_ << " | " << format_bytes(current);
    if (total > 0)
    {
        const auto percent =
            std::clamp((static_cast<double>(current) * 100.0) / static_cast<double>(total), 0.0, 100.0);
        line << " / " << format_bytes(total) << " | " << std::fixed << std::setprecision(1) << percent << "%";
    }
    line << " | " << format_bytes(static_cast<std::uint64_t>(speed)) << "/s";
    std::cout << line.str() << std::flush;

    if (total > 0 && current >= total)
    {
        finish();
    }
}

void ProgressBar::finish()
{
    if (finished_)
    {
        return;
    }
    finished_ = true;
    std::cout << '\n';
}

std::string trim(std::string_view value)
{
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0)
    {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
    {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::string to_lower(std::string value)
{
    std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string url_encode(std::string_view value)
{
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const auto byte : value)
    {
        const unsigned char current = static_cast<unsigned char>(byte);
        if (std::isalnum(current) != 0 || current == '-' || current == '_' || current == '.' || current == '~')
        {
            encoded << static_cast<char>(current);
        }
        else
        {
            encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(current);
        }
    }
    return encoded.str();
}

std::string base64_encode(const std::string &value)
{
    return base64_from_bytes(reinterpret_cast<const unsigned char *>(value.data()), value.size());
}

std::string md5_base64(const std::string &value)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned digest_length = 0;
    if (EVP_Digest(value.data(), value.size(), digest.data(), &digest_length, EVP_md5(), nullptr) != 1)
    {
        throw QuarkException("OpenSSL MD5 失败");
    }
    return base64_from_bytes(digest.data(), digest_length);
}

std::string gmt_http_date_now()
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::put_time(&utc, "%a, %d %b %Y %H:%M:%S GMT");
    return output.str();
}

std::string format_bytes(std::uint64_t bytes)
{
    constexpr std::array<std::string_view, 5> units{"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size())
    {
        value /= 1024.0;
        ++unit;
    }

    std::ostringstream output;
    output << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << units[unit];
    return output.str();
}

HashDigests compute_file_hashes(const std::filesystem::path &file_path)
{
    return HashDigests{
        .md5_hex = digest_to_hex(file_path, EVP_md5()),
        .sha1_hex = digest_to_hex(file_path, EVP_sha1()),
    };
}

std::uint64_t unix_time_milliseconds_now()
{
    const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return static_cast<std::uint64_t>(now.time_since_epoch().count());
}

std::vector<std::string> split_remote_path(std::string_view remote_path)
{
    std::string normalized(remote_path);
    std::ranges::replace(normalized, '\\', '/');
    if (!normalized.empty() && normalized.front() == '/')
    {
        normalized.erase(normalized.begin());
    }
    if (!normalized.empty() && normalized.back() == '/')
    {
        normalized.pop_back();
    }

    std::vector<std::string> segments;
    std::stringstream stream(normalized);
    std::string segment;
    while (std::getline(stream, segment, '/'))
    {
        if (!segment.empty())
        {
            segments.push_back(segment);
        }
    }
    return segments;
}

} // namespace quarkpp
