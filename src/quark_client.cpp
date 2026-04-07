#include "quark_client.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <thread>

namespace quarkpp
{

namespace
{

constexpr std::string_view kApiBase = "https://drive-pc.quark.cn/1/clouddrive";
constexpr std::string_view kApiBaseAlt = "https://drive.quark.cn/1/clouddrive";
constexpr std::string_view kPanBase = "https://pan.quark.cn";
constexpr std::string_view kGrowthBase = "https://drive-m.quark.cn/1/clouddrive";
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
    if (field.is_number_integer())
    {
        return std::to_string(field.get<long long>());
    }
    if (field.is_number_unsigned())
    {
        return std::to_string(field.get<unsigned long long>());
    }
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
        std::from_chars(field.get_ref<const std::string &>().data(),
                        field.get_ref<const std::string &>().data() + field.get_ref<const std::string &>().size(),
                        parsed);
        return parsed;
    }
    return fallback;
}

[[nodiscard]] bool is_directory_entry(const json &value)
{
    if (value.contains("dir") && value.at("dir").is_boolean())
    {
        return value.at("dir").get<bool>();
    }
    if (value.contains("file_type") && value.at("file_type").is_string())
    {
        return value.at("file_type").get<std::string>() == "folder";
    }
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
    return entry;
}

