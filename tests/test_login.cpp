#include "login.hpp"
#include "quarkpp/support.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>
#else
#include <sys/stat.h>
#endif

namespace
{
void check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template <class F> void must_fail(F operation)
{
    try
    {
        operation();
    }
    catch (const quarkpp::QuarkException &)
    {
        return;
    }
    throw std::runtime_error("Operation unexpectedly succeeded");
}

std::string read(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

void write(const std::filesystem::path &path, const std::string &contents)
{
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
    check(static_cast<bool>(stream), "Write test fixture");
}

void private_permissions(const std::filesystem::path &path)
{
#ifdef _WIN32
    PSECURITY_DESCRIPTOR descriptor{};
    PACL acl{};
    PSID owner{};
    check(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION,
                                &owner, nullptr, &acl, nullptr, &descriptor) == ERROR_SUCCESS,
          "Read credential ACL");
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    const bool protected_dacl =
        GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE && (control & SE_DACL_PROTECTED) != 0;
    bool only_owner = acl && acl->AceCount == 1;
    void *entry{};
    if (only_owner && GetAce(acl, 0, &entry))
    {
        const auto ace = static_cast<ACCESS_ALLOWED_ACE *>(entry);
        only_owner = ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && (ace->Header.AceFlags & INHERITED_ACE) == 0 &&
                     EqualSid(owner, &ace->SidStart);
    }
    else
        only_owner = false;
    LocalFree(descriptor);
    check(protected_dacl && only_owner, "Credentials have a protected owner-only DACL");
#else
    struct stat info{};
    check(::stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "Credentials have 0600 permissions");
#endif
}
} // namespace

int main()
{
    namespace fs = std::filesystem;
    using namespace quarkpp;
    using namespace quarkpp::cli;
    const auto directory =
        fs::temp_directory_path() / ("quarkpp-login-test-" + std::to_string(unix_time_milliseconds_now()));
    fs::create_directories(directory);
    try
    {
        const auto config = directory / "nested" / "quarkpp.local.json";
        save_login_cookie(config, "__puus=first; session=test");
        check(json::parse(read(config)).at("cookie") == "__puus=first; session=test", "Create login config");
        private_permissions(config);

        const json old{{"cookie", "old"}, {"download_dir", "my files"}, {"future_setting", {{"nested", {1, 2, 3}}}}};
        write(config, old.dump());
        const auto hardlink = directory / "previous.json";
        fs::create_hard_link(config, hardlink);
        save_login_cookie(config, "__puus=replacement; session=new");
        const auto updated = json::parse(read(config));
        check(updated.at("cookie") == "__puus=replacement; session=new", "Replace login cookie");
        check(updated.at("download_dir") == old.at("download_dir") &&
                  updated.at("future_setting") == old.at("future_setting"),
              "Preserve unknown config fields");
        check(json::parse(read(hardlink)) == old, "Atomic replacement leaves old inode untouched");
        private_permissions(config);

        const auto before = read(config);
        must_fail([&] { save_login_cookie(config, ""); });
        must_fail([&] { save_login_cookie(config, "session=a\r\nInjected: true"); });
        check(read(config) == before, "Invalid credentials do not alter config");

        write(config, "not valid JSON; secret=do-not-log");
        must_fail([&] { save_login_cookie(config, "session=new"); });
        check(read(config) == "not valid JSON; secret=do-not-log", "Malformed config remains unchanged");
        write(config, "[1,2,3]");
        must_fail([&] { save_login_cookie(config, "session=new"); });
        check(read(config) == "[1,2,3]", "Non-object config remains unchanged");

        const auto link = directory / "linked.json";
        std::error_code link_error;
        fs::create_symlink(config, link, link_error);
        if (!link_error)
        {
            must_fail([&] { save_login_cookie(link, "session=new"); });
            check(fs::is_symlink(link) && read(config) == "[1,2,3]",
                  "Symbolic link is rejected without changing target");
        }
#ifndef _WIN32
        else
            throw std::runtime_error("Unable to create symbolic link fixture");
#endif
        const auto directory_target = directory / "directory.json";
        fs::create_directory(directory_target);
        must_fail([&] { save_login_cookie(directory_target, "session=new"); });
        write(config, before);
        LoginOptions invalid_options;
        invalid_options.timeout_seconds = 0;
        must_fail([&] { static_cast<void>(run_login(config, invalid_options)); });
        LoginOptions same_file;
        same_file.qr_svg_path = config;
        must_fail([&] { static_cast<void>(run_login(config, same_file)); });
        check(read(config) == before, "QR image cannot overwrite login credentials");
        for (const auto &entry : fs::recursive_directory_iterator(directory))
            check(entry.path().extension() != ".tmp", "No temporary credential files remain");

        const auto qr_url = "https://example.com/qr?token=local-test-only";
        const auto terminal = render_qr_terminal(qr_url);
        const auto svg = render_qr_svg(qr_url);
        check(terminal.find("\x1b[30;107m") != std::string::npos, "QR has explicit dark modules on a white background");
        check(svg.find("<svg ") == 0 && svg.find("shape-rendering=\"crispEdges\"") != std::string::npos,
              "QR SVG preserves sharp modules");
        check(terminal.find(qr_url) == std::string::npos && svg.find(qr_url) == std::string::npos,
              "QR representations do not embed plaintext login tokens");
        must_fail([&] { static_cast<void>(render_qr_svg("")); });
        fs::remove_all(directory);
        std::cout << "Login persistence and QR rendering tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        fs::remove_all(directory);
        return 1;
    }
}
