set_project("QuarkPlusPlus")
set_version("0.1.0")
set_xmakever("3.0.8")

set_languages("cxx23")
set_warnings("all")

add_rules("mode.debug", "mode.release")
add_requires("nlohmann_json 3.12.0")

target("quarkpp")
    set_kind("binary")
    add_files("src/*.cpp")
    add_packages("nlohmann_json")
    add_defines("UNICODE", "_UNICODE", "WIN32_LEAN_AND_MEAN", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS")
    add_syslinks("winhttp", "bcrypt", "crypt32")
    add_cxxflags("/utf-8", {tools = "cl"})

    if is_mode("release") then
        set_optimize("fastest")
        add_defines("NDEBUG")
    end
