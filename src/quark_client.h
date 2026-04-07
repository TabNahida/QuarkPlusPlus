#pragma once

#include "support.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace quarkpp
{

struct AppConfig
{
    std::string cookie;
    std::string user_agent;
    std::filesystem::path download_dir{"downloads"};
    unsigned request_timeout_ms{120000};
    int upload_retry_count{3};
};

struct RemoteEntry
{
    std::string fid;
    std::string name;
    std::string parent_fid;
    std::uint64_t size{};
    bool dir{false};
    json raw;
};

struct UploadResult
{
    std::string fid;
    std::string file_name;
    std::uint64_t size{};
    bool fast_upload{false};
};

struct DownloadTarget
{
    std::string fid;
    std::filesystem::path relative_path;
    std::uint64_t size{};
};

class QuarkClient
{
  public:
    explicit QuarkClient(AppConfig config);

    [[nodiscard]] const AppConfig &config() const;

    [[nodiscard]] json account_info() const;
    [[nodiscard]] json growth_info() const;
    [[nodiscard]] json growth_sign() const;

    [[nodiscard]] std::vector<RemoteEntry> list_directory(const std::string &pdir_fid) const;
    [[nodiscard]] std::vector<RemoteEntry> search(const std::string &keyword, const std::string &pdir_fid) const;
    [[nodiscard]] RemoteEntry file_info(const std::string &fid) const;
    [[nodiscard]] RemoteEntry resolve_path(const std::string &remote_path) const;

    [[nodiscard]] json create_folder(const std::string &pdir_fid, const std::string &name) const;
    [[nodiscard]] json rename(const std::string &fid, const std::string &new_name) const;
    [[nodiscard]] json delete_items(const std::vector<std::string> &fids) const;

    [[nodiscard]] json share_create(const std::vector<std::string> &fids, const std::string &title, int expired_type,
                                    int url_type, const std::optional<std::string> &passcode) const;
    [[nodiscard]] json share_list(int page, int size) const;
    [[nodiscard]] json transfer_share(const std::string &share_url, const std::string &to_pdir_fid,
                                      const std::optional<std::string> &passcode) const;

    [[nodiscard]] UploadResult upload_file(const std::filesystem::path &local_path, const std::string &pdir_fid,
                                           const std::optional<std::string> &new_name = std::nullopt) const;

    [[nodiscard]] std::vector<DownloadTarget> collect_download_targets(const std::string &fid) const;
    void download(const std::string &fid, const std::filesystem::path &output_root, bool resume) const;

  private:
    struct ParsedShare
    {
        std::string pwd_id;
        std::string share_fid{"0"};
        std::optional<std::string> passcode;
    };

    struct UploadPreflight
    {
        std::string task_id;
        std::string fid;
        std::string obj_key;
        std::string bucket;
        std::string upload_url;
        std::string upload_id;
        std::string auth_info;
        json callback;
        std::uint64_t part_size{};
        json raw;
    };

    [[nodiscard]] QueryList base_params() const;
    [[nodiscard]] HeaderList api_headers() const;
    [[nodiscard]] HeaderList download_api_headers(bool desktop_ua) const;
    [[nodiscard]] HeaderList file_download_headers() const;
    [[nodiscard]] json api_get_json(const std::string &url, const QueryList &query = {},
                                    const HeaderList &headers = {}) const;
    [[nodiscard]] json api_post_json(const std::string &url, const json &body, const QueryList &query = {},
                                     const HeaderList &headers = {}) const;
    [[nodiscard]] json poll_task(const std::string &task_id, int max_retries = 20) const;
    [[nodiscard]] ParsedShare parse_share_url(const std::string &share_url,
                                              const std::optional<std::string> &passcode) const;
    [[nodiscard]] std::string get_share_token(const std::string &pwd_id,
                                              const std::optional<std::string> &passcode) const;
    [[nodiscard]] json get_share_detail(const std::string &pwd_id, const std::string &stoken,
                                        const std::string &pdir_fid) const;
    [[nodiscard]] UploadPreflight prepare_upload(const std::filesystem::path &local_path, const std::string &pdir_fid,
                                                 const std::string &file_name) const;
    [[nodiscard]] json check_fast_upload(const UploadPreflight &preflight, const HashDigests &digests) const;
    [[nodiscard]] std::string get_upload_auth(const UploadPreflight &preflight, const std::string &auth_meta) const;
    [[nodiscard]] std::string make_oss_object_url(const UploadPreflight &preflight) const;
    [[nodiscard]] std::string upload_part(const UploadPreflight &preflight, const std::filesystem::path &file_path,
                                          const std::string &mime_type, std::uint64_t offset, std::uint64_t length,
                                          std::uint64_t part_number, const ProgressCallback &progress) const;
    void complete_upload(const UploadPreflight &preflight, const std::vector<std::string> &etags) const;
    [[nodiscard]] json finish_upload(const UploadPreflight &preflight) const;
    [[nodiscard]] std::unordered_map<std::string, std::string> request_download_urls(
        const std::vector<std::string> &fids) const;
    void collect_download_targets_recursive(const RemoteEntry &entry, const std::filesystem::path &base,
                                            std::vector<DownloadTarget> &output) const;

    AppConfig config_;
    HttpClient http_;
};

} // namespace quarkpp
