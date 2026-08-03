#include <catch_main.hpp>

#include "slic3r/Utils/Http.hpp"

TEST_CASE("Check SSL certificates paths", "[Http][NotWorking]") {
    
    Slic3r::Http g = Slic3r::Http::get("https://github.com/");
    
    unsigned status = 0;
    g.on_error([&status](std::string, std::string, unsigned http_status) {
        status = http_status;
    });
    
    g.on_complete([&status](std::string /* body */, unsigned http_status){
        status = http_status;
    });
    
    g.perform_sync();
    
    REQUIRE(status == 200);
}

TEST_CASE("Http digest authentication", "[Http][NotWorking]") {
    Slic3r::Http g = Slic3r::Http::get("https://jigsaw.w3.org/HTTP/Digest/");

    g.auth_digest("guest", "guest");

    unsigned status = 0;
    g.on_error([&status](std::string, std::string, unsigned http_status) {
        status = http_status;
    });

    g.on_complete([&status](std::string /* body */, unsigned http_status){
        status = http_status;
    });

    g.perform_sync();

    REQUIRE(status == 200);
}

TEST_CASE("Http basic authentication", "[Http][NotWorking]") {
    Slic3r::Http g = Slic3r::Http::get("https://jigsaw.w3.org/HTTP/Basic/");

    g.auth_basic("guest", "guest");

    unsigned status = 0;
    g.on_error([&status](std::string, std::string, unsigned http_status) {
        status = http_status;
    });

    g.on_complete([&status](std::string /* body */, unsigned http_status){
        status = http_status;
    });

    g.perform_sync();

    REQUIRE(status == 200);
}

#include "slic3r/Utils/SnapmakerAccount.hpp"

// The Snapmaker account API reports failures as HTTP 200 with a non-200 body
// code, so this envelope check is what stands between an expired token and a
// "signed in" state with no profile (issues #116/#226). The parser reports
// facts - was this a success envelope, was the token refused, which profile
// fields are present - and each caller applies its own policy.
TEST_CASE("Snapmaker account response envelope", "[sm_login]") {
    Slic3r::SMAccountProfile p;
    bool rejected = false;

    SECTION("success envelope yields the profile") {
        REQUIRE(Slic3r::sm_parse_account_response(
            R"({"code":200,"data":{"id":105467,"nickname":"n","icon":"i","account":"a"}})", p, rejected));
        CHECK(p.id == "105467");
        CHECK(p.nickname == "n");
        CHECK(p.account == "a");
        CHECK_FALSE(rejected);
    }
    SECTION("numeric-string code and id are accepted") {
        REQUIRE(Slic3r::sm_parse_account_response(R"({"code":"200","data":{"id":"105467"}})", p, rejected));
        CHECK(p.id == "105467");
    }
    SECTION("auth failure codes are definitive rejections") {
        for (const char* body : {R"({"code":110002})", R"({"code":110003})", R"({"code":110004})"}) {
            CHECK_FALSE(Slic3r::sm_parse_account_response(body, p, rejected));
            CHECK(rejected);
        }
    }
    SECTION("other failures are not rejections, so the token is kept") {
        for (const char* body : {R"({"code":500,"msg":"maintenance"})", R"({"msg":"weird"})",
                                 "<html>captive portal</html>", ""}) {
            CHECK_FALSE(Slic3r::sm_parse_account_response(body, p, rejected));
            CHECK_FALSE(rejected);
        }
    }
    SECTION("a non-success code is not a success envelope even with data") {
        CHECK_FALSE(Slic3r::sm_parse_account_response(R"({"code":500,"data":{"id":7}})", p, rejected));
        CHECK_FALSE(rejected);
    }
    SECTION("success envelope without an id parses; requiring one is the caller's policy") {
        REQUIRE(Slic3r::sm_parse_account_response(R"({"code":200,"data":{"nickname":"n"}})", p, rejected));
        CHECK(p.id.empty());
        CHECK(p.nickname == "n");
    }
    SECTION("null fields do not throw or abort the parse") {
        REQUIRE(Slic3r::sm_parse_account_response(
            R"({"code":200,"data":{"id":7,"nickname":null,"icon":null}})", p, rejected));
        CHECK(p.id == "7");
        CHECK(p.nickname.empty());
    }
}

TEST_CASE("Snapmaker account API base per region", "[sm_login]") {
    CHECK(Slic3r::sm_account_api_base("CN") == "https://api.snapmaker.cn");
    CHECK(Slic3r::sm_account_api_base("US") == "https://id.snapmaker.com");
    CHECK(Slic3r::sm_account_api_base("Others") == "https://id.snapmaker.com");
}
