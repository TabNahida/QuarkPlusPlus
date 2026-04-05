#include "support.h"

#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <ranges>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")

namespace quarkpp {

namespace {

struct WinHttpCloser {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr) {
            WinHttpCloseHandle(static_cast<HINTERNET>(handle));
        }
    }
};

using UniqueHInternet = std::unique_ptr<void, WinHttpCloser>;

struct ParsedUrl {
    std::wstring host;
    std::wstring path_and_query;
    INTERNET_PORT port {};
    bool secure {false};
};

[[nodiscard]] std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const auto size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        throw QuarkException("UTF-8 转宽字符失败");
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

[[nodiscard]] std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const auto size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw QuarkException("宽字符转 UTF-8 失败");
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

[[nodiscard]] std::string last_error_message(const std::string& prefix) {
    const auto error = GetLastError();
    LPSTR buffer = nullptr;
    const auto length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr,
                                       error,
                                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                       reinterpret_cast<LPSTR>(&buffer),
                                       0,
                                       nullptr);

    std::string message = prefix + " (Win32=" + std::to_string(error) + ")";
    if (length > 0 && buffer != nullptr) {
        message += ": ";
        message += trim(buffer);
        LocalFree(buffer);
    }
    return message;
}

[[noreturn]] void throw_last_error(const std::string& prefix) {
    throw QuarkException(last_error_message(prefix));
}

[[nodiscard]] std::string append_query(const std::string& url, const QueryList& query) {
    if (query.empty()) {
        return url;
    }

    std::ostringstream builder;
    builder << url;
    builder << (url.contains('?') ? '&' : '?');

    bool first = true;
    for (const auto& [key, value] : query) {
        if (!first) {
            builder << '&';
        }
        first = false;
        builder << url_encode(key) << '=' << url_encode(value);
    }
    return builder.str();
}

[[nodiscard]] ParsedUrl parse_url(const std::string& url) {
    auto wide_url = utf8_to_wide(url);

    URL_COMPONENTS components {};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    components.dwSchemeLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &components)) {
        throw_last_error("解析 URL 失败");
    }

    ParsedUrl parsed;
    parsed.host.assign(components.lpszHostName, components.dwHostNameLength);
    parsed.port = components.nPort;
    parsed.secure = components.nScheme == INTERNET_SCHEME_HTTPS;

    std::wstring path;
    if (components.dwUrlPathLength > 0) {
        path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    } else {
        path = L"/";
    }
    if (components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    parsed.path_and_query = std::move(path);
    return parsed;
}

[[nodiscard]] UniqueHInternet open_session(unsigned timeout_ms) {
    UniqueHInternet session(WinHttpOpen(L"QuarkPlusPlus/0.1",
                                        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS,
                                        0));
    if (!session) {
        throw_last_error("WinHttpOpen 失败");
    }

    WinHttpSetTimeouts(static_cast<HINTERNET>(session.get()),
                       static_cast<int>(timeout_ms),
                       static_cast<int>(timeout_ms),
                       static_cast<int>(timeout_ms),
                       static_cast<int>(timeout_ms));
    return session;
}

[[nodiscard]] std::wstring build_header_block(const HeaderList& headers) {
    std::wstring block;
    for (const auto& [key, value] : headers) {
        block += utf8_to_wide(key);
        block += L": ";
        block += utf8_to_wide(value);
        block += L"\r\n";
    }
    return block;
}

void add_headers(HINTERNET request, const HeaderList& headers) {
    if (headers.empty()) {
        return;
    }

    const auto header_block = build_header_block(headers);
    if (!WinHttpAddRequestHeaders(request, header_block.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD)) {
        throw_last_error("添加请求头失败");
    }
}

[[nodiscard]] std::unordered_map<std::string, std::string> parse_response_headers(HINTERNET request) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        throw_last_error("读取响应头大小失败");
    }

    std::wstring raw(static_cast<std::size_t>(size / sizeof(wchar_t)), L'\0');
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_RAW_HEADERS_CRLF,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             raw.data(),
                             &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        throw_last_error("读取响应头失败");
    }

    std::unordered_map<std::string, std::string> headers;
    std::stringstream stream(wide_to_utf8(raw));
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line.starts_with("HTTP/")) {
            continue;
        }
        const auto separator = line.find(':');
        if (separator == std::string::npos) {
            continue;
        }
        headers[to_lower(trim(line.substr(0, separator)))] = trim(line.substr(separator + 1));
    }
    return headers;
}

