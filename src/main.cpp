#include "login.hpp"
#include "webdav.hpp"
#include <quarkpp/client.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace quarkpp
{
namespace
{
using Arguments = std::vector<std::string>;

std::optional<std::string> environment(const char *name)
{
#ifdef _WIN32
    char *value = nullptr;
    std::size_t length{};
    if (_dupenv_s(&value, &length, name) != 0 || !value)
        return std::nullopt;
    std::string result(value);
    std::free(value);
    return result.empty() ? std::nullopt : std::optional(result);
#else
    const auto *value = std::getenv(name);
    return value && *value ? std::optional(std::string(value)) : std::nullopt;
#endif
}

bool flag(Arguments &args, const std::string &name)
{
    const auto it = std::find(args.begin(), args.end(), name);
    if (it == args.end())
        return false;
    args.erase(it);
    return true;
}

std::optional<std::string> option(Arguments &args, const std::string &name)
{
    const auto it = std::find(args.begin(), args.end(), name);
    if (it == args.end())
        return std::nullopt;
    if (it + 1 == args.end() || (it + 1)->starts_with("--"))
        throw QuarkException("Missing value for " + name);
    auto result = *(it + 1);
    args.erase(it, it + 2);
    return result;
}

std::string positional(Arguments &args, const std::string &name)
{
    if (args.empty() || args.front().starts_with("--"))
        throw QuarkException("Missing " + name);
    auto result = args.front();
    args.erase(args.begin());
    return result;
}

void exhausted(const Arguments &args)
{
    if (!args.empty())
        throw QuarkException("Unexpected argument: " + args.front());
}

unsigned number(const std::string &text, const std::string &label, unsigned minimum = 1, unsigned maximum = 1000000)
{
    unsigned result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc() || end != text.data() + text.size() || result < minimum || result > maximum)
        throw QuarkException("Invalid " + label);
    return result;
}

void help()
{
    std::cout << R"(QuarkPlusPlus — a C++20 Quark Drive client

Usage: quarkpp [--config FILE] [--json] COMMAND [OPTIONS]

  login [--timeout SECONDS] [--qr-svg FILE] [--ca-bundle FILE]
                                    Sign in by scanning a terminal QR code
  account                           Show account information
  ls [--fid ID | --path /PATH]       List a directory (default: root)
  search TEXT [--fid ID | --path /PATH]
  info --fid ID | --path /PATH       Show an entry
  mkdir NAME [--parent-fid ID | --parent-path /PATH]
  rename NAME --fid ID | --path /PATH
  rm --fid ID | --path /PATH         Move an entry to the drive's recycle bin
  upload FILE [--to-fid ID | --to-path /PATH] [--name NAME] [--progress]
  download --fid ID | --path /PATH [--out DIRECTORY] [--no-resume] [--progress]
  offline parse SOURCE              Preview a magnet or HTTP(S) source
  offline add SOURCE [--to-fid ID | --to-path /PATH] [--select 0,1]
  offline list [--page N] [--size N]
  offline status TASK_ID [--wait]    Inspect or wait for a cloud task
  serve [--host 127.0.0.1] [--port 8787] [--user quarkpp]
        [--password PASSWORD] [--read-only] [--temp-dir DIRECTORY]
  help | --help
  --version

Configuration defaults to config/quarkpp.local.json in the current directory.
QUARKPP_CONFIG overrides its path; QUARKPP_COOKIE overrides its cookie.
Use QUARKPP_WEBDAV_PASSWORD for the local WebDAV password (required).
The WebDAV bridge supports rclone mount on Windows, Linux, and macOS.
Uploads through WebDAV spool to temporary disk before uploading to Quark.
--progress reports current-file transferred/total bytes to stderr once per second.
)";
}

AppConfig load_config(const std::optional<std::string> &explicit_path)
{
    AppConfig config;
    const auto env_path = environment("QUARKPP_CONFIG");
    const auto path = path_from_utf8(explicit_path.value_or(env_path.value_or("config/quarkpp.local.json")));
    if (std::filesystem::exists(path))
    {
        std::ifstream input(path);
        if (!input)
            throw QuarkException("Cannot read configuration file");
        json data;
        try
        {
            data = json::parse(input);
        }
        catch (const json::parse_error &)
        {
            throw QuarkException("Invalid JSON configuration file");
        }
        config.cookie = data.value("cookie", "");
        config.user_agent = data.value("user_agent", "");
        config.download_dir = path_from_utf8(data.value("download_dir", "downloads"));
        config.request_timeout_ms = data.value("request_timeout_ms", 120000U);
        config.upload_retry_count = data.value("upload_retry_count", 3);
        config.download_retry_count = data.value("download_retry_count", 3);
        config.task_poll_limit = data.value("task_poll_limit", 30U);
        config.ca_bundle = data.value("ca_bundle", "");
        config.api_base = data.value("api_base", config.api_base);
        config.offline_api_base = data.value("offline_api_base", config.offline_api_base);
        config.upload_base = data.value("upload_base", config.upload_base);
        const auto secure_number =
            data.value("offline_secure_number", static_cast<unsigned>(config.offline_secure_number));
        if (secure_number > 65535)
            throw QuarkException("offline_secure_number exceeds 16 bits");
        config.offline_secure_number = static_cast<std::uint16_t>(secure_number);
        config.offline_key = data.value("offline_key", config.offline_key);
    }
    else if (explicit_path || env_path)
        throw QuarkException("Configuration file does not exist");
    if (const auto cookie = environment("QUARKPP_COOKIE"))
        config.cookie = *cookie;
    return config;
}

