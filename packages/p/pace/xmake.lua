package("pace")
    set_homepage("https://github.com/TabNahida/PACE")
    set_description("C++20 coroutine networking with HTTP/1, verified TLS and asynchronous DNS")
    set_license("MIT")

    add_urls("https://github.com/TabNahida/PACE/archive/refs/tags/v$(version).tar.gz")
    add_versions("0.4.0", "46957eec8edaecc1d0806810f9114a8bfd3af685dad44018579288151afbeed6")
    add_configs("shared", {description = "Build shared libraries", default = false, type = "boolean", readonly = true})
    add_deps("openssl3 3.5.7", "c-ares 1.34.8")
    add_links("pace_http1", "pace_tls", "pace_dns", "pace_net", "pace_runtime")
    add_defines("PACE_HAS_HTTP1", "PACE_HAS_TLS", "PACE_HAS_DNS")
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32", "bcrypt")
        add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_WIN32_WINNT=0x0A00")
    else
        add_syslinks("pthread")
    end

    on_install("linux", "macosx", "bsd", "windows", "mingw", function (package)
        import("package.tools.xmake").install(package, {
            cxxstandard = "cxx20",
            http1 = true,
            tls = true,
            dns = true,
            websocket = false,
            quic = false,
            http2 = false,
            http3 = false,
            tests = false,
            examples = false,
            benchmarks = false,
            components = false,
            scaling = false
        })
    end)

    on_test(function (package)
        assert(package:check_cxxsnippets({test = [[
            #include <pace/http.hpp>
            void test() {
                pace::io_context io;
                pace::tls::context trust;
                auto pending = pace::http::async_get(io, "http://127.0.0.1/");
            }
        ]]}, {configs = {languages = "c++20"}}))
    end)
