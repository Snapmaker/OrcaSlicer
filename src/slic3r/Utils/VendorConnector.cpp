#include "VendorConnector.hpp"

#include "HomeTabLogic.hpp"

#include <algorithm>
#include <iterator>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace Slic3r {
namespace Vendors {

using json = nlohmann::json;

// ------------------------------------------------------------------------------ helpers ----

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

static std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return std::string();
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static bool starts_with(const std::string& s, const std::string& head) { return s.compare(0, head.size(), head) == 0; }

static bool is_digits(const std::string& s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

static bool has_control(const std::string& s)
{
    return std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

// An HTTP header name (RFC 7230 token, the usual subset).
static bool is_token(const std::string& s)
{
    return !s.empty() && s.size() <= 64 &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.'; });
}

static std::string text_of(const json& j, const char* key, size_t max_len = 400)
{
    if (!j.is_object() || !j.contains(key))
        return std::string();
    const json& v = j.at(key);
    std::string s = v.is_string() ? v.get<std::string>() : v.is_number() || v.is_boolean() ? v.dump() : std::string();
    s = trim(s);
    if (has_control(s))
        throw std::runtime_error(std::string("\"") + key + "\" has a line break or a control character.");
    if (s.size() > max_len)
        throw std::runtime_error(std::string("\"") + key + "\" is too long.");
    return s;
}

static int int_of(const json& j, const char* key, int fallback, int lo, int hi)
{
    if (!j.is_object() || !j.contains(key))
        return fallback;
    const json& v = j.at(key);
    long long n = fallback;
    if (v.is_number_integer())
        n = v.get<long long>();
    else if (v.is_string() && is_digits(v.get<std::string>()))
        n = std::stoll(v.get<std::string>().substr(0, 9));
    else
        throw std::runtime_error(std::string("\"") + key + "\" must be a number.");
    if (n < lo || n > hi)
        throw std::runtime_error(std::string("\"") + key + "\" must be between " + std::to_string(lo) + " and " + std::to_string(hi) + ".");
    return int(n);
}

static const json& obj(const json& j, const char* key)
{
    static const json empty = json::object();
    return j.is_object() && j.contains(key) && j.at(key).is_object() ? j.at(key) : empty;
}

static std::string host_of(const std::string& url)
{
    const size_t s = url.find("://");
    if (s == std::string::npos)
        return std::string();
    size_t e = url.find_first_of("/?#", s + 3);
    std::string authority = url.substr(s + 3, e == std::string::npos ? std::string::npos : e - s - 3);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos)
        authority = authority.substr(at + 1);
    return lower(authority);
}

static std::string origin_of(const std::string& url)
{
    const size_t s = url.find("://");
    if (s == std::string::npos)
        return std::string();
    const size_t e = url.find_first_of("/?#", s + 3);
    return lower(url.substr(0, e == std::string::npos ? std::string::npos : e));
}

// ------------------------------------------------------------------------------ URLs ----

bool is_allowed_url(const std::string& url)
{
    if (url.size() > 4096 || has_control(url) || url.find(' ') != std::string::npos)
        return false;
    const std::string l = lower(url);
    std::string       host;
    if (starts_with(l, "https://"))
        host = host_of(l);
    else if (starts_with(l, "http://")) {
        host = host_of(l);
        const std::string name = host.substr(0, host.rfind(':') == std::string::npos || host.back() == ']' ? host.size() : host.rfind(':'));
        if (name != "localhost" && name != "127.0.0.1" && name != "[::1]")
            return false;
    } else
        return false;
    // No user:password@ in a URL: credentials only travel in headers the user set up.
    const size_t s = l.find("://") + 3;
    const size_t e = l.find_first_of("/?#", s);
    if (l.substr(s, e == std::string::npos ? std::string::npos : e - s).find('@') != std::string::npos)
        return false;
    return !host.empty();
}

bool same_origin(const std::string& a, const std::string& b)
{
    const std::string oa = origin_of(a);
    return !oa.empty() && oa == origin_of(b);
}

std::string join_url(const std::string& base, const std::string& ref)
{
    const std::string r = trim(ref);
    if (r.empty())
        return std::string();
    const std::string l = lower(r);
    if (starts_with(l, "https://") || starts_with(l, "http://"))
        return r;
    if (l.find("://") != std::string::npos || starts_with(l, "data:") || starts_with(l, "javascript:"))
        return std::string();
    if (starts_with(r, "//"))
        return base.substr(0, base.find("://") + 1) + r;
    const std::string origin = base.substr(0, origin_of(base).size());
    if (origin.empty())
        return std::string();
    return origin + (r[0] == '/' ? r : "/" + r);
}

std::string url_encode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string        out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

static std::string with_query(const std::string& url, const std::vector<std::pair<std::string, std::string>>& params)
{
    std::string out = url;
    for (const auto& [k, v] : params) {
        out += out.find('?') == std::string::npos ? '?' : '&';
        out += url_encode(k) + "=" + url_encode(v);
    }
    return out;
}

// ------------------------------------------------------------------------------ JSON paths ----

json at_path(const json& j, const std::string& path)
{
    if (path.empty())
        return j;
    const json* cur = &j;
    size_t      pos = 0;
    while (pos <= path.size()) {
        const size_t dot = path.find('.', pos);
        const std::string part = path.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        if (cur->is_object()) {
            auto it = cur->find(part);
            if (it == cur->end())
                return json();
            cur = &*it;
        } else if (cur->is_array() && is_digits(part) && part.size() < 9) {
            const size_t i = size_t(std::stoul(part));
            if (i >= cur->size())
                return json();
            cur = &(*cur)[i];
        } else
            return json();
        if (dot == std::string::npos)
            break;
        pos = dot + 1;
    }
    return *cur;
}

std::string scalar_text(const json& j)
{
    if (j.is_string())
        return j.get<std::string>();
    if (j.is_number_integer() || j.is_number_unsigned())
        return j.dump();
    if (j.is_number_float()) {
        const double d = j.get<double>();
        if (d == double((long long) d) && std::abs(d) < 1e15)
            return std::to_string((long long) d);
        return j.dump();
    }
    if (j.is_boolean())
        return j.get<bool>() ? "true" : "false";
    return std::string();
}

static bool truthy(const json& j)
{
    if (j.is_boolean())
        return j.get<bool>();
    if (j.is_number())
        return j.get<double>() != 0;
    if (j.is_string())
        return !j.get<std::string>().empty() && j.get<std::string>() != "false" && j.get<std::string>() != "0";
    return false;
}

static int64_t number_of(const json& j, int64_t fallback = 0)
{
    if (j.is_number())
        return int64_t(j.get<double>());
    if (j.is_string()) {
        const std::string s = trim(j.get<std::string>());
        if (s.size() < 18 && is_digits(s))
            return std::stoll(s);
    }
    return fallback;
}

std::string fill_template(const std::string& templ, const json& item, const json& sub, bool encode)
{
    std::string out;
    size_t      pos = 0;
    while (pos < templ.size()) {
        const size_t open = templ.find('{', pos);
        if (open == std::string::npos) {
            out += templ.substr(pos);
            break;
        }
        const size_t close = templ.find('}', open);
        if (close == std::string::npos) {
            out += templ.substr(pos);
            break;
        }
        out += templ.substr(pos, open - pos);
        const std::string name = templ.substr(open + 1, close - open - 1);
        const std::string value = starts_with(name, "sub.") ? scalar_text(at_path(sub, name.substr(4))) : scalar_text(at_path(item, name));
        out += encode ? url_encode(value) : value;
        pos = close + 1;
    }
    return out;
}

// ------------------------------------------------------------------------------ spec ----

static const std::set<std::string> ITEM_FIELDS { "id", "name", "thumbnail", "page_url", "designer", "license", "tags", "updated", "description" };
static const std::set<std::string> SUB_FIELDS { "id", "name", "variant", "size", "plates", "print_time", "colours", "colour" };

static std::string check_path(const std::string& p, const char* what)
{
    if (p.empty())
        return p;
    if (p[0] != '/' || starts_with(p, "//") || p.find("://") != std::string::npos || p.find(' ') != std::string::npos || p.find('#') != std::string::npos)
        throw std::runtime_error(std::string(what) + " must be a path on the API's address, starting with /.");
    return p;
}

Spec spec_from_json(const json& j)
{
    if (!j.is_object())
        throw std::runtime_error("A connector is a JSON object.");
    Spec s;
    s.id = text_of(j, "id", 64);
    if (!s.id.empty() && !std::all_of(s.id.begin(), s.id.end(), [](unsigned char c) { return std::islower(c) || std::isdigit(c) || c == '-'; }))
        throw std::runtime_error("\"id\" may only have a-z, 0-9 and -.");
    s.name = text_of(j, "name", 80);
    if (s.name.empty())
        throw std::runtime_error("Give the connector a name.");
    s.vendor = text_of(j, "vendor", 80);
    s.license = text_of(j, "license", 80);
    s.base_url = text_of(j, "base_url", 400);
    while (!s.base_url.empty() && s.base_url.back() == '/')
        s.base_url.pop_back();
    if (!is_allowed_url(s.base_url) || s.base_url.find_first_of("?#") != std::string::npos)
        throw std::runtime_error("The API address must start with https:// (http:// only for localhost) and have no query.");

    const json& auth = obj(j, "auth");
    s.auth_type = auth.empty() ? "none" : text_of(auth, "type", 16);
    if (s.auth_type.empty())
        s.auth_type = "none";
    if (s.auth_type != "none" && s.auth_type != "bearer" && s.auth_type != "header" && s.auth_type != "query" && s.auth_type != "basic")
        throw std::runtime_error("Sign-in must be none, bearer, header, query or basic.");
    s.auth_name = text_of(auth, "name", 64);
    if ((s.auth_type == "header" || s.auth_type == "query") && !is_token(s.auth_name))
        throw std::runtime_error("Name the header or query parameter the key goes in.");

    if (j.contains("headers")) {
        if (!j["headers"].is_array() || j["headers"].size() > 20)
            throw std::runtime_error("\"headers\" must be a list of at most 20.");
        std::set<std::string> seen;
        for (const json& h : j["headers"]) {
            Header x;
            x.name   = text_of(h, "name", 64);
            x.secret = h.is_object() && h.value("secret", false);
            x.value  = x.secret ? std::string() : text_of(h, "value", 1000);
            const std::string ln = lower(x.name);
            if (!is_token(x.name) || ln == "host" || ln == "content-length" || ln == "cookie" || ln == "transfer-encoding")
                throw std::runtime_error("\"" + x.name + "\" can't be used as a header.");
            if (!seen.insert(ln).second)
                throw std::runtime_error("The header \"" + x.name + "\" is listed twice.");
            s.headers.push_back(x);
        }
    }

    const json& list = obj(j, "list");
    s.list_path  = check_path(text_of(list, "path", 400), "The list path");
    if (s.list_path.empty())
        s.list_path = "/";
    s.items_path = text_of(list, "items", 200);
    const json& q = obj(list, "query");
    if (q.size() > 30)
        throw std::runtime_error("Too many fixed query parameters.");
    for (auto it = q.begin(); it != q.end(); ++it) {
        if (it.key().empty() || it.key().size() > 64 || has_control(it.key()))
            throw std::runtime_error("A query parameter has a bad name.");
        s.list_query[it.key()] = text_of(q, it.key().c_str(), 400);
    }
    const json& paging = obj(list, "paging");
    s.paging = paging.empty() ? "none" : text_of(paging, "type", 16);
    if (s.paging.empty())
        s.paging = "none";
    if (s.paging != "none" && s.paging != "page" && s.paging != "offset" && s.paging != "cursor" && s.paging != "next")
        throw std::runtime_error("Paging must be none, page, offset, cursor or next.");
    if (paging.contains("param"))
        s.page_param = text_of(paging, "param", 64);
    s.page_start    = int_of(paging, "start", 1, 0, 1);
    s.size_param    = text_of(paging, "size_param", 64);
    s.page_size     = int_of(paging, "size", 100, 1, 1000);
    s.has_more_path = text_of(paging, "has_more", 200);
    s.total_path    = text_of(paging, "total", 200);
    s.cursor_path   = text_of(paging, "cursor", 200);
    if ((s.paging == "page" || s.paging == "offset" || s.paging == "cursor") && s.page_param.empty())
        throw std::runtime_error("Name the paging parameter.");
    if ((s.paging == "cursor" || s.paging == "next") && s.cursor_path.empty())
        throw std::runtime_error("Say where the next page's cursor or link is in the answer.");
    const json& since = obj(list, "since");
    s.since_param = text_of(since, "param", 64);
    s.since_field = text_of(since, "field", 200);
    if (s.since_param.empty() != s.since_field.empty())
        throw std::runtime_error("Incremental sync needs both the parameter and the field it compares.");

    const json& fields = obj(j, "fields");
    for (auto it = fields.begin(); it != fields.end(); ++it)
        if (ITEM_FIELDS.count(it.key()))
            s.fields[it.key()] = text_of(fields, it.key().c_str(), 400);
    if (s.fields["id"].empty())
        s.fields["id"] = "id";
    if (s.fields["name"].empty())
        s.fields["name"] = "name";

    const json& files = obj(j, "files");
    s.subs_path = text_of(files, "path", 200);
    const json& sub_fields = obj(files, "fields");
    for (auto it = sub_fields.begin(); it != sub_fields.end(); ++it)
        if (SUB_FIELDS.count(it.key()))
            s.sub_fields[it.key()] = text_of(sub_fields, it.key().c_str(), 200);
    if (!s.subs_path.empty() && s.sub_fields["id"].empty())
        s.sub_fields["id"] = "id";

    const json& dl = obj(j, "download");
    s.download_path         = check_path(text_of(dl, "path", 400), "The download path");
    s.download_url_path     = text_of(dl, "url_field", 200);
    s.download_direct_field = text_of(dl, "direct_field", 200);
    s.downloads_limited     = dl.is_object() && dl.value("limited", false);
    if (!s.download_path.empty() && s.download_url_path.empty())
        s.download_url_path = "url";

    const json& quota = obj(j, "quota");
    s.quota_used_header  = text_of(quota, "used", 64);
    s.quota_limit_header = text_of(quota, "limit", 64);
    s.quota_reset_header = text_of(quota, "resets", 64);
    for (const std::string* h : {&s.quota_used_header, &s.quota_limit_header, &s.quota_reset_header})
        if (!h->empty() && !is_token(*h))
            throw std::runtime_error("A quota header has a bad name.");
    return s;
}

json spec_to_json(const Spec& s)
{
    json headers = json::array();
    for (const Header& h : s.headers)
        headers.push_back(h.secret ? json{{"name", h.name}, {"secret", true}} : json{{"name", h.name}, {"value", h.value}});
    json list = {{"path", s.list_path}, {"items", s.items_path}, {"query", s.list_query}};
    list["paging"] = {{"type", s.paging}, {"param", s.page_param}, {"start", s.page_start}, {"size_param", s.size_param},
                      {"size", s.page_size}, {"has_more", s.has_more_path}, {"total", s.total_path}, {"cursor", s.cursor_path}};
    list["since"] = {{"param", s.since_param}, {"field", s.since_field}};
    return {{"format", "edgeslicer-vendor-connector"},
            {"version", 1},
            {"id", s.id},
            {"name", s.name},
            {"vendor", s.vendor},
            {"license", s.license},
            {"base_url", s.base_url},
            {"auth", {{"type", s.auth_type}, {"name", s.auth_name}}},
            {"headers", headers},
            {"list", list},
            {"fields", s.fields},
            {"files", {{"path", s.subs_path}, {"fields", s.sub_fields}}},
            {"download", {{"path", s.download_path}, {"url_field", s.download_url_path}, {"direct_field", s.download_direct_field}, {"limited", s.downloads_limited}}},
            {"quota", {{"used", s.quota_used_header}, {"limit", s.quota_limit_header}, {"resets", s.quota_reset_header}}}};
}

std::string make_id(const std::string& name, uint32_t random)
{
    std::string id;
    for (unsigned char c : name) {
        if (std::isalnum(c))
            id += char(std::tolower(c));
        else if (!id.empty() && id.back() != '-')
            id += '-';
        if (id.size() >= 24)
            break;
    }
    while (!id.empty() && id.back() == '-')
        id.pop_back();
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "%08x", random);
    return (id.empty() ? std::string("vendor") : id) + "-" + suffix;
}

std::vector<std::pair<std::string, std::string>> secret_slots(const Spec& s)
{
    std::vector<std::pair<std::string, std::string>> out;
    if (s.auth_type == "basic") {
        out.emplace_back("auth_user", "User name");
        out.emplace_back("auth", "Password");
    } else if (s.auth_type == "bearer")
        out.emplace_back("auth", "API key or token");
    else if (s.auth_type == "header" || s.auth_type == "query")
        out.emplace_back("auth", s.auth_name);
    for (const Header& h : s.headers)
        if (h.secret)
            out.emplace_back("header:" + h.name, h.name);
    return out;
}

// ------------------------------------------------------------------------------ requests ----

static std::string secret(const Secrets& secrets, const std::string& key)
{
    auto it = secrets.find(key);
    // A value with a line break could add headers of its own.
    return it == secrets.end() || has_control(it->second) ? std::string() : it->second;
}

Request authorize(const Spec& spec, const Secrets& secrets, const std::string& url)
{
    Request r;
    r.url = url;
    r.headers.emplace_back("Accept", "application/json");
    for (const Header& h : spec.headers) {
        const std::string v = h.secret ? secret(secrets, "header:" + h.name) : h.value;
        if (!v.empty())
            r.headers.emplace_back(h.name, v);
    }
    const std::string key = secret(secrets, "auth");
    if (spec.auth_type == "bearer" && !key.empty())
        r.headers.emplace_back("Authorization", "Bearer " + key);
    else if (spec.auth_type == "header" && !key.empty())
        r.headers.emplace_back(spec.auth_name, key);
    else if (spec.auth_type == "query" && !key.empty())
        r.url = with_query(r.url, {{spec.auth_name, key}});
    else if (spec.auth_type == "basic") {
        const std::string user = secret(secrets, "auth_user");
        if (!user.empty() || !key.empty())
            r.headers.emplace_back("Authorization", "Basic " + HomeTab::base64(user + ":" + key));
    }
    return r;
}

// What went wrong, for the user: the status and the API's own message, never the URL (it may carry
// a key in its query) nor anything we sent.
static std::string http_error(const Spec& spec, const Response& resp, const Secrets& secrets)
{
    const std::string host = host_of(spec.base_url);
    if (resp.status == 0)
        return "Could not reach " + host + (resp.error.empty() ? std::string() : ": " + resp.error.substr(0, 200));
    std::string message;
    try {
        const json body = json::parse(resp.body);
        for (const char* k : {"error", "message", "detail", "error_description"}) {
            const json v = at_path(body, k);
            if (v.is_string()) {
                message = v.get<std::string>();
                break;
            }
            if (v.is_object() && at_path(v, "message").is_string()) {
                message = at_path(v, "message").get<std::string>();
                break;
            }
        }
    } catch (...) {}
    if (message.size() > 300)
        message = message.substr(0, 300) + "...";
    // An API that echoes what it was sent must not put a credential on screen.
    for (const auto& [key, value] : secrets)
        for (size_t at; value.size() >= 4 && (at = message.find(value)) != std::string::npos;)
            message.replace(at, value.size(), "***");
    if (has_control(message))
        std::replace_if(message.begin(), message.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }, ' ');
    std::string head;
    if (resp.status == 401 || resp.status == 403)
        head = host + " refused the credentials (HTTP " + std::to_string(resp.status) + ")";
    else if (resp.status == 404)
        head = host + " has nothing at that path (HTTP 404)";
    else if (resp.status == 429) {
        head = host + " asks to slow down (HTTP 429)";
        auto ra = resp.headers.find("retry-after");
        if (ra != resp.headers.end() && ra->second.size() < 40)
            head += ", try again after " + ra->second + (is_digits(ra->second) ? " s" : "");
    } else
        head = host + " answered HTTP " + std::to_string(resp.status);
    return message.empty() ? head + "." : head + ": " + message;
}

