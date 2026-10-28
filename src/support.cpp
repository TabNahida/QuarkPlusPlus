#include "thread_group.hpp"
#include "tls_trust.hpp"
#include <httplib.h>
#include <openssl/evp.h>
#include <pace/http.hpp>
#include <quarkpp/support.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>

namespace quarkpp
{
namespace
{
constexpr std::size_t max_api_body = 16 * 1024 * 1024;
constexpr std::size_t transfer_buffer_size = 256 * 1024;
constexpr unsigned max_redirects = 5;
static_assert(sizeof(std::size_t) >= 8, "QuarkPlusPlus requires a 64-bit build for large files");
static_assert(sizeof(std::streamoff) >= 8, "64-bit file offsets are required");

struct Url
{
    std::string origin, target;
    bool tls{};
};

Url parse_url(const std::string &url)
{
    if (url.find_first_of("\r\n\t ") != std::string::npos)
        throw QuarkException("HTTP URL contains whitespace");
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        throw QuarkException("An absolute HTTP(S) URL is required");
    const auto scheme = to_lower(url.substr(0, scheme_end));
    if (scheme != "http" && scheme != "https")
        throw QuarkException("Only HTTP and HTTPS URLs are supported");
    const auto authority_start = scheme_end + 3;
    const auto authority_end = url.find_first_of("/?#", authority_start);
    const auto authority = url.substr(authority_start, authority_end - authority_start);
    if (authority.empty() || authority.find('@') != std::string::npos)
        throw QuarkException("HTTP URL has an empty host or embedded credentials");
    std::string target = authority_end == std::string::npos ? "/" : url.substr(authority_end);
    if (const auto fragment = target.find('#'); fragment != std::string::npos)
        target.resize(fragment);
    if (target.empty() || target.front() != '/')
        target.insert(target.begin(), '/');
    return {scheme + "://" + to_lower(authority), std::move(target), scheme == "https"};
}

std::string append_query(const std::string &url, const QueryList &query)
{
    std::string result = url;
    for (const auto &[key, value] : query)
    {
        result += result.find('?') == std::string::npos ? '?' : '&';
        result += url_encode(key) + "=" + url_encode(value);
    }
    return result;
}

bool redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

std::string redirect_url(const std::string &current, const std::string &location)
{
    if (location.empty())
        throw QuarkException("HTTP redirect is missing Location");
    const auto from = parse_url(current);
    std::string next;
    if (location.find("://") != std::string::npos)
        next = location;
    else if (location.starts_with("//"))
        next = std::string(from.tls ? "https:" : "http:") + location;
    else if (location.front() == '/')
        next = from.origin + location;
    else if (location.front() == '?')
        next = from.origin + from.target.substr(0, from.target.find('?')) + location;
    else
    {
        const auto path = from.target.substr(0, from.target.find('?'));
        next = from.origin + path.substr(0, path.rfind('/') + 1) + location;
    }
    if (from.tls && !parse_url(next).tls)
        throw QuarkException("Refusing an HTTPS to HTTP redirect");
    return next;
}

void redirect_headers(HeaderList &headers, const std::string &from, const std::string &to)
{
    const bool cross_origin = parse_url(from).origin != parse_url(to).origin;
    std::erase_if(headers,
                  [cross_origin](const auto &header)
                  {
                      const auto key = to_lower(header.first);
                      if (key == "host")
                          return true;
                      // Allowlisting prevents cookies, authorization and custom credentials
                      // from reaching an unrelated CDN or redirect destination.
                      return cross_origin && key != "accept" && key != "accept-encoding" && key != "user-agent" &&
                             key != "range" && key != "if-range";
                  });
}

httplib::Headers make_headers(const HeaderList &headers)
{
    httplib::Headers result;
    for (const auto &[key, value] : headers)
    {
        if (key.empty() || key.find_first_of("\r\n:") != std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos)
            throw QuarkException("Invalid HTTP header");
        const auto lower = to_lower(key);
        if (lower != "accept-encoding" && lower != "content-length" && lower != "transfer-encoding")
            result.emplace(key, value);
    }
    result.emplace("Accept-Encoding", "identity");
    return result;
}

std::unique_ptr<httplib::Client> make_client(const Url &url, unsigned timeout_ms, const std::string &ca_bundle)
{
    auto client = std::make_unique<httplib::Client>(url.origin);
    if (!client->is_valid())
        throw QuarkException("Cannot initialize HTTP client (HTTPS requires OpenSSL support)");
    client->set_follow_location(false);
    client->set_path_encode(false); // Preserve signed OSS URL bytes.
    client->set_decompress(false);
    const auto timeout = std::chrono::milliseconds(timeout_ms);
    client->set_connection_timeout(timeout);
    client->set_read_timeout(timeout);
    client->set_write_timeout(timeout);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    client->enable_server_certificate_verification(true);
    client->enable_server_hostname_verification(true);
    if (!ca_bundle.empty())
        client->set_ca_cert_path(ca_bundle.c_str());
#else
    static_cast<void>(ca_bundle);
    if (url.tls)
        throw QuarkException("HTTPS requires a cpp-httplib build with OpenSSL enabled");
#endif
    return client;
}

void capture_response(const httplib::Response &source, const std::string &url, HttpResponse &response)
{
    response.status_code = source.status;
    response.url = url;
    response.headers.clear();
    response.set_cookies.clear();
    for (const auto &[key, value] : source.headers)
    {
        response.headers[to_lower(key)] = value;
        if (to_lower(key) == "set-cookie")
            response.set_cookies.push_back(value);
    }
}

void check_result(const httplib::Result &result, const std::string &error)
{
    if (!error.empty())
        throw QuarkException(error);
    if (!result)
        throw QuarkException("HTTP transport failed: " + httplib::to_string(result.error()));
}

std::optional<std::uint64_t> parse_unsigned(std::string_view value)
{
    if (value.empty())
        return std::nullopt;
    std::uint64_t result{};
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (ec != std::errc() || end != value.data() + value.size())
        return std::nullopt;
    return result;
}

struct ContentRange
{
    std::uint64_t start{}, end{}, total{};
    bool unsatisfied{};
};

std::optional<ContentRange> parse_content_range(std::string_view value)
{
    if (!value.starts_with("bytes "))
        return std::nullopt;
    value.remove_prefix(6);
    const auto slash = value.find('/');
    if (slash == std::string_view::npos)
        return std::nullopt;
    const auto total = parse_unsigned(value.substr(slash + 1));
    if (!total)
        return std::nullopt;
    const auto interval = value.substr(0, slash);
    if (interval == "*")
        return ContentRange{0, 0, *total, true};
    const auto dash = interval.find('-');
    if (dash == std::string_view::npos)
        return std::nullopt;
    const auto start = parse_unsigned(interval.substr(0, dash));
    const auto end = parse_unsigned(interval.substr(dash + 1));
    if (!start || !end || *start > *end || *end >= *total)
        return std::nullopt;
    return ContentRange{*start, *end, *total, false};
}

// A bounded wave keeps at most one MiB per worker in memory. Only consecutive,
// fully validated ranges are committed, so the destination always remains a
// resumable prefix even when a worker fails or the process is interrupted.
std::optional<HttpResponse> download_parallel(const HttpClient &client, const std::string &url,
                                              const HeaderList &headers, const std::filesystem::path &path,
                                              std::uint64_t base, std::uint64_t total, unsigned threads,
                                              const ProgressCallback &progress)
{
    constexpr std::uint64_t chunk_size = 1024 * 1024;
    struct Segment
    {
        std::uint64_t start{}, length{};
        HttpResponse response;
        std::string bytes;
        std::exception_ptr error;
        bool unsupported{};
    };
    std::uint64_t committed = base;
    std::ofstream output;
    HttpResponse result;
    while (committed < total)
    {
        const auto count =
            static_cast<unsigned>(std::min<std::uint64_t>(threads, (total - committed - 1) / chunk_size + 1));
        std::vector<Segment> segments(count);
        {
            detail::ThreadGroup workers(count);
            for (unsigned i = 0; i < count; ++i)
            {
                auto &segment = segments[i];
                segment.start = committed + i * chunk_size;
                segment.length = std::min(chunk_size, total - segment.start);
                workers.emplace_back(
                    [&, i]
                    {
                        auto &part = segments[i];
                        try
                        {
                            auto request_headers = headers;
                            std::erase_if(request_headers, [](const auto &h) { return to_lower(h.first) == "range"; });
                            request_headers.emplace_back("Range", "bytes=" + std::to_string(part.start) + "-" +
                                                                      std::to_string(part.start + part.length - 1));
                            part.bytes.reserve(static_cast<std::size_t>(part.length));
                            part.response = client.request_stream(
                                url, request_headers,
                                [&](const HttpResponse &response)
                                {
                                    if (response.status_code == 200)
                                    {
                                        part.unsupported = true;
                                        return false;
                                    }
                                    const auto range = parse_content_range(response.header("content-range"));
                                    if (response.status_code != 206 || !range || range->unsatisfied ||
                                        range->start != part.start || range->end != part.start + part.length - 1 ||
                                        range->total != total)
                                        throw QuarkException(
                                            "Parallel download Content-Range does not match the requested range");
                                    const auto encoding = to_lower(response.header("content-encoding"));
                                    if (!encoding.empty() && encoding != "identity")
                                        throw QuarkException("Download server ignored Accept-Encoding: identity");
                                    const auto length = response.header("content-length");
                                    if (!length.empty() && parse_unsigned(length) != part.length)
                                        throw QuarkException(
                                            "Parallel download Content-Length does not match the requested range");
                                    return true;
                                },
                                [&](const char *data, std::size_t size)
                                {
                                    if (size > part.length - part.bytes.size())
                                        throw QuarkException("Parallel download exceeds the validated range");
                                    part.bytes.append(data, size);
                                    return true;
                                });
                            if (part.bytes.size() != part.length)
                                throw QuarkException("Parallel download ended before the validated range was complete");
                        }
                        catch (...)
                        {
                            part.error = std::current_exception();
                        }
                    });
            }
        } // join every worker before inspecting its buffers or propagating errors
        if (committed == base &&
            std::any_of(segments.begin(), segments.end(), [](const auto &s) { return s.unsupported; }))
            return std::nullopt; // fall back without changing an existing destination
        for (const auto &segment : segments)
        {
            if (segment.error)
                std::rethrow_exception(segment.error);
            if (!output.is_open())
            {
                if (!path.parent_path().empty())
                    std::filesystem::create_directories(path.parent_path());
                output.open(path, std::ios::binary | (base > 0 ? std::ios::app : std::ios::trunc));
                if (!output)
                    throw QuarkException("Cannot open download destination: " + path_to_utf8(path));
            }
            output.write(segment.bytes.data(), static_cast<std::streamsize>(segment.bytes.size()));
            output.flush();
            if (!output)
                throw QuarkException("Failed writing download destination (check available disk space)");
            committed += segment.length;
            result = segment.response;
            if (progress)
                progress(committed, total);
        }
    }
    return result;
}

std::string base64_from_bytes(const unsigned char *data, std::size_t size)
{
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max() / 4 * 3))
        throw QuarkException("Base64 input is too large");
    std::string encoded(4 * ((size + 2) / 3) + 1, '\0');
    const auto written =
        EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()), data, static_cast<int>(size));
    if (written < 0)
        throw QuarkException("OpenSSL Base64 encoding failed");
    encoded.resize(static_cast<std::size_t>(written));
    return encoded;
}

