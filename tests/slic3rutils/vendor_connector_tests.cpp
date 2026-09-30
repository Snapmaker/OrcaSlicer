#include <catch2/catch.hpp>

#include "slic3r/Utils/VendorConnector.hpp"

#include <atomic>
#include <map>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Vendors;
using json = nlohmann::json;

// The Home tab's Vendors engine, run against a stand-in for a vendor's API (the shape of CPL3D's,
// https://www.cpl3d.com/developers). The slicer (src/slic3r/GUI/HomePanel.cpp) makes the real
// HTTP calls and keeps the secrets in the OS credential store.

namespace {

const char* const KEY     = "pk_0123456789abcdef0123456789abcdef";
const char* const APP_KEY = "app_secret_value";

json cpl3d_spec_json()
{
    return json::parse(R"({
        "name": "CPL3D", "vendor": "CPL3D", "base_url": "https://www.cpl3d.com/api/v1/",
        "auth": {"type": "bearer"},
        "headers": [{"name": "X-App-Key", "secret": true}, {"name": "X-Client", "value": "EdgeSlicer"}],
        "list": {"path": "/library", "items": "models", "query": {"sort": "name"},
                 "paging": {"type": "page", "param": "page", "start": 1, "size_param": "limit", "size": 2, "has_more": "has_next", "total": "total"},
                 "since": {"param": "updated_since", "field": "files_updated_at"}},
        "fields": {"id": "id", "name": "name", "thumbnail": "thumbnail", "page_url": "https://www.cpl3d.com/models/{slug}",
                   "tags": "type", "updated": "files_updated_at", "description": "description"},
        "files": {"path": "print_profiles", "fields": {"id": "id", "name": "name", "variant": "variant", "size": "fileSize",
                  "plates": "numberOfPlates", "print_time": "estimatedPrintTime", "colours": "filamentColors", "colour": "hex"}},
        "download": {"path": "/models/{id}/download?profile={sub.id}", "url_field": "download_url", "limited": true},
        "quota": {"used": "X-Api-Calls-Used", "limit": "X-Api-Calls-Limit", "resets": "X-Api-Period-Resets"}
    })");
}

Secrets cpl3d_secrets() { return {{"auth", KEY}, {"header:X-App-Key", APP_KEY}}; }

std::map<std::string, std::string> query_of(const std::string& url)
{
    std::map<std::string, std::string> q;
    const size_t at = url.find('?');
    if (at == std::string::npos)
        return q;
    std::string rest = url.substr(at + 1);
    size_t      pos  = 0;
    while (pos <= rest.size()) {
        size_t amp = rest.find('&', pos);
        std::string kv = rest.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            std::string v = kv.substr(eq + 1), d;
            for (size_t i = 0; i < v.size(); ++i)
                if (v[i] == '%' && i + 2 < v.size() + 0 && i + 2 <= v.size() - 1) {
                    d += char(std::stoi(v.substr(i + 1, 2), nullptr, 16));
                    i += 2;
                } else
                    d += v[i];
            q[kv.substr(0, eq)] = d;
        }
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
    return q;
}

std::string header_of(const Request& r, const std::string& name)
{
    for (const auto& [k, v] : r.headers)
        if (k == name)
            return v;
    return std::string();
}

// A stand-in for the vendor: models with ids 1..n, a quota, and a log of what was asked.
struct FakeVendor
{
    std::vector<json>        models;
    std::vector<Request>     log;
    int                      used { 0 };
    int                      limit { 100 };
    int                      fail_status { 0 };
    std::string              fail_body;

    explicit FakeVendor(int n)
    {
        for (int i = 1; i <= n; ++i)
            models.push_back(model(i, "2026-09-0" + std::to_string(i % 9 + 1) + "T00:00:00Z"));
    }
    static json model(int i, const std::string& updated)
    {
        return {{"id", i},
                {"name", "Model " + std::to_string(i)},
                {"slug", "model-" + std::to_string(i) + " x"},
                {"thumbnail", "/thumbs/" + std::to_string(i) + ".webp"},
                {"type", "poke-ball"},
                {"description", "d"},
                {"files_updated_at", updated},
                {"print_profiles",
                 {{{"id", 100 + i},
                   {"name", "Standard"},
                   {"variant", "AMS"},
                   {"fileSize", 123456},
                   {"numberOfPlates", 2},
                   {"estimatedPrintTime", 5400},
                   {"filamentColors", {{{"hex", "ff0000"}, {"name", "Red"}, {"weight", 12}}, {{"hex", "#00FF00"}}}}}}}};
    }