[[nodiscard]] int query_status_code(HINTERNET request) {
    DWORD code = 0;
    DWORD size = sizeof(code);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &code,
                             &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        throw_last_error("读取状态码失败");
    }
    return static_cast<int>(code);
}

[[nodiscard]] std::string read_response_body(HINTERNET request) {
    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            throw_last_error("读取响应数据长度失败");
        }
        if (available == 0) {
            break;
        }

        std::string chunk(static_cast<std::size_t>(available), '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            throw_last_error("读取响应数据失败");
        }
        chunk.resize(read);
        body.append(chunk);
    }
    return body;
}

[[nodiscard]] std::vector<unsigned char> compute_digest(const std::filesystem::path& file_path, const wchar_t* algorithm) {
    BCRYPT_ALG_HANDLE algorithm_handle = nullptr;
    BCRYPT_HASH_HANDLE hash_handle = nullptr;

    auto cleanup = [&]() {
        if (hash_handle != nullptr) {
            BCryptDestroyHash(hash_handle);
        }
        if (algorithm_handle != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_handle, 0);
        }
    };

    if (BCryptOpenAlgorithmProvider(&algorithm_handle, algorithm, nullptr, 0) != 0) {
        cleanup();
        throw QuarkException("初始化哈希算法失败");
    }

    DWORD hash_length = 0;
    DWORD result_size = 0;
    if (BCryptGetProperty(algorithm_handle,
                          BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hash_length),
                          sizeof(hash_length),
                          &result_size,
                          0) != 0) {
        cleanup();
        throw QuarkException("读取哈希长度失败");
    }

    if (BCryptCreateHash(algorithm_handle, &hash_handle, nullptr, 0, nullptr, 0, 0) != 0) {
        cleanup();
        throw QuarkException("创建哈希上下文失败");
    }

    std::ifstream input(file_path, std::ios::binary);
    if (!input) {
        cleanup();
        throw QuarkException("无法打开文件用于哈希计算: " + file_path.string());
    }

    std::vector<char> buffer(8 * 1024 * 1024);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = input.gcount();
        if (read <= 0) {
            break;
        }
        if (BCryptHashData(hash_handle,
                           reinterpret_cast<PUCHAR>(buffer.data()),
                           static_cast<ULONG>(read),
                           0) != 0) {
            cleanup();
            throw QuarkException("写入哈希数据失败");
        }
    }

    std::vector<unsigned char> digest(hash_length);
    if (BCryptFinishHash(hash_handle, digest.data(), hash_length, 0) != 0) {
        cleanup();
        throw QuarkException("完成哈希计算失败");
    }

    cleanup();
    return digest;
}

[[nodiscard]] std::string bytes_to_hex(const std::vector<unsigned char>& digest) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        output << std::setw(2) << static_cast<int>(byte);
    }
    return output.str();
}

[[nodiscard]] std::string base64_from_bytes(const unsigned char* data, DWORD size) {
    DWORD encoded_size = 0;
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &encoded_size)) {
        throw QuarkException("Base64 编码长度计算失败");
    }

    std::string encoded(encoded_size, '\0');
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded.data(), &encoded_size)) {
        throw QuarkException("Base64 编码失败");
    }
    if (!encoded.empty() && encoded.back() == '\0') {
        encoded.pop_back();
    }
    return encoded;
}

[[nodiscard]] std::uint64_t parse_uint64_header(const std::unordered_map<std::string, std::string>& headers,
                                                std::string_view key) {
    const auto it = headers.find(std::string(key));
    if (it == headers.end()) {
        return 0;
    }
    std::uint64_t value = 0;
    std::from_chars(it->second.data(), it->second.data() + it->second.size(), value);
    return value;
}

}  // namespace

QuarkException::QuarkException(const std::string& message) : std::runtime_error(message) {}

std::string HttpResponse::header(std::string_view key) const {
    const auto it = headers.find(to_lower(std::string(key)));
    return it == headers.end() ? std::string() : it->second;
}

json HttpResponse::json_body() const {
    try {
        return json::parse(body);
    } catch (const std::exception& error) {
        throw QuarkException("响应不是合法 JSON: " + std::string(error.what()));
    }
}

