#include "quarkpp/client.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <openssl/evp.h>
#include <optional>
#include <set>

#include <sstream>
#include <thread>

namespace quarkpp
{

namespace
{

constexpr std::string_view kDesktopDownloadUa =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "quark-cloud-drive/2.5.56 Chrome/100.0.4896.160 Electron/18.3.5.12-a038f7b798 Safari/537.36 Channel/pckk_other_ch";
constexpr std::string_view kOssUserAgent = "aliyun-sdk-js/6.6.1 Chrome 98.0.4758.80 on Windows 10 64-bit";

[[nodiscard]] std::string json_string(const json &value, std::string_view key, std::string fallback = {})
{
    if (!value.contains(key))
    {
        return fallback;
    }
    const auto &field = value.at(key);
    if (field.is_string())
    {
        return field.get<std::string>();
    }
    if (field.is_number_unsigned())
    {
        return std::to_string(field.get<unsigned long long>());
    }
    if (field.is_number_integer())
        return std::to_string(field.get<long long>());
    return fallback;
}

[[nodiscard]] std::uint64_t json_u64(const json &value, std::string_view key, std::uint64_t fallback = 0)
{
    if (!value.contains(key))
    {
        return fallback;
    }
    const auto &field = value.at(key);
    if (field.is_number_unsigned())
    {
        return field.get<std::uint64_t>();
    }
    if (field.is_number_integer())
    {
        return static_cast<std::uint64_t>(std::max<long long>(0, field.get<long long>()));
    }
    if (field.is_string())
    {
        std::uint64_t parsed = fallback;
        const auto &text = field.get_ref<const std::string &>();
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        return ec == std::errc{} && end == text.data() + text.size() ? parsed : fallback;
    }
    return fallback;
}

[[nodiscard]] bool is_directory_entry(const json &value)
{
    if (value.contains("dir") && value.at("dir").is_boolean())
    {
        return value.at("dir").get<bool>();
    }
    if (value.contains("file") && value.at("file").is_boolean())
        return !value.at("file").get<bool>();
    if (value.contains("file_type") && value.at("file_type").is_string())
    {
        return value.at("file_type").get<std::string>() == "folder";
    }
    if (value.contains("dir") && value.at("dir").is_number_integer())
        return value.at("dir") != 0;
    if (value.contains("file_type") && value.at("file_type").is_number_integer())
        return value.at("file_type") == 0;
    return false;
}

[[nodiscard]] RemoteEntry make_remote_entry(const json &value)
{
    RemoteEntry entry;
    entry.fid = json_string(value, "fid");
    entry.name = json_string(value, "file_name");
    entry.parent_fid = json_string(value, "pdir_fid", "0");
    entry.size = json_u64(value, "size");
    entry.dir = is_directory_entry(value);
    entry.raw = value;
    if (entry.fid.empty() || entry.name.empty())
        throw QuarkException("Malformed file entry: missing fid or file_name");
    return entry;
}

void ensure_api_success(const json &result, const std::string &action)
{
    if (!result.is_object())
        throw QuarkException(action + ": malformed API response");
    const auto code = json_string(result, "code");
    const bool explicit_failure =
        result.contains("success") && result.at("success").is_boolean() && !result.at("success").get<bool>();
    const bool explicit_success =
        result.contains("success") && result.at("success").is_boolean() && result.at("success").get<bool>();
    if (!explicit_failure && (code == "0" || (code.empty() && explicit_success)))
        return;
    throw QuarkException(action + ": " + json_string(result, "message", "API did not report success") +
                         (code.empty() ? "" : " (code " + code + ")"));
}

[[nodiscard]] int clamp_retry_count(int value)
{
    return value <= 0 ? 3 : value;
}

[[nodiscard]] std::string detect_mime_type(const std::filesystem::path &path)
{
    const auto ext = to_lower(path_to_utf8(path.extension()));
    if (ext == ".txt" || ext == ".log" || ext == ".md" || ext == ".json" || ext == ".yaml" || ext == ".yml")
    {
        return "text/plain";
    }
    if (ext == ".jpg" || ext == ".jpeg")
    {
        return "image/jpeg";
    }
    if (ext == ".png")
    {
        return "image/png";
    }
    if (ext == ".gif")
    {
        return "image/gif";
    }
    if (ext == ".mp4")
    {
        return "video/mp4";
    }
    if (ext == ".mp3")
    {
        return "audio/mpeg";
    }
    if (ext == ".pdf")
    {
        return "application/pdf";
    }
    if (ext == ".zip")
    {
        return "application/zip";
    }
    if (ext == ".7z")
    {
        return "application/x-7z-compressed";
    }
    return "application/octet-stream";
}

[[nodiscard]] std::string summarize_http_error(const HttpResponse &response)
{
    if (!response.body.empty())
    {
        try
        {
            const auto body = json::parse(response.body);
            const auto message = body.value("message", "");
            const auto code = json_string(body, "code");
            if (!message.empty() || !code.empty())
            {
                std::string summary;
                if (!code.empty())
                {
                    summary += " code=" + code;
                }
                if (!message.empty())
                {
                    summary += " message=" + message;
                }
                return trim(summary);
            }
        }
        catch (...)
        {
        }
    }
    return {};
}

[[nodiscard]] std::string oss_error_summary(const HttpResponse &response)
{
    // OSS error bodies may contain signatures, signed resource names and upload
    // IDs. Read only the bounded error-code identifier, never return the body.
    std::string code;
    const auto begin = response.body.find("<Code>");
    if (begin != std::string::npos)
    {
        const auto end = response.body.find("</Code>", begin + 6);
        if (end != std::string::npos && end - begin - 6 <= 80)
            code = response.body.substr(begin + 6, end - begin - 6);
    }
    if (code.empty() ||
        !std::all_of(code.begin(), code.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
        return "OSS did not return a recognized error code";
    static const std::unordered_map<std::string, std::string> explanations{
        {"InvalidArgument", "a request argument is invalid"},
        {"InvalidRequest", "the request is incompatible with this upload"},
        {"InvalidPart", "a referenced part is missing or invalid"},
        {"InvalidPartOrder", "part ordering is invalid"},
        {"EntityTooLarge", "the request body exceeds the server limit"},
        {"EntityTooSmall", "the request body is below the server minimum"},
        {"SignatureDoesNotMatch", "the signed request and transmitted request differ"},
        {"BadDigest", "the transmitted content does not match its digest"},
        {"InvalidDigest", "the content digest encoding is invalid"},
        {"IncompleteBody", "the transmitted body is shorter than Content-Length"},
        {"InvalidPartNumber", "the part number is invalid"},
        {"NoSuchUpload", "the multipart upload session is missing or expired"},
        {"AccessDenied", "object storage rejected the upload authorization"},
        {"RequestTimeTooSkewed", "the request clock differs from the server clock"},
        {"MalformedXML", "the completion XML is invalid"}};
    const auto explanation = explanations.find(code);
    return "OSS " + code + (explanation == explanations.end() ? "" : ": " + explanation->second);
}

void validate_name(const std::string &name)
{
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; }))
        throw QuarkException("Invalid file name");
}

std::filesystem::path local_name(const std::string &name)
{
    validate_name(name);
    const auto path = path_from_utf8(name);
    if (path.has_root_path() || path.filename() != path)
        throw QuarkException("Unsafe remote file name");
#ifdef _WIN32
    // Windows reserves device names before the first dot, including CON.a.b.
    const auto stem = to_lower(name.substr(0, name.find('.')));
    if (name.find_first_of(":*?\"<>|") != std::string::npos || name.back() == '.' || name.back() == ' ' ||
        stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
        (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9'))
        throw QuarkException("Remote name cannot be represented safely on Windows: " + name);
#endif
    return path;
}

void reject_symlink_path(const std::filesystem::path &path)
{
    // Never follow an existing local symlink while materializing remote names.
    auto current = std::filesystem::absolute(path).lexically_normal();
    while (!current.empty())
    {
        std::error_code ec;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(current, ec)))
            throw QuarkException("Refusing to download through a symbolic link: " + path_to_utf8(current));
        const auto parent = current.parent_path();
        if (parent == current)
            break;
        current = parent;
    }
}

HeaderList merge_headers(HeaderList base, const HeaderList &overrides)
{
    for (const auto &[key, value] : overrides)
    {
        std::erase_if(base, [&](const auto &item) { return to_lower(item.first) == to_lower(key); });
        base.emplace_back(key, value);
    }
    return base;
}

std::string url_origin(std::string_view url)
{
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos)
        throw QuarkException("Download URL must be absolute");
    const auto end = url.find_first_of("/?#", scheme + 3);
    auto origin = to_lower(std::string(url.substr(0, end)));
    if (origin.find('@') != std::string::npos || origin.find_first_of("\r\n\t ") != std::string::npos)
        throw QuarkException("Download URL contains an invalid authority");
    return origin;
}