void ensure_api_success(const json &result, const std::string &action)
{
    if (result.contains("success") && result.at("success").is_boolean() && result.at("success").get<bool>())
    {
        return;
    }
    if (result.value("code", 0) == 0)
    {
        return;
    }
    throw QuarkException(action + "失败: " + result.value("message", "未知错误"));
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

[[nodiscard]] int extract_gap_ms(const json &task_result)
{
    if (!task_result.contains("metadata"))
    {
        return 1000;
    }
    const auto gap = json_u64(task_result.at("metadata"), "tq_gap", 1000);
    return static_cast<int>(std::clamp<std::uint64_t>(gap, 300, 5000));
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

} // namespace

QuarkClient::QuarkClient(AppConfig config) : config_(std::move(config)), http_(config_.request_timeout_ms)
{
    if (config_.cookie.empty())
    {
        throw QuarkException("缺少夸克 cookie，请在本地配置文件或 QUARKPP_COOKIE 中提供");
    }
    if (config_.user_agent.empty())
    {
        config_.user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                             "(KHTML, like Gecko) Chrome/137.0.0.0 Safari/537.36";
    }
    config_.upload_retry_count = clamp_retry_count(config_.upload_retry_count);
}

const AppConfig &QuarkClient::config() const
{
    return config_;
}

QueryList QuarkClient::base_params() const
{
    return {{"pr", "ucpro"}, {"fr", "pc"}, {"uc_param_str", ""}};
}

HeaderList QuarkClient::api_headers() const
{
    return {
        {"Cookie", config_.cookie},
        {"Accept", "application/json, text/plain, */*"},
        {"Accept-Language", "zh-CN,zh;q=0.9"},
        {"Content-Type", "application/json"},
        {"Origin", "https://pan.quark.cn"},
        {"Referer", "https://pan.quark.cn/"},
        {"Priority", "u=1, i"},
        {"User-Agent", config_.user_agent},
    };
}

HeaderList QuarkClient::download_api_headers(bool desktop_ua) const
{
    auto headers = api_headers();
    for (auto &[key, value] : headers)
    {
        if (to_lower(key) == "user-agent")
        {
            value = desktop_ua ? std::string(kDesktopDownloadUa) : config_.user_agent;
        }
    }
    headers.emplace_back("Accept", "application/json, text/plain, */*");
    return headers;
}

HeaderList QuarkClient::file_download_headers() const
{
    return {
        {"Cookie", config_.cookie},
        {"Origin", "https://pan.quark.cn"},
        {"Referer", "https://pan.quark.cn/"},
        {"User-Agent", config_.user_agent},
    };
}

json QuarkClient::api_get_json(const std::string &url, const QueryList &query, const HeaderList &headers) const
{
    auto merged_headers = api_headers();
    merged_headers.insert(merged_headers.end(), headers.begin(), headers.end());
    auto response = http_.request("GET", url, query, merged_headers);
    if (response.status_code < 200 || response.status_code >= 300)
    {
        auto detail = summarize_http_error(response);
        throw QuarkException("HTTP GET 失败: " + std::to_string(response.status_code) +
                             (detail.empty() ? std::string() : " | " + detail));
    }
    return response.json_body();
}

json QuarkClient::api_post_json(const std::string &url, const json &body, const QueryList &query,
                                const HeaderList &headers) const
{
    auto merged_headers = api_headers();
    merged_headers.insert(merged_headers.end(), headers.begin(), headers.end());
    auto response = http_.request("POST", url, query, merged_headers, body.dump());
    if (response.status_code < 200 || response.status_code >= 300)
    {
        auto detail = summarize_http_error(response);
        throw QuarkException("HTTP POST 失败: " + std::to_string(response.status_code) +
                             (detail.empty() ? std::string() : " | " + detail));
    }
    return response.json_body();
}

json QuarkClient::account_info() const
{
    const auto result = api_get_json(std::string(kPanBase) + "/account/info", {{"platform", "pc"}, {"fr", "pc"}});
    ensure_api_success(result, "读取账号信息");
    const auto data = result.value("data", json::object());
    if (data.is_null() || (data.is_object() && data.empty()))
    {
        throw QuarkException("账号接口返回空数据；当前 cookie 很可能未登录、已过期，或不被夸克网盘接口识别");
    }
    return data;
}

json QuarkClient::growth_info() const
{
    const auto result = api_get_json(std::string(kGrowthBase) + "/capacity/growth/info", base_params());
    ensure_api_success(result, "读取容量签到信息");
    return result.at("data");
}

json QuarkClient::growth_sign() const
{
    const auto result =
        api_post_json(std::string(kGrowthBase) + "/capacity/growth/sign", json::object(), base_params());
    ensure_api_success(result, "容量签到");
    return result.at("data");
}

std::vector<RemoteEntry> QuarkClient::list_directory(const std::string &pdir_fid) const
{
    std::vector<RemoteEntry> entries;
    constexpr int page_size = 100;
    int page = 1;

    for (;;)
    {
        auto query = base_params();
        query.emplace_back("pdir_fid", pdir_fid);
        query.emplace_back("_page", std::to_string(page));
        query.emplace_back("_size", std::to_string(page_size));
        query.emplace_back("_fetch_total", "1");
        query.emplace_back("_fetch_sub_dirs", "0");
        query.emplace_back("_sort", "file_type:asc,file_name:asc");

        const auto result = api_get_json(std::string(kApiBase) + "/file/sort", query);
        ensure_api_success(result, "列目录");

        const auto list = result.at("data").at("list");
        for (const auto &item : list)
        {
            entries.push_back(make_remote_entry(item));
        }

        const auto metadata = result.value("metadata", json::object());
        const auto total = json_u64(metadata, "_total", static_cast<std::uint64_t>(entries.size()));
        if (list.empty() || entries.size() >= total || list.size() < page_size)
        {
            break;
        }
        ++page;
    }
    return entries;
}

std::vector<RemoteEntry> QuarkClient::search(const std::string &keyword, const std::string &pdir_fid) const
{
    auto query = base_params();
    query.emplace_back("pdir_fid", pdir_fid);
    query.emplace_back("query", keyword);

    const auto result = api_get_json(std::string(kApiBase) + "/file/search", query);
    ensure_api_success(result, "搜索文件");

    std::vector<RemoteEntry> entries;
    for (const auto &item : result.at("data").at("list"))
    {
        entries.push_back(make_remote_entry(item));
    }
    return entries;
}

RemoteEntry QuarkClient::file_info(const std::string &fid) const
{
    auto query = base_params();
    query.emplace_back("fid", fid);

    const auto result = api_get_json(std::string(kApiBase) + "/file/info", query);
    ensure_api_success(result, "读取文件信息");
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
        auto children = list_directory(current.fid);
        const auto it = std::find_if(children.begin(), children.end(),
                                     [&](const RemoteEntry &entry) { return entry.name == segment; });
        if (it == children.end())
        {
            throw QuarkException("远端路径不存在: " + remote_path);
        }
        current = *it;
    }
    return current;
}

