#include "quarkpp/auth.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <iomanip>
#include <openssl/rand.h>
#include <sstream>

namespace quarkpp
{
namespace
{
constexpr std::string_view login_user_agent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "quark-cloud-drive/2.5.56 Chrome/100.0.4896.160 Electron/18.3.5.12-a038f7b798 Safari/537.36 "
    "Channel/pckk_other_ch";

struct Address
{
    std::string origin, host, path;
    bool secure{};
};

Address address(const std::string &url)
{
    if (url.find_first_of("\r\n\t \\") != std::string::npos)
        throw QuarkException("Invalid login endpoint");
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        throw QuarkException("Login endpoint must be an absolute HTTPS URL");
    const auto scheme = to_lower(url.substr(0, scheme_end));
    const auto authority_end = url.find_first_of("/?#", scheme_end + 3);
    const auto authority = to_lower(url.substr(scheme_end + 3, authority_end - scheme_end - 3));
    if (authority.empty() || authority.find('@') != std::string::npos)
        throw QuarkException("Invalid login endpoint authority");
    const auto host = authority.starts_with('[') ? authority.substr(0, authority.find(']') + 1)
                                                 : authority.substr(0, authority.find(':'));
    if (scheme != "https" && !(scheme == "http" && (host == "127.0.0.1" || host == "localhost" || host == "[::1]")))
        throw QuarkException("Login requires HTTPS (HTTP is allowed only for loopback tests)");
    auto path = authority_end == std::string::npos ? "/" : url.substr(authority_end);
    path.resize(path.find_first_of("?#") == std::string::npos ? path.size() : path.find_first_of("?#"));
    if (path.empty())
        path = "/";
    return {scheme + "://" + authority, host, path, scheme == "https"};
}

bool domain_matches(const std::string &host, const std::string &domain)
{
    return host == domain || (host.size() > domain.size() && host.ends_with("." + domain));
}

bool token_char(unsigned char c)
{
    return c > 32 && c < 127 &&
           std::string_view("()<>@,;:\\\"/[]?={}").find(static_cast<char>(c)) == std::string_view::npos;
}

std::optional<std::time_t> cookie_date(const std::string &text)
{
    std::tm value{};
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    input >> std::get_time(&value, "%a, %d %b %Y %H:%M:%S GMT");
    if (input.fail())
        return {};
#ifdef _WIN32
    return _mkgmtime(&value);
#else
    return timegm(&value);
#endif
}

struct Cookie
{
    std::string name, value, domain, path;
    bool host_only{true}, secure{};
    std::optional<std::time_t> expires;
};

class CookieJar
{
  public:
    void absorb(const HttpResponse &response)
    {
        const auto source = address(response.url);
        for (const auto &line : response.set_cookies)
        {
            if (line.size() > 65536 || line.find_first_of("\r\n") != std::string::npos)
                throw QuarkException("Invalid login Set-Cookie response");
            const auto first_end = line.find(';');
            const auto pair = std::string_view(line).substr(0, first_end);
            const auto equal = pair.find('=');
            if (equal == std::string_view::npos)
                continue;
            Cookie cookie;
            cookie.name = trim(pair.substr(0, equal));
            cookie.value = trim(pair.substr(equal + 1));
            if (cookie.name.empty() || !std::all_of(cookie.name.begin(), cookie.name.end(), token_char) ||
                !std::all_of(cookie.value.begin(), cookie.value.end(),
                             [](unsigned char c) { return c >= 33 && c < 127 && c != ',' && c != '\\'; }))
                throw QuarkException("Invalid login cookie field");
            cookie.domain = source.host;
            cookie.path = source.path.substr(0, source.path.rfind('/'));
            if (cookie.path.empty())
                cookie.path = "/";
            std::optional<long long> max_age;
            bool valid_domain = true;
            auto start = first_end;
            while (start != std::string::npos)
            {
                ++start;
                const auto end = line.find(';', start);
                const auto attribute = std::string_view(line).substr(start, end - start);
                const auto separator = attribute.find('=');
                const auto key = to_lower(trim(attribute.substr(0, separator)));
                const auto value =
                    separator == std::string_view::npos ? std::string{} : trim(attribute.substr(separator + 1));
                if (key == "domain")
                {
                    cookie.domain = to_lower(value);
                    if (cookie.domain.starts_with('.'))
                        cookie.domain.erase(0, 1);
                    cookie.host_only = false;
                    valid_domain = !cookie.domain.empty() && domain_matches(source.host, cookie.domain) &&
                                   (cookie.domain == source.host || cookie.domain.find('.') != std::string::npos);
                }
                else if (key == "path" && value.starts_with('/'))
                    cookie.path = value;
                else if (key == "secure")
                    cookie.secure = true;
                else if (key == "expires")
                    cookie.expires = cookie_date(value);
                else if (key == "max-age")
                {
                    long long age{};
                    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), age);
                    if (ec == std::errc{} && ptr == value.data() + value.size())
                        max_age = age;
                }
                start = end;
            }
            if (!valid_domain)
                continue;
            const auto now = std::time(nullptr);
            if (max_age)
                cookie.expires = *max_age <= 0 ? now - 1 : now + std::min(*max_age, 315360000LL);
            std::erase_if(
                cookies_, [&](const Cookie &old)
                { return old.name == cookie.name && old.domain == cookie.domain && old.path == cookie.path; });
            if ((!cookie.expires || *cookie.expires > now) && !cookie.value.empty())
                cookies_.push_back(std::move(cookie));
        }
    }