std::string finish_digest(EVP_MD_CTX *ctx)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned size{};
    if (EVP_DigestFinal_ex(ctx, digest.data(), &size) != 1)
        throw QuarkException("OpenSSL digest finalization failed");
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned i = 0; i < size; ++i)
        output << std::setw(2) << static_cast<unsigned>(digest[i]);
    return output.str();
}
} // namespace

QuarkException::QuarkException(const std::string &message) : std::runtime_error(message)
{
}
std::string HttpResponse::header(std::string_view key) const
{
    const auto it = headers.find(to_lower(std::string(key)));
    return it == headers.end() ? std::string{} : it->second;
}
json HttpResponse::json_body() const
{
    try
    {
        return json::parse(body);
    }
    catch (const json::exception &error)
    {
        throw QuarkException("Invalid JSON response: " + std::string(error.what()));
    }
}
HttpClient::HttpClient(unsigned timeout_ms, std::string ca_bundle)
    : timeout_ms_(timeout_ms), ca_bundle_(std::move(ca_bundle))
{
    if (timeout_ms == 0)
        throw QuarkException("HTTP timeout must be positive");
}

HttpResponse HttpClient::request(std::string method, const std::string &url, const QueryList &query,
                                 const HeaderList &headers, const std::string &body, bool follow_redirects) const
{
    std::string current = append_query(url, query);
    auto active_headers = headers;
    for (unsigned attempt = 0; attempt <= max_redirects; ++attempt)
    {
        const auto parsed = parse_url(current);
        HttpResponse response;
        pace::http::request req;
        req.method = method;
        for (const auto &[key, value] : make_headers(active_headers))
            if (to_lower(key) != "host")
                req.headers.push_back({key, value});
        req.body = body;
        try
        {
            const pace::blocking_options wait{.timeout = std::chrono::milliseconds(timeout_ms_)};
            const pace::http::limits bounds{.max_body_bytes = max_api_body};
            pace::http::response received;
            if (!parsed.tls)
                received = pace::http::fetch(current, std::move(req), wait, bounds);
            else if (ca_bundle_.empty())
                received = pace::http::fetch(default_api_trust(), current, std::move(req), wait, bounds);
            else
            {
                pace::tls::context trust(pace::tls::role::client, pace::tls::trust_source::application);
                trust.load_verify_file(ca_bundle_);
                received = pace::http::fetch(trust, current, std::move(req), wait, bounds);
            }
            response.status_code = static_cast<int>(received.status);
            response.body = std::move(received.body);
            response.url = current;
            for (const auto &[key, value] : received.headers)
            {
                response.headers[to_lower(key)] = value;
                if (to_lower(key) == "set-cookie")
                    response.set_cookies.push_back(value);
            }
        }
        catch (const pace::http::protocol_error &error)
        {
            throw QuarkException("PACE HTTP protocol error: " + std::string(error.what()));
        }
        catch (const std::system_error &error)
        {
            // The error code is safe; what() may include an account or signed URL.
            throw QuarkException("PACE HTTP transport failed: " + error.code().message());
        }
        catch (const std::exception &)
        {
            // Upstream errors may contain a signed URL or request credentials.
            throw QuarkException("PACE HTTP request failed (transport, timeout, TLS or response limit)");
        }
        if (!follow_redirects || !redirect_status(response.status_code))
            return response;
        const auto next = redirect_url(current, response.header("location"));
        if (!body.empty() && parse_url(next).origin != parsed.origin)
            throw QuarkException("Refusing to redirect an HTTP request body to another origin");
        if (method != "GET" && method != "HEAD")
        {
            if (response.status_code != 307 && response.status_code != 308)
                throw QuarkException("Refusing a redirect that changes a mutating HTTP request");
            if (parse_url(next).origin != parsed.origin)
                throw QuarkException("Refusing to redirect an HTTP request body to another origin");
        }
        redirect_headers(active_headers, current, next);
        current = next;
    }
    throw QuarkException("HTTP redirect limit exceeded");
}

