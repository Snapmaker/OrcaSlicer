#ifndef slic3r_VendorConnector_hpp_
#define slic3r_VendorConnector_hpp_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// The Home tab's Vendors: connectors the user sets up to list the models a vendor's REST API gives
// them access to. EdgeSlicer ships no vendor, only this engine and the template it runs (a JSON spec
// per connector: where the list is, how it is paged, how its fields map, how a file is downloaded).
// Kept free of wx and of any HTTP library so it can be tested on its own with a stand-in server
// (tests/slic3rutils/vendor_connector_tests.cpp); GUI/HomePanel.cpp supplies the HTTP calls and the
// OS credential store.
//
// Secrets (API keys, tokens, passwords, secret headers) are never part of a spec: a spec names the
// slots it needs (secret_slots()), their values come from the credential store at request time and
// go into request headers or the query only. Errors, the cache, the page and CSV exports never
// carry them.
namespace Slic3r {
namespace Vendors {

struct Header
{
    std::string name;
    std::string value;     // for a fixed header; empty for a secret one
    bool        secret { false };
};

struct Spec
{
    std::string id;        // [a-z0-9-], made when the connector is created
    std::string name;      // shown to the user
    std::string vendor;    // the Vendor tag (the same one the Library's folders use)
    std::string license;   // the licence the user has for this vendor's models ("Commercial"...), when an item gives none
    std::string base_url;  // https://... (http only for localhost, for testing)

    // Auth: "none", "bearer" (Authorization: Bearer <key>), "header" (<auth_name>: <key>),
    // "query" (?<auth_name>=<key>) or "basic" (user and password).
    std::string         auth_type { "none" };
    std::string         auth_name;
    std::vector<Header> headers;

    // The list of models.
    std::string                        list_path { "/" };
    std::map<std::string, std::string> list_query;
    std::string                        items_path;          // the array in the response ("" = the response is the array)
    // Paging: "none", "page" (page number), "offset", "cursor" (a token from the response) or
    // "next" (a full next-page URL from the response).
    std::string paging { "none" };
    std::string page_param { "page" };
    int         page_start { 1 };
    std::string size_param;
    int         page_size { 100 };
    std::string has_more_path;   // a boolean saying there is another page
    std::string total_path;      // or the total number of items
    std::string cursor_path;     // "cursor": where the next token is; "next": where the next URL is
    // Incremental sync: sent as <since_param>=<the newest since_field seen so far>.
    std::string since_param;
    std::string since_field;

    // Item fields, as paths into each item ("a.b.0.c"). page_url may instead be a template on the
    // item's fields: "https://vendor.example/models/{slug}".
    std::map<std::string, std::string> fields; // id name thumbnail page_url designer license tags updated description

    // Files or variants of an item (optional).
    std::string                        subs_path;
    std::map<std::string, std::string> sub_fields; // id name variant size plates print_time colours colour

    // Download (optional): a path template on the item and file ("{id}", "{sub.id}") whose JSON
    // response has the file's URL at download_url_path; or a field of the file with its URL.
    std::string download_path;
    std::string download_url_path;
    std::string download_direct_field;
    bool        downloads_limited { false }; // the vendor counts downloads: ask before each

