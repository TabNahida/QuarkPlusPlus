#include "quark_client.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace quarkpp {

using ArgList = std::vector<std::string>;

namespace {

bool g_use_color = true;
bool g_use_icons = true;

[[nodiscard]] std::optional<std::string> getenv_string(const char* name);

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

[[nodiscard]] std::optional<std::filesystem::path> executable_path() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::nullopt;
    }
    return std::filesystem::weakly_canonical(std::filesystem::path(buffer.c_str()));
#elif defined(__linux__)
    std::vector<char> buffer(1024, '\0');
    for (;;) {
        const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return std::nullopt;
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    return std::nullopt;
#endif
}

[[nodiscard]] std::optional<std::filesystem::path> find_upwards(const std::filesystem::path& start,
                                                                const std::filesystem::path& relative) {
    auto current = std::filesystem::weakly_canonical(start);
    for (;;) {
        const auto candidate = current / relative;
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
        const auto parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
    }
    return std::nullopt;
}

[[nodiscard]] std::filesystem::path resolve_config_path(const std::optional<std::string>& explicit_path) {
    const auto requested = path_from_utf8(explicit_path.value_or(
        getenv_string("QUARKPP_CONFIG").value_or("config/quarkpp.local.json")));
    if (requested.is_absolute() || explicit_path || getenv_string("QUARKPP_CONFIG")) {
        return requested;
    }

    if (std::filesystem::exists(requested)) {
        return requested;
    }

    if (const auto found = find_upwards(std::filesystem::current_path(), requested)) {
        return *found;
    }

    if (const auto exe = executable_path()) {
        if (const auto found = find_upwards(exe->parent_path(), requested)) {
            return *found;
        }
    }

    return requested;
}

void enable_virtual_terminal() {
#if defined(_WIN32)
    auto enable_for = [](DWORD handle_id) {
        const auto handle = GetStdHandle(handle_id);
        if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
            return;
        }
        DWORD mode = 0;
        if (GetConsoleMode(handle, &mode) == 0) {
            return;
        }
        SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    };

    enable_for(STD_OUTPUT_HANDLE);
    enable_for(STD_ERROR_HANDLE);
#endif
}

void init_console_encoding() {
    g_use_color = !getenv_string("NO_COLOR").has_value() && !getenv_string("QUARKPP_NO_COLOR").has_value();
    g_use_icons = !getenv_string("QUARKPP_NO_ICONS").has_value();

#if defined(_WIN32)
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    enable_virtual_terminal();
#endif
}

#if defined(_WIN32)
[[nodiscard]] ArgList collect_args(int argc, wchar_t** argv) {
    ArgList args;
    args.reserve(static_cast<std::size_t>(std::max(0, argc - 1)));
    for (int index = 1; index < argc; ++index) {
        args.push_back(wide_to_utf8(argv[index]));
    }
    return args;
}
#else
[[nodiscard]] ArgList collect_args(int argc, char** argv) {
    ArgList args;
    args.reserve(static_cast<std::size_t>(std::max(0, argc - 1)));
    for (int index = 1; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }
    return args;
}
#endif

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

