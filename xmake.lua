set_project("QuarkPlusPlus")
set_version("0.1.0")

set_languages("cxx23")
set_warnings("all")

add_rules("mode.debug", "mode.release")
add_requires("nlohmann_json", "libcurl", "openssl")

target("quarkpp")
    set_kind("binary")
    set_encodings("utf-8")
    add_files("src/*.cpp")
    add_packages("nlohmann_json", "libcurl", "openssl")