std::string fid(QuarkClient &client, Arguments &args, std::optional<std::string> fallback = {},
                const std::string &id_flag = "--fid", const std::string &path_flag = "--path")
{
    const auto id = option(args, id_flag);
    const auto path = option(args, path_flag);
    if (id && path)
        throw QuarkException("Choose either " + id_flag + " or " + path_flag);
    if (id)
        return *id;
    if (path)
        return client.resolve_path(*path).fid;
    if (fallback)
        return *fallback;
    throw QuarkException("Specify " + id_flag + " or " + path_flag);
}

json entry_json(const RemoteEntry &entry)
{
    return {{"fid", entry.fid},
            {"name", entry.name},
            {"parent_fid", entry.parent_fid},
            {"size", entry.size},
            {"dir", entry.dir}};
}

void print_entries(const std::vector<RemoteEntry> &entries, bool as_json)
{
    if (as_json)
    {
        auto output = json::array();
        for (const auto &entry : entries)
            output.push_back(entry_json(entry));
        std::cout << output.dump(2) << '\n';
        return;
    }
    std::cout << "TYPE  SIZE          FID                               NAME\n";
    for (const auto &entry : entries)
        std::cout << std::left << std::setw(6) << (entry.dir ? "dir" : "file") << std::setw(14)
                  << (entry.dir ? "-" : format_bytes(entry.size)) << std::setw(34) << entry.fid << entry.name << '\n';
}

void print(const json &value)
{
    std::cout << value.dump(2) << '\n';
}

class TransferProgress
{
  public:
    TransferProgress(bool enabled, std::string operation) : enabled_(enabled), operation_(std::move(operation))
    {
    }

    ProgressCallback callback()
    {
        if (!enabled_)
            return {};
        return [this](std::uint64_t transferred, std::uint64_t total)
        {
            transferred_ = transferred;
            total_ = total;
            const auto now = std::chrono::steady_clock::now();
            // Completion is printed only after the operation itself succeeds.
            if (transferred == total || (reported_ && now - last_ < std::chrono::seconds(1)))
                return;
            std::cerr << operation_ << ": " << transferred_ << '/' << total_ << " bytes\n";
            last_ = now;
            reported_ = true;
        };
    }

    void finish() const
    {
        if (enabled_)
            std::cerr << operation_ << ": complete " << transferred_ << '/' << total_ << " bytes\n";
    }

  private:
    bool enabled_;
    std::string operation_;
    bool reported_{false};
    std::uint64_t transferred_{};
    std::uint64_t total_{};
    std::chrono::steady_clock::time_point last_{};
};

