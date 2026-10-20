#pragma once

#include "quarkpp/support.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace quarkpp
{

struct AppConfig
{
    std::string cookie;
    std::string user_agent;
    std::filesystem::path download_dir{"downloads"};
    unsigned request_timeout_ms{120000};
    std::string ca_bundle;
    int upload_retry_count{3};
    int download_retry_count{3};
    unsigned task_poll_limit{30};
    std::string api_base{"https://drive.quark.cn/1/clouddrive"};
    std::string offline_api_base{"https://drive.quark.cn/1/clouddrive"};
    // Optional object-storage gateway, primarily for integration tests/proxies.
    std::string upload_base;
    std::uint16_t offline_secure_number{14100};
    std::string offline_key{"e21247a68938d36f"};
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
    bool dir{false};
};

// Synchronous, reusable API. Methods throw QuarkException on provider/transport
// failures and never print. Each transfer streams bounded buffers to/from disk.
class QuarkClient
{
  public:
    explicit QuarkClient(AppConfig config);
    [[nodiscard]] const AppConfig &config() const;
    [[nodiscard]] json account_info() const;
    [[nodiscard]] std::vector<RemoteEntry> list_directory(const std::string &pdir_fid = "0") const;
    [[nodiscard]] std::vector<RemoteEntry> search(const std::string &keyword, const std::string &pdir_fid = "0") const;
    [[nodiscard]] RemoteEntry file_info(const std::string &fid) const;
    [[nodiscard]] RemoteEntry resolve_path(const std::string &remote_path) const;
    [[nodiscard]] json create_folder(const std::string &pdir_fid, const std::string &name) const;
    [[nodiscard]] json rename(const std::string &fid, const std::string &new_name) const;
    [[nodiscard]] json delete_items(const std::vector<std::string> &fids) const;
    [[nodiscard]] json move_items(const std::vector<std::string> &fids, const std::string &parent_fid) const;

    [[nodiscard]] UploadResult upload_file(const std::filesystem::path &local_path, const std::string &pdir_fid,
                                           const std::optional<std::string> &new_name = std::nullopt,
                                           const ProgressCallback &progress = {}) const;
    [[nodiscard]] std::string download_url(const std::string &fid) const;
    [[nodiscard]] HeaderList download_headers() const;
    [[nodiscard]] std::vector<DownloadTarget> collect_download_targets(const std::string &fid) const;
    // Downloads a file or a complete directory below output_root, preserving names.
    void download(const std::string &fid, const std::filesystem::path &output_root, bool resume = true,
                  const ProgressCallback &progress = {}) const;
    // Downloads one file to an exact local path; used by adapters such as WebDAV.
    void download_to_file(const std::string &fid, const std::filesystem::path &destination, bool resume = true,
                          const ProgressCallback &progress = {}) const;

    // Cloud-side magnet/HTTP(S) download. Parse previews do not submit a task.
    // selected_files are provider file_no values; empty means every parsed file.
    [[nodiscard]] json offline_parse(const std::string &source) const;
    [[nodiscard]] json offline_add(const std::string &source, const std::string &pdir_fid = "0",
                                   const std::vector<int> &selected_files = {}) const;
    [[nodiscard]] json offline_tasks(unsigned page = 1, unsigned size = 50) const;
    [[nodiscard]] json task_status(const std::string &task_id) const;
    [[nodiscard]] json wait_task(const std::string &task_id) const;

  private:
    struct SessionState;
    [[nodiscard]] std::string session_cookie() const;
    void refresh_session_cookie(const HttpResponse &response) const;
    struct UploadPreflight
    {
        std::string task_id, fid, obj_key, bucket, upload_url, upload_id, auth_info;
        json callback;
        std::uint64_t part_size{};
    };

    [[nodiscard]] QueryList base_params() const;
    [[nodiscard]] HeaderList api_headers() const;
    [[nodiscard]] json api_get_json(const std::string &url, const QueryList &query = {},
                                    const HeaderList &headers = {}) const;
    [[nodiscard]] json api_post_json(const std::string &url, const json &body, const QueryList &query = {},
                                     const HeaderList &headers = {}) const;
    [[nodiscard]] json offline_post(const std::string &endpoint, const json &body) const;
    [[nodiscard]] std::vector<RemoteEntry> list_entries(const std::string &endpoint, const QueryList &query) const;
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
    [[nodiscard]] std::unordered_map<std::string, std::string>
    request_download_urls(const std::vector<std::string> &fids) const;
    void collect_download_targets_recursive(const RemoteEntry &entry, const std::filesystem::path &base,
                                            std::vector<DownloadTarget> &output,
                                            std::unordered_set<std::string> &visited, unsigned depth) const;
    void download_entry(const RemoteEntry &entry, const std::filesystem::path &destination, bool resume,
                        const ProgressCallback &progress) const;

    AppConfig config_;
    HttpClient http_;
    std::shared_ptr<SessionState> session_;
};

} // namespace quarkpp