HttpClient::HttpClient(unsigned timeout_ms) : timeout_ms_(timeout_ms) {}

HttpResponse HttpClient::request(std::string method,
                                 const std::string& url,
                                 const QueryList& query,
                                 const HeaderList& headers,
                                 const std::string& body) const {
    const auto full_url = append_query(url, query);
    const auto parsed = parse_url(full_url);

    auto session = open_session(timeout_ms_);
    UniqueHInternet connection(WinHttpConnect(static_cast<HINTERNET>(session.get()),
                                              parsed.host.c_str(),
                                              parsed.port,
                                              0));
    if (!connection) {
        throw_last_error("WinHttpConnect 失败");
    }

    const auto wide_method = utf8_to_wide(method);
    UniqueHInternet request(WinHttpOpenRequest(static_cast<HINTERNET>(connection.get()),
                                               wide_method.c_str(),
                                               parsed.path_and_query.c_str(),
                                               nullptr,
                                               WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               parsed.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        throw_last_error("WinHttpOpenRequest 失败");
    }

    add_headers(static_cast<HINTERNET>(request.get()), headers);

    const auto total_length = body.empty() ? 0U : static_cast<DWORD>(body.size());
    auto* body_ptr = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    if (!WinHttpSendRequest(static_cast<HINTERNET>(request.get()),
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            body_ptr,
                            total_length,
                            total_length,
                            0)) {
        throw_last_error("WinHttpSendRequest 失败");
    }

    if (!WinHttpReceiveResponse(static_cast<HINTERNET>(request.get()), nullptr)) {
        throw_last_error("WinHttpReceiveResponse 失败");
    }

    HttpResponse response;
    response.status_code = query_status_code(static_cast<HINTERNET>(request.get()));
    response.headers = parse_response_headers(static_cast<HINTERNET>(request.get()));
    response.body = read_response_body(static_cast<HINTERNET>(request.get()));
    response.url = full_url;
    return response;
}

HttpResponse HttpClient::upload_file_range(const std::string& url,
                                           const QueryList& query,
                                           const HeaderList& headers,
                                           const std::filesystem::path& file_path,
                                           std::uint64_t offset,
                                           std::uint64_t length,
                                           const ProgressCallback& progress) const {
    if (length > static_cast<std::uint64_t>(std::numeric_limits<DWORD>::max())) {
        throw QuarkException("单个上传分片超过 WinHTTP 支持的范围");
    }

    const auto full_url = append_query(url, query);
    const auto parsed = parse_url(full_url);

    auto session = open_session(timeout_ms_);
    UniqueHInternet connection(WinHttpConnect(static_cast<HINTERNET>(session.get()),
                                              parsed.host.c_str(),
                                              parsed.port,
                                              0));
    if (!connection) {
        throw_last_error("WinHttpConnect 失败");
    }

    UniqueHInternet request(WinHttpOpenRequest(static_cast<HINTERNET>(connection.get()),
                                               L"PUT",
                                               parsed.path_and_query.c_str(),
                                               nullptr,
                                               WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               parsed.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        throw_last_error("WinHttpOpenRequest 失败");
    }

    add_headers(static_cast<HINTERNET>(request.get()), headers);

    if (!WinHttpSendRequest(static_cast<HINTERNET>(request.get()),
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            WINHTTP_NO_REQUEST_DATA,
                            0,
                            static_cast<DWORD>(length),
                            0)) {
        throw_last_error("上传请求发送失败");
    }

    std::ifstream input(file_path, std::ios::binary);
    if (!input) {
        throw QuarkException("无法打开上传文件: " + file_path.string());
    }
    input.seekg(static_cast<std::streamoff>(offset));

    std::vector<char> buffer(1024 * 1024);
    std::uint64_t sent = 0;
    while (sent < length) {
        const auto chunk_size = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - sent));
        input.read(buffer.data(), static_cast<std::streamsize>(chunk_size));
        const auto read = static_cast<std::size_t>(input.gcount());
        if (read == 0) {
            throw QuarkException("读取上传分片数据失败");
        }

        DWORD written = 0;
        if (!WinHttpWriteData(static_cast<HINTERNET>(request.get()), buffer.data(), static_cast<DWORD>(read), &written)) {
            throw_last_error("写入上传数据失败");
        }
        sent += written;
        if (progress) {
            progress(sent, length);
        }
    }

    if (!WinHttpReceiveResponse(static_cast<HINTERNET>(request.get()), nullptr)) {
        throw_last_error("上传响应接收失败");
    }

    HttpResponse response;
    response.status_code = query_status_code(static_cast<HINTERNET>(request.get()));
    response.headers = parse_response_headers(static_cast<HINTERNET>(request.get()));
    response.body = read_response_body(static_cast<HINTERNET>(request.get()));
    response.url = full_url;
    return response;
}