void validate_download_origin(const std::string &url, const std::string &api_base)
{
    const auto origin = url_origin(url);
    // Configured API gateways can return links on their own origin. Production
    // cookies otherwise only go to Quark's HTTPS domain, never arbitrary links.
    if (origin == url_origin(api_base))
        return;
    if (!origin.starts_with("https://"))
        throw QuarkException("Refusing an insecure download URL");
    auto host = origin.substr(8);
    if (const auto port = host.find(':'); port != std::string::npos)
        host.resize(port);
    if (host != "quark.cn" && !host.ends_with(".quark.cn"))
        throw QuarkException("Refusing to send account cookies to an unrelated download host");
}

std::string xml_escape(std::string_view value)
{
    std::string result;
    for (char c : value)
    {
        if (c == '&')
            result += "&amp;";
        else if (c == '<')
            result += "&lt;";
        else if (c == '>')
            result += "&gt;";
        else if (c == '\"')
            result += "&quot;";
        else
            result += c;
    }
    return result;
}

// Quark's versioned wg envelope, documented in docs/protocol.md. The 16-byte
// version key is public protocol material, not an account credential.
std::string wg_crypt(std::string_view input, const std::string &key, bool encrypt)
{
    if (key.size() != 16 || input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max() - 32))
        throw QuarkException("Invalid wg key or oversized payload");
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx ||
        EVP_CipherInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, reinterpret_cast<const unsigned char *>(key.data()),
                          reinterpret_cast<const unsigned char *>(key.data()), encrypt ? 1 : 0) != 1)
        throw QuarkException("Cannot initialize wg encryption");
    std::string output(input.size() + 32, '\0');
    int written = 0, tail = 0;
    if (EVP_CipherUpdate(ctx.get(), reinterpret_cast<unsigned char *>(output.data()), &written,
                         reinterpret_cast<const unsigned char *>(input.data()), static_cast<int>(input.size())) != 1 ||
        EVP_CipherFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(output.data()) + written, &tail) != 1)
        throw QuarkException("Invalid wg ciphertext or padding; the provider key may have changed");
    output.resize(static_cast<std::size_t>(written + tail));
    return output;
}

std::string wg_encode(const std::string &plain, const AppConfig &config)
{
    std::string bytes;
    bytes += static_cast<char>(config.offline_secure_number >> 8);
    bytes += static_cast<char>(config.offline_secure_number & 255);
    bytes += wg_crypt(plain, config.offline_key, true);
    return base64_encode(bytes);
}

std::string wg_decode(const std::string &body, const AppConfig &config)
{
    const auto input = trim(body);
    if (input.empty() || input.size() % 4 != 0 || input.size() > 64 * 1024 * 1024)
        throw QuarkException("Invalid wg response envelope");
    std::string bytes(input.size() / 4 * 3, '\0');
    int count = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(bytes.data()),
                                reinterpret_cast<const unsigned char *>(input.data()), static_cast<int>(input.size()));
    if (count < 0)
        throw QuarkException("Invalid wg base64");
    if (input.back() == '=')
        --count;
    if (input.size() >= 2 && input[input.size() - 2] == '=')
        --count;
    if (count < 18)
        throw QuarkException("Truncated wg response");
    bytes.resize(static_cast<std::size_t>(count));
    const auto number = static_cast<unsigned char>(bytes[0]) * 256 + static_cast<unsigned char>(bytes[1]);
    if (number != config.offline_secure_number)
        throw QuarkException("Unknown wg response version: " + std::to_string(number));
    return wg_crypt(std::string_view(bytes).substr(2), config.offline_key, false);
}

} // namespace

struct QuarkClient::SessionState
{
    explicit SessionState(std::string initial) : cookie(std::move(initial))
    {
    }
    std::mutex mutex;
    std::string cookie;
};

QuarkClient::QuarkClient(AppConfig config)
    : config_(std::move(config)), http_(config_.request_timeout_ms, config_.ca_bundle),
      session_(std::make_shared<SessionState>(config_.cookie))
{
    if (config_.cookie.empty())
    {
        throw QuarkException("Missing Quark cookie; supply AppConfig.cookie or QUARKPP_COOKIE");
    }
    if (config_.user_agent.empty())
    {
        config_.user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                             "(KHTML, like Gecko) Chrome/137.0.0.0 Safari/537.36";
    }
    config_.upload_retry_count = clamp_retry_count(config_.upload_retry_count);
    config_.download_retry_count = clamp_retry_count(config_.download_retry_count);
    if (config_.download_threads == 0 || config_.download_threads > 32 || config_.download_jobs == 0 ||
        config_.download_jobs > 32 || config_.download_batch_size == 0 || config_.download_batch_size > 100)
        throw QuarkException("Download threads/jobs must be between 1 and 32; batch size must be between 1 and 100");
    if (config_.task_poll_limit == 0)
        throw QuarkException("task_poll_limit must be positive");
    while (config_.api_base.ends_with('/'))
        config_.api_base.pop_back();
    while (config_.offline_api_base.ends_with('/'))
        config_.offline_api_base.pop_back();
    if (config_.cookie.find_first_of("\r\n") != std::string::npos ||
        config_.user_agent.find_first_of("\r\n") != std::string::npos)
        throw QuarkException("HTTP credentials and User-Agent must not contain line breaks");
}