json QuarkClient::create_folder(const std::string &pdir_fid, const std::string &name) const
{
    json body{
        {"pdir_fid", pdir_fid},
        {"file_name", name},
        {"dir_path", ""},
        {"dir_init_lock", false},
    };

    const auto result = api_post_json(std::string(kApiBase) + "/file", body, base_params());
    ensure_api_success(result, "创建文件夹");
    return result.at("data");
}

json QuarkClient::rename(const std::string &fid, const std::string &new_name) const
{
    json body{
        {"fid", fid},
        {"file_name", new_name},
    };
    const auto result = api_post_json(std::string(kApiBase) + "/file/rename", body, base_params());
    ensure_api_success(result, "重命名");
    return result.at("data");
}

json QuarkClient::delete_items(const std::vector<std::string> &fids) const
{
    json body{
        {"action_type", 2},
        {"exclude_fids", json::array()},
        {"filelist", fids},
    };
    const auto result = api_post_json(std::string(kApiBase) + "/file/delete", body, base_params());
    ensure_api_success(result, "删除文件");
    return result.value("data", json::object());
}

json QuarkClient::poll_task(const std::string &task_id, int max_retries) const
{
    for (int index = 0; index < max_retries; ++index)
    {
        auto query = base_params();
        query.emplace_back("task_id", task_id);
        query.emplace_back("retry_index", std::to_string(index));
        query.emplace_back("__dt", std::to_string(unix_time_milliseconds_now() % 1000));
        query.emplace_back("__t", std::to_string(unix_time_milliseconds_now()));

        const auto result = api_get_json(std::string(kApiBase) + "/task", query);
        ensure_api_success(result, "轮询任务");

        const auto &data = result.at("data");
        const auto status = json_u64(data, "status", 0);
        if (status == 2)
        {
            return data;
        }
        if (status == 3)
        {
            throw QuarkException("任务失败: " + json_string(data, "message", result.value("message", "未知错误")));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(extract_gap_ms(result)));
    }

    throw QuarkException("任务轮询超时: " + task_id);
}

QuarkClient::ParsedShare QuarkClient::parse_share_url(const std::string &share_url,
                                                      const std::optional<std::string> &passcode) const
{
    static const std::regex pattern(R"(/s/([A-Za-z0-9]+)(?:#/list/share[^/]*/([A-Za-z0-9]+))?)");
    static const std::regex passcode_pattern(R"((?:pwd=|提取码[:：])([A-Za-z0-9]+))");

    std::smatch match;
    if (!std::regex_search(share_url, match, pattern))
    {
        throw QuarkException("无效的分享链接");
    }

    ParsedShare parsed;
    parsed.pwd_id = match[1].str();
    if (match.size() >= 3 && match[2].matched)
    {
        parsed.share_fid = match[2].str();
    }

    if (passcode && !passcode->empty())
    {
        parsed.passcode = passcode;
        return parsed;
    }

    std::smatch passcode_match;
    if (std::regex_search(share_url, passcode_match, passcode_pattern))
    {
        parsed.passcode = passcode_match[1].str();
    }
    return parsed;
}

std::string QuarkClient::get_share_token(const std::string &pwd_id, const std::optional<std::string> &passcode) const
{
    auto query = base_params();
    query.emplace_back("__dt", std::to_string(unix_time_milliseconds_now() % 1000));
    query.emplace_back("__t", std::to_string(unix_time_milliseconds_now()));

    json body{
        {"pwd_id", pwd_id},
        {"passcode", passcode.value_or("")},
    };

    const auto result = api_post_json(std::string(kApiBase) + "/share/sharepage/token", body, query);
    ensure_api_success(result, "获取分享 token");

    if (!result.contains("data") || !result.at("data").contains("stoken"))
    {
        throw QuarkException("分享 token 响应缺少 stoken");
    }
    return result.at("data").at("stoken").get<std::string>();
}

