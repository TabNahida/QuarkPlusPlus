#include "webdav.hpp"
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

namespace
{
using namespace quarkpp;
void check(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}
void reply(httplib::Response &response, const json &data)
{
    response.set_content(json{{"code", 0}, {"data", data}}.dump(), "application/json");
}
struct MockDrive
{
    httplib::Server server;
    std::thread thread;
    std::mutex mutex;
    std::map<std::string, json> files;
    std::atomic<unsigned> next{10};
    std::atomic<std::uint64_t> transferred{};
    std::atomic<unsigned> uploads{};
    std::atomic<bool> fail_listing{false};
    std::atomic<bool> fail_publish{false};
    int port{};
    static constexpr std::uint64_t big_size = 5ULL * 1024 * 1024 * 1024 + 17;
    json make(const std::string &id, const std::string &name, std::uint64_t size = 0, bool directory = false,
              const std::string &parent = "0")
    {
        return {{"fid", id},          {"file_name", name},  {"size", size},
                {"file", !directory}, {"pdir_fid", parent}, {"updated_at", 1700000000000ULL}};
    }
    MockDrive()
    {
        files["a"] = make("a", "a & b.txt", 11);
        files["big"] = make("big", "big.bin", big_size);
        files["dir"] = make("dir", "folder", 0, true);
        server.Get("/api/file/sort",
                   [&](const auto &request, auto &response)
                   {
                       if (fail_listing)
                       {
                           response.status = 503;
                           response.set_content(
                               "provider error secret-cookie-value https://signed.invalid/?token=secret", "text/plain");
                           return;
                       }
                       std::lock_guard lock(mutex);
                       auto items = json::array();
                       for (const auto &[id, file] : files)
                           if (file.at("pdir_fid") == request.get_param_value("pdir_fid"))
                               items.push_back(file);
                       reply(response, {{"list", items}});
                   });
        server.Post("/api/file/download",
                    [&](const auto &request, auto &response)
                    {
                        const auto id = json::parse(request.body).at("fids").at(0).template get<std::string>();
                        reply(response, json::array({{{"fid", id}, {"download_url", base() + "/bytes/" + id}}}));
                    });
        server.Get("/bytes/a", [&](const auto &, auto &response)
                   { response.set_content("hello world", "application/octet-stream"); });
        server.Get("/bytes/big",
                   [&](const auto &, auto &response)
                   {
                       response.set_content_provider(
                           static_cast<std::size_t>(big_size), "application/octet-stream",
                           [&](std::size_t offset, std::size_t length, httplib::DataSink &sink)
                           {
                               check(offset > 4ULL * 1024 * 1024 * 1024 && length <= 64,
                                     "large range must retain 64-bit offset");
                               transferred += length;
                               return sink.write(std::string(length, 'B').data(), length);
                           });
                   });
        server.Post("/api/file",
                    [&](const auto &request, auto &response)
                    {
                        std::lock_guard lock(mutex);
                        const auto body = json::parse(request.body);
                        const auto id = std::to_string(next++);
                        files[id] = make(id, body.at("file_name"), 0, true, body.at("pdir_fid"));
                        reply(response, {{"fid", id}});
                    });
        server.Post("/api/file/rename",
                    [&](const auto &request, auto &response)
                    {
                        std::lock_guard lock(mutex);
                        const auto body = json::parse(request.body);
                        if (body.at("file_name") == "uploaded.bin" && fail_publish.exchange(false))
                        {
                            response.set_content(R"({"code":500,"message":"injected publish failure"})",
                                                 "application/json");
                            return;
                        }
                        files.at(body.at("fid").template get<std::string>())["file_name"] = body.at("file_name");
                        reply(response, json::object());
                    });
        server.Post("/api/file/delete",
                    [&](const auto &request, auto &response)
                    {
                        std::lock_guard lock(mutex);
                        const auto body = json::parse(request.body);
                        for (const auto &id : body.at("filelist"))
                            files.erase(id.template get<std::string>());
                        reply(response, json::object());
                    });
        server.Post("/api/file/upload/pre",
                    [&](const auto &request, auto &response)
                    {
                        std::lock_guard lock(mutex);
                        const auto body = json::parse(request.body);
                        const auto id = std::to_string(next++);
                        files[id] = make(id, body.at("file_name"), body.at("size"), false, body.at("pdir_fid"));
                        ++uploads;
                        reply(response, {{"fid", id}, {"task_id", id}});
                    });
        server.Post("/api/file/update/hash",
                    [&](const auto &request, auto &response)
                    {
                        const auto body = json::parse(request.body);
                        reply(response, {{"finish", true}, {"fid", body.at("task_id")}});
                    });
        port = server.bind_to_any_port("127.0.0.1");
        check(port > 0, "bind mock drive");
        thread = std::thread([&] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~MockDrive()
    {
        server.stop();
        if (thread.joinable())
            thread.join();
    }
    std::string base() const
    {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};
struct RunningDav
{
    WebDavServer server;
    std::thread thread;
    int port;
    RunningDav(QuarkClient &client, WebDavOptions options) : server(client, std::move(options)), port(server.bind())
    {
        thread = std::thread([&] { server.listen(); });
    }
    ~RunningDav()
    {
        server.stop();
        if (thread.joinable())
            thread.join();
    }
    HttpResponse request(const std::string &method, const std::string &path, HeaderList headers = {},
                         const std::string &body = {}, bool auth = true)
    {
        if (auth)
            headers.emplace_back("Authorization", "Basic " + base64_encode("tester:test-password"));
        return HttpClient(5000).request(method, "http://127.0.0.1:" + std::to_string(port) + path, {}, headers, body);
    }
};
} // namespace

int main(int argc, char **argv)
{
    try
    {
        MockDrive mock;
        AppConfig config;
        config.cookie = "secret-cookie-value";
        config.api_base = mock.base() + "/api";
        QuarkClient client(config);
        WebDavOptions options;
        bool rejected = false;
        try
        {
            WebDavServer invalid(client, options);
        }
        catch (const QuarkException &)
        {
            rejected = true;
        }
        check(rejected, "empty WebDAV password rejected");
        options.port = 0;
        options.username = "tester";
        options.password = "test-password";
        auto remote_options = options;
        remote_options.host = "0.0.0.0";
        rejected = false;
        try
        {
            WebDavServer invalid(client, remote_options);
        }
        catch (const QuarkException &)
        {
            rejected = true;
        }
        check(rejected, "plaintext listener restricted to loopback");
        RunningDav dav(client, options);
        if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--serve")
        {
            std::cout << "http://127.0.0.1:" << dav.port << "/\n" << std::flush;
            if (argc == 3)
            {
                const auto stop_file = path_from_utf8(argv[2]);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
                while (!std::filesystem::exists(stop_file) && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            else
            {
                std::string stop;
                std::getline(std::cin, stop);
            }
            return 0;
        }
        auto result = dav.request("OPTIONS", "/", {}, {}, false);
        check(result.status_code == 401 && result.header("WWW-Authenticate").starts_with("Basic"),
              "authentication required");
        result = dav.request("OPTIONS", "/");
        check(result.status_code == 200 && result.header("Allow").find("PROPFIND") != std::string::npos,
              "method advertisement");
        result =
            dav.request("PROPFIND", "/", {{"Depth", "1"}}, "<D:propfind xmlns:D=\"DAV:\"><D:allprop/></D:propfind>");
        check(result.status_code == 207, "PROPFIND routed through cpp-httplib");
        check(result.body.find("a &amp; b.txt") != std::string::npos &&
                  result.body.find("a%20%26%20b.txt") != std::string::npos,
              "XML escaping and percent-encoded hrefs");
        check(result.body.find("5368709137") != std::string::npos, "large content length in metadata");
        check(result.body.find("secret-cookie") == std::string::npos, "cookie never appears in response");
        result = dav.request("PROPFIND", "/", {{"Depth", "0"}});
        check(result.status_code == 207 && result.body.find("big.bin") == std::string::npos, "Depth zero");
        check(dav.request("PROPFIND", "/", {{"Depth", "infinity"}}).status_code == 403, "finite depth required");
        check(dav.request("GET", "/%2e%2e/secret").status_code == 400, "encoded traversal rejected");
        check(dav.request("GET", "/folder%2fsecret").status_code == 400, "encoded separator rejected");
        check(dav.request("GET", "/no-file").status_code == 404, "missing file");
        result = dav.request("HEAD", "/big.bin");
        check(result.status_code == 200 && result.header("Content-Length") == std::to_string(MockDrive::big_size),
              "HEAD 64-bit size: status=" + std::to_string(result.status_code) +
                  " length=" + result.header("Content-Length"));
        result = dav.request("GET", "/a%20%26%20b.txt");
        check(result.status_code == 200 && result.body == "hello world", "full streamed GET");
        result = dav.request("GET", "/a%20%26%20b.txt", {{"Range", "bytes=6-10"}});
        check(result.status_code == 206 && result.body == "world" && result.header("Content-Range") == "bytes 6-10/11",
              "bounded byte range");
        result = dav.request("GET", "/a%20%26%20b.txt", {{"Range", "bytes=-5"}});
        check(result.status_code == 206 && result.body == "world", "suffix byte range");
        result = dav.request("GET", "/big.bin", {{"Range", "bytes=5368709129-5368709136"}});
        check(result.status_code == 206 && result.body == "BBBBBBBB" && mock.transferred == 8,
              "range beyond 4 GiB without whole-file download");
        check(dav.request("GET", "/a%20%26%20b.txt", {{"Range", "bytes=90-100"}}).status_code == 416, "invalid range");
        check(dav.request("MKCOL", "/new").status_code == 201, "MKCOL");
        check(dav.request("MKCOL", "/new").status_code == 405, "duplicate collection rejected");
        check(dav.request("MKCOL", "/withbody", {}, "not-supported").status_code == 415, "MKCOL body rejected");
        check(dav.request("MOVE", "/new", {{"Destination", "/renamed"}}).status_code == 201, "MOVE rename");
        check(dav.request("MOVE", "/renamed", {{"Destination", "http://evil.invalid/path"}}).status_code == 502,
              "cross-server MOVE rejected");
        check(dav.request("DELETE", "/renamed").status_code == 204, "DELETE collection");
        check(dav.request("DELETE", "/").status_code == 403, "root deletion rejected");
        const std::string body(1024 * 1024 + 13, 'U');
        check(dav.request("PUT", "/uploaded.bin", {}, body).status_code == 201, "PUT spools and uploads");
        result = dav.request("HEAD", "/uploaded.bin");
        check(result.status_code == 200 && result.header("Content-Length") == std::to_string(body.size()),
              "uploaded size retained");
        check(dav.request("PUT", "/uploaded.bin", {{"If-None-Match", "*"}}, "new").status_code == 412,
              "create-only PUT precondition");
        check(dav.request("PUT", "/uploaded.bin", {}, "replacement").status_code == 204, "PUT replacement");
        result = dav.request("PROPFIND", "/", {{"Depth", "1"}});
        check(result.body.find(".quarkpp-") == std::string::npos && mock.uploads == 2, "staging and backup cleanup");
        mock.fail_publish = true;
        check(dav.request("PUT", "/uploaded.bin", {}, "failed-replacement").status_code == 502,
              "failed publish reported");
        check(dav.request("HEAD", "/uploaded.bin").header("Content-Length") == "11",
              "previous file restored after rename failure");
        result = dav.request("PROPFIND", "/", {{"Depth", "1"}});
        check(result.body.find(".quarkpp-") == std::string::npos, "failed publication cleans staged file");
        check(dav.request("PUT", "/empty.bin", {}, "").status_code == 201, "empty PUT");
        check(dav.request("HEAD", "/empty.bin").header("Content-Length") == "0", "empty file size");
        mock.fail_listing = true;
        result = dav.request("GET", "/big.bin");
        check(result.status_code == 502 && result.body == "Cloud operation failed" &&
                  result.body.find("secret") == std::string::npos,
              "provider errors do not disclose secrets");
        mock.fail_listing = false;
        options.read_only = true;
        RunningDav readonly(client, options);
        check(readonly.request("PUT", "/x", {}, "x").status_code == 403, "read-only PUT blocked");
        check(readonly.request("DELETE", "/uploaded.bin").status_code == 403, "read-only DELETE blocked");
        std::cout << "WebDAV integration tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "WebDAV test failed: " << error.what() << '\n';
        return 1;
    }
}
