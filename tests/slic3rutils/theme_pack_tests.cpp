#include <catch2/catch.hpp>

#include "slic3r/Utils/ThemePack.hpp"

using namespace Slic3r;
using namespace Slic3r::ThemePack;

// GUI/Theme.cpp loads the active pack with these rules; docs/themes.md is the format for authors.

TEST_CASE("theme pack: colours", "[ThemePack]")
{
    CHECK(normalize_colour("#abc") == "#AABBCC");
    CHECK(normalize_colour("#a1B2c3") == "#A1B2C3");
    CHECK(normalize_colour("#A1B2C3FF") == "#A1B2C3");
    CHECK(normalize_colour("A1B2C3").empty());
    CHECK(normalize_colour("#GGGGGG").empty());
    CHECK(normalize_colour("#12345").empty());
    CHECK(normalize_colour("").empty());
}

TEST_CASE("theme pack: only paths inside the pack", "[ThemePack]")
{
    CHECK(safe_relative_path("images/banner.png"));
    CHECK(safe_relative_path("fonts\\Cinzel-Bold.ttf"));
    CHECK(safe_relative_path("banner.png"));
    CHECK_FALSE(safe_relative_path(""));
    CHECK_FALSE(safe_relative_path("/etc/passwd"));
    CHECK_FALSE(safe_relative_path("\\\\server\\share\\x.png"));
    CHECK_FALSE(safe_relative_path("C:\\Windows\\x.png"));
    CHECK_FALSE(safe_relative_path("c:x.png"));
    CHECK_FALSE(safe_relative_path("../other/banner.png"));
    CHECK_FALSE(safe_relative_path("images/../../banner.png"));
    CHECK_FALSE(safe_relative_path("images//banner.png"));
    CHECK_FALSE(safe_relative_path("./banner.png"));
    CHECK_FALSE(safe_relative_path("images/"));
    CHECK_FALSE(safe_relative_path(std::string("a\nb.png")));
    CHECK_FALSE(safe_relative_path(std::string(300, 'a')));
}

TEST_CASE("theme pack: folder names", "[ThemePack]")
{
    CHECK(valid_id("ember-forge"));
    CHECK(valid_id("Silver Bastion 2"));
    CHECK_FALSE(valid_id(""));
    CHECK_FALSE(valid_id(".hidden"));
    CHECK_FALSE(valid_id("a/b"));
    CHECK_FALSE(valid_id(".."));
    CHECK_FALSE(valid_id("trailing."));
    CHECK_FALSE(valid_id(std::string(65, 'a')));
    CHECK(id_from_name("My Theme!") == "My Theme-");
    CHECK(id_from_name("../../evil") == "-..-evil");
    CHECK(valid_id(id_from_name("../../evil")));
    CHECK(id_from_name("...") == "theme");
    CHECK(id_from_name("") == "theme");
    CHECK(valid_id(id_from_name(std::string(100, 'x'))));
}

TEST_CASE("theme pack: parsing", "[ThemePack]")
{
    Spec        spec;
    std::string error;

    CHECK_FALSE(parse("not json", spec, error));
    CHECK_FALSE(parse("[]", spec, error));
    CHECK_FALSE(parse("{\"base\":\"dark\"}", spec, error));
    CHECK(error.find("name") != std::string::npos);

    REQUIRE(parse(R"({
        "name": "Ember Forge",
        "author": "EdgeSlicer",
        "base": "dark",
        "palette": { "accent": "#b5651d", "window_bg": "#1a1010", "nope": "#000000", "text": "red" },
        "overrides": { "#dfdfdf": "#332222", "x": "#000000" },
        "fonts": { "heading": { "files": ["fonts/Cinzel-Bold.ttf", "../x.ttf"], "face": "Cinzel" },
                   "body": { "file": "fonts/Body.ttf" } },
        "shapes": { "button_radius": 2, "box_radius": 99 },
        "titlebar": { "banner": "images/banner.png", "align": "stretch" },
        "home": { "--bg": "#101010", "--shadow": "none" }
    })", spec, error));
    CHECK(spec.name == "Ember Forge");
    CHECK(spec.base == "dark");
    CHECK(spec.palette.at("accent") == "#B5651D");
    CHECK(spec.palette.count("nope") == 0);
    CHECK(spec.palette.count("text") == 0);
    CHECK(spec.overrides.at("#DFDFDF") == "#332222");
    CHECK(spec.overrides.size() == 1);
    CHECK(spec.heading.face == "Cinzel");
    CHECK(spec.heading.files == std::vector<std::string>{"fonts/Cinzel-Bold.ttf"});
    CHECK(spec.body.face.empty());
    CHECK(spec.button_radius == 2);
    CHECK(spec.box_radius == -1);
    CHECK(spec.banner == "images/banner.png");
    CHECK(spec.banner_align == "stretch");
    CHECK(spec.home.at("--bg") == "#101010");
    CHECK(spec.home.count("--shadow") == 0);
    CHECK(spec.warnings.size() >= 6);

    REQUIRE(parse(R"({"name":"x","base":"blue","titlebar":{"banner":"/abs.png","align":"up"}})", spec, error));
    CHECK(spec.base.empty());
    CHECK(spec.banner.empty());
    CHECK(spec.banner_align == "left");
}

TEST_CASE("theme pack: colour map", "[ThemePack]")
{
    Spec spec;
    spec.palette["accent"]    = "#B5651D";
    spec.palette["window_bg"] = "#1A1010";
    spec.palette["text"]      = "#FFFFFF"; // a stock light colour itself
    spec.overrides["#DFDFDF"] = "#332222";
    spec.overrides["#009688"] = "#AA0000"; // overrides beat roles

    const std::set<std::string> reserved = {"#FFFFFF", "#FFFFFE", "#000000", "#009688", "#DFDFDF"};
    const auto map = colour_map(spec, reserved);
    CHECK(map.at("#019687") == "#B5651D");
    CHECK(map.at("#009688") == "#AA0000");
    CHECK(map.at("#FFFFFF") == "#1A1010");
    CHECK(map.at("#F8F7F7") == "#1A1010");
    CHECK(map.at("#DFDFDF") == "#332222");
    // #FFFFFF and #FFFFFE are both taken, so white text becomes #FFFFFD.
    CHECK(map.at("#000000") == "#FFFFFD");
    CHECK(map.at("#262E30") == "#FFFFFD");
    CHECK(map.count("#6B6B6B") == 0);
    for (const auto& [key, value] : map)
        CHECK(reserved.count(value) == 0);

    // Roles drawn directly add nothing to the map.
    Spec direct;
    direct.palette["titlebar_bg"] = "#101010";
    CHECK(colour_map(direct, reserved).empty());
}

TEST_CASE("theme pack: Home tab variables", "[ThemePack]")
{
    Spec spec;
    spec.palette["accent"]   = "#B5651D";
    spec.palette["panel_bg"] = "#201515";
    spec.home["--bg"]        = "#101010";
    spec.home["--unknown"]   = "#123456";
    const auto css = home_css(spec);
    CHECK(css.at("--accent") == "#B5651D");
    CHECK(css.at("--thumb") == "#201515");
    CHECK(css.at("--bg") == "#101010");
    CHECK_FALSE(css.contains("--unknown"));
    CHECK_FALSE(css.contains("--fg"));
}