HttpResponse HttpClient::request_stream(const std::string &url, const HeaderList &headers,
                                        const std::function<bool(const HttpResponse &)> &on_response,
                                        const std::function<bool(const char *, std::size_t)> &on_chunk) const
{
    std::string current = url;
    auto active_headers = headers;
    for (unsigned attempt = 0; attempt <= max_redirects; ++attempt)
    {
        const auto parsed = parse_url(current);
        auto client = make_client(parsed, timeout_ms_, ca_bundle_);
        HttpResponse response;
        std::string error;
        httplib::Request req;
        req.method = "GET";
        req.path = parsed.target;
        req.headers = make_headers(active_headers);
        req.response_handler = [&](const httplib::Response &res)
        {
            capture_response(res, current, response);
            if (redirect_status(res.status))
                return true;
            if (on_response && !on_response(response))
            {
                error = "Streaming HTTP response rejected by receiver";
                return false;
            }
            return true;
        };
        req.content_receiver = [&](const char *data, std::size_t size, std::uint64_t, std::uint64_t)
        {
            if (redirect_status(response.status_code))
                return true;
            if (on_chunk && !on_chunk(data, size))
            {
                error = "Streaming HTTP transfer canceled by receiver";
                return false;
            }
            return true;
        };
        const auto result = client->send(req);
        check_result(result, error);
        if (response.status_code == 0)
        {
            capture_response(*result, current, response);
            if (on_response && !on_response(response))
                throw QuarkException("Streaming HTTP response rejected by receiver");
        }
        if (!redirect_status(response.status_code))
            return response;
        const auto next = redirect_url(current, response.header("location"));
        redirect_headers(active_headers, current, next);
        current = next;
    }
    throw QuarkException("HTTP redirect limit exceeded");
}

