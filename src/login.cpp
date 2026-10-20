#include "login.hpp"

#include "qrcodegen.hpp"
#include "quarkpp/auth.hpp"
#include "quarkpp/support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <iostream>
#include <limits>
#include <memory>
#include <openssl/rand.h>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace quarkpp::cli
{
namespace
{
constexpr std::size_t max_config_bytes = 1024 * 1024;
volatile std::sig_atomic_t interrupted = 0;

void interrupt_handler(int)
{
    interrupted = 1;
}

struct Signals
{
    using Handler = void (*)(int);
    Handler previous_int{};
    Handler previous_term{};
    Signals()
    {
        interrupted = 0;
        previous_int = std::signal(SIGINT, interrupt_handler);
        previous_term = std::signal(SIGTERM, interrupt_handler);
    }
    ~Signals()
    {
        if (previous_int != SIG_ERR)
            std::signal(SIGINT, previous_int);
        if (previous_term != SIG_ERR)
            std::signal(SIGTERM, previous_term);
    }
};

#ifdef _WIN32
struct Handle
{
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle()
    {
        if (value != INVALID_HANDLE_VALUE && value != nullptr)
            CloseHandle(value);
    }
};

struct PrivateSecurity
{
    Handle token;
    std::vector<unsigned char> user;
    PACL acl{};
    SECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
    PrivateSecurity()
    {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
            throw QuarkException("Unable to establish private credential permissions");
        DWORD size = 0;
        GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
        user.resize(size);
        if (!size || !GetTokenInformation(token.value, TokenUser, user.data(), size, &size))
            throw QuarkException("Unable to identify credential owner");
        auto sid = reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid;
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = GENERIC_ALL;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS)
            throw QuarkException("Unable to create private credential permissions");
        if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorOwner(&descriptor, sid, FALSE) ||
            !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
            !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        {
            LocalFree(acl);
            acl = nullptr;
            throw QuarkException("Unable to establish private credential permissions");
        }
    }
    ~PrivateSecurity()
    {
        if (acl)
            LocalFree(acl);
    }
};

struct Terminal
{
    HANDLE error_handle{GetStdHandle(STD_ERROR_HANDLE)};
    DWORD mode{};
    UINT codepage{};
    bool console{GetConsoleMode(error_handle, &mode) != FALSE};
    Terminal()
    {
        if (console)
        {
            codepage = GetConsoleOutputCP();
            SetConsoleOutputCP(CP_UTF8);
            SetConsoleMode(error_handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
    ~Terminal()
    {
        if (console)
        {
            SetConsoleMode(error_handle, mode);
            SetConsoleOutputCP(codepage);
        }
    }
};
#else
struct FileDescriptor
{
    int value{-1};
    ~FileDescriptor()
    {
        if (value >= 0)
            ::close(value);
    }
};
struct Terminal
{
};
#endif

std::string random_suffix()
{
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw QuarkException("Unable to allocate a private credential file");
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    for (const auto byte : bytes)
    {
        output += digits[byte >> 4];
        output += digits[byte & 15];
    }
    return output;
}

class PrivateFile
{
  public:
    explicit PrivateFile(const std::filesystem::path &path)
    {
        if (path.empty() || path.filename().empty() || path.filename() == "." || path.filename() == "..")
            throw QuarkException("A credential file path is required");
        std::error_code error;
        auto absolute = std::filesystem::absolute(path, error);
        if (error)
            throw QuarkException("Unable to resolve credential directory");
        std::filesystem::create_directories(absolute.parent_path(), error);
        if (error)
            throw QuarkException("Unable to create credential directory");
        // Pin the resolved directory, so aliases such as macOS /var are harmless.
        auto parent = std::filesystem::canonical(absolute.parent_path(), error);
        if (error)
            throw QuarkException("Unable to resolve credential directory");
        path_ = parent / absolute.filename();
#ifndef _WIN32
        directory_.value = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory_.value < 0)
            throw QuarkException("Unable to open credential directory");
#endif
    }

    [[nodiscard]] const std::filesystem::path &path() const
    {
        return path_;
    }

    std::string read()
    {
#ifdef _WIN32
        Handle file{CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() == ERROR_FILE_NOT_FOUND)
                return {};
            throw QuarkException("Unable to read existing credential file");
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file.value, &info) || GetFileType(file.value) != FILE_TYPE_DISK ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
            throw QuarkException("Credential destination must be a regular file, not a link");
        if (info.nFileSizeHigh || info.nFileSizeLow > max_config_bytes)
            throw QuarkException("Existing credential file exceeds the size limit");
        std::string contents(info.nFileSizeLow, '\0');
        DWORD count = 0;
        if (!ReadFile(file.value, contents.data(), static_cast<DWORD>(contents.size()), &count, nullptr) ||
            count != contents.size())
            throw QuarkException("Unable to read existing credential file");
#else
        FileDescriptor file{
            ::openat(directory_.value, path_.filename().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
        if (file.value < 0)
        {
            if (errno == ENOENT)
                return {};
            throw QuarkException("Unable to read credential file; symbolic links are not allowed");
        }
        struct stat info{};
        if (::fstat(file.value, &info) || !S_ISREG(info.st_mode))
            throw QuarkException("Credential destination must be a regular file, not a link");
        if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > max_config_bytes)
            throw QuarkException("Existing credential file exceeds the size limit");
        std::string contents(static_cast<std::size_t>(info.st_size), '\0');
        std::size_t offset = 0;
        while (offset < contents.size())
        {
            const auto count = ::read(file.value, contents.data() + offset, contents.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw QuarkException("Unable to read existing credential file");
            offset += static_cast<std::size_t>(count);
        }
#endif
        return contents;
    }

    void replace(std::string_view contents)
    {
        // Revalidate the destination immediately before replacement. Neither
        // renameat nor MoveFileEx follows a final destination symbolic link.
        static_cast<void>(read());
        auto temporary = path_;
        temporary += ".login-" + random_suffix() + ".tmp";
#ifdef _WIN32
        PrivateSecurity security;
        Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &security.attributes, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE)
            throw QuarkException("Unable to create private credential file");
        try
        {
            DWORD count = 0;
            if (contents.size() > std::numeric_limits<DWORD>::max() ||
                !WriteFile(file.value, contents.data(), static_cast<DWORD>(contents.size()), &count, nullptr) ||
                count != contents.size() || !FlushFileBuffers(file.value))
                throw QuarkException("Unable to write private credential file");
            CloseHandle(file.value);
            file.value = INVALID_HANDLE_VALUE;
            if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw QuarkException("Unable to replace credential file atomically");
        }
        catch (...)
        {
            if (file.value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(file.value);
                file.value = INVALID_HANDLE_VALUE;
            }
            DeleteFileW(temporary.c_str());
            throw;
        }
#else
        FileDescriptor file{::openat(directory_.value, temporary.filename().c_str(),
                                     O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
        if (file.value < 0)
            throw QuarkException("Unable to create private credential file");
        try
        {
            if (::fchmod(file.value, 0600))
                throw QuarkException("Unable to protect credential file permissions");
            std::size_t offset = 0;
            while (offset < contents.size())
            {
                const auto count = ::write(file.value, contents.data() + offset, contents.size() - offset);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    throw QuarkException("Unable to write private credential file");
                offset += static_cast<std::size_t>(count);
            }
            if (::fsync(file.value))
                throw QuarkException("Unable to flush credential file");
            if (::renameat(directory_.value, temporary.filename().c_str(), directory_.value, path_.filename().c_str()))
                throw QuarkException("Unable to replace credential file atomically");
            // File data is durable; directory sync is unsupported on some file systems.
            static_cast<void>(::fsync(directory_.value));
        }
        catch (...)
        {
            ::unlinkat(directory_.value, temporary.filename().c_str(), 0);
            throw;
        }
#endif
    }

  private:
    std::filesystem::path path_;
#ifndef _WIN32
    FileDescriptor directory_;
#endif
};

qrcodegen::QrCode encode(std::string_view url)
{
    if (url.empty() || url.find('\0') != std::string_view::npos || url.size() > 4096)
        throw QuarkException("Invalid QR login URL");
    return qrcodegen::QrCode::encodeText(std::string(url).c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
}
} // namespace

void save_login_cookie(const std::filesystem::path &config_path, std::string_view cookie)
{
    if (cookie.empty() || cookie.size() > 65536 ||
        std::any_of(cookie.begin(), cookie.end(), [](unsigned char value) { return value < 32 || value >= 127; }))
        throw QuarkException("The login service returned an invalid credential");
    PrivateFile destination(config_path);
    const auto existing = destination.read();
    json document = json::object();
    if (!existing.empty())
    {
        document = json::parse(existing, nullptr, false);
        if (!document.is_object())
            throw QuarkException("Existing config is not a JSON object; credentials were not changed");
    }
    document["cookie"] = cookie;
    destination.replace(document.dump(2) + '\n');
}

std::string render_qr_terminal(std::string_view url)
{
    const auto qr = encode(url);
    constexpr int border = 4;
    const auto size = qr.getSize();
    std::string result;
    for (int y = -border; y < size + border; y += 2)
    {
        result += "\x1b[30;107m";
        for (int x = -border; x < size + border; ++x)
        {
            const bool top = qr.getModule(x, y), bottom = qr.getModule(x, y + 1);
            result += top ? (bottom ? "\xe2\x96\x88" : "\xe2\x96\x80") : (bottom ? "\xe2\x96\x84" : " ");
        }
        result += "\x1b[0m\n";
    }
    return result;
}

std::string render_qr_svg(std::string_view url)
{
    const auto qr = encode(url);
    constexpr int border = 4;
    const auto size = qr.getSize() + border * 2;
    std::ostringstream result;
    result << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << size << ' ' << size << "\" width=\""
           << size * 8 << "\" height=\"" << size * 8
           << "\" shape-rendering=\"crispEdges\"><rect width=\"100%\" height=\"100%\" fill=\"white\"/>"
              "<path fill=\"black\" d=\"";
    for (int y = 0; y < qr.getSize(); ++y)
        for (int x = 0; x < qr.getSize(); ++x)
            if (qr.getModule(x, y))
                result << 'M' << x + border << ',' << y + border << "h1v1h-1z";
    result << "\"/></svg>\n";
    return result.str();
}

int run_login(const std::filesystem::path &config_path, const LoginOptions &options)
{
    if (!options.timeout_seconds || options.timeout_seconds > 300)
        throw QuarkException("Login timeout must be between 1 and 300 seconds");
    // Validate the config before asking the user to scan; never replace bad JSON.
    PrivateFile destination(config_path);
    const auto existing = destination.read();
    if (!existing.empty() && !json::parse(existing, nullptr, false).is_object())
        throw QuarkException("Existing config is not a JSON object; credentials were not changed");
    std::unique_ptr<PrivateFile> svg;
    if (!options.qr_svg_path.empty())
    {
        svg = std::make_unique<PrivateFile>(options.qr_svg_path);
        std::error_code error;
        if (svg->path() == destination.path() || std::filesystem::equivalent(svg->path(), destination.path(), error))
            throw QuarkException("QR image and credential config must use different files");
        static_cast<void>(svg->read());
    }
    QrLoginOptions auth_options;
    auth_options.login_timeout_seconds = options.timeout_seconds;
    auth_options.ca_bundle = options.ca_bundle;
    QrLoginClient auth(auth_options);
    Signals signals;
    [[maybe_unused]] Terminal terminal;
    const auto code = auth.start();
    if (svg)
        svg->replace(render_qr_svg(code.url));
    std::cerr << "Scan this QR code with the Quark mobile app, then confirm login.\n"
              << render_qr_terminal(code.url) << "Waiting for confirmation (Ctrl+C to cancel, expires in "
              << code.expires_in_seconds << " seconds).\n"
              << std::flush;
    for (;;)
    {
        if (interrupted)
        {
            auth.cancel();
            std::cerr << "Login cancelled.\n";
            return 130;
        }
        const auto result = auth.poll();
        if (interrupted)
            continue;
        if (result.status == QrLoginStatus::authenticated)
        {
            save_login_cookie(config_path, result.cookie);
            std::cerr << "Login successful. Credentials saved privately.\n";
            std::cout << json{{"logged_in", true}, {"config", path_to_utf8(config_path)}}.dump(2) << '\n';
            return 0;
        }
        if (result.status == QrLoginStatus::expired)
            throw QuarkException("QR login expired; run login again to get a new code");
        if (result.status == QrLoginStatus::cancelled)
            return 130;
        for (int i = 0; i < 20 && !interrupted; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
} // namespace quarkpp::cli
