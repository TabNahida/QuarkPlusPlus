#include "quarkpp/client.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <memory>
#include <openssl/evp.h>
#include <thread>

namespace
{
using namespace quarkpp;
void check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> std::string fails(F operation, const char *message)
{
    try
    {
        operation();
    }
    catch (const std::exception &error)
    {
        return error.what();
    }
    throw std::runtime_error(message);
}
void reply(httplib::Response &response, const json &data)
{
    response.set_content(json{{"code", 0}, {"data", data}}.dump(), "application/json");
}
json entry(std::string id, std::string name, std::uint64_t size = 0, bool directory = false)
{
    return {{"fid", id}, {"file_name", name}, {"pdir_fid", "0"}, {"size", size}, {"file", !directory}};
}
std::string decrypt_wg(const std::string &encoded)
{
    std::string bytes(encoded.size(), '\0');
    int count =
        EVP_DecodeBlock(reinterpret_cast<unsigned char *>(bytes.data()),
                        reinterpret_cast<const unsigned char *>(encoded.data()), static_cast<int>(encoded.size()));
    check(count >= 18, "wg envelope length");
    if (encoded.back() == '=')
        --count;
    if (encoded[encoded.size() - 2] == '=')
        --count;
    bytes.resize(static_cast<std::size_t>(count));
    check(static_cast<unsigned char>(bytes[0]) == 0x37 && static_cast<unsigned char>(bytes[1]) == 0x14, "wg version");
    const auto *key = reinterpret_cast<const unsigned char *>("e21247a68938d36f");
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    check(EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, key, key) == 1, "wg decrypt init");
    std::string clear(bytes.size(), '\0');
    int size = 0, tail = 0;
    check(EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(clear.data()), &size,
                            reinterpret_cast<const unsigned char *>(bytes.data()) + 2, count - 2) == 1,
          "wg decrypt update");
    check(EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(clear.data()) + size, &tail) == 1,
          "wg padding");
    clear.resize(static_cast<std::size_t>(size + tail));
    return clear;
}
std::string encrypt_wg(const std::string &clear)
{
    const auto *key = reinterpret_cast<const unsigned char *>("e21247a68938d36f");
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    check(EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, key, key) == 1, "wg encrypt init");
    std::string bytes(clear.size() + 34, '\0');
    bytes[0] = 0x37;
    bytes[1] = 0x14;
    int size = 0, tail = 0;
    check(EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(bytes.data()) + 2, &size,
                            reinterpret_cast<const unsigned char *>(clear.data()), static_cast<int>(clear.size())) == 1,
          "wg encrypt update");
    check(EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(bytes.data()) + size + 2, &tail) == 1,
          "wg encrypt final");
    bytes.resize(static_cast<std::size_t>(size + tail + 2));
    return base64_encode(bytes);
}
struct Server
{
    httplib::Server http;
    std::thread thread;
    int port{};
    void start()
    {
        port = http.bind_to_any_port("127.0.0.1");
        check(port > 0, "bind test server");
        thread = std::thread([this] { http.listen_after_bind(); });
        for (int i = 0; i < 100 && !http.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(http.is_running(), "start test server");
    }
    ~Server()
    {
        http.stop();
        if (thread.joinable())
            thread.join();
    }
    std::string base() const
    {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};
struct Temp
{
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("quarkpp-client-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp()
    {
        std::filesystem::create_directories(path);
    }
    ~Temp()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
} // namespace

int main()
{
    try
    {
        using namespace quarkpp;
        check(decrypt_wg("NxTFGiL/qySwJSpexPNDuS1r") == "hello", "known wg protocol vector");
        check(encrypt_wg("hello") == "NxTFGiL/qySwJSpexPNDuS1r", "known wg encryption vector");
        Server server;
        Temp temp;
        std::atomic<int> list_calls{0}, puts{0}, finishes{0}, hash_calls{0}, download_calls{0};
        std::string mode = "listing";
        std::string uploaded;
        std::string issued_download_ua;
        json submitted;
        server.http.Get("/api/file/sort",
                        [&](const httplib::Request &request, httplib::Response &response)
                        {
                            check(request.get_header_value("User-Agent") == "quarkpp-test-browser/1",
                                  "normal API must retain the configured browser identity");
                            if (mode == "listing")
                            {
                                if (list_calls > 0)
                                    check(request.get_header_value("Cookie").find("__puus=from-list") !=
                                              std::string::npos,
                                          "next API page must use the refreshed session cookie");
                                response.set_header("Set-Cookie", "__puus=from-list; Path=/; HttpOnly");
                                response.set_header("Set-Cookie", "unrelated=ignored; Path=/");
                            }
                            ++list_calls;
                            if (mode == "failed")
                            {
                                response.set_content(R"({"success":false,"code":0})", "application/json");
                                return;
                            }
                            if (mode == "missing-status")
                            {
                                response.set_content(R"({"data":{"list":[]}})", "application/json");
                                return;
                            }
                            json items = json::array();
                            const auto page = std::stoi(request.get_param_value("_page"));
                            if (mode == "unsafe")
                                items.push_back(entry("bad", "../escape"));
                            else if (mode == "reserved-device")
                                items.push_back(entry("bad", "CON.notes.txt"));
                            else if (mode == "empty-folder")
                                items.push_back(entry("empty", "empty", 0, true));
                            else if (mode == "listing" || mode == "repeat")
                            {
                                const int count = page == 1 || mode == "repeat" ? 100 : 1;
                                for (int i = 0; i < count; ++i)
                                    items.push_back(entry(std::to_string((mode == "repeat" ? 0 : page - 1) * 100 + i),
                                                          "file" + std::to_string(i), 5ULL * 1024 * 1024 * 1024));
                            }
                            reply(response, {{"list", items}}); // Intentionally no _total.
                        });
        server.http.Get("/api/file/info",
                        [&](const httplib::Request &request, httplib::Response &response)
                        {
                            const auto id = request.get_param_value("fid");
                            reply(response,
                                  entry(id, id == "file" ? "data.bin" : "folder", id == "file" ? 11 : 0, id != "file"));
                        });
        server.http.Post("/api/file/upload/pre",
                         [&](const httplib::Request &request, httplib::Response &response)
                         {
                             const auto body = json::parse(request.body);
                             check(!body.contains("parallel_upload"),
                                   "sequential multipart preflight must not declare parallel upload");
                             reply(response, {{"task_id", "upload"},
                                              {"fid", "uploaded"},
                                              {"obj_key", "object"},
                                              {"bucket", "bucket"},
                                              {"upload_url", "http://oss.example"},
                                              {"upload_id", "multipart"},
                                              {"auth_info", "auth"},
                                              {"callback", json::object()}});
                             auto envelope = json::parse(response.body);
                             envelope["metadata"] = {{"part_size", 4}};
                             response.set_content(envelope.dump(), "application/json");
                             check(body.at("size").get<std::uint64_t>() == (mode == "zero" ? 0 : 11),
                                   "upload declared size");
                         });
        server.http.Post("/api/file/update/hash",
                         [&](const httplib::Request &request, httplib::Response &response)
                         {
                             ++hash_calls;
                             const auto body = json::parse(request.body);
                             check(body.at("md5").get<std::string>().size() == 32 &&
                                       body.at("sha1").get<std::string>().size() == 40,
                                   "whole-file hashes");
                             reply(response, {{"finish", mode == "fast"}, {"fid", "uploaded"}});
                         });
        server.http.Post("/api/file/upload/auth",
                         [&](const httplib::Request &request, httplib::Response &response)
                         {
                             const auto body = json::parse(request.body);
                             check(body.at("auth_meta").get<std::string>().find("/bucket/object?") != std::string::npos,
                                   "OSS resource in signature");
                             reply(response, {{"auth_key", "test-auth"}});
                         });
        server.http.Put("/object",
                        [&](const httplib::Request &request, httplib::Response &response)
                        {
                            const auto attempt = ++puts;
                            if (attempt == 1)
                            {
                                response.status = 503;
                                return;
                            }
                            check(request.get_header_value("Authorization") == "test-auth", "OSS authorization");
                            if (mode == "no-hash-context" && request.get_param_value("partNumber") == "2")
                            {
                                response.status = 400;
                                response.set_content(
                                    "<Error><Code>NoHashContext</Code><Message>sensitive-message</Message>"
                                    "<RequestId>sensitive-request-id</RequestId>"
                                    "<SignatureProvided>sensitive-signature</SignatureProvided>"
                                    "<StringToSign>sensitive-string-to-sign</StringToSign></Error>",
                                    "application/xml");
                                return;
                            }
                            uploaded += request.body;
                            response.set_header("ETag", "\"part-etag\"");
                            response.status = 200;
                        });
        server.http.Post(
            "/object",
            [&](const httplib::Request &request, httplib::Response &response)
            {
                check(request.body.find("&quot;part-etag&quot;") != std::string::npos, "completion XML escaped ETags");
                check(!request.get_header_value("Content-MD5").empty(), "completion MD5");
                if (mode == "xml-error")
                {
                    response.set_content("<Error xmlns=\"https://oss.aliyuncs.com\"><Code>InvalidPart</Code></Error>",
                                         "application/xml");
                    return;
                }
                if (mode == "callback-error")
                {
                    response.set_content(R"({"code":123,"message":"callback rejected"})", "application/json");
                    return;
                }
                response.set_content("<CompleteMultipartUploadResult/>", "application/xml");
            });
        server.http.Post("/api/file/upload/finish",
                         [&](const httplib::Request &, httplib::Response &response)
                         {
                             ++finishes;
                             if (mode == "unconfirmed-finish")
                             {
                                 reply(response, json::object());
                                 return;
                             }
                             if (mode == "malformed-finish")
                             {
                                 reply(response, {{"finish", "true"}});
                                 return;
                             }
                             reply(response, {{"fid", "uploaded"}}); // Some server versions omit finish.
                         });
        server.http.Post(
            "/api/file/download",
            [&](const httplib::Request &request, httplib::Response &response)
            {
                issued_download_ua = request.get_header_value("User-Agent");
                check(issued_download_ua.find("quark-cloud-drive/") != std::string::npos,
                      "download URL issuance must use the desktop identity");
                if (mode == "retry-url" && download_calls++ == 0)
                {
                    response.status = 503;
                    return;
                }
                response.set_header("Set-Cookie", "__puus=from-download; Path=/; HttpOnly");
                response.set_header("Set-Cookie", "unrelated=ignored; Path=/");
                reply(response,
                      json::array({{{"fid", "file"},
                                    {"download_url", mode == "bad-host" ? "https://quark.cn.attacker.example/file"
                                                                        : server.base() + "/bytes"}}}));
            });
        server.http.Get(
            "/bytes",
            [&](const httplib::Request &request, httplib::Response &response)
            {
                check(!issued_download_ua.empty() && request.get_header_value("User-Agent") == issued_download_ua,
                      "byte GET must use the same identity as URL issuance");
                const auto cookie = request.get_header_value("Cookie");
                check(cookie.find("__puus=from-download") != std::string::npos &&
                          cookie.find("test=value") != std::string::npos &&
                          cookie.find("other=preserved") != std::string::npos &&
                          cookie.find("unrelated=") == std::string::npos && cookie.find("stale") == std::string::npos,
                      "byte GET must use the refreshed cookie and preserve other credentials");
                check(request.get_header_value("Range") == (mode == "retry-url" ? "" : "bytes=5-"), "resume offset");
                // cpp-httplib's server applies the requested Range to the full body.
                response.set_content("hello world", "application/octet-stream");
            });
        server.http.Post(
            "/api/offline/download/parse",
            [&](const httplib::Request &request, httplib::Response &response)
            {
                check(request.get_header_value("X-U-Content-Encoding") == "wg", "offline encryption marker");
                check(request.get_param_value("api_ver") == "2", "offline API version");
                const auto body = json::parse(decrypt_wg(request.body));
                check(body.at("entry") == "download" && body.at("auto_download") == false,
                      "preview cannot create task");
                reply(response, {{"token", "parse-token"},
                                 {"finished", true},
                                 {"title", "test"},
                                 {"files", json::array({{{"file_no", 7}, {"file_name", "a"}},
                                                        {{"file_no", "9,10"}, {"file_name", "folder"}}})}});
                if (mode == "encrypted-preview")
                {
                    response.set_header("X-U-Content-Encoding", "wg");
                    response.set_content(encrypt_wg(response.body), "application/json");
                }
            });
        server.http.Post("/api/offline/download/submit",
                         [&](const httplib::Request &request, httplib::Response &response)
                         {
                             submitted = json::parse(decrypt_wg(request.body));
                             reply(response, {{"task_id", "cloud-task"}});
                         });
        server.http.Get("/api/task", [&](const httplib::Request &, httplib::Response &response)
                        { reply(response, {{"task_id", "cloud-task"}, {"status", mode == "task-failed" ? 3 : 2}}); });
        server.http.Get("/api/member",
                        [&](const httplib::Request &, httplib::Response &response)
                        {
                            if (mode == "cookie-redirect")
                            {
                                response.set_redirect(server.base() + "/outside-member");
                                return;
                            }
                            response.set_header("Set-Cookie",
                                                mode == "cookie-clear" ? "__puus=; Max-Age=0" : "__puus=bad,value");
                            reply(response, {{"member_type", "test"}});
                        });
        server.http.Get("/outside-member",
                        [&](const httplib::Request &, httplib::Response &response)
                        {
                            response.set_header("Set-Cookie", "__puus=untrusted");
                            reply(response, {{"member_type", "test"}});
                        });
        server.start();
        AppConfig config;
        config.cookie = "test=value; __puus=stale; other=preserved";
        config.user_agent = "quarkpp-test-browser/1";
        config.api_base = config.offline_api_base = server.base() + "/api";
        config.upload_base = server.base();
        config.upload_retry_count = config.download_retry_count = 2;
        config.task_poll_limit = 1;
        QuarkClient client(config);
        const auto current_cookie = [&]()
        {
            for (const auto &[name, value] : client.download_headers())
                if (to_lower(name) == "cookie")
                    return value;
            throw std::runtime_error("Missing download cookie header");
        };

        const auto files = client.list_directory();
        check(files.size() == 101 && list_calls == 2, "pagination without total must include second page");
        check(files.front().size == 5ULL * 1024 * 1024 * 1024, "64-bit file sizes");
        check(client.file_info("folder").dir, "file=false detects directory");
        mode = "repeat";
        fails([&] { (void)client.list_directory(); }, "repeated page must fail");
        mode = "failed";
        fails([&] { (void)client.list_directory(); }, "explicit failure must override code zero");
        mode = "missing-status";
        fails([&] { (void)client.list_directory(); }, "missing success status must fail");
        mode = "unsafe";
        fails([&] { client.download("0", temp.path / "download"); }, "path traversal must fail");
        check(!std::filesystem::exists(temp.path / "escape"), "no path escaped");
#ifdef _WIN32
        mode = "reserved-device";
        fails([&] { client.download("0", temp.path / "download"); },
              "Windows reserved device name with multiple dots must fail");
#endif

        const auto source = temp.path / "source.bin";
        {
            std::ofstream file(source, std::ios::binary);
            file << "hello world";
        }
        mode = "upload";
        std::uint64_t last = 0;
        const auto upload = client.upload_file(source, "0", {},
                                               [&](auto current, auto total)
                                               {
                                                   check(current >= last && total == 11, "monotonic upload progress");
                                                   last = current;
                                               });
        check(upload.fid == "uploaded" && !upload.fast_upload && uploaded == "hello world" && puts == 4 &&
                  finishes == 1,
              "multipart retry/offset/finish");
        mode = "fast";
        const auto fast = client.upload_file(source, "0");
        check(fast.fast_upload && finishes == 1 && puts == 4, "fast upload must skip multipart finish");
        mode = "zero";
        const auto empty = temp.path / "empty.bin";
        {
            std::ofstream file(empty, std::ios::binary);
        }
        (void)client.upload_file(empty, "0");
        check(puts == 5 && finishes == 2, "zero-byte file must still complete a part");

        mode = "no-hash-context";
        const auto part_error =
            fails([&] { (void)client.upload_file(source, "0"); }, "OSS part-2 NoHashContext must fail");
        check(part_error.find("NoHashContext") != std::string::npos && part_error.find("part=2") != std::string::npos &&
                  part_error.find("bytes=4") != std::string::npos,
              "OSS part failure must identify the code, part number and byte count");
        for (const auto *sensitive : {"sensitive-", "Message", "RequestId", "SignatureProvided", "StringToSign"})
            check(part_error.find(sensitive) == std::string::npos, "OSS error must omit sensitive response fields");
        check(finishes == 2, "OSS part failure must not call file upload finish");

        mode = "unconfirmed-finish";
        fails([&] { (void)client.upload_file(source, "0"); },
              "empty upload acknowledgement must not report completion");
        mode = "malformed-finish";
        fails([&] { (void)client.upload_file(source, "0"); }, "non-boolean finish flag must be rejected");
        const auto finishes_before_xml_error = finishes.load();
        mode = "xml-error";
        fails([&] { (void)client.upload_file(source, "0"); }, "OSS HTTP 200 Error with XML attributes must fail");
        check(finishes == finishes_before_xml_error, "OSS completion failure must not call file upload finish");
        mode = "callback-error";
        fails([&] { (void)client.upload_file(source, "0"); }, "OSS HTTP 200 callback business failure must fail");
        check(finishes == finishes_before_xml_error, "OSS callback failure must not call file upload finish");

        const auto output = temp.path / "resumed.bin";
        {
            std::ofstream file(output, std::ios::binary);
            file << "hello";
        }
        client.download_to_file("file", output, true);
        std::string adapter_download_ua;
        for (const auto &[name, value] : client.download_headers())
            if (to_lower(name) == "user-agent")
                adapter_download_ua = value;
        check(adapter_download_ua == issued_download_ua,
              "WebDAV download headers must preserve the URL request identity");
        std::ifstream input(output, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input), {}};
        if (bytes != "hello world")
            throw std::runtime_error("resumed download bytes: " + bytes);
        input.close();
        mode = "retry-url";
        {
            std::ofstream file(output, std::ios::binary);
            file << "OLD!";
        }
        client.download_to_file("file", output, false);
        {
            std::ifstream file(output, std::ios::binary);
            const std::string fresh{std::istreambuf_iterator<char>(file), {}};
            check(fresh == "hello world", "failed URL lookup must not turn no-resume into appending");
        }
        mode = "bad-host";
        fails([&] { (void)client.download_url("file"); }, "unrelated hosts must not receive account cookies");
        mode = "encrypted-preview";
        check(client.offline_parse("https://example.com/file").at("finished") == true, "encrypted offline response");
        mode = "offline";
        (void)client.offline_add("magnet:?xt=urn:btih:0123456789012345678901234567890123456789", "destination", {9});
        check(submitted.at("selected_files") == json::array({9}) && submitted.at("pdir_fid") == "destination",
              "selected provider file numbers");
        fails([&] { (void)client.offline_add("https://example.com/file", "0", {999}); }, "invalid offline selection");
        check(client.wait_task("cloud-task").at("status") == 2, "completed cloud task");
        mode = "task-failed";
        fails([&] { (void)client.wait_task("cloud-task"); }, "failed cloud task must fail");
        const auto preserved_cookie = current_cookie();
        mode = "cookie-redirect";
        (void)client.account_info();
        check(current_cookie() == preserved_cookie, "redirect outside API must not replace the session cookie");
        mode = "cookie-invalid";
        (void)client.account_info();
        check(current_cookie() == preserved_cookie, "invalid cookie value must not replace credentials");
        mode = "cookie-clear";
        (void)client.account_info();
        check(current_cookie().find("__puus=") == std::string::npos &&
                  current_cookie().find("other=preserved") != std::string::npos,
              "cleared refresh cookie must preserve other credentials");
        check(client.config().cookie == config.cookie, "session refresh must not mutate caller configuration");
        std::cout << "Client integration tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "Client tests failed: " << error.what() << '\n';
        return 1;
    }
}