static void read_quota(const Spec& spec, const Response& resp, Quota& quota)
{
    auto get = [&resp](const std::string& name, std::string& out) {
        if (name.empty())
            return;
        auto it = resp.headers.find(lower(name));
        if (it != resp.headers.end() && it->second.size() < 64 && !has_control(it->second))
            out = it->second;
    };
    get(spec.quota_used_header, quota.used);
    get(spec.quota_limit_header, quota.limit);
    get(spec.quota_reset_header, quota.resets);
}

bool Quota::exhausted() const
{
    const int64_t u = number_of(json(used), -1), l = number_of(json(limit), -1);
    return u >= 0 && l > 0 && u >= l;
}

// ------------------------------------------------------------------------------ items ----

static std::string clean_url(const Spec& spec, const std::string& ref)
{
    const std::string url = join_url(spec.base_url, ref);
    return is_allowed_url(url) ? url : std::string();
}

static std::string field(const Spec& spec, const json& item, const char* name, size_t max_len = 400)
{
    auto it = spec.fields.find(name);
    if (it == spec.fields.end() || it->second.empty())
        return std::string();
    std::string v = trim(scalar_text(at_path(item, it->second)));
    return v.size() > max_len ? v.substr(0, max_len) : v;
}

static std::string colour_of(const json& c, const std::string& path)
{
    std::string v = scalar_text(path.empty() || c.is_string() ? c : at_path(c, path));
    if (v.empty() && c.is_object())
        for (const char* k : {"hex", "color", "colour"})
            if (at_path(c, k).is_string()) {
                v = at_path(c, k).get<std::string>();
                break;
            }
    v = trim(v);
    if (!v.empty() && v[0] != '#')
        v = "#" + v;
    return HomeTab::clean_colour(v);
}

