#include "quark_client.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>
#endif

namespace quarkpp {

namespace {

using ArgList = std::vector<std::string>;

#if defined(_WIN32)
[[nodiscard]] std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }

    const auto size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw QuarkException("宽字符转 UTF-8 失败");
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

[[nodiscard]] std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) {
        return {};
    }

    const auto size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        throw QuarkException("UTF-8 转宽字符失败");
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
#endif

[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view value) {
#if defined(_WIN32)
    return std::filesystem::path(utf8_to_wide(value));
#else
    return std::filesystem::path(std::string(value));
#endif
}

void init_console_encoding() {
#if defined(_WIN32)
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
}

[[nodiscard]] ArgList collect_args(int argc, char** argv) {
#if defined(_WIN32)
    int wide_argc = 0;
    LPWSTR* wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (wide_argv != nullptr) {
        ArgList args;
        args.reserve(static_cast<std::size_t>(std::max(0, wide_argc - 1)));
        for (int index = 1; index < wide_argc; ++index) {
            args.push_back(wide_to_utf8(wide_argv[index]));
        }
        LocalFree(wide_argv);
        return args;
    }
#endif

    ArgList args;
    args.reserve(static_cast<std::size_t>(std::max(0, argc - 1)));
    for (int index = 1; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }
    return args;
}

[[nodiscard]] std::optional<std::string> getenv_string(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr || length == 0) {
        return std::nullopt;
    }
    std::unique_ptr<char, decltype(&std::free)> holder(value, &std::free);
    return std::string(holder.get());
#else
    if (const auto* value = std::getenv(name); value != nullptr && *value != '\0') {
        return std::string(value);
    }
    return std::nullopt;
#endif
}

[[nodiscard]] bool consume_flag(ArgList& args, std::string_view flag) {
    const auto it = std::ranges::find(args, flag);
    if (it == args.end()) {
        return false;
    }
    args.erase(it);
    return true;
}

[[nodiscard]] std::optional<std::string> consume_option(ArgList& args, std::string_view name) {
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == name) {
            if (index + 1 >= args.size()) {
                throw QuarkException("缺少参数值: " + std::string(name));
            }
            auto value = args[index + 1];
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(index),
                       args.begin() + static_cast<std::ptrdiff_t>(index + 2));
            return value;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string require_positional(ArgList& args, std::string_view label) {
    if (args.empty()) {
        throw QuarkException("缺少参数: " + std::string(label));
    }
    auto value = args.front();
    args.erase(args.begin());
    return value;
}

[[nodiscard]] json entry_to_json(const RemoteEntry& entry) {
    return {
        {"fid", entry.fid},
        {"file_name", entry.name},
        {"pdir_fid", entry.parent_fid},
        {"dir", entry.dir},
        {"size", entry.size},
    };
}

void print_help() {
    std::cout
        << "QuarkPlusPlus CLI\n\n"
        << "Usage:\n"
        << "  quarkpp [--config path] [--json] <command> [options]\n\n"
        << "Commands:\n"
        << "  account                              Show account info\n"
        << "  growth-info                          Show daily growth/sign-in info\n"
        << "  growth-sign                          Perform daily growth sign-in\n"
        << "  ls [--fid id | --path /remote]       List a folder, default root\n"
        << "  search <keyword> [--fid id | --path /remote]\n"
        << "  info --fid id | --path /remote       Show file/folder info\n"
        << "  mkdir <name> [--parent-fid id | --parent-path /remote]\n"
        << "  rename <new-name> --fid id | --path /remote\n"
        << "  rm --fid id | --path /remote         Delete a file/folder\n"
        << "  share-create --fid id | --path /remote [--title t] [--expire permanent|1d|7d|30d] [--passcode code]\n"
        << "  share-list [--page n] [--size n]\n"
        << "  transfer <share-url> [--passcode code] [--to-fid id | --to-path /remote]\n"
        << "  upload <local-file> [--to-fid id | --to-path /remote] [--name remote-name]\n"
        << "  download --fid id | --path /remote [--out dir] [--no-resume]\n\n"
        << "Configuration:\n"
        << "  Default config path: config/quarkpp.local.json\n"
        << "  Ignored example path: config/quarkpp.example.json\n"
        << "  Environment overrides: QUARKPP_CONFIG, QUARKPP_COOKIE\n";
}