const AppConfig &QuarkClient::config() const
{
    return config_;
}

std::string QuarkClient::session_cookie() const
{
    std::lock_guard lock(session_->mutex);
    return session_->cookie;
}

void QuarkClient::refresh_session_cookie(const HttpResponse &response) const
{
    // Only the configured API endpoints may refresh this account credential.
    // Redirect targets and object-storage responses are not cookie authorities.
    if (!response.url.starts_with(config_.api_base + "/") && !response.url.starts_with(config_.offline_api_base + "/"))
        return;
    for (const auto &field : response.set_cookies)
    {
        const auto pair = trim(std::string_view(field).substr(0, field.find(';')));
        const auto equals = pair.find('=');
        if (equals == std::string::npos || trim(std::string_view(pair).substr(0, equals)) != "__puus")
            continue;
        const auto value = pair.substr(equals + 1);
        if (value.size() > 16384 || std::any_of(value.begin(), value.end(), [](unsigned char ch)
                                                { return ch < 0x21 || ch > 0x7e || ch == ';' || ch == ','; }))
            continue;
        std::lock_guard lock(session_->mutex);
        std::string updated;
        std::size_t begin = 0;
        while (begin < session_->cookie.size())
        {
            const auto end = session_->cookie.find(';', begin);
            const auto existing = trim(std::string_view(session_->cookie).substr(begin, end - begin));
            const auto split = existing.find('=');
            if (!existing.empty() && trim(std::string_view(existing).substr(0, split)) != "__puus")
            {
                if (!updated.empty())
                    updated += "; ";
                updated += existing;
            }
            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
        if (!value.empty())
        {
            if (!updated.empty())
                updated += "; ";
            updated += "__puus=" + value;
        }
        session_->cookie = std::move(updated);
    }
}

QueryList QuarkClient::base_params() const
{
    return {{"pr", "ucpro"}, {"fr", "pc"}, {"uc_param_str", ""}};
}

HeaderList QuarkClient::api_headers() const
{
    return {
        {"Cookie", session_cookie()},
        {"Accept", "application/json, text/plain, */*"},
        {"Accept-Language", "zh-CN,zh;q=0.9"},
        {"Content-Type", "application/json"},
        {"Origin", "https://pan.quark.cn"},
        {"Referer", "https://pan.quark.cn/"},
        {"Priority", "u=1, i"},
        {"User-Agent", config_.user_agent},
    };
}

HeaderList QuarkClient::download_headers() const
{
    return {
        {"Cookie", session_cookie()},
        {"Origin", "https://pan.quark.cn"},
        {"Referer", "https://pan.quark.cn/"},
        {"User-Agent", std::string(kDesktopDownloadUa)},
    };
}

json QuarkClient::api_get_json(const std::string &url, const QueryList &query, const HeaderList &headers) const
{
    auto merged_headers = merge_headers(api_headers(), headers);
    auto response = http_.request("GET", url, query, merged_headers);
    refresh_session_cookie(response);
    if (response.status_code < 200 || response.status_code >= 300)
    {
        auto detail = summarize_http_error(response);
        throw QuarkException("HTTP GET failed: " + std::to_string(response.status_code) +
                             (detail.empty() ? std::string() : " | " + detail));
    }
    auto result = response.json_body();
    ensure_api_success(result, "GET API");
    return result;
}

json QuarkClient::api_post_json(const std::string &url, const json &body, const QueryList &query,
                                const HeaderList &headers) const
{
    auto merged_headers = merge_headers(api_headers(), headers);
    auto response = http_.request("POST", url, query, merged_headers, body.dump());
    refresh_session_cookie(response);
    if (response.status_code < 200 || response.status_code >= 300)
    {
        auto detail = summarize_http_error(response);
        throw QuarkException("HTTP POST failed: " + std::to_string(response.status_code) +
                             (detail.empty() ? std::string() : " | " + detail));
    }
    auto result = response.json_body();
    ensure_api_success(result, "POST API");
    return result;
}

json QuarkClient::account_info() const
{
    const auto result = api_get_json(config_.api_base + "/member", base_params());
    ensure_api_success(result, "Read account information");
    const auto data = result.value("data", json::object());
    if (data.is_null() || (data.is_object() && data.empty()))
    {
        throw QuarkException("Account API returned no data; the cookie may have expired");
    }
    return data;
}

std::vector<RemoteEntry> QuarkClient::list_entries(const std::string &endpoint, const QueryList &extra) const
{
    std::vector<RemoteEntry> entries;
    std::unordered_set<std::string> seen;
    constexpr unsigned page_size = 100;
    for (unsigned page = 1; page <= 100000; ++page)
    {
        auto query = base_params();
        query.insert(query.end(), extra.begin(), extra.end());
        query.emplace_back("_page", std::to_string(page));
        query.emplace_back("_size", std::to_string(page_size));
        query.emplace_back("_fetch_total", "1");
        query.emplace_back("_fetch_sub_dirs", "0");
        query.emplace_back("_sort", "file_type:asc,file_name:asc");
        const auto result = api_get_json(config_.api_base + endpoint, query);
        const auto &list = result.at("data").at("list");
        if (!list.is_array())
            throw QuarkException("Listing did not return an array");
        const auto previous_size = entries.size();
        for (const auto &item : list)
        {
            auto entry = make_remote_entry(item);
            if (seen.insert(entry.fid).second)
                entries.push_back(std::move(entry));
        }
        const auto metadata = result.value("metadata", json::object());
        const auto total = json_u64(metadata, "_total", std::numeric_limits<std::uint64_t>::max());
        if (entries.size() >= total || (list.size() < page_size && !metadata.contains("_total")))
            return entries;
        if (list.empty())
        {
            if (metadata.contains("_total") && entries.size() < total)
                throw QuarkException("Directory changed during pagination; retry listing");
            return entries;
        }
        if (entries.size() == previous_size)
            throw QuarkException("Server repeated a listing page; refusing incomplete results");
    }
    throw QuarkException("Listing exceeded the pagination limit");
}

std::vector<RemoteEntry> QuarkClient::list_directory(const std::string &pdir_fid) const
{
    return list_entries("/file/sort", {{"pdir_fid", pdir_fid}});
}

std::vector<RemoteEntry> QuarkClient::search(const std::string &keyword, const std::string &pdir_fid) const
{
    return list_entries("/file/search", {{"pdir_fid", pdir_fid}, {"q", keyword}});
}

RemoteEntry QuarkClient::file_info(const std::string &fid) const
{
    if (fid == "0")
        return RemoteEntry{"0", "/", "", 0, true, json::object()};
    auto query = base_params();
    query.emplace_back("fid", fid);

    const auto result = api_get_json(config_.api_base + "/file/info", query);
    ensure_api_success(result, "Read file information");
    return make_remote_entry(result.at("data"));
}

RemoteEntry QuarkClient::resolve_path(const std::string &remote_path) const
{
    if (remote_path.empty() || remote_path == "/")
    {
        return RemoteEntry{.fid = "0", .name = "/", .parent_fid = "", .size = 0, .dir = true, .raw = json::object()};
    }

    auto current =
        RemoteEntry{.fid = "0", .name = "/", .parent_fid = "", .size = 0, .dir = true, .raw = json::object()};
    for (const auto &segment : split_remote_path(remote_path))
    {
        if (segment == "." || segment == "..")
            throw QuarkException("Remote paths cannot contain dot segments");
        if (!current.dir)
            throw QuarkException("A remote path component is not a directory");
        auto children = list_directory(current.fid);
        const auto it = std::find_if(children.begin(), children.end(),
                                     [&](const RemoteEntry &entry) { return entry.name == segment; });
        if (it == children.end())
        {
            throw QuarkException("Remote path does not exist: " + remote_path);
        }
        current = *it;
    }
    return current;
}

json QuarkClient::create_folder(const std::string &pdir_fid, const std::string &name) const
{
    validate_name(name);
    json body{
        {"pdir_fid", pdir_fid},
        {"file_name", name},
        {"dir_path", ""},
        {"dir_init_lock", false},
    };

    const auto result = api_post_json(config_.api_base + "/file", body, base_params());
    ensure_api_success(result, "Create folder");
    return result.at("data");
}

json QuarkClient::rename(const std::string &fid, const std::string &new_name) const
{
    validate_name(new_name);
    if (fid.empty() || fid == "0")
        throw QuarkException("Cannot rename the root directory");
    json body{
        {"fid", fid},
        {"file_name", new_name},
    };
    const auto result = api_post_json(config_.api_base + "/file/rename", body, base_params());
    ensure_api_success(result, "Rename");
    return result.at("data");
}

json QuarkClient::delete_items(const std::vector<std::string> &fids) const
{
    if (fids.empty() || std::find(fids.begin(), fids.end(), "0") != fids.end() ||
        std::find(fids.begin(), fids.end(), "") != fids.end())
        throw QuarkException("Invalid file selection");
    json body{
        {"action_type", 1},
        {"exclude_fids", json::array()},
        {"filelist", fids},
    };
    const auto result = api_post_json(config_.api_base + "/file/delete", body, base_params());
    ensure_api_success(result, "Delete files");
    return result.value("data", json::object());
}

json QuarkClient::move_items(const std::vector<std::string> &fids, const std::string &parent_fid) const
{
    if (fids.empty() || std::find(fids.begin(), fids.end(), "0") != fids.end() ||
        std::find(fids.begin(), fids.end(), "") != fids.end())
        throw QuarkException("Invalid file selection");
    const auto result = api_post_json(
        config_.api_base + "/file/move",
        {{"action_type", 1}, {"exclude_fids", json::array()}, {"filelist", fids}, {"to_pdir_fid", parent_fid}},
        base_params());
    return result.value("data", json::object());
}

QuarkClient::UploadPreflight QuarkClient::prepare_upload(const std::filesystem::path &local_path,
                                                         const std::string &pdir_fid,
                                                         const std::string &file_name) const
{
    json body{
        {"ccp_hash_update", true},
        {"pdir_fid", pdir_fid},
        {"dir_name", ""},
        {"file_name", file_name},
        {"format_type", detect_mime_type(local_path)},
        {"l_created_at", unix_time_milliseconds_now()},
        {"l_updated_at", unix_time_milliseconds_now()},
        {"size", std::filesystem::file_size(local_path)},
    };

    const auto result = api_post_json(config_.api_base + "/file/upload/pre", body, base_params());
    ensure_api_success(result, "Upload preflight");

    UploadPreflight preflight;
    preflight.task_id = json_string(result.at("data"), "task_id");
    preflight.fid = json_string(result.at("data"), "fid");
    preflight.obj_key = json_string(result.at("data"), "obj_key");
    preflight.bucket = json_string(result.at("data"), "bucket");
    preflight.upload_url = json_string(result.at("data"), "upload_url");
    preflight.upload_id = json_string(result.at("data"), "upload_id");
    preflight.auth_info = json_string(result.at("data"), "auth_info");
    preflight.callback = result.at("data").value("callback", json::object());
    preflight.part_size = json_u64(result.value("metadata", json::object()), "part_size", 16ULL * 1024ULL * 1024ULL);

    if (preflight.task_id.empty())
    {
        throw QuarkException("Upload preflight is missing task_id");
    }
    return preflight;
}

json QuarkClient::check_fast_upload(const UploadPreflight &preflight, const HashDigests &digests) const
{
    json body{
        {"task_id", preflight.task_id},
        {"md5", digests.md5_hex},
        {"sha1", digests.sha1_hex},
    };

    const auto result = api_post_json(config_.api_base + "/file/update/hash", body, base_params());
    ensure_api_success(result, "Fast upload check");
    return result.value("data", json::object());
}

std::string QuarkClient::get_upload_auth(const UploadPreflight &preflight, const std::string &auth_meta) const
{
    const auto result = api_post_json(config_.api_base + "/file/upload/auth",
                                      json{
                                          {"auth_info", preflight.auth_info},
                                          {"auth_meta", auth_meta},
                                          {"task_id", preflight.task_id},
                                      },
                                      base_params());
    ensure_api_success(result, "Get upload authorization");
    const auto auth_key = json_string(result.at("data"), "auth_key");
    if (auth_key.empty())
    {
        throw QuarkException("Upload authorization response is missing auth_key");
    }
    return auth_key;
}

std::string QuarkClient::make_oss_object_url(const UploadPreflight &preflight) const
{
    std::string object_path;
    for (const auto &segment : split_remote_path(preflight.obj_key))
    {
        if (!object_path.empty())
            object_path += '/';
        object_path += url_encode(segment);
    }
    if (!config_.upload_base.empty())
        return config_.upload_base + "/" + object_path;
    std::string suffix = preflight.upload_url;
    if (suffix.starts_with("https://"))
    {
        suffix.erase(0, 8);
    }
    else if (suffix.starts_with("http://"))
    {
        suffix.erase(0, 7);
    }
    while (suffix.ends_with('/'))
        suffix.pop_back();
    return std::format("https://{}.{}/{}", preflight.bucket, suffix, object_path);
}

std::string QuarkClient::upload_part(const UploadPreflight &preflight, const std::filesystem::path &file_path,
                                     const std::string &mime_type, std::uint64_t offset, std::uint64_t length,
                                     std::uint64_t part_number, const ProgressCallback &progress) const
{
    const auto now = gmt_http_date_now();
    const auto auth_meta =
        std::format("PUT\n\n{}\n{}\nx-oss-date:{}\nx-oss-user-agent:{}\n/{}/{}?partNumber={}&uploadId={}", mime_type,
                    now, now, kOssUserAgent, preflight.bucket, preflight.obj_key, part_number, preflight.upload_id);
    const auto auth_key = get_upload_auth(preflight, auth_meta);

    HeaderList headers{
        {"Authorization", auth_key},
        {"Content-Type", mime_type},
        {"Content-Length", std::to_string(length)},
        {"Referer", "https://pan.quark.cn/"},
        {"x-oss-date", now},
        {"x-oss-user-agent", std::string(kOssUserAgent)},
    };

    const auto response =
        http_.upload_file_range(make_oss_object_url(preflight),
                                {{"partNumber", std::to_string(part_number)}, {"uploadId", preflight.upload_id}},
                                headers, file_path, offset, length, progress);
    if (response.status_code < 200 || response.status_code >= 300)
    {
        throw QuarkException("Part upload failed, HTTP " + std::to_string(response.status_code) +
                             ", part=" + std::to_string(part_number) + ", bytes=" + std::to_string(length) + "; " +
                             oss_error_summary(response));
    }

    auto etag = response.header("etag");
    if (etag.empty())
    {
        throw QuarkException("Part upload response is missing ETag");
    }
    return etag;
}

void QuarkClient::complete_upload(const UploadPreflight &preflight, const std::vector<std::string> &etags) const
{
    std::ostringstream xml;
    xml << R"(<?xml version="1.0" encoding="UTF-8"?>)"
        << "<CompleteMultipartUpload>";
    for (std::size_t index = 0; index < etags.size(); ++index)
    {
        xml << "<Part><PartNumber>" << (index + 1) << "</PartNumber><ETag>" << xml_escape(etags[index])
            << "</ETag></Part>";
    }
    xml << "</CompleteMultipartUpload>";

    const auto xml_body = xml.str();
    const auto content_md5 = md5_base64(xml_body);
    const auto now = gmt_http_date_now();
    const auto callback_b64 = preflight.callback.is_null() || preflight.callback.empty()
                                  ? std::string()
                                  : base64_encode(preflight.callback.dump());

    std::ostringstream auth_meta;
    auth_meta << "POST\n" << content_md5 << "\napplication/xml\n" << now << '\n';
    if (!callback_b64.empty())
    {
        auth_meta << "x-oss-callback:" << callback_b64 << '\n';
    }
    auth_meta << "x-oss-date:" << now << '\n'
              << "x-oss-user-agent:" << kOssUserAgent << '\n'
              << '/' << preflight.bucket << '/' << preflight.obj_key << "?uploadId=" << preflight.upload_id;

    const auto auth_key = get_upload_auth(preflight, auth_meta.str());

    HeaderList headers{
        {"Authorization", auth_key},
        {"Content-MD5", content_md5},
        {"Content-Type", "application/xml"},
        {"Referer", "https://pan.quark.cn/"},
        {"x-oss-date", now},
        {"x-oss-user-agent", std::string(kOssUserAgent)},
    };
    if (!callback_b64.empty())
    {
        headers.emplace_back("x-oss-callback", callback_b64);
    }

    const auto response =
        http_.request("POST", make_oss_object_url(preflight), {{"uploadId", preflight.upload_id}}, headers, xml_body);
    if (response.status_code < 200 || response.status_code >= 300)
    {
        throw QuarkException("Multipart completion failed, HTTP " + std::to_string(response.status_code) + "; " +
                             oss_error_summary(response));
    }
    for (auto position = response.body.find("<Error"); position != std::string::npos;
         position = response.body.find("<Error", position + 6))
    {
        const auto next = position + 6;
        if (next < response.body.size() && (response.body[next] == '>' || response.body[next] == '/' ||
                                            std::isspace(static_cast<unsigned char>(response.body[next]))))
            throw QuarkException("Object storage rejected multipart completion");
    }
    const auto callback_result = json::parse(response.body, nullptr, false);
    if (callback_result.is_object() && (callback_result.contains("code") || callback_result.contains("success")))
        ensure_api_success(callback_result, "Object storage completion callback");
}

json QuarkClient::finish_upload(const UploadPreflight &preflight) const
{
    json body{
        {"task_id", preflight.task_id},
        {"obj_key", preflight.obj_key},
    };

    for (int attempt = 0; attempt < 5; ++attempt)
    {
        const auto result = api_post_json(config_.api_base + "/file/upload/finish", body, base_params());
        ensure_api_success(result, "Finish upload");
        const auto data = result.value("data", json::object());
        if (!data.is_object())
            throw QuarkException("Upload finish returned malformed data");
        if (data.contains("finish") && !data.at("finish").is_boolean())
            throw QuarkException("Upload finish returned a non-boolean confirmation");
        if ((data.contains("finish") && data.at("finish").get<bool>()) ||
            (!data.contains("finish") && !json_string(data, "fid").empty()))
        {
            return data;
        }
        if (!data.contains("finish"))
            throw QuarkException("Upload finish returned no completion confirmation or file ID");
        std::this_thread::sleep_for(std::chrono::seconds(1 + attempt));
    }
    throw QuarkException("Server did not confirm upload completion");
}

std::unordered_map<std::string, std::string>
QuarkClient::request_download_urls(const std::vector<std::string> &fids) const
{
    auto query = base_params();
    query.emplace_back("sys", "win32");
    query.emplace_back("ve", "2.5.56");
    query.emplace_back("ut", "");
    query.emplace_back("guid", "");

    const json body{{"fids", fids}};
    // URL issuance and byte reads must share the desktop identity required by
    // large-file downloads. Changing only the URL request leaves a mismatch.
    auto headers = api_headers();
    for (auto &[key, value] : headers)
        if (to_lower(key) == "user-agent")
            value = kDesktopDownloadUa;
    auto response = http_.request("POST", config_.api_base + "/file/download", query, headers, body.dump());
    refresh_session_cookie(response);
    json result;
    if (!response.body.empty())
        result = response.json_body();

    if (response.status_code < 200 || response.status_code >= 300)
    {
        auto detail = summarize_http_error(response);
        throw QuarkException("HTTP POST failed: " + std::to_string(response.status_code) +
                             (detail.empty() ? std::string() : " | " + detail));
    }
    ensure_api_success(result, "Get download URL");

    std::unordered_map<std::string, std::string> links;
    for (const auto &item : result.at("data"))
    {
        links[json_string(item, "fid")] = json_string(item, "download_url");
    }
    if (fids.size() == 1 && !links.contains(fids.front()) && result.at("data").size() == 1)
        links[fids.front()] = json_string(result.at("data").front(), "download_url");
    return links;
}

UploadResult QuarkClient::upload_file(const std::filesystem::path &local_path, const std::string &pdir_fid,
                                      const std::optional<std::string> &new_name,
                                      const ProgressCallback &progress) const
{
    if (!std::filesystem::is_regular_file(local_path))
        throw QuarkException("Upload source is not a regular file");
    const auto file_name = new_name.value_or(path_to_utf8(local_path.filename()));
    validate_name(file_name);
    const std::uint64_t file_size = std::filesystem::file_size(local_path);
    const auto modified = std::filesystem::last_write_time(local_path);
    const auto unchanged = [&]
    {
        if (std::filesystem::file_size(local_path) != file_size ||
            std::filesystem::last_write_time(local_path) != modified)
            throw QuarkException("Upload source changed during transfer; retry with an immutable file");
    };
    const auto digests = compute_file_hashes(local_path);
    unchanged();
    const auto preflight = prepare_upload(local_path, pdir_fid, file_name);
    const auto fast = check_fast_upload(preflight, digests);
    // The hash endpoint's finish flag is authoritative: a fast upload is already
    // committed and sending a multipart finish for it can fail or create duplicates.
    if (fast.value("finish", false))
    {
        if (progress)
            progress(file_size, file_size);
        return UploadResult{json_string(fast, "fid", preflight.fid), file_name, file_size, true};
    }
    if (preflight.obj_key.empty() || preflight.bucket.empty() || preflight.upload_url.empty() ||
        preflight.upload_id.empty() || preflight.auth_info.empty() || preflight.part_size == 0)
        throw QuarkException("Multipart preflight is missing required fields");
    const auto part_count =
        file_size == 0 ? 1 : file_size / preflight.part_size + (file_size % preflight.part_size != 0);
    if (part_count > 10000)
        throw QuarkException("Provider part size exceeds the OSS 10,000 part limit for this file");
    const auto mime_type = detect_mime_type(local_path);
    std::vector<std::string> etags;
    etags.reserve(static_cast<std::size_t>(part_count));
    std::uint64_t high_water = 0;
    std::exception_ptr callback_error;
    const auto report = [&](std::uint64_t current)
    {
        if (!progress)
            return;
        high_water = std::max(high_water, std::min(current, file_size));
        try
        {
            progress(high_water, file_size);
        }
        catch (...)
        {
            callback_error = std::current_exception();
            throw;
        }
    };
    for (std::uint64_t part = 0; part < part_count; ++part)
    {
        unchanged();
        const auto offset = part * preflight.part_size;
        const auto length = std::min(preflight.part_size, file_size - offset);
        for (int attempt = 0;; ++attempt)
        {
            try
            {
                etags.push_back(upload_part(preflight, local_path, mime_type, offset, length, part + 1,
                                            [&](std::uint64_t current, std::uint64_t) { report(offset + current); }));
                report(offset + length);
                break;
            }
            catch (...)
            {
                if (callback_error)
                    std::rethrow_exception(callback_error);
                if (attempt + 1 >= config_.upload_retry_count)
                    throw;
                std::this_thread::sleep_for(std::chrono::milliseconds(250 * (attempt + 1)));
            }
        }
    }
    unchanged();
    complete_upload(preflight, etags);
    const auto finished = finish_upload(preflight);
    report(file_size);
    return UploadResult{json_string(finished, "fid", preflight.fid), file_name, file_size, false};
}

std::string QuarkClient::download_url(const std::string &fid) const
{
    const auto urls = request_download_urls({fid});
    const auto it = urls.find(fid);
    if (it == urls.end() || it->second.empty())
        throw QuarkException("Provider returned no download URL");
    validate_download_origin(it->second, config_.api_base);
    return it->second;
}

void QuarkClient::collect_download_targets_recursive(const RemoteEntry &entry, const std::filesystem::path &base,
                                                     std::vector<DownloadTarget> &output,
                                                     std::unordered_set<std::string> &visited, unsigned depth) const
{
    if (depth > 128 || !visited.insert(entry.fid).second)
        throw QuarkException("Remote directory cycle or excessive nesting");
    const auto relative = entry.fid == "0" ? base : base / local_name(entry.name);
    output.push_back(DownloadTarget{entry.fid, relative, entry.size, entry.dir});
    if (entry.dir)
        for (const auto &child : list_directory(entry.fid))
            collect_download_targets_recursive(child, relative, output, visited, depth + 1);
}

std::vector<DownloadTarget> QuarkClient::collect_download_targets(const std::string &fid) const
{
    std::vector<DownloadTarget> output;
    std::unordered_set<std::string> visited;
    collect_download_targets_recursive(file_info(fid), {}, output, visited, 0);
    return output;
}

void QuarkClient::download_entry(const RemoteEntry &entry, const std::filesystem::path &destination, bool resume,
                                 const ProgressCallback &progress, const std::string &initial_url) const
{
    if (entry.dir)
        throw QuarkException("download_to_file expects a file");
    reject_symlink_path(destination);
    if (std::filesystem::exists(destination))
    {
        if (!std::filesystem::is_regular_file(destination))
            throw QuarkException("Download destination is not a regular file");
        const auto existing = std::filesystem::file_size(destination);
        if (resume && existing > entry.size)
            throw QuarkException("Local file is larger than remote file; restart without resume");
        if (resume && existing == entry.size)
        {
            if (progress)
                progress(entry.size, entry.size);
            return;
        }
    }
    std::exception_ptr callback_error;
    bool can_resume = resume;
    const auto report = [&](std::uint64_t current, std::uint64_t)
    {
        // A failed URL lookup or rejected HTTP response must never switch a
        // --no-resume retry into appending to an unrelated existing file.
        if (current > 0)
            can_resume = true;
        if (!progress)
            return;
        try
        {
            progress(current, entry.size);
        }
        catch (...)
        {
            callback_error = std::current_exception();
            throw;
        }
    };
    for (int attempt = 0;; ++attempt)
    {
        try
        {
            // Signed URLs expire. Every retry asks the API for a fresh one and
            // resumes the bytes already written by the streaming transport.
            const auto url = attempt == 0 && !initial_url.empty() ? initial_url : download_url(entry.fid);
            validate_download_origin(url, config_.api_base);
            const auto headers = download_headers();
            const auto threads = entry.size > config_.small_file_threshold ? config_.download_threads : 1;
            const auto response =
                http_.download_file(url, headers, destination, can_resume, report, entry.size, threads);
            if ((response.status_code < 200 || response.status_code >= 300) && response.status_code != 416)
                throw QuarkException("Download failed, HTTP " + std::to_string(response.status_code));
            if (!std::filesystem::exists(destination) || std::filesystem::file_size(destination) != entry.size)
                throw QuarkException("Downloaded size does not match remote metadata");
            return;
        }
        catch (...)
        {
            if (callback_error)
                std::rethrow_exception(callback_error);
            if (attempt + 1 >= config_.download_retry_count)
                throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(250 * (attempt + 1)));
        }
    }
}