    // Response headers with the API quota (optional).
    std::string quota_used_header;
    std::string quota_limit_header;
    std::string quota_reset_header;
};

// A spec from JSON (a file the user imported, or app_config), checked: throws std::runtime_error
// with a message for the user when it cannot be used. Unknown keys are ignored.
Spec           spec_from_json(const nlohmann::json& j);
nlohmann::json spec_to_json(const Spec& spec); // never holds a secret
// A fresh id for a new connector from its name and a random suffix.
std::string make_id(const std::string& name, uint32_t random);

// The secret values a spec needs: key -> what to ask the user for ("API key", "X-App-Key"...).
// Keys: "auth", "auth_user" (basic), "header:<name>".
std::vector<std::pair<std::string, std::string>> secret_slots(const Spec& spec);
using Secrets = std::map<std::string, std::string>;

struct Request
{
    std::string                                      url;
    std::vector<std::pair<std::string, std::string>> headers;
};
struct Response
{
    int                                status { 0 };   // 0 = no response
    std::map<std::string, std::string> headers;        // names lower-cased
    std::string                        body;
    std::string                        error;          // transport error
};
using HttpFn = std::function<Response(const Request&)>;

struct SubItem
{
    std::string              id, name, variant;
    int64_t                  size { 0 };
    int                      plates { 0 };
    int64_t                  print_time_s { 0 };
    std::string              print_time_text; // when the API gives it as text
    std::vector<std::string> colours;         // "#RRGGBB"
    std::string              download;        // the file's direct URL (download_direct_field), host-only
};

struct Item
{
    std::string              id, name, thumbnail, page_url, designer, description, updated;
    std::string              license;         // the item's field, else the spec's
    std::vector<std::string> tags;
    std::vector<SubItem>     subs;
    std::string              download;        // as for SubItem
};
nlohmann::json item_to_json(const Item& item);
Item           item_from_json(const nlohmann::json& j);

struct Quota
{
    std::string used, limit, resets;
    bool        exhausted() const; // used >= limit, both numbers
};

// What a connector last fetched: kept in <data_dir>/vendors/<id>/items.json.
struct Cache
{
    std::vector<Item> items;
    std::string       since;       // the newest since_field value seen
    int64_t           synced_at { 0 };
    Quota             quota;
    std::string       last_error;
};
nlohmann::json cache_to_json(const Cache& c);
Cache          cache_from_json(const nlohmann::json& j);

struct SyncOptions
{
    bool   full { false };     // ignore `since`: fetch everything and drop what is gone
    int    max_pages { 500 };
    size_t max_items { 50000 };
};

struct SyncResult
{
    bool        ok { false };
    bool        partial { false }; // stopped early (quota, cancel, limit): what came is merged
    std::string error;             // for the user; never holds a secret or a request URL
    int         requests { 0 };
    Cache       cache;             // `previous` with this sync merged in
};

// Fetch the list page by page and merge it into `previous` by item id (an incremental sync only
// adds and updates; a full one replaces). Stops on an error, a 429, a spent quota, `cancel`, or
// the limits.
SyncResult sync(const Spec& spec, const Secrets& secrets, const Cache& previous, const HttpFn& http,
                const std::atomic<bool>& cancel, int64_t now, const SyncOptions& options = {});

// One small request to check the address and the credentials: how many items the first page had
// (and the total when the API says), or an error.
struct TestResult
{
    bool        ok { false };
    std::string error;
    size_t      items { 0 };
    int64_t     total { -1 };
    Quota       quota;
};
TestResult test_connection(const Spec& spec, const Secrets& secrets, const HttpFn& http);

// The request that gives the file for `sub` of `item` (or the item itself when it has no files):
// either the download endpoint (resolve_download() reads its answer) or the file's direct URL.
// download_path placeholders are the mapped fields: {id} {name} {sub.id} {sub.name} {sub.variant}.
// false when the spec has no way to download it.
bool build_download_request(const Spec& spec, const Secrets& secrets, const Item& item, const SubItem* sub, Request& out,
                            bool& is_direct);
// The file URL in the download endpoint's answer, or "" with `error` set.
std::string resolve_download(const Spec& spec, const Secrets& secrets, const Response& response, std::string& error);

// Request plumbing, exposed for the tests.
Request     authorize(const Spec& spec, const Secrets& secrets, const std::string& url);
std::string url_encode(const std::string& s);
// A URL found in a response, made absolute against the API's address: "https://..." stays,
// "//host/x" takes the base's scheme, "/x" and "x" are on the base's host.
std::string join_url(const std::string& base, const std::string& ref);
bool        is_allowed_url(const std::string& url);                     // https, or http to localhost
bool        same_origin(const std::string& a, const std::string& b);
nlohmann::json at_path(const nlohmann::json& j, const std::string& path); // null when missing
std::string    scalar_text(const nlohmann::json& j);                        // "" for objects/arrays/null
std::string    fill_template(const std::string& templ, const nlohmann::json& item, const nlohmann::json& sub, bool encode);

// Items mapped from one parsed response page.
std::vector<Item> map_items(const Spec& spec, const nlohmann::json& page);

// CSV of the items, one row per file (one row for an item without files): UTF-8, RFC 4180 quoting,
// and a cell that a spreadsheet would run as a formula (= + - @ tab CR) prefixed with '.
std::string to_csv(const std::string& connector, const std::string& vendor, const std::vector<Item>& items);

} // namespace Vendors
} // namespace Slic3r

#endif
