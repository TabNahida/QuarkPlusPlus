#pragma once

#include <filesystem>
#include <memory>
#include <quarkpp/client.hpp>
#include <string>

namespace quarkpp
{
struct WebDavOptions
{
    std::string host{"127.0.0.1"};
    int port{8787};
    std::string username{"quarkpp"};
    std::string password;
    bool read_only{false};
    std::filesystem::path temp_directory{std::filesystem::temp_directory_path()};
};

// The client must outlive the server. GET streams directly from the provider;
// PUT spools into an isolated temporary directory before the cloud upload.
class WebDavServer
{
  public:
    WebDavServer(QuarkClient &client, WebDavOptions options);
    ~WebDavServer();
    WebDavServer(const WebDavServer &) = delete;
    WebDavServer &operator=(const WebDavServer &) = delete;
    int bind(); // port 0 selects an available port, useful for embedders/tests
    bool listen();
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace quarkpp
