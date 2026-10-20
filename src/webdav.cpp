#include "webdav.hpp"

#include <httplib.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>

namespace quarkpp
{
namespace
{
struct DavError : std::runtime_error
{
    int status;
    DavError(int code, const std::string &message) : std::runtime_error(message), status(code)
    {
    }
};

std::string random_id()
{
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw QuarkException("Cannot obtain secure random bytes");
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes)
    {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

struct TemporaryUpload
{
    std::filesystem::path directory;
    std::filesystem::path file;
    explicit TemporaryUpload(const std::filesystem::path &base)
    {
        if (!std::filesystem::is_directory(base))
            throw DavError(507, "Temporary directory does not exist");
        directory = base / ("quarkpp-" + random_id());
        if (!std::filesystem::create_directory(directory))
            throw DavError(507, "Cannot create temporary directory");
        std::error_code error;
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, error);
        file = directory / "upload";
    }
    ~TemporaryUpload()
    {
        // Only our unpredictable, newly-created directory is ever removed.
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

bool constant_equal(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    unsigned char different{};
    for (std::size_t i = 0; i < a.size(); ++i)
        different |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return different == 0;
}

int hex(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

// Decode once from the raw request target; cpp-httplib's decoded req.path is
// deliberately not used. Encoded path separators cannot introduce components.
std::string remote_path(std::string_view raw)
{
    if (raw.empty() || raw.front() != '/' || raw.starts_with("//"))
        throw DavError(400, "Invalid path");
    if (raw.find_first_of("?#") != std::string_view::npos)
        throw DavError(400, "Query strings are not supported");
    std::string decoded;
    for (std::size_t i = 0; i < raw.size(); ++i)
    {
        auto value = static_cast<unsigned char>(raw[i]);
        if (value == '%')
        {
            if (i + 2 >= raw.size() || hex(raw[i + 1]) < 0 || hex(raw[i + 2]) < 0)
                throw DavError(400, "Malformed percent escape");
            value = static_cast<unsigned char>(hex(raw[i + 1]) * 16 + hex(raw[i + 2]));
            i += 2;
            if (value == '/' || value == '\\')
                throw DavError(400, "Encoded separators are not allowed");
        }
        if (value < 32 || value == 127 || value == '\\')
            throw DavError(400, "Unsafe path character");
        decoded += static_cast<char>(value);
    }
    std::string result, segment;
    std::istringstream components(decoded);
    while (std::getline(components, segment, '/'))
    {
        if (segment == "." || segment == "..")
            throw DavError(400, "Dot segments are not allowed");
        if (!segment.empty())
            result += '/' + segment;
    }
    return result.empty() ? "/" : result;
}

std::pair<std::string, std::string> split_parent(const std::string &path)
{
    if (path == "/")
        throw DavError(403, "The root directory cannot be modified");
    const auto separator = path.find_last_of('/');
    return {separator == 0 ? "/" : path.substr(0, separator), path.substr(separator + 1)};
}

std::string xml(std::string_view value)
{
    std::string result;
    for (const auto c : value)
    {
        switch (c)
        {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        case '\'':
            result += "&apos;";
            break;
        default:
            if (static_cast<unsigned char>(c) >= 32)
                result += c;
        }
    }
    return result;
}

std::string href(const std::string &path, bool directory)
{
    std::string result;
    for (const auto &segment : split_remote_path(path))
        result += '/' + url_encode(segment);
    if (result.empty() || directory)
        result += '/';
    return result;
}

std::uint64_t modified_time(const RemoteEntry &entry)
{
    if (!entry.raw.is_object())
        return 0;
    for (const auto *key : {"updated_at", "last_update_at", "created_at"})
    {
        const auto it = entry.raw.find(key);
        if (it != entry.raw.end() && it->is_number_unsigned())
            return it->get<std::uint64_t>();
        if (it != entry.raw.end() && it->is_number_integer() && it->get<std::int64_t>() > 0)
            return static_cast<std::uint64_t>(it->get<std::int64_t>());
    }
    return 0;
}

std::string etag(const RemoteEntry &entry)
{
    return '"' + url_encode(entry.fid) + '-' + std::to_string(entry.size) + '-' + std::to_string(modified_time(entry)) +
           '"';
}

std::string last_modified(const RemoteEntry &entry)
{
    auto seconds = modified_time(entry);
    if (seconds > 100000000000ULL)
        seconds /= 1000;
    const auto time = static_cast<std::time_t>(seconds);
    std::tm tm{};
#ifdef _WIN32
    if (gmtime_s(&tm, &time) != 0)
        return "Thu, 01 Jan 1970 00:00:00 GMT";
#else
    if (!gmtime_r(&time, &tm))
        return "Thu, 01 Jan 1970 00:00:00 GMT";
#endif
    std::array<char, 64> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buffer.data();
}

std::string properties(const RemoteEntry &entry, const std::string &path)
{
    return "<D:response><D:href>" + xml(href(path, entry.dir)) + "</D:href><D:propstat><D:prop><D:displayname>" +
           xml(entry.name) + "</D:displayname><D:resourcetype>" + (entry.dir ? "<D:collection/>" : "") +
           "</D:resourcetype><D:getcontentlength>" + std::to_string(entry.size) +
           "</D:getcontentlength><D:getcontenttype>" +
           (entry.dir ? "httpd/unix-directory" : "application/octet-stream") +
           "</D:getcontenttype><D:getlastmodified>" + last_modified(entry) + "</D:getlastmodified><D:getetag>" +
           xml(etag(entry)) + "</D:getetag></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>";
}

std::uint64_t integer(std::string_view value)
{
    std::uint64_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size())
        throw DavError(400, "Invalid integer header");
    return result;
}

void check_conditions(const httplib::Request &request, const std::optional<RemoteEntry> &entry)
{
    // Lock-token conditions cannot be honored because this bridge has no LOCK.
    if (request.has_header("If"))
        throw DavError(412, "Lock conditions are not supported");
    if (request.has_header("If-Match"))
    {
        const auto value = request.get_header_value("If-Match");
        if (!entry || (value != "*" && value != etag(*entry)))
            throw DavError(412, "If-Match failed");
    }
    if (request.has_header("If-None-Match"))
    {
        const auto value = request.get_header_value("If-None-Match");
        if (entry && (value == "*" || value == etag(*entry)))
            throw DavError(412, "If-None-Match failed");
    }
}
} // namespace

struct WebDavServer::Impl
{
    QuarkClient &client;
    WebDavOptions options;
    httplib::Server server;
    std::mutex mutation_mutex;
    std::string expected_authorization;

    Impl(QuarkClient &drive, WebDavOptions settings) : client(drive), options(std::move(settings))
    {
        if (options.host != "127.0.0.1" && options.host != "::1" && options.host != "localhost")
            throw QuarkException("WebDAV HTTP is restricted to loopback; use a TLS reverse proxy for remote access");
        if (options.username.empty() || options.password.empty() || options.username.find(':') != std::string::npos)
            throw QuarkException("WebDAV requires a nonempty username and password (QUARKPP_WEBDAV_PASSWORD)");
        if (options.port < 0 || options.port > 65535)
            throw QuarkException("Invalid WebDAV port");
        expected_authorization = "Basic " + base64_encode(options.username + ':' + options.password);
        server.set_payload_max_length(std::numeric_limits<std::size_t>::max());
        server.set_read_timeout(120, 0);
        server.set_write_timeout(120, 0);
        server.set_pre_routing_handler(
            [this](const httplib::Request &request, httplib::Response &response)
            {
                // Custom methods are handled before cpp-httplib consumes a body.
                // Close these connections so unread XML cannot become a request.
                response.set_header("Connection", "close");
                response.set_header("DAV", "1");
                response.set_header("Cache-Control", "no-store");
                response.set_header("X-Content-Type-Options", "nosniff");
                if (!constant_equal(request.get_header_value("Authorization"), expected_authorization))
                {
                    response.status = 401;
                    response.set_header("WWW-Authenticate", "Basic realm=\"QuarkPlusPlus\", charset=\"UTF-8\"");
                    return httplib::Server::HandlerResponse::Handled;
                }
                if (request.method == "PUT")
                    return httplib::Server::HandlerResponse::Unhandled;
                handle(response, [&] { dispatch(request, response); });
                return httplib::Server::HandlerResponse::Handled;
            });
        server.Put(R"(/.*)", [this](const httplib::Request &request, httplib::Response &response,
                                    const httplib::ContentReader &reader)
                   { handle(response, [&] { put(request, response, reader); }); });
    }

    template <class Function> void handle(httplib::Response &response, Function &&function)
    {
        try
        {
            function();
        }
        catch (const DavError &error)
        {
            response.status = error.status;
            response.set_content(error.what(), "text/plain");
        }
        // Provider errors can contain signed URLs. Never return them to callers.
        catch (const std::exception &)
        {
            response.status = 502;
            response.set_content("Cloud operation failed", "text/plain");
        }
    }

    std::optional<RemoteEntry> find(const std::string &path)
    {
        RemoteEntry current{"0", "Quark Drive", "", 0, true, json::object()};
        for (const auto &segment : split_remote_path(path))
        {
            if (!current.dir)
                return std::nullopt;
            const auto entries = client.list_directory(current.fid);
            const auto it =
                std::find_if(entries.begin(), entries.end(), [&](const auto &entry) { return entry.name == segment; });
            if (it == entries.end())
                return std::nullopt;
            current = *it;
        }
        return current;
    }

    RemoteEntry require(const std::string &path, int missing = 404)
    {
        const auto entry = find(path);
        if (!entry)
            throw DavError(missing, "Remote path does not exist");
        return *entry;
    }

    void writable()
    {
        if (options.read_only)
            throw DavError(403, "This WebDAV bridge is read only");
    }

    void dispatch(const httplib::Request &request, httplib::Response &response)
    {
        const auto path = remote_path(request.target);
        if (request.method == "OPTIONS")
        {
            response.set_header("Allow", options.read_only ? "OPTIONS, PROPFIND, GET, HEAD"
                                                           : "OPTIONS, PROPFIND, GET, HEAD, PUT, MKCOL, DELETE, MOVE");
            response.status = 200;
        }
        else if (request.method == "PROPFIND")
        {
            const auto depth = request.has_header("Depth") ? request.get_header_value("Depth") : "1";
            if (depth != "0" && depth != "1")
            {
                response.status = 403;
                response.set_content("<D:error xmlns:D=\"DAV:\"><D:propfind-finite-depth/></D:error>",
                                     "application/xml");
                return;
            }
            const auto entry = require(path);
            auto body = std::string("<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">") +
                        properties(entry, path);
            if (entry.dir && depth == "1")
                for (const auto &child : client.list_directory(entry.fid))
                    body += properties(child, (path == "/" ? "" : path) + '/' + child.name);
            response.status = 207;
            response.set_content(body + "</D:multistatus>", "application/xml; charset=utf-8");
        }
        else if (request.method == "GET" || request.method == "HEAD")
            get(request, response, path);
        else if (request.method == "MKCOL")
        {
            writable();
            std::lock_guard lock(mutation_mutex);
            const auto [parent_path, name] = split_parent(path);
            const auto existing = find(path);
            check_conditions(request, existing);
            if (existing)
                throw DavError(405, "Entry already exists");
            if (request.has_header("Transfer-Encoding") ||
                (request.has_header("Content-Length") && integer(request.get_header_value("Content-Length")) != 0))
                throw DavError(415, "MKCOL bodies are unsupported");
            const auto parent = require(parent_path, 409);
            if (!parent.dir)
                throw DavError(409, "Parent is not a directory");
            (void)client.create_folder(parent.fid, name);
            response.status = 201;
        }
        else if (request.method == "DELETE")
        {
            writable();
            std::lock_guard lock(mutation_mutex);
            (void)split_parent(path);
            const auto entry = require(path);
            check_conditions(request, entry);
            (void)client.delete_items({entry.fid});
            response.status = 204;
        }
        else if (request.method == "MOVE")
            move(request, response, path);
        else
            throw DavError(405, "Method not supported");
    }

    void get(const httplib::Request &request, httplib::Response &response, const std::string &path)
    {
        const auto entry = require(path);
        if (entry.dir)
            throw DavError(405, "Use PROPFIND to list a collection");
        if (entry.size > std::numeric_limits<std::size_t>::max())
            throw DavError(507, "File exceeds platform size limit");
        if (request.has_header("Range") && request.get_header_value("Range").find(',') != std::string::npos)
            throw DavError(416, "Only single byte ranges are supported");
        if (request.has_header("If-Range") && request.get_header_value("If-Range") != etag(entry) &&
            request.get_header_value("If-Range") != last_modified(entry))
            throw DavError(412, "If-Range validator no longer matches; retry without Range");
        response.set_header("Accept-Ranges", "bytes");
        response.set_header("ETag", etag(entry));
        response.set_header("Last-Modified", last_modified(entry));
        response.status = request.has_header("Range") ? 206 : 200;
        if (request.method == "HEAD")
        {
            if (entry.size == 0)
                response.set_content("", "application/octet-stream");
            else
                response.set_content_provider(static_cast<std::size_t>(entry.size), "application/octet-stream",
                                              [](std::size_t, std::size_t, httplib::DataSink &) { return false; });
            return;
        }
        if (entry.size == 0)
        {
            response.set_content("", "application/octet-stream");
            return;
        }
        const auto url = client.download_url(entry.fid);
        const auto headers = client.download_headers();
        const auto timeout = client.config().request_timeout_ms;
        const auto ca_bundle = client.config().ca_bundle;
        response.set_content_provider(
            static_cast<std::size_t>(entry.size), "application/octet-stream",
            [url, headers, timeout, ca_bundle, total = entry.size](std::size_t offset, std::size_t length,
                                                                   httplib::DataSink &sink)
            {
                if (length == 0)
                    return true;
                auto upstream_headers = headers;
                upstream_headers.emplace_back("Range", "bytes=" + std::to_string(offset) + '-' +
                                                           std::to_string(offset + length - 1));
                upstream_headers.emplace_back("Accept-Encoding", "identity");
                std::uint64_t received{};
                try
                {
                    (void)HttpClient(timeout, ca_bundle)
                        .request_stream(
                            url, upstream_headers,
                            [&](const HttpResponse &upstream)
                            {
                                if (!upstream.header("Content-Encoding").empty() &&
                                    upstream.header("Content-Encoding") != "identity")
                                    return false;
                                if (upstream.status_code == 206)
                                {
                                    const auto expected = "bytes " + std::to_string(offset) + '-' +
                                                          std::to_string(offset + length - 1) + '/' +
                                                          std::to_string(total);
                                    if (upstream.header("Content-Range") != expected)
                                        return false;
                                }
                                else if (upstream.status_code != 200 || offset != 0 || length != total)
                                    return false;
                                const auto content_length = upstream.header("Content-Length");
                                return content_length.empty() || integer(content_length) == length;
                            },
                            [&](const char *data, std::size_t count)
                            {
                                if (count > length - received)
                                    return false;
                                if (!sink.write(data, count))
                                    return false;
                                received += count;
                                return true;
                            });
                    return received == length;
                }
                catch (const std::exception &)
                {
                    return false;
                }
            });
    }

    void put(const httplib::Request &request, httplib::Response &response, const httplib::ContentReader &reader)
    {
        writable();
        const auto path = remote_path(request.target);
        const auto [parent_path, name] = split_parent(path);
        if (request.has_header("Content-Range"))
            throw DavError(400, "Partial PUT is unsupported");
        TemporaryUpload temporary(options.temp_directory);
        std::ofstream output(temporary.file, std::ios::binary | std::ios::trunc);
        if (!output)
            throw DavError(507, "Cannot create temporary upload");
        std::uint64_t received{};
        const auto ok = reader(
            [&](const char *data, std::size_t count)
            {
                output.write(data, static_cast<std::streamsize>(count));
                received += count;
                return output.good();
            });
        output.close();
        if (!ok || !output)
            throw DavError(507, "Upload interrupted or temporary disk is full");
        if (request.has_header("Content-Length") && integer(request.get_header_value("Content-Length")) != received)
            throw DavError(400, "Incomplete upload body");
        std::lock_guard lock(mutation_mutex);
        const auto parent = require(parent_path, 409);
        if (!parent.dir)
            throw DavError(409, "Parent is not a directory");
        const auto existing = find(path);
        check_conditions(request, existing);
        if (existing && existing->dir)
            throw DavError(405, "Cannot replace a collection with a file");
        // Upload under a distinct name before changing the old file. If a final
        // rename fails, the previous file is restored where the provider allows.
        const auto staged_name = ".quarkpp-upload-" + random_id();
        const auto uploaded = client.upload_file(temporary.file, parent.fid, staged_name);
        bool old_renamed = false;
        try
        {
            if (existing)
            {
                (void)client.rename(existing->fid, ".quarkpp-backup-" + random_id());
                old_renamed = true;
            }
            (void)client.rename(uploaded.fid, name);
        }
        catch (...)
        {
            bool restored = !old_renamed;
            if (old_renamed)
                try
                {
                    (void)client.rename(existing->fid, name);
                    restored = true;
                }
                catch (...)
                {
                }
            // Keep staging data if rollback fails; a user can recover both files.
            if (restored)
                try
                {
                    (void)client.delete_items({uploaded.fid});
                }
                catch (...)
                {
                }
            throw;
        }
        // Failed cleanup must not turn a successful replacement into a retry.
        if (existing)
            try
            {
                (void)client.delete_items({existing->fid});
            }
            catch (...)
            {
            }
        response.status = existing ? 204 : 201;
        response.set_header("Location", href(path, false));
    }

    void move(const httplib::Request &request, httplib::Response &response, const std::string &path)
    {
        writable();
        std::lock_guard lock(mutation_mutex);
        auto destination = request.get_header_value("Destination");
        if (destination.empty())
            throw DavError(400, "Missing Destination");
        if (destination.starts_with("http://") || destination.starts_with("https://"))
        {
            const auto start = destination.find("://") + 3;
            const auto slash = destination.find('/', start);
            const auto authority = destination.substr(start, slash - start);
            if (to_lower(authority) != to_lower(request.get_header_value("Host")))
                throw DavError(502, "Cross-server MOVE is unsupported");
            destination = slash == std::string::npos ? "/" : destination.substr(slash);
        }
        destination = remote_path(destination);
        const auto [source_parent, source_name] = split_parent(path);
        const auto [target_parent, target_name] = split_parent(destination);
        if (source_parent != target_parent)
            throw DavError(409, "MOVE currently supports renaming within a directory");
        const auto entry = require(path);
        check_conditions(request, entry);
        if (path == destination)
        {
            response.status = 204;
            return;
        }
        // Deliberately refuse destructive MOVE replacement; rclone can remove
        // the destination explicitly if that behavior is desired.
        if (find(destination))
            throw DavError(412, "MOVE destination already exists");
        (void)client.rename(entry.fid, target_name);
        response.status = 201;
        response.set_header("Location", href(destination, entry.dir));
    }
};

WebDavServer::WebDavServer(QuarkClient &client, WebDavOptions options)
    : impl_(std::make_unique<Impl>(client, std::move(options)))
{
}
WebDavServer::~WebDavServer()
{
    stop();
}
int WebDavServer::bind()
{
    if (impl_->options.port == 0)
    {
        const auto port = impl_->server.bind_to_any_port(impl_->options.host);
        if (port < 0)
            throw QuarkException("Cannot bind WebDAV listener");
        return port;
    }
    if (!impl_->server.bind_to_port(impl_->options.host, impl_->options.port))
        throw QuarkException("Cannot bind WebDAV listener");
    return impl_->options.port;
}
bool WebDavServer::listen()
{
    return impl_->server.listen_after_bind();
}
void WebDavServer::stop()
{
    if (impl_)
        impl_->server.stop();
}
} // namespace quarkpp
