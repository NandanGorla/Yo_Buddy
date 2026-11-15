#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/theme.h"

using namespace buddy::theme;
using json = nlohmann::json;

namespace {

json valid_theme() {
    return {
        {"schema", 1},
        {"id", "test-theme"},
        {"name", "Test Theme"},
        {"kind", "dark"},
        {"tokens",
         {{"bg", "#101010"},
          {"surface", "#181818"},
          {"surface-raised", "#202020"},
          {"text", "#eeeeee"},
          {"text-muted", "#999999"},
          {"accent", "#3388ff"},
          {"accent-text", "#fff"},
          {"border", "#333333"},
          {"danger", "#ff5555"},
          {"code-bg", "#000000"},
          {"code-text", "#dddddd"},
          {"radius", "6px"},
          {"font-ui", "Segoe UI, sans-serif"},
          {"font-mono", "Cascadia Code, monospace"}}},
        {"overlay", {{"highlight", "#ffcc00"}, {"label-bg", "#000000cc"}, {"label-text", "#ffffff"}}},
    };
}

// A scratch folder that is removed when the test ends.
struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const std::string& name) {
        path = std::filesystem::temp_directory_path() / ("buddy-test-" + name);
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    void write(const std::string& file, const std::string& content) const {
        std::ofstream out(path / file, std::ios::binary);
        out << content;
    }
};

}  // namespace

TEST_CASE("theme: a complete theme parses", "[theme]") {
    const ParseResult result = parse_theme(valid_theme().dump());
    INFO(result.error);
    REQUIRE(result.ok());
    CHECK(result.theme->id == "test-theme");
    CHECK(result.theme->name == "Test Theme");
    CHECK(result.theme->kind == "dark");
    CHECK(result.theme->tokens.size() == 14);
    CHECK(result.theme->tokens.at("accent") == "#3388ff");
    CHECK(result.theme->overlay.at("label-bg") == "#000000cc");
}

TEST_CASE("theme: the shipped light and dark themes are valid", "[theme]") {
    const ThemeSet set = load_themes({std::filesystem::path(BUDDY_SOURCE_DIR) / "themes"});
    for (const LoadIssue& issue : set.issues) FAIL_CHECK(issue.file << ": " << issue.problem);
    REQUIRE(set.themes.size() >= 2);
    CHECK(pick_theme(set, "light").id == "light");
    CHECK(pick_theme(set, "light").kind == "light");
    CHECK(pick_theme(set, "dark").id == "dark");
    CHECK(pick_theme(set, "dark").kind == "dark");
}

TEST_CASE("theme: colour values", "[theme]") {
    CHECK(is_valid_color("#fff"));
    CHECK(is_valid_color("#A1b2C3"));
    CHECK(is_valid_color("#11223344"));
    for (const char* bad : {"", "#", "fff", "#ffff", "#fffff", "#gggggg", "red", "rgb(0,0,0)", "#fff;", " #fff",
                            "#fff ", "var(--x)", "url(https://evil.example/x)", "#ffffff}body{display:none"}) {
        INFO("colour: " << bad);
        CHECK_FALSE(is_valid_color(bad));
    }
}

TEST_CASE("theme: length values", "[theme]") {
    for (const char* good : {"0", "8px", "12px", "0.5rem", "1.25em", "999px"}) {
        INFO("length: " << good);
        CHECK(is_valid_length(good));
    }
    for (const char* bad : {"", "px", "8", "-4px", "1000px", "8 px", "8px;", "1.px", "1.234px", "calc(1px + 2px)",
                            "8pt", "100%", "8px 8px"}) {
        INFO("length: " << bad);
        CHECK_FALSE(is_valid_length(bad));
    }
}

TEST_CASE("theme: font lists", "[theme]") {
    CHECK(is_valid_font_list("Segoe UI"));
    CHECK(is_valid_font_list("Segoe UI Variable Text, Segoe UI, sans-serif"));
    CHECK(is_valid_font_list("Cascadia Code,Consolas,monospace"));
    for (const char* bad : {"", ",", "Segoe UI,", "\"Segoe UI\"", "Arial; color: red", "url(x)", "Arial, 3D Font",
                            "a/b", "Arial\nCourier", "expression(alert(1))", "-webkit-body"}) {
        INFO("font list: " << bad);
        CHECK_FALSE(is_valid_font_list(bad));
    }
    CHECK_FALSE(is_valid_font_list(std::string(201, 'a')));
}