std::vector<Item> map_items(const Spec& spec, const json& page)
{
    std::vector<Item> out;
    const json        arr = spec.items_path.empty() ? page : at_path(page, spec.items_path);
    if (!arr.is_array())
        return out;
    for (const json& raw : arr) {
        if (!raw.is_object())
            continue;
        Item it;
        it.id   = field(spec, raw, "id", 200);
        it.name = field(spec, raw, "name", 300);
        if (it.id.empty())
            it.id = it.name;
        if (it.id.empty())
            continue;
        if (it.name.empty())
            it.name = it.id;
        it.thumbnail = clean_url(spec, field(spec, raw, "thumbnail", 4096));
        auto pu = spec.fields.find("page_url");
        if (pu != spec.fields.end() && !pu->second.empty())
            it.page_url = clean_url(spec, pu->second.find('{') != std::string::npos ? fill_template(pu->second, raw, json(), true)
                                                                                      : scalar_text(at_path(raw, pu->second)));
        it.designer    = field(spec, raw, "designer", 200);
        it.license     = field(spec, raw, "license", 80);
        if (it.license.empty())
            it.license = spec.license;
        it.description = field(spec, raw, "description", 2000);
        it.updated     = field(spec, raw, "updated", 64);
        auto tg = spec.fields.find("tags");
        if (tg != spec.fields.end() && !tg->second.empty()) {
            const json t = at_path(raw, tg->second);
            auto add = [&it](const std::string& s) {
                const std::string v = trim(s);
                if (!v.empty() && v.size() <= 80 && it.tags.size() < 30 && std::find(it.tags.begin(), it.tags.end(), v) == it.tags.end())
                    it.tags.push_back(v);
            };
            if (t.is_array()) {
                for (const json& x : t)
                    add(x.is_object() ? scalar_text(!at_path(x, "name").is_null() ? at_path(x, "name") : at_path(x, "label")) : scalar_text(x));
            } else
                add(scalar_text(t));
        }
        if (!spec.download_direct_field.empty() && spec.subs_path.empty())
            it.download = clean_url(spec, scalar_text(at_path(raw, spec.download_direct_field)));

        if (!spec.subs_path.empty()) {
            const json subs = at_path(raw, spec.subs_path);
            if (subs.is_array())
                for (const json& rs : subs) {
                    if (!rs.is_object() || it.subs.size() >= 200)
                        continue;
                    SubItem s;
                    auto sf = [&spec, &rs](const char* name) -> json {
                        auto f = spec.sub_fields.find(name);
                        return f == spec.sub_fields.end() || f->second.empty() ? json() : at_path(rs, f->second);
                    };
                    s.id      = trim(scalar_text(sf("id"))).substr(0, 200);
                    s.name    = trim(scalar_text(sf("name"))).substr(0, 300);
                    s.variant = trim(scalar_text(sf("variant"))).substr(0, 200);
                    s.size    = std::max<int64_t>(0, number_of(sf("size")));
                    s.plates  = int(std::max<int64_t>(0, std::min<int64_t>(number_of(sf("plates")), 999)));
                    const json pt = sf("print_time");
                    s.print_time_s = std::max<int64_t>(0, number_of(pt));
                    if (s.print_time_s == 0 && pt.is_string())
                        s.print_time_text = trim(pt.get<std::string>()).substr(0, 40);
                    const json cs = sf("colours");
                    auto       cf = spec.sub_fields.find("colour");
                    if (cs.is_array())
                        for (const json& c : cs) {
                            const std::string v = colour_of(c, cf == spec.sub_fields.end() ? std::string() : cf->second);
                            if (!v.empty() && s.colours.size() < 16)
                                s.colours.push_back(v);
                        }
                    if (!spec.download_direct_field.empty())
                        s.download = clean_url(spec, scalar_text(at_path(rs, spec.download_direct_field)));
                    if (s.id.empty())
                        s.id = s.name;
                    it.subs.push_back(std::move(s));
                }
        }
        out.push_back(std::move(it));
    }
    return out;
}