HttpResponse HttpClient::download_file(const std::string& url,
                                       const HeaderList& headers,
                                       const std::filesystem::path& output_path,
                                       bool resume,
                                       const ProgressCallback& progress) const {
    auto request_headers = headers;
    std::uint64_t resume_from = 0;
    if (resume && std::filesystem::exists(output_path) && std::filesystem::is_regular_file(output_path)) {
        resume_from = std::filesystem::file_size(output_path);
        if (resume_from > 0) {
            request_headers.emplace_back("Range", "bytes=" + std::to_string(resume_from) + "-");
        }
    }

    const auto parsed = parse_url(url);
    auto session = open_session(timeout_ms_);
    UniqueHInternet connection(WinHttpConnect(static_cast<HINTERNET>(session.get()),
                                              parsed.host.c_str(),
                                              parsed.port,
                                              0));
    if (!connection) {
        throw_last_error("WinHttpConnect 失败");
    }

    UniqueHInternet request(WinHttpOpenRequest(static_cast<HINTERNET>(connection.get()),
                                               L"GET",
                                               parsed.path_and_query.c_str(),
                                               nullptr,
                                               WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               parsed.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        throw_last_error("WinHttpOpenRequest 失败");
    }

    add_headers(static_cast<HINTERNET>(request.get()), request_headers);

    if (!WinHttpSendRequest(static_cast<HINTERNET>(request.get()),
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            WINHTTP_NO_REQUEST_DATA,
                            0,
                            0,
                            0)) {
        throw_last_error("下载请求发送失败");
    }

    if (!WinHttpReceiveResponse(static_cast<HINTERNET>(request.get()), nullptr)) {
        throw_last_error("下载响应接收失败");
    }

    HttpResponse response;
    response.status_code = query_status_code(static_cast<HINTERNET>(request.get()));
    response.headers = parse_response_headers(static_cast<HINTERNET>(request.get()));
    response.url = url;

    if (response.status_code == 416) {
        return response;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        response.body = read_response_body(static_cast<HINTERNET>(request.get()));
        return response;
    }

    std::filesystem::create_directories(output_path.parent_path());
    const bool append = resume_from > 0 && response.status_code == 206;
    if (!append) {
        resume_from = 0;
    }

    std::ofstream output(output_path,
                         std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!output) {
        throw QuarkException("无法打开下载输出文件: " + output_path.string());
    }

    const auto content_length = parse_uint64_header(response.headers, "content-length");
    const auto total = content_length == 0 ? 0 : content_length + resume_from;
    std::uint64_t received = resume_from;

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(static_cast<HINTERNET>(request.get()), &available)) {
            throw_last_error("查询下载数据长度失败");
        }
        if (available == 0) {
            break;
        }

        std::string chunk(static_cast<std::size_t>(available), '\0');
        DWORD read = 0;
        if (!WinHttpReadData(static_cast<HINTERNET>(request.get()), chunk.data(), available, &read)) {
            throw_last_error("读取下载数据失败");
        }
        output.write(chunk.data(), static_cast<std::streamsize>(read));
        received += read;
        if (progress) {
            progress(received, total);
        }
    }

    return response;
}

ProgressBar::ProgressBar(std::string label)
    : label_(std::move(label)),
      started_(std::chrono::steady_clock::now()),
      last_rendered_(started_) {}