void QuarkClient::download_to_file(const std::string &fid, const std::filesystem::path &destination, bool resume,
                                   const ProgressCallback &progress) const
{
    // Resolve the caller's directory choice, including macOS /var -> /private/var.
    // Preserve the final component so an existing destination symlink is rejected.
    const auto absolute = std::filesystem::absolute(destination);
    const auto resolved = std::filesystem::weakly_canonical(absolute.parent_path()) / absolute.filename();
    download_entry(file_info(fid), resolved, resume, progress);
}

void QuarkClient::download(const std::string &fid, const std::filesystem::path &output_root, bool resume,
                           const ProgressCallback &progress) const
{
    download_batch({fid}, output_root, resume, progress);
}

void QuarkClient::download_batch(const std::vector<std::string> &fids, const std::filesystem::path &output_root,
                                 bool resume, const ProgressCallback &progress) const
{
    // The caller may deliberately select a directory alias. Only this trusted
    // root is resolved; every remote-derived component below it is checked.
    const auto root = std::filesystem::weakly_canonical(std::filesystem::absolute(output_root));
    std::vector<DownloadTarget> targets;
    std::unordered_set<std::string> roots;
    for (const auto &fid : fids)
    {
        if (!roots.insert(fid).second)
            continue;
        auto selection = collect_download_targets(fid);
        targets.insert(targets.end(), std::make_move_iterator(selection.begin()),
                       std::make_move_iterator(selection.end()));
    }
    // Validate all materialized paths before writing any file.
    std::unordered_set<std::string> names;
    std::unordered_set<std::string> file_names;
    for (const auto &target : targets)
    {
        auto name = path_to_utf8(target.relative_path);
#if defined(_WIN32) || defined(__APPLE__)
        name = to_lower(name);
#endif
        if (!names.insert(name).second)
            throw QuarkException("Remote names collide on the local filesystem");
        if (!target.dir)
            file_names.insert(name);
        const auto destination = root / target.relative_path;
        reject_symlink_path(destination);
        if (std::filesystem::exists(destination) &&
            (target.dir ? !std::filesystem::is_directory(destination) : !std::filesystem::is_regular_file(destination)))
            throw QuarkException("Download destination has an incompatible file type");
        // A selected file must never also be an ancestor of another selection.
        auto parent = target.relative_path.parent_path();
        while (!parent.empty())
        {
            const auto ancestor = root / parent;
            if (std::filesystem::exists(ancestor) && !std::filesystem::is_directory(ancestor))
                throw QuarkException("Download parent is not a directory");
            parent = parent.parent_path();
        }
    }
    for (const auto &target : targets)
    {
        auto parent = target.relative_path.parent_path();
        while (!parent.empty())
        {
            auto ancestor = path_to_utf8(parent);
#if defined(_WIN32) || defined(__APPLE__)
            ancestor = to_lower(ancestor);
#endif
            if (file_names.contains(ancestor))
                throw QuarkException("A selected file overlaps a download directory");
            parent = parent.parent_path();
        }
    }

    std::uint64_t total = 0;
    for (const auto &target : targets)
        if (!target.dir)
        {
            if (target.size > std::numeric_limits<std::uint64_t>::max() - total)
                throw QuarkException("Combined download size exceeds the supported range");
            total += target.size;
            const auto destination = root / target.relative_path;
            if (resume && std::filesystem::exists(destination) && std::filesystem::file_size(destination) > target.size)
                throw QuarkException("Local file is larger than remote file; restart without resume");
        }

    std::vector<std::uint64_t> completed(targets.size(), 0);
    std::uint64_t current = 0;
    std::mutex progress_mutex;
    std::exception_ptr progress_error;
    const auto report = [&](std::size_t index, std::uint64_t bytes)
    {
        std::lock_guard lock(progress_mutex);
        if (progress_error)
            std::rethrow_exception(progress_error);
        const auto next = std::max(completed[index], std::min(bytes, targets[index].size));
        current += next - completed[index];
        completed[index] = next;
        if (progress)
            try
            {
                progress(current, total);
            }
            catch (...)
            {
                progress_error = std::current_exception();
                throw;
            }
    };
    if (progress)
        progress(0, total);
    std::vector<std::size_t> small, large;
    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        const auto &target = targets[i];
        const auto destination = root / target.relative_path;
        if (target.dir)
            std::filesystem::create_directories(destination);
        else
        {
            if (resume && std::filesystem::exists(destination))
            {
                report(i, std::filesystem::file_size(destination));
                if (completed[i] == target.size)
                    continue;
            }
            (target.size <= config_.small_file_threshold ? small : large).push_back(i);
        }
    }

    const auto transfer = [&](std::size_t index, const std::string &url)
    {
        const auto &target = targets[index];
        download_entry(
            RemoteEntry{target.fid, "", "", target.size, false, json::object()}, root / target.relative_path, resume,
            [&](std::uint64_t bytes, std::uint64_t) { report(index, bytes); }, url);
        report(index, target.size);
    };
    const auto run_group = [&](const std::vector<std::size_t> &indices, unsigned jobs)
    {
        for (std::size_t begin = 0; begin < indices.size(); begin += config_.download_batch_size)
        {
            const auto count = std::min<std::size_t>(config_.download_batch_size, indices.size() - begin);
            std::vector<std::string> ids;
            ids.reserve(count);
            for (std::size_t n = 0; n < count; ++n)
                ids.push_back(targets[indices[begin + n]].fid);
            std::unordered_map<std::string, std::string> links;
            for (int attempt = 0;; ++attempt)
                try
                {
                    links = request_download_urls(ids);
                    break;
                }
                catch (...)
                {
                    if (attempt + 1 >= config_.download_retry_count)
                        throw;
                    std::this_thread::sleep_for(std::chrono::milliseconds(250 * (attempt + 1)));
                }
            for (const auto &id : ids)
            {
                const auto link = links.find(id);
                if (link == links.end() || link->second.empty())
                    throw QuarkException("Provider returned no download URL for a selected file");
                validate_download_origin(link->second, config_.api_base);
            }

            std::atomic<std::size_t> next{0};
            std::atomic<bool> stopped{false};
            std::exception_ptr error;
            std::mutex error_mutex;
            const auto worker = [&]
            {
                while (!stopped)
                {
                    const auto item = next.fetch_add(1);
                    if (item >= count)
                        return;
                    try
                    {
                        transfer(indices[begin + item], links.at(ids[item]));
                    }
                    catch (...)
                    {
                        stopped = true;
                        std::lock_guard lock(error_mutex);
                        if (!error)
                            error = std::current_exception();
                    }
                }
            };
            {
                std::vector<std::jthread> workers;
                workers.reserve(std::min<std::size_t>(jobs, count));
                for (std::size_t n = 0; n < std::min<std::size_t>(jobs, count); ++n)
                    workers.emplace_back(worker);
            }
            if (error)
                std::rethrow_exception(error);
        }
    };
    run_group(small, config_.download_jobs);
    // Segmented large-file transfers already have their own bounded workers.
    run_group(large, 1);
}