json item_to_json(const Item& i)
{
    json subs = json::array();
    for (const SubItem& s : i.subs)
        subs.push_back({{"id", s.id}, {"name", s.name}, {"variant", s.variant}, {"size", s.size}, {"plates", s.plates},
                        {"print_time_s", s.print_time_s}, {"print_time_text", s.print_time_text}, {"colours", s.colours},
                        {"download", s.download}});
    return {{"id", i.id}, {"name", i.name}, {"thumbnail", i.thumbnail}, {"page_url", i.page_url}, {"designer", i.designer},
            {"license", i.license}, {"description", i.description}, {"updated", i.updated}, {"tags", i.tags}, {"subs", subs}, {"download", i.download}};
}

template<class T> static T get_or(const json& j, const char* key, T fallback)
{
    if (!j.is_object() || !j.contains(key))
        return fallback;
    try {
        return j.at(key).get<T>();
    } catch (...) {
        return fallback;
    }
}

Item item_from_json(const json& j)
{
    Item i;
    i.id          = get_or<std::string>(j, "id", "");
    i.name        = get_or<std::string>(j, "name", "");
    i.thumbnail   = get_or<std::string>(j, "thumbnail", "");
    i.page_url    = get_or<std::string>(j, "page_url", "");
    i.designer    = get_or<std::string>(j, "designer", "");
    i.license     = get_or<std::string>(j, "license", "");
    i.description = get_or<std::string>(j, "description", "");
    i.updated     = get_or<std::string>(j, "updated", "");
    i.tags        = get_or<std::vector<std::string>>(j, "tags", {});
    i.download    = get_or<std::string>(j, "download", "");
    if (j.is_object() && j.contains("subs") && j["subs"].is_array())
        for (const json& x : j["subs"]) {
            SubItem s;
            s.id              = get_or<std::string>(x, "id", "");
            s.name            = get_or<std::string>(x, "name", "");
            s.variant         = get_or<std::string>(x, "variant", "");
            s.size            = get_or<int64_t>(x, "size", 0);
            s.plates          = get_or<int>(x, "plates", 0);
            s.print_time_s    = get_or<int64_t>(x, "print_time_s", 0);
            s.print_time_text = get_or<std::string>(x, "print_time_text", "");
            s.colours         = get_or<std::vector<std::string>>(x, "colours", {});
            s.download        = get_or<std::string>(x, "download", "");
            i.subs.push_back(std::move(s));
        }
    return i;
}