    std::string header(const std::string &url) const
    {
        const auto target = address(url);
        std::vector<const Cookie *> selected;
        for (const auto &cookie : cookies_)
        {
            const bool host_match =
                cookie.host_only ? cookie.domain == target.host : domain_matches(target.host, cookie.domain);
            const bool path_match =
                target.path == cookie.path || (target.path.starts_with(cookie.path) &&
                                               (cookie.path.ends_with('/') || target.path[cookie.path.size()] == '/'));
            if (host_match && path_match && (!cookie.secure || target.secure) &&
                (!cookie.expires || *cookie.expires > std::time(nullptr)))
                selected.push_back(&cookie);
        }
        std::stable_sort(selected.begin(), selected.end(),
                         [](const Cookie *a, const Cookie *b) { return a->path.size() > b->path.size(); });
        std::string result;
        for (const auto *cookie : selected)
        {
            if (!result.empty())
                result += "; ";
            result += cookie->name + "=" + cookie->value;
        }
        return result;
    }

  private:
    std::vector<Cookie> cookies_;
};

std::string request_id()
{
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw QuarkException("Cannot generate login request identifier");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes)
    {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

json response_json(const HttpResponse &response)
{
    if (response.status_code != 200)
        throw QuarkException("QR login HTTP status " + std::to_string(response.status_code));
    auto data = json::parse(response.body, nullptr, false);
    if (!data.is_object())
        throw QuarkException("QR login returned invalid JSON");
    return data;
}

int status_code(const json &data, const char *name)
{
    const auto field = data.find(name);
    if (field == data.end() || !field->is_number_integer())
        throw QuarkException("QR login response is missing a numeric status");
    try
    {
        return field->get<int>();
    }
    catch (const json::exception &)
    {
        throw QuarkException("QR login returned an invalid status");
    }
}

std::string member(const json &data, const char *name)
{
    const auto field = data.find("data");
    if (field == data.end() || !field->is_object())
        throw QuarkException("QR login response is missing data");
    const auto members = field->find("members");
    if (members == field->end() || !members->is_object())
        throw QuarkException("QR login response is missing members");
    const auto value = members->find(name);
    if (value == members->end() || !value->is_string() || value->get_ref<const std::string &>().empty() ||
        value->get_ref<const std::string &>().size() > 8192)
        throw QuarkException("QR login response is missing its credential");
    return value->get<std::string>();
}

bool has_cookie(const std::string &header, std::string_view name)
{
    return header.starts_with(std::string(name) + "=") ||
           header.find("; " + std::string(name) + "=") != std::string::npos;
}
} // namespace

struct QrLoginClient::Impl
{
    QrLoginOptions options;
    HttpClient http;
    CookieJar cookies;
    std::string token, ticket, final_cookie;
    std::chrono::steady_clock::time_point deadline{};
    QrLoginStatus state{QrLoginStatus::cancelled};
    bool exchanged{};

    explicit Impl(QrLoginOptions value) : options(std::move(value)), http(options.request_timeout_ms, options.ca_bundle)
    {
        if (!options.request_timeout_ms || !options.login_timeout_seconds || options.login_timeout_seconds > 300)
            throw QuarkException("QR login timeouts must be positive; login timeout cannot exceed 300 seconds");
        for (auto *base : {&options.cas_base, &options.pan_base, &options.drive_api_base})
        {
            const auto parsed = address(*base);
            static_cast<void>(parsed);
            if (base->find_first_of("?#") != std::string::npos)
                throw QuarkException("Login endpoint cannot contain a query or fragment");
            while (base->ends_with('/'))
                base->pop_back();
        }
    }