[[nodiscard]] AppConfig load_config(const std::optional<std::string>& config_path_override) {
    AppConfig config;
    const auto config_path = path_from_utf8(config_path_override.value_or(
        getenv_string("QUARKPP_CONFIG").value_or("config/quarkpp.local.json")));

    if (std::filesystem::exists(config_path)) {
        std::ifstream input(config_path);
        if (!input) {
            throw QuarkException("无法打开配置文件: " + config_path.string());
        }
        json file_config = json::parse(input);
        config.cookie = file_config.value("cookie", "");
        config.user_agent = file_config.value("user_agent", "");
        config.download_dir = path_from_utf8(file_config.value("download_dir", "downloads"));
        config.request_timeout_ms = file_config.value("request_timeout_ms", 120000U);
        config.upload_retry_count = file_config.value("upload_retry_count", 3);
    }

    if (const auto cookie = getenv_string("QUARKPP_COOKIE")) {
        config.cookie = *cookie;
    }
    return config;
}

[[nodiscard]] std::string resolve_fid(QuarkClient& client,
                                      ArgList& args,
                                      std::optional<std::string> default_fid = std::nullopt,
                                      std::string_view fid_flag = "--fid",
                                      std::string_view path_flag = "--path") {
    if (const auto fid = consume_option(args, fid_flag)) {
        return *fid;
    }
    if (const auto path = consume_option(args, path_flag)) {
        return client.resolve_path(*path).fid;
    }
    if (default_fid) {
        return *default_fid;
    }
    throw QuarkException("需要提供 --fid 或 --path");
}

[[nodiscard]] int parse_expire_type(const std::optional<std::string>& raw) {
    if (!raw || raw->empty() || *raw == "permanent") {
        return 1;
    }
    if (*raw == "1d") {
        return 2;
    }
    if (*raw == "7d") {
        return 3;
    }
    if (*raw == "30d") {
        return 4;
    }
    throw QuarkException("无效的过期时间，请使用 permanent|1d|7d|30d");
}

void print_entries(const std::vector<RemoteEntry>& entries) {
    std::cout << std::left << std::setw(8) << "TYPE"
              << std::setw(18) << "SIZE"
              << std::setw(24) << "FID"
              << "NAME\n";
    for (const auto& entry : entries) {
        std::cout << std::left << std::setw(8) << (entry.dir ? "dir" : "file")
                  << std::setw(18) << (entry.dir ? "-" : format_bytes(entry.size))
                  << std::setw(24) << entry.fid
                  << entry.name << '\n';
    }
}

void print_json_or_entries(const std::vector<RemoteEntry>& entries, bool json_output) {
    if (json_output) {
        json output = json::array();
        for (const auto& entry : entries) {
            output.push_back(entry_to_json(entry));
        }
        std::cout << output.dump(2) << '\n';
        return;
    }
    print_entries(entries);
}

void print_json_or_value(const json& value, bool json_output) {
    if (json_output) {
        std::cout << value.dump(2) << '\n';
        return;
    }
    std::cout << value.dump(2) << '\n';
}

}  // namespace

}  // namespace quarkpp