json cache_to_json(const Cache& c)
{
    json items = json::array();
    for (const Item& i : c.items)
        items.push_back(item_to_json(i));
    return {{"version", 1},        {"items", items},
            {"since", c.since},    {"synced_at", c.synced_at},
            {"quota", {{"used", c.quota.used}, {"limit", c.quota.limit}, {"resets", c.quota.resets}}},
            {"last_error", c.last_error}};
}

Cache cache_from_json(const json& j)
{
    Cache c;
    if (!j.is_object())
        return c;
    if (j.contains("items") && j["items"].is_array())
        for (const json& x : j["items"]) {
            Item i = item_from_json(x);
            if (!i.id.empty())
                c.items.push_back(std::move(i));
        }
    c.since      = get_or<std::string>(j, "since", "");
    c.synced_at  = get_or<int64_t>(j, "synced_at", 0);
    c.last_error = get_or<std::string>(j, "last_error", "");
    const json q = j.contains("quota") ? j["quota"] : json::object();
    c.quota.used   = get_or<std::string>(q, "used", "");
    c.quota.limit  = get_or<std::string>(q, "limit", "");
    c.quota.resets = get_or<std::string>(q, "resets", "");
    return c;
}

// ------------------------------------------------------------------------------ sync ----

// One page of the list, with the paging state advanced; false when there is no more to fetch.
struct Pager
{
    const Spec& spec;
    int         page;
    int64_t     offset { 0 };
    std::string cursor;
    std::string next_url;
    explicit Pager(const Spec& s) : spec(s), page(s.page_start) {}