json QuarkClient::get_share_detail(const std::string &pwd_id, const std::string &stoken,
                                   const std::string &pdir_fid) const
{
    json combined{
        {"share", json::object()},
        {"list", json::array()},
    };

    constexpr int page_size = 50;
    int page = 1;
    for (;;)
    {
        auto query = base_params();
        query.emplace_back("pwd_id", pwd_id);
        query.emplace_back("stoken", stoken);
        query.emplace_back("pdir_fid", pdir_fid);
        query.emplace_back("force", "0");
        query.emplace_back("_page", std::to_string(page));
        query.emplace_back("_size", std::to_string(page_size));
        query.emplace_back("_fetch_total", "1");
        query.emplace_back("_sort", "file_type:asc,updated_at:desc");

        const auto result = api_get_json(std::string(kApiBase) + "/share/sharepage/detail", query);
        ensure_api_success(result, "读取分享详情");

        if (combined.at("share").empty() && result.at("data").contains("share"))
        {
            combined["share"] = result.at("data").at("share");
        }

        const auto &list = result.at("data").at("list");
        for (const auto &item : list)
        {
            combined["list"].push_back(item);
        }

        const auto metadata = result.value("metadata", json::object());
        const auto total = json_u64(metadata, "_total", combined.at("list").size());
        if (list.empty() || combined.at("list").size() >= total || list.size() < page_size)
        {
            break;
        }
        ++page;
    }

    return combined;
}

json QuarkClient::share_create(const std::vector<std::string> &fids, const std::string &title, int expired_type,
                               int url_type, const std::optional<std::string> &passcode) const
{
    auto query = base_params();
    query.emplace_back("__dt", std::to_string(unix_time_milliseconds_now() % 1000));
    query.emplace_back("__t", std::to_string(unix_time_milliseconds_now()));

    json body{
        {"fid_list", fids},
        {"title", title},
        {"expired_type", expired_type},
        {"url_type", url_type},
    };
    if (passcode && !passcode->empty())
    {
        body["passcode"] = *passcode;
    }

    const auto result = api_post_json(std::string(kApiBase) + "/share", body, query);
    ensure_api_success(result, "创建分享");

    const auto task_id = json_string(result.at("data"), "task_id");
    if (task_id.empty())
    {
        throw QuarkException("分享任务未返回 task_id");
    }

    auto task = poll_task(task_id);
    const auto share_id = json_string(task, "share_id");
    if (share_id.empty())
    {
        throw QuarkException("分享任务完成后未返回 share_id");
    }

    const auto password_info =
        api_post_json(std::string(kApiBase) + "/share/password", json{{"share_id", share_id}}, base_params());
    ensure_api_success(password_info, "获取分享链接");

    json output = password_info.at("data");
    output["share_id"] = share_id;
    output["title"] = title;
    if (!output.contains("share_url") && output.contains("pwd_id"))
    {
        output["share_url"] = std::string("https://pan.quark.cn/s/") + output.at("pwd_id").get<std::string>();
    }
    return output;
}

json QuarkClient::share_list(int page, int size) const
{
    auto query = base_params();
    query.emplace_back("_page", std::to_string(page));
    query.emplace_back("_size", std::to_string(size));
    query.emplace_back("_order_field", "created_at");
    query.emplace_back("_order_type", "desc");
    query.emplace_back("_fetch_total", "1");
    query.emplace_back("_fetch_notify_follow", "1");

    const auto result = api_get_json(std::string(kApiBase) + "/share/mypage/detail", query);
    ensure_api_success(result, "读取分享列表");
    return result;
}

