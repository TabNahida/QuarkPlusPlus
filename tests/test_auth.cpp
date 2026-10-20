#include "quarkpp/auth.hpp"

#include <atomic>
#include <httplib.h>
#include <iostream>
#include <thread>

namespace
{
using namespace quarkpp;
void check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> std::string fails(F operation, const char *message)
{
    try
    {
        operation();
    }
    catch (const std::exception &error)
    {
        return error.what();
    }
    throw std::runtime_error(message);
}
void reply(httplib::Response &response, const json &value)
{
    response.set_content(value.dump(), "application/json");
}
struct Server
{
    httplib::Server http;
    std::thread thread;
    int port{};
    void start()
    {
        port = http.bind_to_any_port("127.0.0.1");
        check(port > 0, "bind login server");
        thread = std::thread([this] { http.listen_after_bind(); });
        for (int i = 0; i < 100 && !http.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(http.is_running(), "start login server");
    }
    ~Server()
    {
        http.stop();
        if (thread.joinable())
            thread.join();
    }
    std::string base() const
    {
        return "http://127.0.0.1:" + std::to_string(port);
    }
    QrLoginOptions options() const
    {
        QrLoginOptions result;
        result.cas_base = base();
        result.pan_base = base();
        result.drive_api_base = base() + "/1/clouddrive";
        return result;
    }
};
} // namespace

int main()
{
    try
    {
        using namespace quarkpp;
        Server server, attacker;
        std::atomic<int> polls{0}, exchanges{0}, configurations{0}, attacker_requests{0};
        std::atomic<bool> token_parameters{false}, polling_cookie{false}, exchange_parameters{false},
            redirect_cookie{false}, config_cookie{false};
        std::atomic<int> mode{0};
        attacker.http.Get("/capture",
                          [&](const auto &, auto &response)
                          {
                              ++attacker_requests;
                              response.set_content("{}", "application/json");
                          });
        attacker.start();
        server.http.Get("/cas/ajax/getTokenForQrcodeLogin",
                        [&](const auto &request, auto &response)
                        {
                            token_parameters = request.get_param_value("client_id") == "532" &&
                                               request.get_param_value("v") == "1.2" &&
                                               request.get_param_value("request_id").size() == 32;
                            response.set_header("Set-Cookie", "cas_session=cas-secret; Path=/cas; HttpOnly");
                            if (mode == 5)
                            {
                                response.status = 302;
                                response.set_header("Location", attacker.base() + "/capture?token=secret-value");
                            }
                            else if (mode == 6)
                                response.set_content("invalid-json-secret-value", "text/plain");
                            else
                                reply(response,
                                      {{"status", 2000000}, {"data", {{"members", {{"token", "token+/=&"}}}}}});
                        });
        server.http.Get(
            "/cas/ajax/getServiceTicketByQrcodeToken",
            [&](const auto &request, auto &response)
            {
                polling_cookie = request.get_header_value("Cookie") == "cas_session=cas-secret" &&
                                 request.get_param_value("token") == "token+/=&";
                const auto count = ++polls;
                if (mode == 1 || mode == 2 || mode == 3)
                    reply(response, {{"status", 50004001 + mode.load()}});
                else if (mode == 4)
                    reply(response, {{"status", 1234567}, {"message", "secret-value"}});
                else if (count == 1)
                    reply(response, {{"status", 50004001}});
                else
                    reply(response, {{"status", 2000000}, {"data", {{"members", {{"service_ticket", "ticket+/=&"}}}}}});
            });
        server.http.Get(
            "/account/info",
            [&](const auto &request, auto &response)
            {
                ++exchanges;
                exchange_parameters = request.get_param_value("st") == "ticket+/=&" &&
                                      request.get_param_value("lw") == "scan" &&
                                      request.get_header_value("Cookie").find("cas_session") == std::string::npos;
                response.set_header("Set-Cookie", "__pus=pus-secret; Path=/; HttpOnly");
                response.set_header("Set-Cookie", "__uid=uid-secret; Path=/; HttpOnly");
                response.set_header("Set-Cookie", "__kps=kps-secret; Path=/; HttpOnly");
                response.set_header("Set-Cookie", "__puus=old-secret; Path=/; HttpOnly");
                response.set_header("Set-Cookie", "remove_me=temporary; Path=/");
                response.set_header("Set-Cookie", "past_cookie=expired; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
                response.set_header("Set-Cookie", "wrong_domain=forbidden; Domain=example.org; Path=/");
                response.status = 302;
                response.set_header("Location", "/account/complete");
            });
        server.http.Get("/account/complete",
                        [&](const auto &request, auto &response)
                        {
                            const auto cookie = request.get_header_value("Cookie");
                            redirect_cookie = cookie.find("__pus=pus-secret") != std::string::npos &&
                                              cookie.find("__uid=uid-secret") != std::string::npos;
                            response.set_header("Set-Cookie", "remove_me=deleted; Path=/; Max-Age=0");
                            json code = "OK";
                            if (mode == 9)
                                code = 0;
                            else if (mode == 10)
                                code = "DENIED-secret-value";
                            else if (mode == 11)
                                code = nullptr;
                            else if (mode == 12)
                                code = 0.0;
                            else if (mode == 13)
                                code = true;
                            reply(response, {{"success", mode != 14}, {"code", code}, {"data", {}}});
                        });
        server.http.Get("/1/clouddrive/config",
                        [&](const auto &request, auto &response)
                        {
                            ++configurations;
                            const auto cookie = request.get_header_value("Cookie");
                            config_cookie = request.get_param_value("pr") == "ucpro" &&
                                            request.get_param_value("fr") == "pc" &&
                                            cookie.find("__pus=pus-secret") != std::string::npos &&
                                            cookie.find("__kps=kps-secret") != std::string::npos &&
                                            cookie.find("remove_me") == std::string::npos &&
                                            cookie.find("past_cookie") == std::string::npos &&
                                            cookie.find("wrong_domain") == std::string::npos &&
                                            cookie.find("cas_session") == std::string::npos;
                            if (mode == 7 && configurations == 1)
                            {
                                response.status = 503;
                                response.set_content("secret-value", "text/plain");
                            }
                            else
                            {
                                response.set_header("Set-Cookie", "__puus=new-secret; Path=/; HttpOnly");
                                if (mode == 8)
                                    response.set_header("Set-Cookie", "__pus=deleted; Path=/; Max-Age=0");
                                reply(response, {{"code", 0}, {"data", json::object()}});
                            }
                        });
        server.start();

        QrLoginClient login(server.options());
        check(login.poll().status == QrLoginStatus::cancelled, "unused session is cancelled");
        auto code = login.start();
        check(code.url.starts_with("https://su.quark.cn/4_eMHBJ?token=token%2B%2F%3D%26&"),
              "official QR URL escapes token");
        check(code.expires_in_seconds == 300 && token_parameters, "CAS request parameters");
        check(login.poll().status == QrLoginStatus::waiting && polling_cookie, "waiting status and CAS cookie");
        auto result = login.poll();
        check(result.status == QrLoginStatus::authenticated && exchange_parameters && redirect_cookie && config_cookie,
              "ticket exchange and cookie propagation");
        check(result.cookie == "__pus=pus-secret; __uid=uid-secret; __kps=kps-secret; __puus=new-secret",
              "all response cookies merged and refreshed");
        check(login.poll().cookie == result.cookie && exchanges == 1, "completed session avoids ticket replay");
        login.cancel();
        check(login.poll().cookie.empty() && login.poll().status == QrLoginStatus::cancelled,
              "cancel clears credentials");

        for (int expired = 1; expired <= 3; ++expired)
        {
            mode = expired;
            static_cast<void>(login.start());
            check(login.poll().status == QrLoginStatus::expired && login.poll().cookie.empty(),
                  "provider expiry clears session");
        }
        mode = 4;
        static_cast<void>(login.start());
        auto error = fails([&] { static_cast<void>(login.poll()); }, "unknown status must fail");
        check(error.find("1234567") != std::string::npos && error.find("secret-value") == std::string::npos,
              "provider errors are sanitized");
        mode = 5;
        error = fails([&] { static_cast<void>(login.start()); }, "cross-origin login redirect must fail");
        check(attacker_requests == 0 && error.find("secret-value") == std::string::npos,
              "redirect does not leak authentication data");
        mode = 6;
        error = fails([&] { static_cast<void>(login.start()); }, "malformed JSON must fail");
        check(error.find("secret-value") == std::string::npos, "malformed JSON is not logged");
        mode = 7;
        configurations = 0;
        exchanges = 0;
        static_cast<void>(login.start());
        error = fails([&] { static_cast<void>(login.poll()); }, "configuration outage must fail");
        check(error.find("503") != std::string::npos && error.find("secret-value") == std::string::npos,
              "HTTP failure is sanitized");
        check(login.poll().status == QrLoginStatus::authenticated && exchanges == 1 && configurations == 2,
              "retry drive bootstrap without replaying consumed ticket");
        mode = 8;
        static_cast<void>(login.start());
        static_cast<void>(fails([&] { static_cast<void>(login.poll()); }, "incomplete credentials must fail"));
        mode = 9;
        static_cast<void>(login.start());
        check(login.poll().status == QrLoginStatus::authenticated, "numeric zero exchange code is also accepted");
        for (int invalid = 10; invalid <= 14; ++invalid)
        {
            mode = invalid;
            const auto previous_configurations = configurations.load();
            static_cast<void>(login.start());
            error = fails([&] { static_cast<void>(login.poll()); }, "invalid ticket exchange result must fail");
            check(error.find("secret-value") == std::string::npos && configurations == previous_configurations,
                  "rejected exchange is sanitized and never used for drive authentication");
        }
        mode = 0;
        auto options = server.options();
        options.login_timeout_seconds = 1;
        QrLoginClient expiring(options);
        static_cast<void>(expiring.start());
        const auto prior_polls = polls.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        check(expiring.poll().status == QrLoginStatus::expired && polls == prior_polls,
              "local deadline avoids additional requests");
        options.cas_base = "http://example.org";
        static_cast<void>(fails([&] { QrLoginClient unsafe(options); }, "cleartext remote login must fail"));
        std::cout << "QR login integration tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