    std::string url(const std::string& since, int size) const
    {
        if (spec.paging == "next" && !next_url.empty())
            return next_url;
        std::vector<std::pair<std::string, std::string>> params(spec.list_query.begin(), spec.list_query.end());
        if (!spec.size_param.empty())
            params.emplace_back(spec.size_param, std::to_string(size));
        if (!since.empty() && !spec.since_param.empty())
            params.emplace_back(spec.since_param, since);
        if (spec.paging == "page")
            params.emplace_back(spec.page_param, std::to_string(page));
        else if (spec.paging == "offset")
            params.emplace_back(spec.page_param, std::to_string(offset));
        else if (spec.paging == "cursor" && !cursor.empty())
            params.emplace_back(spec.page_param, cursor);
        return with_query(spec.base_url + (spec.list_path == "/" ? std::string() : spec.list_path), params);
    }

    // After a page of `count` raw items out of `seen` so far.
    bool advance(const json& body, size_t count, size_t seen, int size)
    {
        if (spec.paging == "none" || count == 0)
            return false;
        if (spec.paging == "cursor" || spec.paging == "next") {
            const std::string token = scalar_text(at_path(body, spec.cursor_path));
            if (token.empty() || (!spec.has_more_path.empty() && !truthy(at_path(body, spec.has_more_path))))
                return false;
            if (spec.paging == "cursor") {
                if (token == cursor)
                    return false; // the same page again
                cursor = token;
            } else {
                const std::string next = join_url(spec.base_url, token);
                if (next == next_url || !same_origin(next, spec.base_url))
                    return false; // never follow a link to another site with our credentials
                next_url = next;
            }
            return true;
        }
        bool more;
        if (!spec.has_more_path.empty())
            more = truthy(at_path(body, spec.has_more_path));
        else if (!spec.total_path.empty() && at_path(body, spec.total_path).is_number())
            more = int64_t(seen) < number_of(at_path(body, spec.total_path));
        else
            more = int(count) >= size;
        if (more) {
            ++page;
            offset += int64_t(count);
        }
        return more;
    }
};