int main(int argc, char** argv) {
    using namespace quarkpp;

    try {
        init_console_encoding();
        auto args = collect_args(argc, argv);

        const bool json_output = consume_flag(args, "--json");
        const auto config_path = consume_option(args, "--config");

        if (args.empty() || consume_flag(args, "--help") || consume_flag(args, "-h") || args.front() == "help") {
            print_help();
            return 0;
        }

        const auto command = require_positional(args, "command");
        auto client = QuarkClient(load_config(config_path));

        if (command == "account") {
            print_json_or_value(client.account_info(), json_output);
            return 0;
        }

        if (command == "growth-info") {
            print_json_or_value(client.growth_info(), json_output);
            return 0;
        }

        if (command == "growth-sign") {
            print_json_or_value(client.growth_sign(), json_output);
            return 0;
        }

        if (command == "ls") {
            const auto fid = resolve_fid(client, args, std::string("0"));
            print_json_or_entries(client.list_directory(fid), json_output);
            return 0;
        }

        if (command == "search") {
            const auto keyword = require_positional(args, "keyword");
            const auto fid = resolve_fid(client, args, std::string("0"));
            print_json_or_entries(client.search(keyword, fid), json_output);
            return 0;
        }

        if (command == "info") {
            const auto fid = resolve_fid(client, args);
            print_json_or_value(entry_to_json(client.file_info(fid)), json_output);
            return 0;
        }

        if (command == "mkdir") {
            const auto name = require_positional(args, "name");
            const auto parent_fid = resolve_fid(client, args, std::string("0"), "--parent-fid", "--parent-path");
            print_json_or_value(client.create_folder(parent_fid, name), json_output);
            return 0;
        }

        if (command == "rename") {
            const auto new_name = require_positional(args, "new-name");
            const auto fid = resolve_fid(client, args);
            print_json_or_value(client.rename(fid, new_name), json_output);
            return 0;
        }

        if (command == "rm") {
            const auto fid = resolve_fid(client, args);
            print_json_or_value(client.delete_items({fid}), json_output);
            return 0;
        }

        if (command == "share-create") {
            const auto fid = resolve_fid(client, args);
            const auto title = consume_option(args, "--title").value_or(client.file_info(fid).name);
            const auto expire_type = parse_expire_type(consume_option(args, "--expire"));
            const auto passcode = consume_option(args, "--passcode");
            print_json_or_value(client.share_create({fid}, title, expire_type, 1, passcode), json_output);
            return 0;
        }

        if (command == "share-list") {
            const auto page = consume_option(args, "--page").value_or("1");
            const auto size = consume_option(args, "--size").value_or("50");
            print_json_or_value(client.share_list(std::stoi(page), std::stoi(size)), json_output);
            return 0;
        }

        if (command == "transfer") {
            const auto share_url = require_positional(args, "share-url");
            const auto passcode = consume_option(args, "--passcode");
            const auto target_fid = resolve_fid(client, args, std::string("0"), "--to-fid", "--to-path");
            print_json_or_value(client.transfer_share(share_url, target_fid, passcode), json_output);
            return 0;
        }

        if (command == "upload") {
            const auto local_file = require_positional(args, "local-file");
            const auto target_fid = resolve_fid(client, args, std::string("0"), "--to-fid", "--to-path");
            const auto remote_name = consume_option(args, "--name");
            const auto result = client.upload_file(path_from_utf8(local_file), target_fid, remote_name);
            json output {
                {"fid", result.fid},
                {"file_name", result.file_name},
                {"size", result.size},
                {"fast_upload", result.fast_upload},
            };
            print_json_or_value(output, json_output);
            return 0;
        }

        if (command == "download") {
            const auto fid = resolve_fid(client, args);
            const auto out_dir = consume_option(args, "--out");
            const bool resume = !consume_flag(args, "--no-resume");
            const auto output_dir = out_dir ? path_from_utf8(*out_dir) : client.config().download_dir;
            client.download(fid, output_dir, resume);
            if (json_output) {
                std::cout << json {{"status", "ok"}, {"output", output_dir.string()}}.dump(2) << '\n';
            }
            return 0;
        }

        throw QuarkException("未知命令: " + command);
    } catch (const QuarkException& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