HttpResponse HttpClient::upload_file_range(const std::string &url, const QueryList &query, const HeaderList &headers,
                                           const std::filesystem::path &file_path, std::uint64_t offset,
                                           std::uint64_t length, const ProgressCallback &progress) const
{
    const auto file_size = std::filesystem::file_size(file_path);
    if (offset > file_size || length > file_size - offset ||
        offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        length > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) - offset)
        throw QuarkException("Upload range lies outside the source file");
    std::ifstream input(file_path, std::ios::binary);
    if (!input)
        throw QuarkException("Cannot open upload file: " + path_to_utf8(file_path));
    std::string current = append_query(url, query);
    auto active_headers = headers;
    std::vector<char> buffer(transfer_buffer_size);
    for (unsigned attempt = 0; attempt <= max_redirects; ++attempt)
    {
        const auto parsed = parse_url(current);
        auto client = make_client(parsed, timeout_ms_, ca_bundle_);
        HttpResponse response;
        std::string error;
        httplib::Request req;
        req.method = "PUT";
        req.path = parsed.target;
        req.headers = make_headers(active_headers);
        req.content_length_ = static_cast<std::size_t>(length);
        req.content_provider_ = [&](std::size_t position, std::size_t requested, httplib::DataSink &sink)
        {
            if (position > length || requested > length - position)
            {
                error = "Invalid upload provider range";
                return false;
            }
            input.clear();
            input.seekg(static_cast<std::streamoff>(offset + position));
            if (!input)
            {
                error = "Cannot seek upload source file";
                return false;
            }
            std::size_t sent{};
            while (sent < requested)
            {
                const auto count = std::min(buffer.size(), requested - sent);
                input.read(buffer.data(), static_cast<std::streamsize>(count));
                if (input.gcount() != static_cast<std::streamsize>(count) || input.bad())
                {
                    error = "Upload source became unreadable or shorter during transfer";
                    return false;
                }
                if (!sink.write(buffer.data(), count))
                    return false;
                sent += count;
                if (progress)
                    progress(static_cast<std::uint64_t>(position + sent), length);
            }
            return true;
        };
        req.response_handler = [&](const httplib::Response &res)
        {
            capture_response(res, current, response);
            return true;
        };
        req.content_receiver = [&](const char *data, std::size_t size, std::uint64_t, std::uint64_t)
        {
            if (size > max_api_body - response.body.size())
            {
                error = "Upload response exceeds the 16 MiB limit";
                return false;
            }
            response.body.append(data, size);
            return true;
        };
        const auto result = client->send(req);
        check_result(result, error);
        capture_response(*result, current, response);
        if (!redirect_status(response.status_code))
        {
            if (progress && response.status_code >= 200 && response.status_code < 300)
                progress(length, length);
            return response;
        }
        const auto next = redirect_url(current, response.header("location"));
        if ((response.status_code != 307 && response.status_code != 308) || parse_url(next).origin != parsed.origin)
            throw QuarkException("Refusing an unsafe upload redirect");
        redirect_headers(active_headers, current, next);
        current = next;
    }
    throw QuarkException("HTTP redirect limit exceeded");
}