    HttpResponse get(std::string url, QueryList query = {})
    {
        const auto origin = address(url).origin;
        for (unsigned redirects = 0; redirects <= 5; ++redirects)
        {
            HeaderList headers{{"User-Agent", std::string(login_user_agent)},
                               {"Accept", "application/json, text/plain, */*"},
                               {"Origin", address(options.pan_base).origin},
                               {"Referer", options.pan_base + "/"}};
            const auto cookie = cookies.header(url);
            if (!cookie.empty())
                headers.emplace_back("Cookie", cookie);
            auto response = http.request("GET", url, query, headers, {}, false);
            cookies.absorb(response);
            if (response.status_code != 301 && response.status_code != 302 && response.status_code != 303 &&
                response.status_code != 307 && response.status_code != 308)
                return response;
            const auto location = response.header("location");
            if (location.empty())
                throw QuarkException("QR login redirect is missing Location");
            if (location.starts_with("//"))
                url = origin.substr(0, origin.find(':') + 1) + location;
            else if (location.find("://") != std::string::npos)
                url = location;
            else if (location.starts_with('/'))
                url = origin + location;
            else if (location.starts_with('?'))
                url = origin + address(response.url).path + location;
            else
            {
                const auto from = address(response.url);
                url = origin + from.path.substr(0, from.path.rfind('/') + 1) + location;
            }
            if (address(url).origin != origin)
                throw QuarkException("QR login refused an authority-changing redirect");
            query.clear();
        }
        throw QuarkException("QR login redirect limit exceeded");
    }

    QrLoginResult finish()
    {
        if (!exchanged)
        {
            const auto data = response_json(get(options.pan_base + "/account/info", {{"st", ticket}, {"lw", "scan"}}));
            const auto code = data.find("code");
            const bool code_ok =
                code == data.end() || (code->is_number_integer() && *code == 0) || (code->is_string() && *code == "OK");
            if ((data.contains("success") && data["success"] != true) || !code_ok)
                throw QuarkException("QR login ticket exchange was rejected");
            exchanged = true;
            ticket.clear();
        }
        const auto config = response_json(
            get(options.drive_api_base + "/config", {{"pr", "ucpro"}, {"fr", "pc"}, {"uc_param_str", ""}}));
        if (status_code(config, "code") != 0)
            throw QuarkException("QR login drive authentication was rejected");
        auto cookie = cookies.header(options.drive_api_base + "/");
        if (!has_cookie(cookie, "__pus") || (!has_cookie(cookie, "__uid") && !has_cookie(cookie, "__kps")) ||
            !has_cookie(cookie, "__puus"))
            throw QuarkException("QR login did not return complete drive credentials");
        final_cookie = std::move(cookie);
        token.clear();
        cookies = {};
        state = QrLoginStatus::authenticated;
        return {state, final_cookie};
    }
};

QrLoginClient::QrLoginClient(QrLoginOptions options) : impl_(std::make_unique<Impl>(std::move(options)))
{
}
QrLoginClient::~QrLoginClient() = default;
QrLoginClient::QrLoginClient(QrLoginClient &&) noexcept = default;
QrLoginClient &QrLoginClient::operator=(QrLoginClient &&) noexcept = default;

QrLoginCode QrLoginClient::start()
{
    cancel();
    const auto data = response_json(impl_->get(impl_->options.cas_base + "/cas/ajax/getTokenForQrcodeLogin",
                                               {{"client_id", "532"}, {"v", "1.2"}, {"request_id", request_id()}}));
    const auto status = status_code(data, "status");
    if (status != 2000000)
        throw QuarkException("QR login token request failed with status " + std::to_string(status));
    impl_->token = member(data, "token");
    impl_->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(impl_->options.login_timeout_seconds);
    impl_->state = QrLoginStatus::waiting;
    return {"https://su.quark.cn/4_eMHBJ?token=" + url_encode(impl_->token) +
                "&client_id=532&ssb=weblogin&uc_param_str=&uc_biz_str=S%3Acustom%7COPT%3ASAREA%400%7COPT%3AIMMERSIVE%"
                "401%7COPT%3ABACK_BTN_STYLE%400",
            impl_->options.login_timeout_seconds};
}

QrLoginResult QrLoginClient::poll()
{
    if (impl_->state != QrLoginStatus::waiting)
        return {impl_->state, impl_->final_cookie};
    if (std::chrono::steady_clock::now() >= impl_->deadline)
    {
        cancel();
        impl_->state = QrLoginStatus::expired;
        return {impl_->state, {}};
    }
    if (impl_->exchanged || !impl_->ticket.empty())
        return impl_->finish();
    const auto data = response_json(
        impl_->get(impl_->options.cas_base + "/cas/ajax/getServiceTicketByQrcodeToken",
                   {{"client_id", "532"}, {"v", "1.2"}, {"token", impl_->token}, {"request_id", request_id()}}));
    const auto status = status_code(data, "status");
    if (status == 50004001)
        return {};
    if (status == 50004002 || status == 50004003 || status == 50004004)
    {
        cancel();
        impl_->state = QrLoginStatus::expired;
        return {impl_->state, {}};
    }
    if (status != 2000000)
        throw QuarkException("QR login polling failed with status " + std::to_string(status));
    impl_->ticket = member(data, "service_ticket");
    return impl_->finish();
}

void QrLoginClient::cancel()
{
    impl_->token.clear();
    impl_->ticket.clear();
    impl_->final_cookie.clear();
    impl_->cookies = {};
    impl_->exchanged = false;
    impl_->state = QrLoginStatus::cancelled;
}
} // namespace quarkpp