TEST_CASE("theme: structural problems are rejected", "[theme]") {
    CHECK_FALSE(parse_theme("").ok());
    CHECK_FALSE(parse_theme("not json").ok());
    CHECK_FALSE(parse_theme("[]").ok());

    SECTION("wrong schema version") {
        json t = valid_theme();
        t["schema"] = 2;
        CHECK_FALSE(parse_theme(t.dump()).ok());
    }
    SECTION("unknown top-level key, such as an attempt to add CSS") {
        json t = valid_theme();
        t["css"] = "body { display: none }";
        CHECK_FALSE(parse_theme(t.dump()).ok());
    }
    SECTION("missing token") {
        json t = valid_theme();
        t["tokens"].erase("accent");
        const ParseResult r = parse_theme(t.dump());
        CHECK_FALSE(r.ok());
        CHECK(r.error.find("accent") != std::string::npos);
    }
    SECTION("unknown token") {
        json t = valid_theme();
        t["tokens"]["background-image"] = "#ffffff";
        CHECK_FALSE(parse_theme(t.dump()).ok());
    }
    SECTION("token of the wrong JSON type") {
        json t = valid_theme();
        t["tokens"]["radius"] = 8;
        CHECK_FALSE(parse_theme(t.dump()).ok());
    }
    SECTION("missing overlay group") {
        json t = valid_theme();
        t.erase("overlay");
        CHECK_FALSE(parse_theme(t.dump()).ok());
    }
    SECTION("bad id") {
        json t = valid_theme();
        t["id"] = "../escape";
        CHECK_FALSE(parse_theme(t.dump()).ok());
    }
    SECTION("bad kind") {
        json t = valid_theme();
        t["kind"] = "neon";
        CHECK_FALSE(parse_theme(t.dump()).ok());
    }
    SECTION("name with markup") {
        json t = valid_theme();
        t["name"] = "<img src=x onerror=alert(1)>";
        CHECK_FALSE(parse_theme(t.dump()).ok());
    }
    SECTION("oversized file") {
        CHECK_FALSE(parse_theme(std::string(kMaxThemeFileBytes + 1, ' ')).ok());
    }
}

TEST_CASE("theme: values that try to smuggle CSS or script are rejected", "[theme]") {
    const char* attacks[] = {
        "url(javascript:alert(1))",
        "red; background: url(https://evil.example/leak)",
        "#fff} * { display:none } .x{",
        "expression(alert(1))",
        "</style><script>alert(1)</script>",
        "var(--bd-text)",
        "#fff /* comment */",
    };
    for (const char* attack : attacks) {
        INFO("value: " << attack);
        for (const char* token : {"bg", "radius", "font-ui"}) {
            json t = valid_theme();
            t["tokens"][token] = attack;
            CHECK_FALSE(parse_theme(t.dump()).ok());
        }
        json t = valid_theme();
        t["overlay"]["highlight"] = attack;
        CHECK_FALSE(parse_theme(t.dump()).ok());
    }
}

TEST_CASE("theme: a folder with good and bad files loads the good ones", "[theme]") {
    const TempDir dir("themes-mixed");
    dir.write("good.json", valid_theme().dump());
    dir.write("broken.json", "{ this is not json");
    json bad = valid_theme();
    bad["id"] = "bad-colour";
    bad["tokens"]["bg"] = "blue";
    dir.write("bad-colour.json", bad.dump());
    dir.write("notes.txt", "ignored: not a .json file");

    const ThemeSet set = load_themes({dir.path});
    REQUIRE(set.themes.size() == 1);
    CHECK(set.themes[0].id == "test-theme");
    CHECK(set.issues.size() == 2);
}

TEST_CASE("theme: with nothing usable the built-in theme is used", "[theme]") {
    const TempDir dir("themes-empty");
    dir.write("broken.json", "nope");

    const ThemeSet set = load_themes({dir.path, dir.path / "does-not-exist"});
    REQUIRE(set.themes.size() == 1);
    CHECK(set.themes[0].id == builtin_theme().id);
    CHECK(set.issues.size() == 1);
    // The built-in theme must itself be complete and valid.
    CHECK(parse_theme(json{{"schema", 1},
                           {"id", builtin_theme().id},
                           {"name", builtin_theme().name},
                           {"kind", builtin_theme().kind},
                           {"tokens", builtin_theme().tokens},
                           {"overlay", builtin_theme().overlay}}
                          .dump())
              .ok());
}

TEST_CASE("theme: a later folder overrides an earlier one with the same id", "[theme]") {
    const TempDir shipped("themes-shipped");
    const TempDir user("themes-user");
    shipped.write("t.json", valid_theme().dump());
    json custom = valid_theme();
    custom["name"] = "My Version";
    custom["tokens"]["accent"] = "#00ff00";
    user.write("t.json", custom.dump());

    const ThemeSet set = load_themes({shipped.path, user.path});
    REQUIRE(set.themes.size() == 1);
    CHECK(set.themes[0].name == "My Version");
    CHECK(set.themes[0].tokens.at("accent") == "#00ff00");
}

TEST_CASE("theme: picking an unknown id falls back to an installed theme", "[theme]") {
    const TempDir dir("themes-pick");
    dir.write("good.json", valid_theme().dump());
    const ThemeSet set = load_themes({dir.path});
    CHECK(pick_theme(set, "no-such-theme").id == "test-theme");
    CHECK(pick_theme(ThemeSet{}, "anything").id == builtin_theme().id);
}

TEST_CASE("theme: to_json carries every token to the UI", "[theme]") {
    const ParseResult parsed = parse_theme(valid_theme().dump());
    REQUIRE(parsed.ok());
    const json out = to_json(*parsed.theme);
    CHECK(out["id"] == "test-theme");
    CHECK(out["tokens"].size() == 14);
    CHECK(out["overlay"].size() == 3);
    CHECK(out["tokens"]["font-mono"] == "Cascadia Code, monospace");
}