HttpResponse HttpClient::download_file(const std::string &url, const HeaderList &headers,
                                       const std::filesystem::path &output_path, bool resume,
                                       const ProgressCallback &progress, std::optional<std::uint64_t> expected_size,
                                       unsigned threads) const
{
    if (threads == 0 || threads > 32)
        throw QuarkException("Download threads must be between 1 and 32");
    const bool exists = std::filesystem::exists(output_path);
    if (exists && !std::filesystem::is_regular_file(output_path))
        throw QuarkException("Download destination must be a regular file");
    const std::uint64_t base = resume && exists ? std::filesystem::file_size(output_path) : 0;
    if (expected_size && base > *expected_size)
        throw QuarkException("Local partial file is larger than the expected download size");
    if (threads > 1 && expected_size && *expected_size - base > 1024 * 1024)
        if (const auto response =
                download_parallel(*this, url, headers, output_path, base, *expected_size, threads, progress))
            return *response;
    auto active_headers = headers;
    std::erase_if(active_headers, [](const auto &h) { return to_lower(h.first) == "range"; });
    if (base > 0)
        active_headers.emplace_back("Range", "bytes=" + std::to_string(base) + "-");
    std::string current = url;
    for (unsigned attempt = 0; attempt <= max_redirects; ++attempt)
    {
        const auto parsed = parse_url(current);
        auto client = make_client(parsed, timeout_ms_, ca_bundle_);
        HttpResponse response;
        std::string error;
        std::ofstream output;
        std::uint64_t received{}, total = expected_size.value_or(0);
        std::optional<std::uint64_t> expected;
        if (expected_size)
            expected = *expected_size - base;
        bool already_complete = false;
        httplib::Request req;
        req.method = "GET";
        req.path = parsed.target;
        req.headers = make_headers(active_headers);
        req.response_handler = [&](const httplib::Response &res)
        {
            capture_response(res, current, response);
            if (redirect_status(res.status))
                return true;
            const auto range = parse_content_range(response.header("content-range"));
            if (res.status == 416)
            {
                already_complete = base > 0 && range && range->unsatisfied && range->total == base &&
                                   (!expected_size || *expected_size == base);
                if (!already_complete)
                    error = "Download range is unsatisfiable and the local file is not a confirmed complete file";
                return false;
            }
            if (res.status != 200 && res.status != 206)
            {
                error = "Download returned HTTP " + std::to_string(res.status);
                return false;
            }
            const auto encoding = to_lower(response.header("content-encoding"));
            if (!encoding.empty() && encoding != "identity")
            {
                error = "Download server ignored Accept-Encoding: identity";
                return false;
            }
            if (res.status == 206)
            {
                if (!range || range->unsatisfied || range->start != base || range->end + 1 != range->total)
                {
                    error = "Download Content-Range does not match the requested offset and total";
                    return false;
                }
                if (expected_size && range->total != *expected_size)
                {
                    error = "Download Content-Range total does not match the expected file size";
                    return false;
                }
                expected = range->total - base;
                total = range->total;
            }
            else if (base > 0)
            {
                error = "Download server ignored Range; existing partial file was preserved";
                return false;
            }
            const auto content_length = response.header("content-length");
            if (!content_length.empty())
            {
                const auto length = parse_unsigned(content_length);
                if (!length || (expected && *expected != *length))
                {
                    error = "Download Content-Length does not match Content-Range";
                    return false;
                }
                expected = *length;
                total = base + *length;
            }
            if (!output_path.parent_path().empty())
                std::filesystem::create_directories(output_path.parent_path());
            output.open(output_path, std::ios::binary | (base > 0 ? std::ios::app : std::ios::trunc));
            if (!output)
            {
                error = "Cannot open download destination: " + path_to_utf8(output_path);
                return false;
            }
            return true;
        };
        req.content_receiver = [&](const char *data, std::size_t size, std::uint64_t, std::uint64_t)
        {
            if (redirect_status(response.status_code))
                return true;
            if (!output || (expected && size > *expected - received))
            {
                error = "Download body exceeds the validated byte range";
                return false;
            }
            output.write(data, static_cast<std::streamsize>(size));
            if (!output)
            {
                error = "Failed writing download destination (check available disk space)";
                return false;
            }
            received += size;
            if (progress)
                progress(base + received, total);
            return true;
        };
        const auto result = client->send(req);
        if (output.is_open())
        {
            output.flush();
            if (!output && error.empty())
                error = "Failed flushing download destination";
            output.close();
        }
        if (already_complete)
        {
            if (progress)
                progress(base, base);
            return response;
        }
        check_result(result, error);
        if (response.status_code == 0)
        {
            capture_response(*result, current, response);
            throw QuarkException("Download returned HTTP " + std::to_string(response.status_code));
        }
        if (redirect_status(response.status_code))
        {
            const auto next = redirect_url(current, response.header("location"));
            redirect_headers(active_headers, current, next);
            current = next;
            continue;
        }
        if (expected && received != *expected)
            throw QuarkException("Download ended before the validated byte range was complete");
        if (progress)
            progress(base + received, total ? total : base + received);
        return response;
    }
    throw QuarkException("HTTP redirect limit exceeded");
}