[[nodiscard]] std::uint32_t next_codepoint(std::string_view text, std::size_t& index) {
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80) {
        ++index;
        return lead;
    }
    if ((lead >> 5) == 0x6 && index + 1 < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[index + 1]);
        index += 2;
        return ((lead & 0x1F) << 6) | (b1 & 0x3F);
    }
    if ((lead >> 4) == 0xE && index + 2 < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[index + 1]);
        const auto b2 = static_cast<unsigned char>(text[index + 2]);
        index += 3;
        return ((lead & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
    }
    if ((lead >> 3) == 0x1E && index + 3 < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[index + 1]);
        const auto b2 = static_cast<unsigned char>(text[index + 2]);
        const auto b3 = static_cast<unsigned char>(text[index + 3]);
        index += 4;
        return ((lead & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
    }
    ++index;
    return 0xFFFD;
}

[[nodiscard]] int codepoint_width(std::uint32_t cp) {
    if (cp == 0 || cp == '\n' || cp == '\r' || cp == '\t') {
        return 0;
    }
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) {
        return 0;
    }
    if ((cp >= 0x0300 && cp <= 0x036F) ||
        (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x1DC0 && cp <= 0x1DFF) ||
        (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE20 && cp <= 0xFE2F)) {
        return 0;
    }
    if (cp >= 0x1100 &&
        (cp <= 0x115F ||
         cp == 0x2329 || cp == 0x232A ||
         (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
         (cp >= 0xAC00 && cp <= 0xD7A3) ||
         (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0xFE10 && cp <= 0xFE19) ||
         (cp >= 0xFE30 && cp <= 0xFE6F) ||
         (cp >= 0xFF00 && cp <= 0xFF60) ||
         (cp >= 0xFFE0 && cp <= 0xFFE6) ||
         (cp >= 0x1F300 && cp <= 0x1FAFF) ||
         (cp >= 0x20000 && cp <= 0x3FFFD))) {
        return 2;
    }
    return 1;
}

[[nodiscard]] std::size_t display_width(std::string_view text) {
    std::size_t width = 0;
    for (std::size_t index = 0; index < text.size();) {
        width += static_cast<std::size_t>(codepoint_width(next_codepoint(text, index)));
    }
    return width;
}

[[nodiscard]] std::string style(std::string_view text, std::string_view code) {
    if (!g_use_color || code.empty()) {
        return std::string(text);
    }
    return "\x1b[" + std::string(code) + "m" + std::string(text) + "\x1b[0m";
}

[[nodiscard]] std::string pad_right(std::string text, std::size_t visible, std::size_t target) {
    if (visible >= target) {
        return text;
    }
    text.append(target - visible, ' ');
    return text;
}

[[nodiscard]] std::string pad_left(std::string text, std::size_t visible, std::size_t target) {
    if (visible >= target) {
        return text;
    }
    return std::string(target - visible, ' ') + text;
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
        << "  Environment overrides: QUARKPP_CONFIG, QUARKPP_COOKIE\n"
        << "  Output toggles: NO_COLOR, QUARKPP_NO_COLOR, QUARKPP_NO_ICONS\n";
}

[[nodiscard]] AppConfig load_config(const std::optional<std::string>& config_path_override) {
    AppConfig config;
    const auto config_path = resolve_config_path(config_path_override);

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
    struct Row {
        std::string type_plain;
        std::string size_plain;
        std::string fid_plain;
        std::string name_plain;
        std::string type_render;
        std::string size_render;
        std::string fid_render;
        std::string name_render;
    };

    std::vector<Row> rows;
    rows.reserve(entries.size());

    std::size_t type_width = display_width("TYPE");
    std::size_t size_width = display_width("SIZE");
    std::size_t fid_width = display_width("FID");

    for (const auto& entry : entries) {
        const auto icon = g_use_icons ? (entry.dir ? "󰉋 " : "󰈔 ") : "";
        const auto type_plain = entry.dir ? "dir" : "file";
        const auto size_plain = entry.dir ? std::string("-") : format_bytes(entry.size);
        const auto fid_plain = entry.fid;
        const auto name_plain = std::string(icon) + entry.name;

        type_width = std::max(type_width, display_width(type_plain));
        size_width = std::max(size_width, display_width(size_plain));
        fid_width = std::max(fid_width, display_width(fid_plain));

        rows.push_back(Row {
            .type_plain = type_plain,
            .size_plain = size_plain,
            .fid_plain = fid_plain,
            .name_plain = name_plain,
            .type_render = style(type_plain, entry.dir ? "1;36" : "1;32"),
            .size_render = style(size_plain, entry.dir ? "2" : "33"),
            .fid_render = style(fid_plain, "2"),
            .name_render = style(name_plain, entry.dir ? "1;36" : "0"),
        });
    }

    const auto header_type = pad_right(style("TYPE", "1"), display_width("TYPE"), type_width);
    const auto header_size = pad_left(style("SIZE", "1"), display_width("SIZE"), size_width);
    const auto header_fid = pad_right(style("FID", "1"), display_width("FID"), fid_width);
    const auto header_name = style("NAME", "1");

    std::cout << header_type << "  "
              << header_size << "  "
              << header_fid << "  "
              << header_name << '\n';

    for (const auto& row : rows) {
        std::cout << pad_right(row.type_render, display_width(row.type_plain), type_width) << "  "
                  << pad_left(row.size_render, display_width(row.size_plain), size_width) << "  "
                  << pad_right(row.fid_render, display_width(row.fid_plain), fid_width) << "  "
                  << row.name_render << '\n';
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

int run_app(ArgList args) {
    init_console_encoding();

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
}

}  // namespace quarkpp

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    try {
        return quarkpp::run_app(quarkpp::collect_args(argc, argv));
    } catch (const quarkpp::QuarkException& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
#else
int main(int argc, char** argv) {
    try {
        return quarkpp::run_app(quarkpp::collect_args(argc, argv));
    } catch (const quarkpp::QuarkException& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
#endif
