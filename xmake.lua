set_project("QuarkPlusPlus")
set_version("0.1.0")

set_languages("cxx23")
set_warnings("all")
set_encodings("utf-8")

add_rules("mode.debug", "mode.release")
add_requires("nlohmann_json", "libcurl", "openssl")

target("quarkpp")
    set_kind("binary")
    add_files("src/*.cpp")
    add_packages("nlohmann_json", "libcurl", "openssl")
    add_installfiles("config/quarkpp.example.json", {prefixdir = "share/quarkpp/config"})
    add_installfiles("README.md", "README.en.md", "LICENSE", {prefixdir = "share/quarkpp"})