std::string trim(std::string_view value)
{
    std::size_t start{}, end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start])))
        ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return std::string(value.substr(start, end - start));
}
std::string to_lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
std::string url_encode(std::string_view value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char c : value)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            encoded += static_cast<char>(c);
        else
        {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
        }
    }
    return encoded;
}
std::string base64_encode(const std::string &value)
{
    return base64_from_bytes(reinterpret_cast<const unsigned char *>(value.data()), value.size());
}
std::string md5_base64(const std::string &value)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned size{};
    if (EVP_Digest(value.data(), value.size(), digest.data(), &size, EVP_md5(), nullptr) != 1)
        throw QuarkException("OpenSSL MD5 failed");
    return base64_from_bytes(digest.data(), size);
}
HashDigests compute_file_hashes(const std::filesystem::path &file_path)
{
    std::ifstream input(file_path, std::ios::binary);
    if (!input)
        throw QuarkException("Cannot open file for hashing: " + path_to_utf8(file_path));
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Context md5(EVP_MD_CTX_new(), EVP_MD_CTX_free), sha1(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!md5 || !sha1 || EVP_DigestInit_ex(md5.get(), EVP_md5(), nullptr) != 1 ||
        EVP_DigestInit_ex(sha1.get(), EVP_sha1(), nullptr) != 1)
        throw QuarkException("OpenSSL hash initialization failed");
    std::vector<char> buffer(1024 * 1024);
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && (EVP_DigestUpdate(md5.get(), buffer.data(), static_cast<std::size_t>(count)) != 1 ||
                          EVP_DigestUpdate(sha1.get(), buffer.data(), static_cast<std::size_t>(count)) != 1))
            throw QuarkException("OpenSSL hash update failed");
    }
    if (input.bad() || !input.eof())
        throw QuarkException("File read failed during hashing");
    return {finish_digest(md5.get()), finish_digest(sha1.get())};
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
std::uint64_t unix_time_milliseconds_now()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}
std::string format_bytes(std::uint64_t bytes)
{
    constexpr std::array<std::string_view, 6> units{"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit{};
    while (value >= 1024.0 && unit + 1 < units.size())
    {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream output;
    output << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << ' ' << units[unit];
    return output.str();
}
std::vector<std::string> split_remote_path(std::string_view remote_path)
{
    std::string normalized(remote_path);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::stringstream input(normalized);
    std::vector<std::string> result;
    std::string part;
    while (std::getline(input, part, '/'))
    {
        if (part == "." || part == "..")
            throw QuarkException("Remote paths cannot contain . or .. segments");
        if (!part.empty())
            result.push_back(part);
    }
    return result;
}
std::filesystem::path path_from_utf8(std::string_view value)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(value.data()), value.size()));
}
std::string path_to_utf8(const std::filesystem::path &value)
{
    const auto utf8 = value.u8string();
    return std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size());
}
} // namespace quarkpp