SyncResult sync(const Spec& spec, const Secrets& secrets, const Cache& previous, const HttpFn& http, const std::atomic<bool>& cancel,
                int64_t now, const SyncOptions& options)
{
    SyncResult result;
    result.cache            = previous;
    result.cache.last_error = std::string();
    const bool  incremental = !options.full && !spec.since_param.empty() && !previous.since.empty() && !previous.items.empty();
    const std::string since = incremental ? previous.since : std::string();

    Pager             pager(spec);
    std::vector<Item> fetched;
    std::string       newest = previous.since;
    size_t            seen   = 0;
    bool              done   = false;
    for (int p = 0; p < options.max_pages; ++p) {
        if (cancel) {
            result.partial = true;
            break;
        }
        const std::string url = pager.url(since, spec.page_size);
        if (!is_allowed_url(url)) {
            result.error = "The API address is not allowed.";
            break;
        }
        const Response resp = http(authorize(spec, secrets, url));
        ++result.requests;
        read_quota(spec, resp, result.cache.quota);
        if (resp.status < 200 || resp.status >= 300) {
            result.error = http_error(spec, resp, secrets);
            break;
        }
        json body;
        try {
            body = json::parse(resp.body);
        } catch (...) {
            result.error = host_of(spec.base_url) + " did not answer with JSON.";
            break;
        }
        const json arr = spec.items_path.empty() ? body : at_path(body, spec.items_path);
        if (!arr.is_array()) {
            result.error = "The answer has no list at \"" + (spec.items_path.empty() ? std::string("(top level)") : spec.items_path) + "\".";
            break;
        }
        seen += arr.size();
        for (Item& i : map_items(spec, body))
            fetched.push_back(std::move(i));
        if (!spec.since_field.empty())
            for (const json& raw : arr) {
                const std::string v = scalar_text(at_path(raw, spec.since_field));
                if (v > newest && v.size() < 64 && !has_control(v))
                    newest = v;
            }
        if (fetched.size() >= options.max_items) {
            result.partial = true;
            result.error   = "Stopped at " + std::to_string(options.max_items) + " items.";
            break;
        }
        if (!pager.advance(body, arr.size(), seen, spec.page_size)) {
            done = true;
            break;
        }
        if (result.cache.quota.exhausted()) {
            result.partial = true;
            result.error   = "The API's request limit is used up" +
                           (result.cache.quota.resets.empty() ? std::string(".") : ", it resets " + result.cache.quota.resets + ".");
            break;
        }
    }
    if (!done && result.error.empty() && !result.partial) {
        result.partial = true;
        result.error   = "Stopped after " + std::to_string(options.max_pages) + " pages.";
    }

    const bool any = done || !fetched.empty();
    if (!any) {
        result.cache.last_error = result.error;
        return result;
    }
    if (done && !incremental) {
        // A complete listing: what is not in it anymore is gone.
        result.cache.items = std::move(fetched);
    } else {
        std::map<std::string, size_t> at;
        for (size_t i = 0; i < result.cache.items.size(); ++i)
            at[result.cache.items[i].id] = i;
        for (Item& i : fetched) {
            auto f = at.find(i.id);
            if (f != at.end())
                result.cache.items[f->second] = std::move(i);
            else {
                at[i.id] = result.cache.items.size();
                result.cache.items.push_back(std::move(i));
            }
        }
    }
    // The newest change seen only moves on when the whole list came through.
    if (done)
        result.cache.since = newest;
    result.cache.synced_at  = now;
    result.cache.last_error = result.error;
    result.ok               = done;
    return result;
}