json QuarkClient::transfer_share(const std::string &share_url, const std::string &to_pdir_fid,
                                 const std::optional<std::string> &passcode) const
{
    const auto parsed = parse_share_url(share_url, passcode);
    const auto stoken = get_share_token(parsed.pwd_id, parsed.passcode);
    const auto detail = get_share_detail(parsed.pwd_id, stoken, parsed.share_fid);

    std::vector<std::string> fid_list;
    std::vector<std::string> fid_token_list;
    for (const auto &item : detail.at("list"))
    {
        const auto fid = json_string(item, "fid");
        const auto token = json_string(item, "share_fid_token");
        if (!fid.empty() && !token.empty())
        {
            fid_list.push_back(fid);
            fid_token_list.push_back(token);
        }
    }

    if (fid_list.empty() || fid_list.size() != fid_token_list.size())
    {
        throw QuarkException("无法从分享详情中提取 fid 与 share_fid_token");
    }

    auto query = base_params();
    query.emplace_back("__dt", std::to_string(unix_time_milliseconds_now() % 1000));
    query.emplace_back("__t", std::to_string(unix_time_milliseconds_now()));

    json body{
        {"fid_list", fid_list},
        {"fid_token_list", fid_token_list},
        {"to_pdir_fid", to_pdir_fid},
        {"pwd_id", parsed.pwd_id},
        {"stoken", stoken},
        {"pdir_fid", parsed.share_fid},
        {"scene", "link"},
    };

    auto result = api_post_json(std::string(kApiBase) + "/share/sharepage/save", body, query);
    if (result.value("code", 0) != 0)
    {
        result = api_post_json(std::string(kApiBaseAlt) + "/share/sharepage/save", body, query);
    }
    ensure_api_success(result, "转存分享");

    const auto task_id = json_string(result.at("data"), "task_id");
    if (task_id.empty())
    {
        throw QuarkException("转存任务未返回 task_id");
    }
    return poll_task(task_id);
}

QuarkClient::UploadPreflight QuarkClient::prepare_upload(const std::filesystem::path &local_path,
                                                         const std::string &pdir_fid,
                                                         const std::string &file_name) const
{
    json body{
        {"ccp_hash_update", true},
        {"parallel_upload", true},
        {"pdir_fid", pdir_fid},
        {"dir_name", ""},
        {"file_name", file_name},
        {"format_type", detect_mime_type(local_path)},
        {"l_created_at", unix_time_milliseconds_now()},
        {"l_updated_at", unix_time_milliseconds_now()},
        {"size", std::filesystem::file_size(local_path)},
    };

    const auto result = api_post_json(std::string(kApiBase) + "/file/upload/pre", body, base_params());
    ensure_api_success(result, "预上传");

    UploadPreflight preflight;
    preflight.task_id = json_string(result.at("data"), "task_id");
    preflight.fid = json_string(result.at("data"), "fid");
    preflight.obj_key = json_string(result.at("data"), "obj_key");
    preflight.bucket = json_string(result.at("data"), "bucket");
    preflight.upload_url = json_string(result.at("data"), "upload_url");
    preflight.upload_id = json_string(result.at("data"), "upload_id");
    preflight.auth_info = json_string(result.at("data"), "auth_info");
    preflight.callback = result.at("data").value("callback", json::object());
    preflight.part_size = json_u64(result.at("metadata"), "part_size", 16ULL * 1024ULL * 1024ULL);
    preflight.raw = result;

    if (preflight.task_id.empty() || preflight.obj_key.empty() || preflight.bucket.empty() ||
        preflight.upload_url.empty())
    {
        throw QuarkException("预上传响应缺少关键字段");
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

    const auto result = api_post_json(std::string(kApiBase) + "/file/update/hash", body, base_params());
    ensure_api_success(result, "秒传检查");
    return result.value("data", json::object());
}

std::string QuarkClient::get_upload_auth(const UploadPreflight &preflight, const std::string &auth_meta) const
{
    const auto result = api_post_json(std::string(kApiBase) + "/file/upload/auth",
                                      json{
                                          {"auth_info", preflight.auth_info},
                                          {"auth_meta", auth_meta},
                                          {"task_id", preflight.task_id},
                                      },
                                      base_params());
    ensure_api_success(result, "获取上传授权");
    const auto auth_key = json_string(result.at("data"), "auth_key");
    if (auth_key.empty())
    {
        throw QuarkException("上传授权响应缺少 auth_key");
    }
    return auth_key;
}

std::string QuarkClient::make_oss_object_url(const UploadPreflight &preflight) const
{
    std::string suffix = preflight.upload_url;
    if (suffix.starts_with("https://"))
    {
        suffix.erase(0, 8);
    }
    else if (suffix.starts_with("http://"))
    {
        suffix.erase(0, 7);
    }
    return std::format("https://{}.{}/{}", preflight.bucket, suffix, preflight.obj_key);
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
        throw QuarkException("分片上传失败，HTTP 状态码: " + std::to_string(response.status_code));
    }

    auto etag = response.header("etag");
    if (etag.empty())
    {
        throw QuarkException("分片上传成功但响应中没有 ETag");
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
        xml << "<Part><PartNumber>" << (index + 1) << "</PartNumber><ETag>" << etags[index] << "</ETag></Part>";
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
        throw QuarkException("合并上传分片失败，HTTP 状态码: " + std::to_string(response.status_code));
    }
}

