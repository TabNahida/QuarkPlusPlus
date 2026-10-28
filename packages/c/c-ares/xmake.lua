package("c-ares")
    set_homepage("https://c-ares.org/")
    set_description("Asynchronous DNS library required by the PACE URL client")
    set_license("MIT")

    add_urls("https://github.com/c-ares/c-ares/releases/download/v$(version)/c-ares-$(version).tar.gz")
    add_versions("1.34.8", "c222b6d681096f9444d2c4863d2c1174019e27cacca0a4a5c114d36dd7d7bf78")
    add_deps("cmake")
    on_load(function (package)
        if package:is_plat("windows", "mingw") then
            package:add("syslinks", "iphlpapi", "ws2_32")
        elseif package:is_plat("macosx") then
            package:add("syslinks", "resolv")
        end
    end)

    on_install(function (package)
        local shared = package:config("shared")
        local configs = {
            "-DCARES_BUILD_TESTS=OFF",
            "-DCARES_BUILD_TOOLS=OFF",
            "-DCMAKE_BUILD_TYPE=" .. (package:is_debug() and "Debug" or "Release"),
            "-DCARES_SHARED=" .. (shared and "ON" or "OFF"),
            "-DCARES_STATIC=" .. (shared and "OFF" or "ON")
        }
        if not shared then
            package:add("defines", "CARES_STATICLIB")
        end
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("ares_library_init", {includes = "ares.h"}))
    end)