json QuarkClient::offline_post(const std::string &endpoint, const json &body) const
{
    auto headers = api_headers();
    headers.emplace_back("X-U-Content-Encoding", "wg");
    auto query = base_params();
    query.emplace_back("api_ver", "2");
    const auto response =
        http_.request("POST", config_.offline_api_base + endpoint, query, headers, wg_encode(body.dump(), config_));
    refresh_session_cookie(response);
    if (response.status_code < 200 || response.status_code >= 300)
        throw QuarkException("Offline API HTTP " + std::to_string(response.status_code) + ": " +
                             summarize_http_error(response));
    const auto result = to_lower(response.header("x-u-content-encoding")) == "wg"
                            ? json::parse(wg_decode(response.body, config_))
                            : response.json_body();
    ensure_api_success(result, "Offline API");
    return result.contains("data") && !result.at("data").is_null() ? result.at("data") : result;
}

json QuarkClient::offline_parse(const std::string &source) const
{
    if (!(source.starts_with("magnet:?") || source.starts_with("https://") || source.starts_with("http://")))
        throw QuarkException("Offline source must be a magnet or HTTP(S) URL");
    if (source.find_first_of("\r\n") != std::string::npos)
        throw QuarkException("Invalid offline source");
    const json body{{"url", source},
                    {"fid", ""},
                    {"title", ""},
                    {"parse_mode", 0},
                    {"cookie", ""},
                    {"referer", ""},
                    {"page_url", ""},
                    {"entry", "download"},
                    {"req_info", {{"method", ""}, {"body", ""}, {"is_multipart", false}}},
                    {"conflict_mode", 4},
                    {"save_path", ""},
                    {"auto_download", false},
                    {"support_v2_play", true}};
    for (unsigned attempt = 0; attempt < std::min(15U, config_.task_poll_limit); ++attempt)
    {
        const auto data = offline_post("/offline/download/parse", body);
        if (data.value("finished", false))
        {
            if (json_string(data, "token").empty() || !data.contains("files") || !data.at("files").is_array())
                throw QuarkException("Offline parse response is missing token or file list");
            return data;
        }
        if (attempt + 1 < std::min(15U, config_.task_poll_limit))
            std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    throw QuarkException("Offline resource parsing timed out");
}

json QuarkClient::offline_add(const std::string &source, const std::string &pdir_fid,
                              const std::vector<int> &selected_files) const
{
    const auto parsed = offline_parse(source);
    std::set<int> available;
    for (const auto &file : parsed.at("files"))
    {
        // Quark sometimes groups a folder as comma-separated file_no values.
        const auto ids = json_string(file, "file_no");
        std::istringstream input(ids);
        std::string item;
        while (std::getline(input, item, ','))
        {
            int number = -1;
            const auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), number);
            if (ec != std::errc{} || end != item.data() + item.size() || number < 0)
                throw QuarkException("Invalid provider file_no in offline parse response");
            available.insert(number);
        }
    }
    std::vector<int> selection = selected_files;
    if (selection.empty())
        selection.assign(available.begin(), available.end());
    if (selection.empty())
        throw QuarkException("Offline source contains no selectable files");
    for (int id : selection)
        if (!available.contains(id))
            throw QuarkException("Selected offline file number does not exist");
    std::sort(selection.begin(), selection.end());
    selection.erase(std::unique(selection.begin(), selection.end()), selection.end());
    auto result = offline_post("/offline/download/submit", {{"token", parsed.at("token")},
                                                            {"entry", "download"},
                                                            {"parse_mode", 0},
                                                            {"pdir_fid", pdir_fid},
                                                            {"auto_select", false},
                                                            {"selected_files", selection},
                                                            {"title", json_string(parsed, "title")}});
    const auto id = json_string(result, "task_id");
    if (id.empty())
        throw QuarkException("Offline submission returned no task_id");
    // Submission is an acknowledgement, not a completed cloud download.
    result["state"] = "submitted";
    return result;
}