json QuarkClient::finish_upload(const UploadPreflight &preflight) const
{
    json body{
        {"task_id", preflight.task_id},
        {"obj_key", preflight.obj_key},
    };

    for (int attempt = 0; attempt < 5; ++attempt)
    {
        const auto result = api_post_json(std::string(kApiBase) + "/file/upload/finish", body, base_params());
        ensure_api_success(result, "通知服务器完成上传");
        const auto data = result.value("data", json::object());
        if (data.value("finish", false))
        {
            return data;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1 + attempt));
    }
    throw QuarkException("服务器未确认上传完成");
}

UploadResult QuarkClient::upload_file(const std::filesystem::path &local_path, const std::string &pdir_fid,
                                      const std::optional<std::string> &new_name) const
{
    if (!std::filesystem::exists(local_path) || !std::filesystem::is_regular_file(local_path))
    {
        throw QuarkException("本地文件不存在: " + path_to_utf8(local_path));
    }

    const auto file_name = new_name.value_or(path_to_utf8(local_path.filename()));
    const auto file_size = std::filesystem::file_size(local_path);
    const auto mime_type = detect_mime_type(local_path);
    const auto digests = compute_file_hashes(local_path);
    const auto preflight = prepare_upload(local_path, pdir_fid, file_name);
    const auto fast_upload = check_fast_upload(preflight, digests);

    if (fast_upload.value("finish", false))
    {
        const auto finished = finish_upload(preflight);
        return UploadResult{
            .fid = json_string(finished, "fid", preflight.fid),
            .file_name = file_name,
            .size = file_size,
            .fast_upload = true,
        };
    }

    std::vector<std::string> etags;
    ProgressBar progress_bar("upload " + file_name);

    std::uint64_t uploaded = 0;
    const auto part_size = preflight.part_size == 0 ? 16ULL * 1024ULL * 1024ULL : preflight.part_size;
    const auto part_count = (file_size + part_size - 1) / part_size;

    for (std::uint64_t part = 0; part < part_count; ++part)
    {
        const auto offset = part * part_size;
        const auto length = std::min<std::uint64_t>(part_size, file_size - offset);

        std::exception_ptr last_error;
        for (int attempt = 0; attempt < config_.upload_retry_count; ++attempt)
        {
            try
            {
                const auto etag = upload_part(
                    preflight, local_path, mime_type, offset, length, part + 1,
                    [&](std::uint64_t current, std::uint64_t) { progress_bar.update(uploaded + current, file_size); });
                etags.push_back(etag);
                uploaded += length;
                progress_bar.update(uploaded, file_size);
                last_error = nullptr;
                break;
            }
            catch (...)
            {
                last_error = std::current_exception();
                std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
            }
        }

        if (last_error)
        {
            std::rethrow_exception(last_error);
        }
    }

    complete_upload(preflight, etags);
    progress_bar.finish();
    const auto finished = finish_upload(preflight);

    return UploadResult{
        .fid = json_string(finished, "fid", preflight.fid),
        .file_name = file_name,
        .size = file_size,
        .fast_upload = false,
    };
}

