#include <httplib.h>
#include <quarkpp/support.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

namespace
{
void check(bool value, const std::string &message)
{
    if (!value)
        throw std::runtime_error(message);
}
template <class Function> void expect_failure(Function &&function, const std::string &message)
{
    try
    {
        function();
    }
    catch (const std::exception &)
    {
        return;
    }
    throw std::runtime_error("Expected failure: " + message);
}
void write(const std::filesystem::path &path, const std::string &body)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << body;
    check(output.good(), "write fixture");
}
std::string read(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void sparse_tail(const std::filesystem::path &path, std::uint64_t offset, std::string_view tail)
{
#ifdef _WIN32
    const auto file =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(file != INVALID_HANDLE_VALUE, "create sparse fixture");
    DWORD bytes{};
    const bool sparse = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes, nullptr) != 0;
    CloseHandle(file);
    check(sparse, "mark sparse fixture");
#else
    write(path, "");
#endif
    std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
    output.seekp(static_cast<std::streamoff>(offset));
    output.write(tail.data(), static_cast<std::streamsize>(tail.size()));
    check(output.good(), "write sparse fixture tail");
}
struct TemporaryDirectory
{
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("quarkpp-transport-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory()
    {
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
struct Server
{
    httplib::Server server;
    std::thread worker;
    std::string url;
    void start()
    {
        const auto port = server.bind_to_any_port("127.0.0.1");
        check(port > 0, "bind loopback server");
        url = "http://127.0.0.1:" + std::to_string(port);
        worker = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
        check(server.is_running(), "start loopback server");
    }
    ~Server()
    {
        server.stop();
        if (worker.joinable())
            worker.join();
    }
};
// Disable httplib's automatic range response rewriting so fixtures can emulate
// broken servers (ignored Range, malformed Content-Range and premature EOF).
void raw_range_fixture(const httplib::Request &request)
{
    const_cast<httplib::Request &>(request).ranges.clear();
}

void test_tls_trust()
{
    // Generate an ephemeral test-only certificate so fixtures never expire or
    // require a separately installed openssl command-line executable.
    TemporaryDirectory temp;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"),
                                                            EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    check(key && certificate, "generate local TLS identity");
    check(X509_set_version(certificate.get(), 2) == 1 &&
              ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1 &&
              X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) &&
              X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) &&
              X509_set_pubkey(certificate.get(), key.get()) == 1,
          "initialize local TLS certificate");
    auto *subject = X509_get_subject_name(certificate.get());
    check(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("localhost"),
                                     -1, -1, 0) == 1 &&
              X509_set_issuer_name(certificate.get(), subject) == 1,
          "set local TLS certificate subject");
    for (const auto &[nid, value] :
         {std::pair{NID_basic_constraints, "critical,CA:TRUE"},
          std::pair{NID_key_usage, "critical,digitalSignature,keyCertSign"}, std::pair{NID_ext_key_usage, "serverAuth"},
          std::pair{NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1"}})
    {
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
            X509V3_EXT_conf_nid(nullptr, nullptr, nid, value), X509_EXTENSION_free);
        check(extension && X509_add_ext(certificate.get(), extension.get(), -1) == 1,
              "add local TLS certificate extension");
    }
    check(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0, "sign local TLS certificate");
    std::unique_ptr<BIO, decltype(&BIO_free)> cert_buffer(BIO_new(BIO_s_mem()), BIO_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> key_buffer(BIO_new(BIO_s_mem()), BIO_free);
    check(cert_buffer && key_buffer && PEM_write_bio_X509(cert_buffer.get(), certificate.get()) == 1 &&
              PEM_write_bio_PrivateKey(key_buffer.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1,
          "encode local TLS identity");
    char *cert_data{}, *key_data{};
    const auto cert_size = BIO_get_mem_data(cert_buffer.get(), &cert_data);
    const auto key_size = BIO_get_mem_data(key_buffer.get(), &key_data);
    const auto ca_path = temp.path / "local-ca.pem";
    write(ca_path, std::string(cert_data, static_cast<std::size_t>(cert_size)));
    struct TlsServer
    {
        httplib::SSLServer server;
        std::thread worker;
        explicit TlsServer(const httplib::SSLServer::PemMemory &pem) : server(pem)
        {
        }
        ~TlsServer()
        {
            server.stop();
            if (worker.joinable())
                worker.join();
        }
    } fixture({cert_data, static_cast<std::size_t>(cert_size), key_data, static_cast<std::size_t>(key_size), nullptr, 0,
               nullptr});
    check(fixture.server.is_valid(), "initialize local HTTPS server");
    fixture.server.Get("/trusted", [](const httplib::Request &, httplib::Response &response)
                       { response.set_content("trusted locally", "text/plain"); });
    const auto port = fixture.server.bind_to_any_port("127.0.0.1");
    check(port > 0, "bind local HTTPS server");
    fixture.worker = std::thread([&] { fixture.server.listen_after_bind(); });
    fixture.server.wait_until_ready();
    const auto url = "https://127.0.0.1:" + std::to_string(port) + "/trusted";
    quarkpp::HttpClient trusted(3000, quarkpp::path_to_utf8(ca_path));
    check(trusted.request("GET", url).body == "trusted locally", "explicit CA must validate the PACE HTTPS request");
    quarkpp::HttpClient untrusted(3000);
    expect_failure([&] { static_cast<void>(untrusted.request("GET", url)); },
                   "native default roots must reject an untrusted local HTTPS server");
    quarkpp::HttpClient missing_ca(3000, quarkpp::path_to_utf8(temp.path / "missing-ca.pem"));
    expect_failure([&] { static_cast<void>(missing_ca.request("GET", url)); }, "explicit missing HTTPS CA must fail");
}

void test_parallel_downloads()
{
    TemporaryDirectory temp;
    Server origin;
    constexpr std::uint64_t chunk = 1024 * 1024;
    constexpr std::uint64_t large_offset = (std::uint64_t{1} << 32) + 37;
    constexpr std::uint64_t large_tail = 2 * chunk + 33;
    std::string payload(5 * chunk + 137, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>('a' + i % 23);
    std::atomic<int> active{0}, peak{0}, full_requests{0};
    std::atomic<bool> barrier{true};
    origin.server.Get(
        R"(/(parallel|wrong-first|wrong-second|short-range|ignored-range|encoded-range|huge-parallel))",
        [&](const httplib::Request &req, httplib::Response &res)
        {
            raw_range_fixture(req);
            check(req.get_header_value("Accept-Encoding") == "identity", "parallel identity encoding");
            const auto range = req.get_header_value("Range");
            if (req.path == "/ignored-range")
            {
                if (range.empty())
                    ++full_requests;
                res.set_content(payload, "application/octet-stream");
                return;
            }
            check(range.starts_with("bytes="), "parallel requests must carry explicit byte ranges");
            const auto dash = range.find('-');
            check(dash != std::string::npos && dash + 1 < range.size(), "parallel range must be closed");
            const auto start = std::stoull(range.substr(6, dash - 6));
            const auto end = std::stoull(range.substr(dash + 1));
            const bool huge = req.path == "/huge-parallel";
            const auto total = huge ? large_offset + large_tail : payload.size();
            const auto position = huge ? start - large_offset : start;
            check(end >= start && end < total && end - start + 1 <= chunk,
                  "parallel byte ranges must be bounded and lie within the expected file");
            const auto length = end - start + 1;
            const auto body = payload.substr(static_cast<std::size_t>(position), static_cast<std::size_t>(length));
            const auto inflight = ++active;
            auto maximum = peak.load();
            while (maximum < inflight && !peak.compare_exchange_weak(maximum, inflight))
            {
            }
            if (barrier && req.path == "/parallel" && start < 3 * chunk)
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (peak < 3 && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                --active;
                check(peak >= 3, "parallel range requests must actually overlap");
            }
            else
                --active;
            res.status = 206;
            const bool wrong =
                (req.path == "/wrong-first" && start == 3) || (req.path == "/wrong-second" && start == chunk + 3);
            res.set_header("Content-Range", "bytes " + std::to_string(start + (wrong ? 1 : 0)) + "-" +
                                                std::to_string(end) + "/" + std::to_string(total));
            if (req.path == "/encoded-range")
                res.set_header("Content-Encoding", "gzip");
            if (req.path == "/short-range")
            {
                res.set_content_provider(static_cast<std::size_t>(length), "application/octet-stream",
                                         [body](std::size_t offset, std::size_t, httplib::DataSink &sink)
                                         { return offset == 0 && sink.write(body.data(), body.size() / 2); });
                return;
            }
            res.set_content(body, "application/octet-stream");
        });
    origin.start();
    quarkpp::HttpClient client(3000);
    const auto file = temp.path / "parallel.bin";
    std::uint64_t last = 0;
    const auto result = client.download_file(
        origin.url + "/parallel", {}, file, false,
        [&](std::uint64_t current, std::uint64_t total)
        {
            check(current >= last && total == payload.size(), "parallel progress must be monotonic with known total");
            check(std::filesystem::file_size(file) == current, "parallel progress must describe a committed prefix");
            last = current;
        },
        payload.size(), 3);
    check(result.status_code == 206 && read(file) == payload && peak == 3 && last == payload.size(),
          "parallel transfer must overlap ranges, concatenate in order and preserve every byte");
    barrier = false;
    write(file, payload.substr(0, 17));
    static_cast<void>(client.download_file(origin.url + "/parallel", {}, file, true, {}, payload.size(), 3));
    check(read(file) == payload, "parallel resume must retain its exact prefix and download only missing bytes");
    for (const auto endpoint : {"wrong-first", "short-range", "encoded-range"})
    {
        write(file, payload.substr(0, 3));
        expect_failure(
            [&]
            {
                static_cast<void>(
                    client.download_file(origin.url + "/" + endpoint, {}, file, true, {}, payload.size(), 3));
            },
            "invalid parallel segment");
        check(read(file) == payload.substr(0, 3), std::string(endpoint) + " must preserve the original prefix");
    }
    write(file, payload.substr(0, 3));
    expect_failure(
        [&]
        {
            static_cast<void>(
                client.download_file(origin.url + "/wrong-second", {}, file, true, {}, payload.size(), 3));
        },
        "failed later parallel segment");
    check(read(file) == payload.substr(0, chunk + 3),
          "later parallel failure may retain only successfully validated contiguous segments");
    write(file, "unrelated old destination");
    static_cast<void>(client.download_file(origin.url + "/ignored-range", {}, file, false, {}, payload.size(), 3));
    check(read(file) == payload && full_requests == 1, "ignored ranges must fall back to one fresh full download");
    write(file, payload.substr(0, 3));
    expect_failure(
        [&]
        {
            static_cast<void>(
                client.download_file(origin.url + "/ignored-range", {}, file, true, {}, payload.size(), 3));
        },
        "resumed download whose server ignores ranges");
    check(read(file) == payload.substr(0, 3), "ignored resumed ranges must preserve the existing prefix");
    write(file, "unrelated old destination");
    bool callback_error = false;
    try
    {
        static_cast<void>(client.download_file(
            origin.url + "/parallel", {}, file, false, [](std::uint64_t, std::uint64_t)
            { throw std::runtime_error("parallel-callback-aborted"); }, payload.size(), 3));
    }
    catch (const std::exception &error)
    {
        callback_error = std::string(error.what()) == "parallel-callback-aborted";
    }
    check(callback_error && active == 0 && read(file) == payload.substr(0, chunk),
          "parallel callback failures must propagate after joining workers and retain a resumable prefix");
    const auto sparse = temp.path / "huge-parallel.bin";
    sparse_tail(sparse, large_offset - 1, "x");
    static_cast<void>(
        client.download_file(origin.url + "/huge-parallel", {}, sparse, true, {}, large_offset + large_tail, 3));
    check(std::filesystem::file_size(sparse) == large_offset + large_tail,
          "parallel resume beyond 4 GiB must use 64-bit offsets");
    std::ifstream tail(sparse, std::ios::binary);
    tail.seekg(static_cast<std::streamoff>(large_offset - 1));
    std::string actual(large_tail + 1, '\0');
    tail.read(actual.data(), static_cast<std::streamsize>(actual.size()));
    check(actual == "x" + payload.substr(0, large_tail), "parallel sparse high-offset bytes must match exactly");
}
} // namespace

int main()
{
    try
    {
        test_parallel_downloads();
        test_tls_trust();
        TemporaryDirectory temp;
        Server origin, other;
        std::atomic<bool> identity_seen{false}, safe_redirect_seen{false}, upload_seen{false};
        constexpr std::uint64_t large_offset = (std::uint64_t{1} << 32) + 17;
        other.server.Get("/destination",
                         [&](const httplib::Request &req, httplib::Response &res)
                         {
                             safe_redirect_seen = !req.has_header("Cookie") && !req.has_header("Authorization") &&
                                                  !req.has_header("X-Secret") &&
                                                  req.get_header_value("Accept-Encoding") == "identity";
                             res.set_content("safe", "text/plain");
                         });
        other.start();
        origin.server.Get("/redirect", [&](const httplib::Request &, httplib::Response &res)
                          { res.set_redirect(other.url + "/destination"); });
        origin.server.Get("/valid",
                          [&](const httplib::Request &req, httplib::Response &res)
                          {
                              raw_range_fixture(req);
                              identity_seen = req.get_header_value("Accept-Encoding") == "identity" &&
                                              req.get_header_value("Range") == "bytes=3-";
                              res.status = 206;
                              res.set_header("Content-Range", "bytes 3-5/6");
                              res.set_content("def", "application/octet-stream");
                          });
        origin.server.Get("/ignore",
                          [](const httplib::Request &req, httplib::Response &res)
                          {
                              raw_range_fixture(req);
                              res.set_content("abcdef", "application/octet-stream");
                          });
        origin.server.Get("/malformed",
                          [](const httplib::Request &req, httplib::Response &res)
                          {
                              raw_range_fixture(req);
                              res.status = 206;
                              res.set_header("Content-Range", "bytes 2-4/6");
                              res.set_content("bad", "application/octet-stream");
                          });
        origin.server.Get("/length",
                          [](const httplib::Request &req, httplib::Response &res)
                          {
                              raw_range_fixture(req);
                              res.status = 206;
                              res.set_header("Content-Range", "bytes 3-6/7");
                              res.set_content("bad", "application/octet-stream");
                          });
        origin.server.Get("/encoded",
                          [](const httplib::Request &req, httplib::Response &res)
                          {
                              raw_range_fixture(req);
                              res.status = 206;
                              res.set_header("Content-Range", "bytes 3-5/6");
                              res.set_header("Content-Encoding", "gzip");
                              res.set_content("bad", "application/octet-stream");
                          });
        origin.server.Get("/complete",
                          [](const httplib::Request &req, httplib::Response &res)
                          {
                              raw_range_fixture(req);
                              res.status = 416;
                              res.set_header("Content-Range", "bytes */3");
                              res.set_content("range error", "text/plain");
                          });
        origin.server.Get("/bad416",
                          [](const httplib::Request &req, httplib::Response &res)
                          {
                              raw_range_fixture(req);
                              res.status = 416;
                              res.set_header("Content-Range", "bytes */7");
                              res.set_content("range error", "text/plain");
                          });
        origin.server.Get("/error",
                          [](const httplib::Request &req, httplib::Response &res)
                          {
                              raw_range_fixture(req);
                              res.status = 403;
                              res.set_content("expired signed URL", "text/plain");
                          });
        origin.server.Get("/truncated",
                          [](const httplib::Request &req, httplib::Response &res)
                          {
                              raw_range_fixture(req);
                              res.status = 206;
                              res.set_header("Content-Range", "bytes 3-8/9");
                              res.set_content_provider(6, "application/octet-stream",
                                                       [](std::size_t offset, std::size_t, httplib::DataSink &sink)
                                                       { return offset == 0 && sink.write("def", 3); });
                          });
        origin.server.Get("/huge",
                          [](const httplib::Request &req, httplib::Response &res)
                          {
                              raw_range_fixture(req);
                              res.status = 206;
                              res.set_header("Content-Range", "bytes " + std::to_string(large_offset) + "-" +
                                                                  std::to_string(large_offset + 3) + "/" +
                                                                  std::to_string(large_offset + 4));
                              res.set_content("tail", "application/octet-stream");
                          });
        origin.server.Put("/upload",
                          [&](const httplib::Request &req, httplib::Response &res)
                          {
                              upload_seen = req.body == "tail" && req.get_header_value("Content-Length") == "4" &&
                                            req.get_header_value("X-Part") == "7" &&
                                            req.get_header_value("Accept-Encoding") == "identity";
                              res.set_header("ETag", "part-etag");
                              res.set_content("ok", "text/plain");
                          });
        origin.server.Get("/bounded",
                          [](const httplib::Request &, httplib::Response &res)
                          {
                              res.set_content_provider(17 * 1024 * 1024, "application/octet-stream",
                                                       [](std::size_t, std::size_t count, httplib::DataSink &sink)
                                                       {
                                                           const std::string chunk(std::min<std::size_t>(count, 65536),
                                                                                   'x');
                                                           return sink.write(chunk.data(), chunk.size());
                                                       });
                          });
        origin.server.Get("/head",
                          [](const httplib::Request &, httplib::Response &res)
                          {
                              res.set_header("X-Head", "present");
                              res.set_content("hello", "text/plain");
                          });
        origin.server.Get("/no-content",
                          [](const httplib::Request &, httplib::Response &res)
                          {
                              res.status = 204;
                              res.set_header("X-Empty", "present");
                          });
        origin.server.Get("/empty-file", [](const httplib::Request &, httplib::Response &res)
                          { res.set_content("", "application/octet-stream"); });
        origin.start();
        quarkpp::HttpClient client(2000);
        quarkpp::HttpClient http_with_unused_ca(2000, quarkpp::path_to_utf8(temp.path / "absent-http-ca.pem"));
        check(http_with_unused_ca.request("GET", origin.url + "/head").body == "hello",
              "plain HTTP must ignore an explicit HTTPS-only CA bundle");
        const auto head = client.request("HEAD", origin.url + "/head");
        check(head.status_code == 200 && head.header("X-Head") == "present" && head.body.empty(),
              "HEAD response metadata");
        const auto empty_response = client.request("GET", origin.url + "/no-content");
        check(empty_response.status_code == 204 && empty_response.header("X-Empty") == "present",
              "204 response metadata");
        bool empty_headers = false;
        static_cast<void>(client.request_stream(
            origin.url + "/no-content", {},
            [&](const quarkpp::HttpResponse &response)
            {
                empty_headers = response.status_code == 204;
                return true;
            },
            [](const char *, std::size_t) -> bool { throw std::runtime_error("204 unexpectedly had a body"); }));
        check(empty_headers, "204 invokes streaming header callback");
        const auto empty_file = temp.path / "empty-file.bin";
        const auto empty_download = client.download_file(origin.url + "/empty-file", {}, empty_file, true, {}, 0);
        check(empty_download.status_code == 200 && std::filesystem::exists(empty_file) &&
                  std::filesystem::file_size(empty_file) == 0,
              "HTTP 200 zero-byte file creates empty destination");
        const auto absent_file = temp.path / "absent-file.bin";
        expect_failure([&]
                       { static_cast<void>(client.download_file(origin.url + "/no-content", {}, absent_file, false)); },
                       "HTTP 204 is not a successful file download");
        check(!std::filesystem::exists(absent_file), "HTTP 204 must not create download destination");
        const auto file = temp.path / "download.bin";
        for (const auto endpoint : {"ignore", "malformed", "length", "encoded", "bad416", "error"})
        {
            write(file, "abc");
            expect_failure([&]
                           { static_cast<void>(client.download_file(origin.url + "/" + endpoint, {}, file, true)); },
                           endpoint);
            check(read(file) == "abc", std::string(endpoint) + " must preserve partial file");
        }
        write(file, "abc");
        expect_failure([&] { static_cast<void>(client.download_file(origin.url + "/valid", {}, file, true, {}, 7)); },
                       "range total conflicts with expected metadata");
        check(read(file) == "abc", "conflicting range total must preserve existing bytes");
        expect_failure([&] { static_cast<void>(client.download_file(origin.url + "/ignore", {}, file, false, {}, 7)); },
                       "content length conflicts with expected metadata");
        check(read(file) == "abc", "conflicting content length must not truncate destination");
        const auto resumed = client.download_file(origin.url + "/valid", {{"Accept-Encoding", "gzip"}}, file, true);
        check(resumed.status_code == 206 && read(file) == "abcdef" && identity_seen,
              "valid range append and identity header");
        write(file, "abc");
        const auto complete = client.download_file(origin.url + "/complete", {}, file, true);
        check(complete.status_code == 416 && read(file) == "abc", "validated complete 416");
        expect_failure([&] { static_cast<void>(client.download_file(origin.url + "/truncated", {}, file, true)); },
                       "truncated range");
        check(read(file) == "abcdef", "truncation must preserve correct newly received bytes");
        const auto missing = temp.path / "missing.bin";
        expect_failure([&] { static_cast<void>(client.download_file(origin.url + "/error", {}, missing, false)); },
                       "HTTP error");
        check(!std::filesystem::exists(missing), "HTTP error must not create destination");
        const auto redirected =
            client.request("GET", origin.url + "/redirect", {},
                           {{"Cookie", "private=1"}, {"Authorization", "Bearer secret"}, {"X-Secret", "secret"}});
        check(redirected.body == "safe" && safe_redirect_seen, "cross-origin redirects remove credentials");
        std::string streamed;
        bool stream_headers = false;
        const auto stream_result = client.request_stream(
            origin.url + "/valid", {{"Range", "bytes=3-"}},
            [&](const quarkpp::HttpResponse &response)
            {
                stream_headers = response.status_code == 206 && response.header("content-range") == "bytes 3-5/6";
                return stream_headers;
            },
            [&](const char *data, std::size_t size)
            {
                streamed.append(data, size);
                return true;
            });
        check(stream_headers && streamed == "def" && stream_result.body.empty(), "bounded streaming callback API");
        expect_failure(
            [&]
            {
                static_cast<void>(client.request_stream(origin.url + "/valid", {},
                                                        [](const quarkpp::HttpResponse &) { return false; }, {}));
            },
            "streaming response rejection");
        expect_failure([&] { static_cast<void>(client.request("GET", origin.url + "/bounded")); },
                       "bounded API response");
        const auto sparse = temp.path / "large.bin";
        sparse_tail(sparse, large_offset, "tail");
        std::uint64_t upload_progress{};
        const auto uploaded =
            client.upload_file_range(origin.url + "/upload", {}, {{"X-Part", "7"}}, sparse, large_offset, 4,
                                     [&](std::uint64_t sent, std::uint64_t total)
                                     {
                                         check(total == 4, "upload total");
                                         upload_progress = sent;
                                     });
        check(upload_seen && uploaded.header("etag") == "part-etag" && upload_progress == 4, "upload beyond 4 GiB");
        expect_failure(
            [&]
            { static_cast<void>(client.upload_file_range(origin.url + "/upload", {}, {}, sparse, large_offset, 5)); },
            "out-of-bounds upload");
        std::filesystem::resize_file(sparse, large_offset);
        static_cast<void>(client.download_file(origin.url + "/huge", {}, sparse, true));
        check(std::filesystem::file_size(sparse) == large_offset + 4, "download resume beyond 4 GiB");
        std::ifstream tail(sparse, std::ios::binary);
        tail.seekg(static_cast<std::streamoff>(large_offset));
        std::array<char, 4> bytes{};
        tail.read(bytes.data(), bytes.size());
        check(std::string(bytes.data(), bytes.size()) == "tail", "large download tail");
        write(file, "abc");
        const auto hashes = quarkpp::compute_file_hashes(file);
        check(hashes.md5_hex == "900150983cd24fb0d6963f7d28e17f72" &&
                  hashes.sha1_hex == "a9993e364706816aba3e25717850c26c9cd0d89d",
              "single-pass file hashes");
        check(quarkpp::base64_encode("abc") == "YWJj" && quarkpp::base64_encode("").empty(), "base64 padding");
        std::cout << "transport tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "transport test failed: " << error.what() << '\n';
        return 1;
    }
}