json QuarkClient::offline_tasks(unsigned page, unsigned size) const
{
    if (page == 0 || size == 0 || size > 100)
        throw QuarkException("Task page must be positive; size must be 1..100");
    auto query = base_params();
    query.insert(query.end(), {{"task_type", "5"},
                               {"status", "0,1,2,3,4"},
                               {"_page", std::to_string(page)},
                               {"_size", std::to_string(size)}});
    const auto result = api_get_json(config_.offline_api_base + "/task/current/list", query);
    return result.value("data", json::object());
}

json QuarkClient::task_status(const std::string &task_id) const
{
    if (task_id.empty())
        throw QuarkException("Missing task ID");
    auto query = base_params();
    query.emplace_back("task_id", task_id);
    const auto result = api_get_json(config_.offline_api_base + "/task", query);
    const auto data = result.contains("data") && !result.at("data").is_null() ? result.at("data") : result;
    if (!data.contains("status"))
        throw QuarkException("Task response did not contain status");
    return data;
}

json QuarkClient::wait_task(const std::string &task_id) const
{
    for (unsigned attempt = 0; attempt < config_.task_poll_limit; ++attempt)
    {
        auto data = task_status(task_id);
        const auto status = json_u64(data, "status", 99);
        if (status == 2)
            return data;
        if (status == 3 || status == 4)
            throw QuarkException("Cloud task " + task_id + (status == 3 ? " failed" : " was cancelled"));
        if (status > 4)
            throw QuarkException("Unrecognized cloud task status");
        if (attempt + 1 < config_.task_poll_limit)
            std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    throw QuarkException("Cloud task is still pending; query task ID " + task_id + " again later");
}

} // namespace quarkpp