int run(Arguments args)
{
    const auto config_path = option(args, "--config");
    const auto as_json = flag(args, "--json");
    if (args.empty() || flag(args, "--help") || flag(args, "-h") || args.front() == "help")
    {
        help();
        return 0;
    }
    if (flag(args, "--version"))
    {
        exhausted(args);
        std::cout << "QuarkPlusPlus 0.2.0\n";
        return 0;
    }
    const auto command = positional(args, "command");
    if (command == "login")
    {
        cli::LoginOptions options;
        options.timeout_seconds = number(option(args, "--timeout").value_or("300"), "login timeout", 1, 300);
        if (const auto svg = option(args, "--qr-svg"))
            options.qr_svg_path = path_from_utf8(*svg);
        options.ca_bundle = option(args, "--ca-bundle").value_or("");
        exhausted(args);
        const auto destination =
            path_from_utf8(config_path.value_or(environment("QUARKPP_CONFIG").value_or("config/quarkpp.local.json")));
        return cli::run_login(destination, options);
    }
    QuarkClient client(load_config(config_path));
    if (command == "account")
    {
        exhausted(args);
        print(client.account_info());
    }
    else if (command == "ls" || command == "search")
    {
        const auto keyword = command == "search" ? positional(args, "search text") : std::string{};
        const auto id = fid(client, args, "0");
        exhausted(args);
        print_entries(command == "ls" ? client.list_directory(id) : client.search(keyword, id), as_json);
    }
    else if (command == "info" || command == "rm")
    {
        const auto id = fid(client, args);
        exhausted(args);
        print(command == "info" ? entry_json(client.file_info(id)) : client.delete_items({id}));
    }
    else if (command == "mkdir" || command == "rename")
    {
        const auto name = positional(args, "name");
        const auto id =
            command == "mkdir" ? fid(client, args, "0", "--parent-fid", "--parent-path") : fid(client, args);
        exhausted(args);
        print(command == "mkdir" ? client.create_folder(id, name) : client.rename(id, name));
    }
    else if (command == "upload")
    {
        const auto local = path_from_utf8(positional(args, "local file"));
        const auto id = fid(client, args, "0", "--to-fid", "--to-path");
        const auto name = option(args, "--name");
        TransferProgress progress(flag(args, "--progress"), "upload");
        exhausted(args);
        const auto result = client.upload_file(local, id, name, progress.callback());
        progress.finish();
        print({{"fid", result.fid},
               {"name", result.file_name},
               {"size", result.size},
               {"fast_upload", result.fast_upload}});
    }
    else if (command == "download")
    {
        const auto id = fid(client, args);
        const auto output = option(args, "--out");
        const auto resume = !flag(args, "--no-resume");
        TransferProgress progress(flag(args, "--progress"), "download");
        exhausted(args);
        const auto destination = output ? path_from_utf8(*output) : client.config().download_dir;
        client.download(id, destination, resume, progress.callback());
        progress.finish();
        print({{"status", "ok"}, {"output", path_to_utf8(destination)}});
    }
    else if (command == "offline")
    {
        const auto action = positional(args, "offline action");
        if (action == "parse")
        {
            const auto source = positional(args, "source");
            exhausted(args);
            print(client.offline_parse(source));
        }
        else if (action == "add")
        {
            const auto source = positional(args, "source");
            const auto id = fid(client, args, "0", "--to-fid", "--to-path");
            const auto select = option(args, "--select");
            exhausted(args);
            std::vector<int> selected;
            if (select)
            {
                std::size_t start{};
                do
                {
                    const auto end = select->find(',', start);
                    selected.push_back(static_cast<int>(number(select->substr(start, end - start), "file index", 0)));
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                } while (true);
            }
            print(client.offline_add(source, id, selected));
        }
        else if (action == "list")
        {
            const auto page = number(option(args, "--page").value_or("1"), "page");
            const auto size = number(option(args, "--size").value_or("50"), "page size", 1, 100);
            exhausted(args);
            print(client.offline_tasks(page, size));
        }
        else if (action == "status")
        {
            const auto task = positional(args, "task id");
            const auto wait = flag(args, "--wait");
            exhausted(args);
            print(wait ? client.wait_task(task) : client.task_status(task));
        }
        else
            throw QuarkException("Unknown offline action: " + action);
    }
    else if (command == "serve")
    {
        WebDavOptions options;
        options.host = option(args, "--host").value_or("127.0.0.1");
        options.port = static_cast<int>(number(option(args, "--port").value_or("8787"), "port", 1, 65535));
        options.username = option(args, "--user").value_or("quarkpp");
        options.password = option(args, "--password").value_or(environment("QUARKPP_WEBDAV_PASSWORD").value_or(""));
        options.read_only = flag(args, "--read-only");
        if (const auto path = option(args, "--temp-dir"))
            options.temp_directory = path_from_utf8(*path);
        exhausted(args);
        WebDavServer server(client, options);
        const auto port = server.bind();
        const auto host = options.host == "::1" ? "[::1]" : options.host;
        std::cout << "WebDAV listening on http://" << host << ':' << port << "/ ("
                  << (options.read_only ? "read only" : "read/write") << ")\n"
                  << "Username: " << options.username << "; password: supplied separately. Press Ctrl+C to stop.\n"
                  << std::flush;
        if (!server.listen())
            throw QuarkException("WebDAV server stopped unexpectedly");
    }
    else
        throw QuarkException("Unknown command: " + command + ". Use --help for usage.");
    return 0;
}
} // namespace
} // namespace quarkpp

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        const int length = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string value(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, value.data(), length, nullptr, nullptr);
        if (!value.empty())
            value.pop_back();
        args.push_back(std::move(value));
    }
#else
int main(int argc, char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
#endif
    try
    {
        return quarkpp::run(std::move(args));
    }
    catch (const std::exception &error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