std::unordered_map<std::string, std::string> QuarkClient::request_download_urls(
    const std::vector<std::string> &fids) const
{
    auto query = base_params();
    query.emplace_back("sys", "win32");
    query.emplace_back("ve", "2.5.56");
    query.emplace_back("ut", "");
    query.emplace_back("guid", "");

    const json body{{"fids", fids}};
    const auto request_download = [&](bool desktop_ua) -> std::pair<HttpResponse, json> {
        auto headers = download_api_headers(desktop_ua);
        auto response = http_.request("POST", std::string(kApiBase) + "/file/download", query, headers, body.dump());
        json result;
        if (!response.body.empty())
        {
            result = response.json_body();
        }
        return {std::move(response), std::move(result)};
    };

    auto [response, result] = request_download(false);
    const auto result_code = result.is_object() ? result.value("code", -1) : -1;
    const auto result_message = result.is_object() ? result.value("message", "") : std::string();
    if (result_code == 23018 || result_message.contains("download file size limit"))
    {
        std::tie(response, result) = request_download(true);
    }

    if (response.status_code < 200 || response.status_code >= 300)
    {
        auto detail = summarize_http_error(response);
        throw QuarkException("HTTP POST 失败: " + std::to_string(response.status_code) +
                             (detail.empty() ? std::string() : " | " + detail));
    }
    ensure_api_success(result, "获取下载链接");

    std::unordered_map<std::string, std::string> links;
    for (const auto &item : result.at("data"))
    {
        links[json_string(item, "fid")] = json_string(item, "download_url");
    }
    return links;
}

void QuarkClient::collect_download_targets_recursive(const RemoteEntry &entry, const std::filesystem::path &base,
                                                     std::vector<DownloadTarget> &output) const
{
    if (!entry.dir)
    {
        output.push_back(DownloadTarget{
            .fid = entry.fid,
            .relative_path = base / path_from_utf8(entry.name),
            .size = entry.size,
        });
        return;
    }

    const auto next_base = entry.fid == "0" ? base : base / entry.name;
    for (const auto &child : list_directory(entry.fid))
    {
        collect_download_targets_recursive(child, next_base, output);
    }
}

std::vector<DownloadTarget> QuarkClient::collect_download_targets(const std::string &fid) const
{
    std::vector<DownloadTarget> output;
    if (fid == "0")
    {
        collect_download_targets_recursive(
            RemoteEntry{.fid = "0", .name = "", .parent_fid = "", .size = 0, .dir = true, .raw = json::object()}, {},
            output);
        return output;
    }
    collect_download_targets_recursive(file_info(fid), {}, output);
    return output;
}

void QuarkClient::download(const std::string &fid, const std::filesystem::path &output_root, bool resume) const
{
    auto targets = collect_download_targets(fid);
    if (targets.empty())
    {
        throw QuarkException("没有可下载的文件");
    }

    constexpr std::size_t batch_size = 20;
    for (std::size_t offset = 0; offset < targets.size(); offset += batch_size)
    {
        const auto end = std::min<std::size_t>(targets.size(), offset + batch_size);
        std::vector<std::string> batch_fids;
        batch_fids.reserve(end - offset);
        for (std::size_t index = offset; index < end; ++index)
        {
            batch_fids.push_back(targets[index].fid);
        }

        const auto urls = request_download_urls(batch_fids);
        for (std::size_t index = offset; index < end; ++index)
        {
            const auto &target = targets[index];
            const auto it = urls.find(target.fid);
            if (it == urls.end() || it->second.empty())
            {
                throw QuarkException("下载链接缺失，fid=" + target.fid);
            }

            const auto destination = output_root / target.relative_path;
            ProgressBar progress_bar("download " + path_to_utf8(target.relative_path.filename()));
            const auto response = http_.download_file(
                it->second, file_download_headers(), destination, resume,
                [&](std::uint64_t current, std::uint64_t total) { progress_bar.update(current, total); });
            progress_bar.finish();
            if (response.status_code == 416)
            {
                std::cout << path_to_utf8(destination) << " already complete\n";
                continue;
            }
            if (response.status_code < 200 || response.status_code >= 300)
            {
                throw QuarkException("下载失败，HTTP 状态码: " + std::to_string(response.status_code));
            }
        }
    }
}

} // namespace quarkpp
