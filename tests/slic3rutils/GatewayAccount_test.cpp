#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/GatewayAccount.hpp"

#include <nlohmann/json.hpp>

#include <string>

using namespace Slic3r::Gateway;

TEST_CASE("GatewayAccount parses an online frame with the primary aliases", "[gateway][account]")
{
    AccountSnapshot      snapshot;
    const nlohmann::json frame = {
        {"is_login", true},
        {"status", "online"},
        {"userid", "1008611"},
        {"token", "token-primary"},
        {"nickname", "Tian"},
        {"account", "maker@example.com"},
        {"email", "maker@example.com"},
    };

    REQUIRE(GatewayAccount::parse(frame, snapshot));
    REQUIRE(snapshot.is_login);
    REQUIRE(snapshot.userid == "1008611");
    REQUIRE(snapshot.token == "token-primary");
    REQUIRE(snapshot.nickname == "Tian");
    REQUIRE(snapshot.account == "maker@example.com");
    REQUIRE(snapshot.email == "maker@example.com");
}

TEST_CASE("GatewayAccount falls back to the secondary alias fields", "[gateway][account]")
{
    AccountSnapshot      snapshot;
    const nlohmann::json frame = {
        {"isLogin", true},
        {"stauts", "online"},
        {"id", "1008611"},
        {"accessToken", "token-tertiary"},
    };

    REQUIRE(GatewayAccount::parse(frame, snapshot));
    REQUIRE(snapshot.is_login);
    REQUIRE(snapshot.userid == "1008611");
    REQUIRE(snapshot.token == "token-tertiary");
    REQUIRE(snapshot.nickname.empty());
    REQUIRE(snapshot.account.empty());
    REQUIRE(snapshot.email.empty());
}

TEST_CASE("GatewayAccount parses an offline frame with null tokens", "[gateway][account]")
{
    AccountSnapshot snapshot;
    snapshot.is_login          = true;
    snapshot.userid            = "stale";
    snapshot.token             = "stale";
    const nlohmann::json frame = {
        {"is_login", false}, {"isLogin", false}, {"status", "offline"}, {"stauts", "offline"},     {"userid", ""},
        {"id", ""},          {"uid", nullptr},   {"token", nullptr},    {"access_token", nullptr}, {"accessToken", nullptr},
        {"nickname", ""},    {"account", ""},    {"email", ""},
    };

    REQUIRE(GatewayAccount::parse(frame, snapshot));
    REQUIRE(!snapshot.is_login);
    REQUIRE(snapshot.userid.empty());
    REQUIRE(snapshot.token.empty());
    REQUIRE(snapshot.nickname.empty());
    REQUIRE(snapshot.account.empty());
    REQUIRE(snapshot.email.empty());
}

TEST_CASE("GatewayAccount treats malformed fields as missing", "[gateway][account]")
{
    AccountSnapshot      snapshot;
    const nlohmann::json frame = {
        {"is_login", "yes"}, {"isLogin", 1}, {"status", 7}, {"userid", 42}, {"token", false}, {"nickname", 3.5},
    };

    REQUIRE(GatewayAccount::parse(frame, snapshot));
    REQUIRE(!snapshot.is_login);
    REQUIRE(snapshot.userid.empty());
    REQUIRE(snapshot.token.empty());
    REQUIRE(snapshot.nickname.empty());
}

TEST_CASE("GatewayAccount rejects non-object frames", "[gateway][account]")
{
    AccountSnapshot snapshot;
    REQUIRE(!GatewayAccount::parse(nlohmann::json::array({"is_login"}), snapshot));
    REQUIRE(!GatewayAccount::parse(nlohmann::json("online"), snapshot));
    REQUIRE(!GatewayAccount::parse(nlohmann::json(nullptr), snapshot));
}

TEST_CASE("GatewayAccount parses the wrapped offline account response", "[gateway][account]")
{
    AccountSnapshot      snapshot;
    const nlohmann::json frame = {
        {"code", -1},
        {"data",
         {{"id", nullptr},
          {"icon", nullptr},
          {"nickname", nullptr},
          {"account", nullptr},
          {"email", nullptr},
          {"status", nullptr},
          {"accessToken", nullptr},
          {"refreshToken", nullptr}}},
    };

    REQUIRE(GatewayAccount::parse(frame, snapshot));
    REQUIRE(!snapshot.is_login);
    REQUIRE(snapshot.userid.empty());
    REQUIRE(snapshot.token.empty());
    REQUIRE(snapshot.nickname.empty());
}

TEST_CASE("GatewayAccount parses the wrapped online account response", "[gateway][account]")
{
    AccountSnapshot      snapshot;
    const nlohmann::json frame = {
        {"code", 0},
        {"data",
         {{"id", 145608},
          {"nickname", "Tian"},
          {"account", "maker@example.com"},
          {"email", "maker@example.com"},
          {"status", "online"},
          {"accessToken", "eyJ-wrapped"}}},
    };

    REQUIRE(GatewayAccount::parse(frame, snapshot));
    REQUIRE(snapshot.is_login);
    REQUIRE(snapshot.userid == "145608");
    REQUIRE(snapshot.token == "eyJ-wrapped");
    REQUIRE(snapshot.nickname == "Tian");
    REQUIRE(snapshot.account == "maker@example.com");
    REQUIRE(snapshot.email == "maker@example.com");
}
