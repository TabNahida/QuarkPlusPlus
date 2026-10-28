set_project("QuarkPlusPlus")
set_version("0.3.0")
set_xmakever("2.9.8")

set_languages("c++20")
set_warnings("all")
set_encodings("utf-8")
if is_plat("windows") then
    set_runtimes("MT")
end

add_rules("mode.debug", "mode.release")
add_repositories("quarkpp-packages .")
add_requires("pace 0.4.0", {system = false})
add_requires("nlohmann_json 3.12.0")
add_requires("openssl3 3.6.1", {configs = {shared = false}})
add_requires("cpp-httplib 0.34.0", {configs = {ssl = true}})
add_requireconfs("cpp-httplib.openssl3", {version = "3.6.1", override = true})
add_requireconfs("pace.openssl3", {version = "3.6.1", override = true,
    system = false, configs = {shared = false}})
add_requireconfs("pace.c-ares", {system = false, configs = {shared = false}})

option("tests")
    set_default(false)
    set_showmenu(true)
    set_description("Build offline integration tests")
option_end()

rule("quarkpp.webdav_methods")
    on_load(function (target)
        import("scripts.httplib", {alias = "webdav_httplib"})
        target:add("sysincludedirs", webdav_httplib.include_directory(target))
    end)
    on_config(function (target)
        import("scripts.httplib", {alias = "webdav_httplib"})
        webdav_httplib.configure(target)
    end)
    before_build(function (target)
        import("scripts.httplib", {alias = "webdav_httplib"})
        webdav_httplib.configure(target)
    end)

target("quarkpp_core")
    add_rules("quarkpp.webdav_methods")
    set_kind("static")
    set_basename("quarkpp")
    add_files("src/quark_client.cpp", "src/support.cpp", "src/auth.cpp", "src/tls_trust.cpp")
    add_headerfiles("include/(quarkpp/*.hpp)")
    add_includedirs("include", {public = true})
    add_packages("pace", "nlohmann_json", "openssl3", "cpp-httplib", {public = true})
    add_defines("CPPHTTPLIB_OPENSSL_SUPPORT", {public = true})
    if is_plat("windows") then
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", {public = true})
        add_syslinks("ws2_32", "crypt32", {public = true})
    else
        add_syslinks("pthread", {public = true})
        if is_plat("macosx") then
            add_defines("CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN", {public = true})
            add_frameworks("Security", "CoreFoundation", {public = true})
        end
    end

target("quarkpp")
    add_rules("quarkpp.webdav_methods")
    set_kind("binary")
    add_files("src/main.cpp", "src/webdav.cpp", "src/login.cpp", "third_party/qrcodegen/qrcodegen.cpp")
    add_includedirs("third_party/qrcodegen")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_deps("quarkpp_core")
    add_installfiles("config/quarkpp.example.json", {prefixdir = "share/quarkpp/config"})
    add_installfiles("README.md", "README.zh-CN.md", "LICENSE", {prefixdir = "share/quarkpp"})
    add_installfiles("docs/*.md", {prefixdir = "share/quarkpp/docs"})
    add_installfiles("third_party/qrcodegen/LICENSE", {prefixdir = "share/quarkpp/licenses/qrcodegen"})

if has_config("tests") then
    for _, suite in ipairs({"transport", "client", "webdav", "auth", "login"}) do
        target("quarkpp_" .. suite .. "_tests")
            add_rules("quarkpp.webdav_methods")
            set_kind("binary")
            set_default(false)
            add_files("tests/test_" .. suite .. ".cpp")
            add_deps("quarkpp_core")
            if suite == "webdav" then
                add_files("src/webdav.cpp")
                add_includedirs("src")
            end
            if suite == "login" then
                add_files("src/login.cpp", "third_party/qrcodegen/qrcodegen.cpp")
                add_includedirs("src", "third_party/qrcodegen")
                if is_plat("windows") then
                    add_syslinks("advapi32")
                end
            end
            add_tests("default", {run_timeout = 120000})
    end
end