void ProgressBar::update(std::uint64_t current, std::uint64_t total) {
    if (finished_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (current < total && now - last_rendered_ < std::chrono::milliseconds(100)) {
        return;
    }
    last_rendered_ = now;

    const auto elapsed = std::chrono::duration<double>(now - started_).count();
    const auto speed = elapsed <= 0.0 ? 0.0 : static_cast<double>(current) / elapsed;

    std::ostringstream line;
    line << '\r' << label_ << " | " << format_bytes(current);
    if (total > 0) {
        const auto percent = std::clamp((static_cast<double>(current) * 100.0) / static_cast<double>(total), 0.0, 100.0);
        line << " / " << format_bytes(total) << " | "
             << std::fixed << std::setprecision(1) << percent << "%";
    }
    line << " | " << format_bytes(static_cast<std::uint64_t>(speed)) << "/s";
    std::cout << line.str() << std::flush;

    if (total > 0 && current >= total) {
        finish();
    }
}

void ProgressBar::finish() {
    if (finished_) {
        return;
    }
    finished_ = true;
    std::cout << '\n';
}

std::string trim(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::string to_lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string url_encode(std::string_view value) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const auto byte : value) {
        const unsigned char current = static_cast<unsigned char>(byte);
        if (std::isalnum(current) != 0 || current == '-' || current == '_' || current == '.' || current == '~') {
            encoded << static_cast<char>(current);
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(current);
        }
    }
    return encoded.str();
}

std::string base64_encode(const std::string& value) {
    return base64_from_bytes(reinterpret_cast<const unsigned char*>(value.data()), static_cast<DWORD>(value.size()));
}

std::string md5_base64(const std::string& value) {
    BCRYPT_ALG_HANDLE algorithm_handle = nullptr;
    BCRYPT_HASH_HANDLE hash_handle = nullptr;

    auto cleanup = [&]() {
        if (hash_handle != nullptr) {
            BCryptDestroyHash(hash_handle);
        }
        if (algorithm_handle != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_handle, 0);
        }
    };

    if (BCryptOpenAlgorithmProvider(&algorithm_handle, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0) {
        cleanup();
        throw QuarkException("初始化 MD5 算法失败");
    }

    DWORD hash_length = 0;
    DWORD result_size = 0;
    if (BCryptGetProperty(algorithm_handle,
                          BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hash_length),
                          sizeof(hash_length),
                          &result_size,
                          0) != 0) {
        cleanup();
        throw QuarkException("读取 MD5 长度失败");
    }

    if (BCryptCreateHash(algorithm_handle, &hash_handle, nullptr, 0, nullptr, 0, 0) != 0) {
        cleanup();
        throw QuarkException("创建 MD5 上下文失败");
    }

    if (BCryptHashData(hash_handle,
                       reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
                       static_cast<ULONG>(value.size()),
                       0) != 0) {
        cleanup();
        throw QuarkException("写入 MD5 数据失败");
    }

    std::vector<unsigned char> digest(hash_length);
    if (BCryptFinishHash(hash_handle, digest.data(), hash_length, 0) != 0) {
        cleanup();
        throw QuarkException("完成 MD5 失败");
    }

    cleanup();
    return base64_from_bytes(digest.data(), hash_length);
}

std::string gmt_http_date_now() {
    SYSTEMTIME utc {};
    GetSystemTime(&utc);

    std::tm time {};
    time.tm_year = utc.wYear - 1900;
    time.tm_mon = utc.wMonth - 1;
    time.tm_mday = utc.wDay;
    time.tm_hour = utc.wHour;
    time.tm_min = utc.wMinute;
    time.tm_sec = utc.wSecond;

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::put_time(&time, "%a, %d %b %Y %H:%M:%S GMT");
    return output.str();
}

std::string format_bytes(std::uint64_t bytes) {
    constexpr std::array<std::string_view, 5> units {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }

    std::ostringstream output;
    output << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << units[unit];
    return output.str();
}

HashDigests compute_file_hashes(const std::filesystem::path& file_path) {
    HashDigests digests;
    digests.md5_hex = bytes_to_hex(compute_digest(file_path, BCRYPT_MD5_ALGORITHM));
    digests.sha1_hex = bytes_to_hex(compute_digest(file_path, BCRYPT_SHA1_ALGORITHM));
    return digests;
}

std::uint64_t unix_time_milliseconds_now() {
    const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return static_cast<std::uint64_t>(now.time_since_epoch().count());
}

std::vector<std::string> split_remote_path(std::string_view remote_path) {
    std::string normalized(remote_path);
    std::ranges::replace(normalized, '\\', '/');
    if (!normalized.empty() && normalized.front() == '/') {
        normalized.erase(normalized.begin());
    }
    if (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }

    std::vector<std::string> segments;
    std::stringstream stream(normalized);
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        if (!segment.empty()) {
            segments.push_back(segment);
        }
    }
    return segments;
}

}  // namespace quarkpp