TestResult test_connection(const Spec& spec, const Secrets& secrets, const HttpFn& http)
{
    TestResult r;
    Pager      pager(spec);
    const Response resp = http(authorize(spec, secrets, pager.url(std::string(), std::min(spec.page_size, 5))));
    read_quota(spec, resp, r.quota);
    if (resp.status < 200 || resp.status >= 300) {
        r.error = http_error(spec, resp, secrets);
        return r;
    }
    json body;
    try {
        body = json::parse(resp.body);
    } catch (...) {
        r.error = host_of(spec.base_url) + " did not answer with JSON.";
        return r;
    }
    const json arr = spec.items_path.empty() ? body : at_path(body, spec.items_path);
    if (!arr.is_array()) {
        r.error = "The answer has no list at \"" + (spec.items_path.empty() ? std::string("(top level)") : spec.items_path) + "\".";
        return r;
    }
    r.items = map_items(spec, body).size();
    if (!spec.total_path.empty() && at_path(body, spec.total_path).is_number())
        r.total = number_of(at_path(body, spec.total_path));
    r.ok = true;
    return r;
}

// ------------------------------------------------------------------------------ download ----

bool build_download_request(const Spec& spec, const Secrets& secrets, const Item& item, const SubItem* sub, Request& out, bool& is_direct)
{
    const std::string direct = sub != nullptr ? sub->download : item.download;
    if (!direct.empty() && is_allowed_url(direct)) {
        // Credentials go only to the API's own site.
        out       = same_origin(direct, spec.base_url) ? authorize(spec, secrets, direct) : Request { direct, {} };
        is_direct = true;
        return true;
    }
    if (spec.download_path.empty())
        return false;
    const json i = {{"id", item.id}, {"name", item.name}};
    const json s = sub != nullptr ? json{{"id", sub->id}, {"name", sub->name}, {"variant", sub->variant}} : json::object();
    const std::string url = spec.base_url + fill_template(spec.download_path, i, s, true);
    if (!is_allowed_url(url))
        return false;
    out       = authorize(spec, secrets, url);
    is_direct = false;
    return true;
}

std::string resolve_download(const Spec& spec, const Secrets& secrets, const Response& response, std::string& error)
{
    if (response.status < 200 || response.status >= 300) {
        error = http_error(spec, response, secrets);
        return std::string();
    }
    try {
        const std::string url = clean_url(spec, scalar_text(at_path(json::parse(response.body), spec.download_url_path)));
        if (url.empty())
            error = "The download answer has no file address at \"" + spec.download_url_path + "\".";
        return url;
    } catch (...) {
        error = host_of(spec.base_url) + " did not answer with JSON.";
        return std::string();
    }
}

// ------------------------------------------------------------------------------ CSV ----

static std::string csv_cell(std::string v)
{
    if (!v.empty() && (v[0] == '=' || v[0] == '+' || v[0] == '-' || v[0] == '@' || v[0] == '\t' || v[0] == '\r'))
        v = "'" + v;
    if (v.find_first_of(",\"\r\n") != std::string::npos || (!v.empty() && (v.front() == ' ' || v.back() == ' '))) {
        std::string q = "\"";
        for (char c : v) {
            if (c == '"')
                q += '"';
            q += c;
        }
        return q + "\"";
    }
    return v;
}

std::string to_csv(const std::string& connector, const std::string& vendor, const std::vector<Item>& items)
{
    static const char* const header[] = {"connector", "vendor",    "item_id",   "name",       "designer", "license",
                                         "tags",      "updated",   "page_url",  "thumbnail",  "file_id",  "file_name",
                                         "variant",   "size_bytes", "plates",   "print_time", "colours"};
    std::string out = "\xEF\xBB\xBF"; // a UTF-8 mark, so spreadsheets read names right
    auto row = [&out](const std::vector<std::string>& cells) {
        for (size_t i = 0; i < cells.size(); ++i) {
            if (i) out += ',';
            out += csv_cell(cells[i]);
        }
        out += "\r\n";
    };
    row(std::vector<std::string>(std::begin(header), std::end(header)));
    for (const Item& i : items) {
        std::string tags;
        for (const std::string& t : i.tags)
            tags += (tags.empty() ? "" : "; ") + t;
        const std::vector<std::string> head = {connector, vendor, i.id, i.name, i.designer, i.license, tags, i.updated, i.page_url,
                                               i.thumbnail};
        if (i.subs.empty()) {
            std::vector<std::string> cells = head;
            cells.resize(std::size(header));
            row(cells);
            continue;
        }
        for (const SubItem& s : i.subs) {
            std::string colours;
            for (const std::string& c : s.colours)
                colours += (colours.empty() ? "" : " ") + c;
            std::vector<std::string> cells = head;
            cells.insert(cells.end(), {s.id, s.name, s.variant, s.size > 0 ? std::to_string(s.size) : std::string(),
                                       s.plates > 0 ? std::to_string(s.plates) : std::string(),
                                       s.print_time_s > 0 ? std::to_string(s.print_time_s) : s.print_time_text, colours});
            row(cells);
        }
    }
    return out;
}

} // namespace Vendors
} // namespace Slic3r