    Response operator()(const Request& r)
    {
        log.push_back(r);
        Response resp;
        resp.headers["x-api-calls-used"]    = std::to_string(++used);
        resp.headers["x-api-calls-limit"]   = std::to_string(limit);
        resp.headers["x-api-period-resets"] = "2026-10-29T00:00:00Z";
        if (fail_status) {
            resp.status = fail_status;
            resp.body   = fail_body;
            return resp;
        }
        if (header_of(r, "Authorization") != std::string("Bearer ") + KEY) {
            resp.status = 401;
            resp.body   = R"({"error":"Invalid API key"})";
            return resp;
        }
        const auto q = query_of(r.url);
        if (r.url.find("/download?") != std::string::npos) {
            resp.status = 200;
            resp.body   = R"({"download_url":"https://www.cpl3d.com/api/v1/download/tok123","expires_in":300})";
            return resp;
        }
        const int page  = q.count("page") ? std::stoi(q.at("page")) : 1;
        const int size  = q.count("limit") ? std::stoi(q.at("limit")) : 20;
        std::vector<json> list;
        for (const json& m : models)
            if (!q.count("updated_since") || m["files_updated_at"].get<std::string>() > q.at("updated_since"))
                list.push_back(m);
        json out = {{"models", json::array()}, {"total", list.size()}, {"page", page}, {"limit", size}};
        for (size_t i = size_t((page - 1) * size); i < list.size() && i < size_t(page * size); ++i)
            out["models"].push_back(list[i]);
        out["has_next"] = size_t(page * size) < list.size();
        resp.status = 200;
        resp.body   = out.dump();
        return resp;
    }
};

} // namespace

TEST_CASE("vendors: a connector spec is checked", "[Vendors]")
{
    const Spec s = spec_from_json(cpl3d_spec_json());
    CHECK(s.base_url == "https://www.cpl3d.com/api/v1"); // no trailing slash
    CHECK(s.paging == "page");
    CHECK(s.page_size == 2);
    CHECK(s.downloads_limited);

    auto rejects = [](const std::function<void(json&)>& edit) {
        json j = cpl3d_spec_json();
        edit(j);
        INFO(j.dump());
        CHECK_THROWS_AS(spec_from_json(j), std::runtime_error);
    };
    rejects([](json& j) { j["base_url"] = "http://www.cpl3d.com/api"; });        // not https
    rejects([](json& j) { j["base_url"] = "https://user:pw@www.cpl3d.com/api"; }); // credentials in the URL
    rejects([](json& j) { j["base_url"] = "ftp://x.example"; });
    rejects([](json& j) { j["name"] = ""; });
    rejects([](json& j) { j["headers"][1]["value"] = "x\r\nInjected: 1"; });       // header injection
    rejects([](json& j) { j["headers"][1]["name"] = "Bad Header"; });
    rejects([](json& j) { j["headers"][1]["name"] = "Host"; });
    rejects([](json& j) { j["headers"][1]["name"] = "x-app-key"; });               // twice
    rejects([](json& j) { j["auth"]["type"] = "oauth"; });
    rejects([](json& j) { j["auth"] = {{"type", "header"}}; });                    // which header?
    rejects([](json& j) { j["list"]["path"] = "https://evil.example/list"; });
    rejects([](json& j) { j["list"]["paging"]["size"] = 0; });
    rejects([](json& j) { j["list"]["paging"] = {{"type", "cursor"}, {"param", "c"}}; }); // where is the cursor?
    rejects([](json& j) { j["list"]["since"] = {{"param", "updated_since"}}; });
    rejects([](json& j) { j["download"]["path"] = "//evil.example/x"; });

    // http is fine for a local test server.
    json local = cpl3d_spec_json();
    local["base_url"] = "http://localhost:8080/api";
    CHECK_NOTHROW(spec_from_json(local));
}

TEST_CASE("vendors: a spec never holds a secret", "[Vendors]")
{
    json j = cpl3d_spec_json();
    j["headers"][0]["value"] = "should-not-survive";
    const Spec s = spec_from_json(j);
    const std::string dumped = spec_to_json(s).dump();
    CHECK(dumped.find("should-not-survive") == std::string::npos);
    // Round trip.
    const Spec again = spec_from_json(spec_to_json(s));
    CHECK(spec_to_json(again) == spec_to_json(s));

    const auto slots = secret_slots(s);
    REQUIRE(slots.size() == 2);
    CHECK(slots[0].first == "auth");
    CHECK(slots[1].first == "header:X-App-Key");

    CHECK(make_id("CPL3D Library!", 0xabc) == "cpl3d-library-00000abc");
    CHECK(make_id("***", 1) == "vendor-00000001");
}

TEST_CASE("vendors: requests carry the credentials the spec asks for", "[Vendors]")
{
    Spec s = spec_from_json(cpl3d_spec_json());
    Request r = authorize(s, cpl3d_secrets(), "https://www.cpl3d.com/api/v1/library");
    CHECK(header_of(r, "Authorization") == std::string("Bearer ") + KEY);
    CHECK(header_of(r, "X-App-Key") == APP_KEY);
    CHECK(header_of(r, "X-Client") == "EdgeSlicer");
    CHECK(header_of(r, "Accept") == "application/json");

    SECTION("a key in the query")
    {
        s.auth_type = "query";
        s.auth_name = "api_key";
        r = authorize(s, {{"auth", "a b&c"}}, "https://x.example/list?page=1");
        CHECK(r.url == "https://x.example/list?page=1&api_key=a%20b%26c");
        CHECK(header_of(r, "Authorization").empty());
    }
    SECTION("basic")
    {
        s.auth_type = "basic";
        r = authorize(s, {{"auth_user", "Aladdin"}, {"auth", "open sesame"}}, "https://x.example/");
        CHECK(header_of(r, "Authorization") == "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
    }
    SECTION("a stored value with a line break is not sent")
    {
        r = authorize(s, {{"auth", "pk_x\r\nX-Evil: 1"}}, "https://x.example/");
        CHECK(header_of(r, "Authorization").empty());
    }
    SECTION("no secret, no header")
    {
        r = authorize(s, {}, "https://x.example/");
        CHECK(header_of(r, "Authorization").empty());
        CHECK(header_of(r, "X-App-Key").empty());
    }
}

TEST_CASE("vendors: URLs and paths", "[Vendors]")
{
    CHECK(is_allowed_url("https://a.example/x"));
    CHECK(is_allowed_url("http://localhost:3000/x"));
    CHECK(is_allowed_url("http://127.0.0.1/x"));
    CHECK_FALSE(is_allowed_url("http://a.example/x"));
    CHECK_FALSE(is_allowed_url("http://localhost.evil.example/x"));
    CHECK_FALSE(is_allowed_url("javascript:alert(1)"));
    CHECK_FALSE(is_allowed_url("https://u:p@a.example/"));
    CHECK_FALSE(is_allowed_url("https://a.example/x y"));
    CHECK_FALSE(is_allowed_url("file:///etc/passwd"));

    const std::string base = "https://www.cpl3d.com/api/v1";
    CHECK(join_url(base, "/thumbs/1.webp") == "https://www.cpl3d.com/thumbs/1.webp");
    CHECK(join_url(base, "thumbs/1.webp") == "https://www.cpl3d.com/thumbs/1.webp");
    CHECK(join_url(base, "//cdn.cpl3d.com/1.webp") == "https://cdn.cpl3d.com/1.webp");
    CHECK(join_url(base, "https://cdn.example/1.webp") == "https://cdn.example/1.webp");
    CHECK(join_url(base, "javascript:alert(1)").empty());
    CHECK(join_url(base, "data:image/png;base64,xx").empty());
    CHECK(join_url(base, "").empty());

    CHECK(same_origin("https://www.cpl3d.com/api/v1/x", "https://WWW.cpl3d.com/y"));
    CHECK_FALSE(same_origin("https://www.cpl3d.com/x", "https://cpl3d.com/x"));
    CHECK_FALSE(same_origin("https://www.cpl3d.com/x", "http://www.cpl3d.com/x"));

    const json j = json::parse(R"({"a":{"b":[{"c":5},{"c":"six"}]},"n":1.0,"f":1.5,"t":true})");
    CHECK(at_path(j, "a.b.1.c") == "six");
    CHECK(at_path(j, "a.b.2.c").is_null());
    CHECK(at_path(j, "a.x").is_null());
    CHECK(scalar_text(at_path(j, "a.b.0.c")) == "5");
    CHECK(scalar_text(at_path(j, "n")) == "1");
    CHECK(scalar_text(at_path(j, "f")) == "1.5");
    CHECK(scalar_text(at_path(j, "t")) == "true");
    CHECK(scalar_text(at_path(j, "a")).empty());
    CHECK(fill_template("/m/{id}/f?p={sub.id}&x={missing}", {{"id", "a/b"}}, {{"id", 7}}, true) == "/m/a%2Fb/f?p=7&x=");
    CHECK(url_encode("é") == "%C3%A9");
}

TEST_CASE("vendors: items are mapped from the answer", "[Vendors]")
{
    const Spec s = spec_from_json(cpl3d_spec_json());
    json page = {{"models", {FakeVendor::model(7, "2026-09-07T00:00:00Z"), "not an object", {{"name", "No id"}}, {{"slug", "nothing"}}}}};
    page["models"][0]["thumbnail"] = "javascript:alert(1)";
    const std::vector<Item> items = map_items(s, page);
    REQUIRE(items.size() == 2); // the string and the item with neither id nor name are skipped
    const Item& i = items[0];
    CHECK(i.id == "7");
    CHECK(i.name == "Model 7");
    CHECK(i.thumbnail.empty());                                             // not a web address
    CHECK(i.page_url == "https://www.cpl3d.com/models/model-7%20x");       // template, encoded
    CHECK(i.tags == std::vector<std::string>{"poke-ball"});
    CHECK(i.updated == "2026-09-07T00:00:00Z");
    REQUIRE(i.subs.size() == 1);
    CHECK(i.subs[0].id == "107");
    CHECK(i.subs[0].size == 123456);
    CHECK(i.subs[0].plates == 2);
    CHECK(i.subs[0].print_time_s == 5400);
    CHECK(i.subs[0].colours == std::vector<std::string>{"#FF0000", "#00FF00"});
    CHECK(items[1].id == "No id"); // the name stands in for a missing id

    const Item back = item_from_json(item_to_json(i));
    CHECK(item_to_json(back) == item_to_json(i));
}

TEST_CASE("vendors: an item's licence, else the connector's", "[Vendors]")
{
    json j        = cpl3d_spec_json();
    j["license"]  = "Commercial";
    j["fields"]["license"] = "license.name";
    const Spec s = spec_from_json(j);
    CHECK(s.license == "Commercial");
    CHECK(spec_from_json(spec_to_json(s)).license == "Commercial");
    json own = FakeVendor::model(1, "2026-09-01T00:00:00Z");
    own["license"] = {{"name", "Personal use"}};
    const std::vector<Item> items = map_items(s, {{"models", {own, FakeVendor::model(2, "2026-09-02T00:00:00Z")}}});
    REQUIRE(items.size() == 2);
    CHECK(items[0].license == "Personal use");
    CHECK(items[1].license == "Commercial");
    CHECK(item_from_json(item_to_json(items[0])).license == "Personal use");
    CHECK(map_items(spec_from_json(cpl3d_spec_json()), {{"models", {own}}})[0].license.empty()); // not mapped, no default
}

TEST_CASE("vendors: sync pages through, then only asks for changes", "[Vendors]")
{
    const Spec        s = spec_from_json(cpl3d_spec_json());
    FakeVendor        vendor(5);
    std::atomic<bool> cancel { false };
    HttpFn            http = [&vendor](const Request& r) { return vendor(r); };

    SyncResult first = sync(s, cpl3d_secrets(), Cache(), http, cancel, 1000);
    REQUIRE(first.ok);
    CHECK(first.error.empty());
    CHECK(first.requests == 3); // 2 + 2 + 1
    CHECK(first.cache.items.size() == 5);
    CHECK(first.cache.synced_at == 1000);
    CHECK(first.cache.since == "2026-09-06T00:00:00Z"); // the newest files_updated_at
    CHECK(first.cache.quota.used == "3");
    CHECK(first.cache.quota.limit == "100");
    CHECK(first.cache.quota.resets == "2026-10-29T00:00:00Z");
    const auto q = query_of(vendor.log[0].url);
    CHECK(q.at("sort") == "name");
    CHECK(q.at("limit") == "2");
    CHECK(q.at("page") == "1");
    CHECK(query_of(vendor.log[2].url).at("page") == "3");
    CHECK_FALSE(q.count("updated_since"));

    // One model changes and one is added: an incremental sync fetches only those.
    vendor.models[0]["files_updated_at"] = "2026-09-20T00:00:00Z";
    vendor.models[0]["name"]             = "Model 1 v2";
    vendor.models.push_back(FakeVendor::model(6, "2026-09-21T00:00:00Z"));
    vendor.log.clear();
    SyncResult second = sync(s, cpl3d_secrets(), first.cache, http, cancel, 2000);
    REQUIRE(second.ok);
    CHECK(second.requests == 1);
    CHECK(vendor.log[0].url.find("updated_since=2026-09-06T00%3A00%3A00Z") != std::string::npos);
    CHECK(second.cache.items.size() == 6);
    CHECK(second.cache.items[0].name == "Model 1 v2"); // updated in place
    CHECK(second.cache.since == "2026-09-21T00:00:00Z");

    // A model goes away: only a full sync notices.
    vendor.models.erase(vendor.models.begin() + 2);
    CHECK(sync(s, cpl3d_secrets(), second.cache, http, cancel, 3000).cache.items.size() == 6);
    SyncOptions full;
    full.full = true;
    CHECK(sync(s, cpl3d_secrets(), second.cache, http, cancel, 3000, full).cache.items.size() == 5);
}

TEST_CASE("vendors: sync stops and says why", "[Vendors]")
{
    const Spec        s = spec_from_json(cpl3d_spec_json());
    std::atomic<bool> cancel { false };

    SECTION("wrong key: the vendor's message, never the key or the URL")
    {
        FakeVendor vendor(3);
        vendor.fail_status = 401;
        vendor.fail_body   = std::string(R"({"error":"Invalid API key )") + KEY + R"("})";
        const SyncResult r = sync(s, cpl3d_secrets(), Cache(), [&vendor](const Request& q) { return vendor(q); }, cancel, 1);
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("refused the credentials (HTTP 401)") != std::string::npos);
        CHECK(r.error.find("Invalid API key ***") != std::string::npos);
        CHECK(r.error.find(KEY) == std::string::npos);
        CHECK(r.error.find("/library") == std::string::npos);
        CHECK(r.cache.last_error == r.error);
    }
    SECTION("slow down")
    {
        FakeVendor vendor(3);
        vendor.fail_status = 429;
        const SyncResult r = sync(s, cpl3d_secrets(), Cache(), [&vendor](const Request& q) {
            Response resp = vendor(q);
            resp.headers["retry-after"] = "120";
            return resp;
        }, cancel, 1);
        CHECK(r.error.find("try again after 120 s") != std::string::npos);
    }
    SECTION("not reachable")
    {
        const SyncResult r = sync(s, cpl3d_secrets(), Cache(), [](const Request&) { Response x; x.error = "timeout"; return x; }, cancel, 1);
        CHECK(r.error == "Could not reach www.cpl3d.com: timeout");
    }
    SECTION("not JSON, or no list where the spec says")
    {
        CHECK(sync(s, cpl3d_secrets(), Cache(), [](const Request&) { Response x; x.status = 200; x.body = "<html>"; return x; }, cancel, 1)
                  .error.find("did not answer with JSON") != std::string::npos);
        CHECK(sync(s, cpl3d_secrets(), Cache(), [](const Request&) { Response x; x.status = 200; x.body = "{}"; return x; }, cancel, 1)
                  .error == "The answer has no list at \"models\".");
    }
    SECTION("the quota runs out part way: what came is kept, the next sync starts over")
    {
        FakeVendor vendor(5);
        vendor.limit = 2;
        const SyncResult r = sync(s, cpl3d_secrets(), Cache(), [&vendor](const Request& q) { return vendor(q); }, cancel, 1);
        CHECK_FALSE(r.ok);
        CHECK(r.partial);
        CHECK(r.requests == 2);
        CHECK(r.cache.items.size() == 4);
        CHECK(r.cache.since.empty());
        CHECK(r.error.find("request limit is used up, it resets 2026-10-29") != std::string::npos);
    }
    SECTION("cancel")
    {
        FakeVendor vendor(5);
        cancel = true;
        const SyncResult r = sync(s, cpl3d_secrets(), Cache(), [&vendor](const Request& q) { return vendor(q); }, cancel, 1);
        CHECK(r.partial);
        CHECK(r.requests == 0);
    }
    SECTION("limits")
    {
        FakeVendor  vendor(9);
        SyncOptions o;
        o.max_pages = 2;
        const SyncResult r = sync(s, cpl3d_secrets(), Cache(), [&vendor](const Request& q) { return vendor(q); }, cancel, 1, o);
        CHECK(r.partial);
        CHECK(r.cache.items.size() == 4);
        CHECK(r.error == "Stopped after 2 pages.");
    }
}

TEST_CASE("vendors: other paging styles", "[Vendors]")
{
    std::atomic<bool> cancel { false };
    auto base = [](const std::string& paging) {
        json j = json::parse(R"({"name":"T","base_url":"https://api.example/v2","list":{"path":"/items","items":"data"}})");
        j["list"]["paging"] = json::parse(paging);
        return spec_from_json(j);
    };
    auto items = [](int from, int to) {
        json a = json::array();
        for (int i = from; i < to; ++i)
            a.push_back({{"id", i}, {"name", "n" + std::to_string(i)}});
        return a;
    };

    SECTION("offset with a total")
    {
        const Spec s = base(R"({"type":"offset","param":"offset","size_param":"count","size":3,"total":"meta.total"})");
        std::vector<std::string> urls;
        const SyncResult r = sync(s, {}, Cache(), [&](const Request& q) {
            urls.push_back(q.url);
            const int off = std::stoi(query_of(q.url).at("offset"));
            Response x;
            x.status = 200;
            x.body   = json{{"data", items(off, std::min(off + 3, 7))}, {"meta", {{"total", 7}}}}.dump();
            return x;
        }, cancel, 1);
        CHECK(r.ok);
        CHECK(r.cache.items.size() == 7);
        CHECK(urls.size() == 3);
        CHECK(query_of(urls[2]).at("offset") == "6");
    }
    SECTION("cursor")
    {
        const Spec s = base(R"({"type":"cursor","param":"after","cursor":"next_cursor"})");
        int calls = 0;
        const SyncResult r = sync(s, {}, Cache(), [&](const Request& q) {
            ++calls;
            const auto qs = query_of(q.url);
            Response x;
            x.status = 200;
            if (!qs.count("after"))
                x.body = json{{"data", items(0, 2)}, {"next_cursor", "c2"}}.dump();
            else
                x.body = json{{"data", items(2, 3)}, {"next_cursor", nullptr}}.dump();
            return x;
        }, cancel, 1);
        CHECK(r.ok);
        CHECK(calls == 2);
        CHECK(r.cache.items.size() == 3);
    }
    SECTION("next links, never to another site")
    {
        const Spec s = base(R"({"type":"next","cursor":"links.next"})");
        std::vector<Request> seen;
        const SyncResult r = sync(s, {{"auth", "k"}}, Cache(), [&](const Request& q) {
            seen.push_back(q);
            Response x;
            x.status = 200;
            if (seen.size() == 1)
                x.body = json{{"data", items(0, 2)}, {"links", {{"next", "/v2/items?page=2"}}}}.dump();
            else
                x.body = json{{"data", items(2, 4)}, {"links", {{"next", "https://evil.example/steal"}}}}.dump();
            return x;
        }, cancel, 1);
        CHECK(r.ok);
        REQUIRE(seen.size() == 2);
        CHECK(seen[1].url == "https://api.example/v2/items?page=2");
        CHECK(r.cache.items.size() == 4);
    }
    SECTION("no paging: one request")
    {
        const Spec s = spec_from_json(json::parse(R"({"name":"T","base_url":"https://api.example","list":{"path":"/all"}})"));
        int calls = 0;
        const SyncResult r = sync(s, {}, Cache(), [&](const Request& q) {
            ++calls;
            CHECK(q.url == "https://api.example/all");
            Response x;
            x.status = 200;
            x.body   = items(0, 3).dump(); // the answer is the list itself
            return x;
        }, cancel, 1);
        CHECK(r.ok);
        CHECK(calls == 1);
        CHECK(r.cache.items.size() == 3);
    }
}

TEST_CASE("vendors: test connection", "[Vendors]")
{
    const Spec s = spec_from_json(cpl3d_spec_json());
    FakeVendor vendor(5);
    TestResult t = test_connection(s, cpl3d_secrets(), [&vendor](const Request& q) { return vendor(q); });
    CHECK(t.ok);
    CHECK(t.items == 2);
    CHECK(t.total == 5);
    CHECK(t.quota.used == "1");
    t = test_connection(s, {{"auth", "wrong"}}, [&vendor](const Request& q) { return vendor(q); });
    CHECK_FALSE(t.ok);
    CHECK(t.error == "www.cpl3d.com refused the credentials (HTTP 401): Invalid API key");
}

TEST_CASE("vendors: downloads", "[Vendors]")
{
    const Spec s = spec_from_json(cpl3d_spec_json());
    const std::vector<Item> items = map_items(s, {{"models", {FakeVendor::model(3, "x")}}});
    REQUIRE(items.size() == 1);
    Request r;
    bool    direct = true;
    REQUIRE(build_download_request(s, cpl3d_secrets(), items[0], &items[0].subs[0], r, direct));
    CHECK_FALSE(direct);
    CHECK(r.url == "https://www.cpl3d.com/api/v1/models/3/download?profile=103");
    CHECK(header_of(r, "X-App-Key") == APP_KEY);

    FakeVendor  vendor(1);
    std::string error;
    CHECK(resolve_download(s, cpl3d_secrets(), vendor(r), error) == "https://www.cpl3d.com/api/v1/download/tok123");
    CHECK(error.empty());
    Response bad;
    bad.status = 403;
    bad.body   = R"({"error":"Downloads need an approved app key"})";
    CHECK(resolve_download(s, cpl3d_secrets(), bad, error).empty());
    CHECK(error.find("approved app key") != std::string::npos);

    SECTION("a direct link on another site gets no credentials")
    {
        json j = cpl3d_spec_json();
        j["download"] = {{"direct_field", "file_url"}};
        const Spec d = spec_from_json(j);
        json       m = FakeVendor::model(4, "x");
        m["print_profiles"][0]["file_url"] = "https://cdn.example/f.3mf";
        const std::vector<Item> di = map_items(d, {{"models", {m}}});
        REQUIRE(build_download_request(d, cpl3d_secrets(), di[0], &di[0].subs[0], r, direct));
        CHECK(direct);
        CHECK(r.url == "https://cdn.example/f.3mf");
        CHECK(r.headers.empty());
    }
    SECTION("nothing to download with")
    {
        json j = cpl3d_spec_json();
        j.erase("download");
        CHECK_FALSE(build_download_request(spec_from_json(j), {}, items[0], nullptr, r, direct));
    }
}

TEST_CASE("vendors: CSV export", "[Vendors]")
{
    Item a;
    a.id       = "1";
    a.name     = "=HYPERLINK(\"http://evil\")";
    a.designer = "Smith, J";
    a.tags     = {"x", "y"};
    a.license  = "Commercial";
    SubItem s1;
    s1.id           = "10";
    s1.name         = "Standard";
    s1.size         = 99;
    s1.plates       = 2;
    s1.print_time_s = 60;
    s1.colours      = {"#FF0000", "#00FF00"};
    s1.download     = "https://secret.example/tok";
    SubItem s2 = s1;
    s2.id      = "11";
    a.subs     = {s1, s2};
    Item b;
    b.id   = "2";
    b.name = "Line\nbreak";

    const std::string csv = to_csv("CPL3D", "CPL3D", {a, b});
    CHECK(csv.compare(0, 3, "\xEF\xBB\xBF") == 0);
    CHECK(csv.find("connector,vendor,item_id,name,designer,license,tags,updated,page_url,thumbnail,file_id,file_name,variant,size_bytes,plates,print_time,colours\r\n") == 3);
    CHECK(csv.find("CPL3D,CPL3D,1,\"'=HYPERLINK(\"\"http://evil\"\")\",\"Smith, J\",Commercial,x; y,,,,10,Standard,,99,2,60,#FF0000 #00FF00\r\n") != std::string::npos);
    CHECK(csv.find(",11,Standard,") != std::string::npos); // one row per file
    CHECK(csv.find("CPL3D,CPL3D,2,\"Line\nbreak\",,,,,,,,,,,,,\r\n") != std::string::npos);
    CHECK(csv.find("secret.example") == std::string::npos); // download links never
}

TEST_CASE("vendors: redirects never carry credentials to another site", "[Vendors]")
{
    const Spec            s = spec_from_json(cpl3d_spec_json());
    std::vector<Request>  log;
    std::map<std::string, Response> answers;
    auto redirect = [](const std::string& to, int status = 302) {
        Response r;
        r.status              = status;
        r.headers["location"] = to;
        return r;
    };
    Response ok;
    ok.status = 200;
    ok.body   = "file";
    HttpFn http = [&](const Request& q) {
        log.push_back(q);
        auto it = answers.find(q.url);
        return it == answers.end() ? ok : it->second;
    };
    auto has_secret = [](const Request& q) {
        for (const auto& [k, v] : q.headers)
            if (v.find(KEY) != std::string::npos || v == APP_KEY)
                return true;
        return q.url.find(KEY) != std::string::npos;
    };

    SECTION("to another host: a bare request")
    {
        answers["https://www.cpl3d.com/api/v1/library"] = redirect("https://evil.example/steal");
        const Response r = fetch(s, cpl3d_secrets(), http, "https://www.cpl3d.com/api/v1/library", true);
        CHECK(r.status == 200);
        REQUIRE(log.size() == 2);
        CHECK(has_secret(log[0]));
        CHECK(log[1].url == "https://evil.example/steal");
        CHECK_FALSE(has_secret(log[1]));
        CHECK(log[1].headers.empty());
    }
    SECTION("on the API's own site: credentials again")
    {
        answers["https://www.cpl3d.com/api/v1/library"] = redirect("/api/v2/library", 301);
        fetch(s, cpl3d_secrets(), http, "https://www.cpl3d.com/api/v1/library", true);
        REQUIRE(log.size() == 2);
        CHECK(log[1].url == "https://www.cpl3d.com/api/v2/library");
        CHECK(has_secret(log[1]));
    }
    SECTION("away and back: the credentials stay home only")
    {
        answers["https://cdn.example/a"] = redirect("https://www.cpl3d.com/api/v1/b");
        fetch(s, cpl3d_secrets(), http, "https://cdn.example/a", true);
        REQUIRE(log.size() == 2);
        CHECK_FALSE(has_secret(log[0]));
        CHECK(has_secret(log[1]));
        log.clear();
        fetch(s, cpl3d_secrets(), http, "https://www.cpl3d.com/api/v1/c", false); // a request that never had them
        CHECK_FALSE(has_secret(log[0]));
    }
    SECTION("to plain http, or other schemes: refused")
    {
        answers["https://www.cpl3d.com/x"] = redirect("http://www.cpl3d.com/x");
        const Response r = fetch(s, cpl3d_secrets(), http, "https://www.cpl3d.com/x", true);
        CHECK(r.status == 0);
        CHECK_FALSE(r.error.empty());
        CHECK(log.size() == 1);
        answers["https://www.cpl3d.com/y"] = redirect("file:///etc/passwd");
        CHECK(fetch(s, cpl3d_secrets(), http, "https://www.cpl3d.com/y", true).status == 0);
    }
    SECTION("a loop ends")
    {
        answers["https://www.cpl3d.com/loop"] = redirect("https://www.cpl3d.com/loop");
        const Response r = fetch(s, cpl3d_secrets(), http, "https://www.cpl3d.com/loop", true);
        CHECK(r.status == 0);
        CHECK(log.size() == 6); // the first request and 5 redirects
    }
    SECTION("a sink is passed on to every hop")
    {
        answers["https://www.cpl3d.com/f"] = redirect("https://files.example/f");
        size_t got = 0;
        fetch(s, cpl3d_secrets(), http, "https://www.cpl3d.com/f", true, [&got](const char*, size_t n) { got += n; return true; });
        REQUIRE(log.size() == 2);
        CHECK(log[0].sink);
        CHECK(log[1].sink);
    }
    SECTION("sync and test follow the same rule")
    {
        std::atomic<bool> cancel { false };
        HttpFn            list = [&](const Request& q) {
            log.push_back(q);
            return q.url.find("https://www.cpl3d.com/api/v1/library?") == 0 ? redirect("https://evil.example/list") : ok;
        };
        sync(s, cpl3d_secrets(), Cache(), list, cancel, 1);
        REQUIRE(log.size() >= 2);
        CHECK(log[1].url == "https://evil.example/list");
        CHECK_FALSE(has_secret(log[1]));
    }
}

TEST_CASE("vendors: the cache round-trips", "[Vendors]")
{
    const Spec        s = spec_from_json(cpl3d_spec_json());
    FakeVendor        vendor(3);
    std::atomic<bool> cancel { false };
    const Cache c = sync(s, cpl3d_secrets(), Cache(), [&vendor](const Request& q) { return vendor(q); }, cancel, 5).cache;
    const Cache back = cache_from_json(json::parse(cache_to_json(c).dump()));
    CHECK(cache_to_json(back) == cache_to_json(c));
    CHECK(cache_to_json(c).dump().find(KEY) == std::string::npos);
    CHECK(cache_from_json(json::array()).items.empty());
    CHECK(cache_from_json(json::parse(R"({"items":[{"name":"no id"},5]})")).items.empty());
}
