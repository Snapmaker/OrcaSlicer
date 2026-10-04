#ifndef slic3r_HubAddresses_hpp_
#define slic3r_HubAddresses_hpp_

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

// Which of this PC's addresses the hub tells a phone about (the `ips` list in /pair, /state and
// /hub/info, and the host the LAN link is built from).
//
// The hub used to hand over everything this PC's own name resolved to, so a PC with WSL, Docker or
// a VPN client advertised 172.x / 10.x addresses of adapters that exist only inside the PC. The
// app turns each entry of `ips` into an origin on the *scanned* URL's scheme and port, so those
// became candidates the phone can never reach, and, when the QR named the Tailscale Serve origin
// (https, 443), also `https://<tailscale ip>` (the certificate only covers the ts.net name) and
// `https://<lan ip>` (the hub's own listener is plain http on another port).
//
// Everything here is pure, so tests/slic3rutils/hub_addresses_tests.cpp can drive it with fake
// adapter lists. RemoteHub.cpp fills the Adapter list from GetAdaptersAddresses (Windows) or
// getifaddrs (elsewhere).
namespace Slic3r {
namespace HubAddresses {

// The IANA ifType numbers GetAdaptersAddresses reports (IfType). POSIX adapters leave if_type 0.
constexpr unsigned IF_ETHERNET      = 6;
constexpr unsigned IF_PPP           = 23;
constexpr unsigned IF_LOOPBACK      = 24;
constexpr unsigned IF_FASTETHER     = 62;
constexpr unsigned IF_FASTETHER_FX  = 69;
constexpr unsigned IF_IEEE80211     = 71;
constexpr unsigned IF_GIGABIT       = 117;
constexpr unsigned IF_TUNNEL        = 131;

struct Adapter
{
    std::string name;        // Windows friendly name ("vEthernet (WSL)"), POSIX interface name ("docker0")
    std::string description; // Windows adapter description ("Hyper-V Virtual Ethernet Adapter"), "" on POSIX
    unsigned    if_type { 0 };       // IANA ifType; 0 = unknown / not reported
    bool        up { true };         // operationally up
    bool        loopback { false };
    bool        has_gateway { false }; // this adapter carries a default gateway
    std::vector<std::string> ipv4;   // dotted-quad addresses, in the order the system lists them
};

inline std::string lower_ascii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

inline bool contains_any(const std::string& haystack_lower, std::initializer_list<const char*> needles)
{
    for (const char* n : needles)
        if (haystack_lower.find(n) != std::string::npos) return true;
    return false;
}

inline bool starts_with_any(const std::string& s_lower, std::initializer_list<const char*> prefixes)
{
    for (const char* p : prefixes)
        if (s_lower.compare(0, std::char_traits<char>::length(p), p) == 0) return true;
    return false;
}

// "a.b.c.d" -> its four octets; false for anything that is not a plain dotted quad.
inline bool parse_ipv4(const std::string& s, int (&o)[4])
{
    size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        size_t end = pos;
        while (end < s.size() && std::isdigit((unsigned char) s[end])) ++end;
        if (end == pos || end - pos > 3) return false;
        const int v = std::atoi(s.substr(pos, end - pos).c_str());
        if (v > 255) return false;
        o[i] = v;
        if (i < 3) {
            if (end >= s.size() || s[end] != '.') return false;
            pos = end + 1;
        } else if (end != s.size()) {
            return false;
        }
    }
    return true;
}

// 100.64.0.0/10, the carrier-grade NAT range every tailnet address comes from.
inline bool is_tailscale_range(const std::string& ip)
{
    int o[4];
    return parse_ipv4(ip, o) && o[0] == 100 && o[1] >= 64 && o[1] <= 127;
}

inline bool is_unusable_v4(const std::string& ip)
{
    int o[4];
    if (!parse_ipv4(ip, o)) return true;
    return o[0] == 0 || o[0] == 127 || (o[0] == 169 && o[1] == 254) || o[0] >= 224; // unspecified, loopback, link-local, multicast+
}

inline bool is_rfc1918(const std::string& ip)
{
    int o[4];
    if (!parse_ipv4(ip, o)) return false;
    return o[0] == 10 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 192 && o[1] == 168);
}

// Tailscale's own adapter ("Tailscale" / "Tailscale Tunnel" on Windows, tailscale0 on Linux).
inline bool is_tailscale_adapter(const Adapter& a)
{
    return lower_ascii(a.name + " " + a.description).find("tailscale") != std::string::npos;
}

// "lo", "lo0": the POSIX loopback names. Not a prefix test: Windows' "Local Area Connection" starts with "lo".
inline bool is_posix_loopback_name(const std::string& n_lower)
{
    if (n_lower.size() < 2 || n_lower[0] != 'l' || n_lower[1] != 'o') return false;
    return std::all_of(n_lower.begin() + 2, n_lower.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// An adapter that exists only inside the PC (or only reaches other software on it), so a phone
// can never be on its network: WSL, Hyper-V host/internal switches, Docker, VirtualBox and VMware
// host-only networks, Bluetooth PAN, Wi-Fi Direct, tunnelling pseudo-adapters, Linux bridges and
// veth pairs. A Hyper-V *external* switch is the one case the name alone cannot settle: it carries
// the PC's real LAN address and a default gateway, which the internal switches never have, so a
// virtual-looking adapter that has a gateway is kept.
inline bool is_virtual_adapter(const Adapter& a)
{
    if (a.has_gateway) return false;
    const std::string both = lower_ascii(a.name + " " + a.description);
    if (contains_any(both, { "vethernet", "hyper-v", "docker", "wsl", "virtualbox", "vbox", "vmware", "vmnet",
                             "host-only", "bluetooth", "wi-fi direct", "teredo", "isatap", "6to4", "npcap",
                             "loopback", "pseudo-interface", "virtual switch", "default switch", "tap-windows", "tap-win32",
                             "openvpn", "wireguard" }))
        return true;
    const std::string n = lower_ascii(a.name);
    // POSIX interface names (and macOS's).
    return starts_with_any(n, { "docker", "br-", "veth", "virbr", "vboxnet", "vmnet", "lxcbr", "lxdbr", "cni", "flannel",
                                "cali", "kube", "podman", "tun", "tap", "wg", "zt", "utun", "awdl", "llw", "bridge",
                                "gif", "stf", "anpi", "vmenet", "ap1" }) ||
           is_posix_loopback_name(n);
}

// The adapter types a phone can share a network with. Windows reports a real type for these;
// 0 means "not reported" (POSIX), where the name test above is all there is. PPP / tunnel / the
// 53 "proprietary virtual" every VPN client uses are not among them, which is what keeps an
// "NGI VPN" or NordLynx address out.
inline bool is_lan_if_type(unsigned t)
{
    return t == 0 || t == IF_ETHERNET || t == IF_IEEE80211 || t == IF_GIGABIT || t == IF_FASTETHER || t == IF_FASTETHER_FX;
}

enum class Kind { Lan, Tailscale, Rejected };

inline Kind classify(const Adapter& a, const std::string& ip)
{
    if (a.loopback || !a.up || a.if_type == IF_LOOPBACK || is_unusable_v4(ip)) return Kind::Rejected;
    if (is_tailscale_adapter(a)) return Kind::Tailscale;
    if (a.if_type == IF_TUNNEL || a.if_type == IF_PPP) return Kind::Rejected;
    if (!is_lan_if_type(a.if_type) || is_virtual_adapter(a)) return Kind::Rejected;
    return Kind::Lan;
}

// The addresses to advertise, best first:
//   1. the address on the default-route interface (`preferred`, when it classifies as a LAN
//      address), because that is the network the PC actually sits on;
//   2. the other LAN addresses, private ones (10/8, 172.16/12, 192.168/16) ahead of any other;
//   3. Tailscale addresses last - a phone on the tailnet has the ts.net name for those.
// Duplicates drop. Nothing is returned for loopback, link-local, down or virtual adapters.
inline std::vector<std::string> candidate_ips(const std::vector<Adapter>& adapters, const std::string& preferred = std::string())
{
    std::vector<std::string> pref, priv, other, ts;
    auto add = [](std::vector<std::string>& v, const std::string& ip) {
        if (std::find(v.begin(), v.end(), ip) == v.end()) v.push_back(ip);
    };
    for (const Adapter& a : adapters) {
        for (const std::string& ip : a.ipv4) {
            switch (classify(a, ip)) {
            case Kind::Lan:
                if (!preferred.empty() && ip == preferred) add(pref, ip);
                else if (is_rfc1918(ip)) add(priv, ip);
                else add(other, ip);
                break;
            case Kind::Tailscale: add(ts, ip); break;
            case Kind::Rejected: break;
            }
        }
    }
    std::vector<std::string> out;
    for (const auto* v : { &pref, &priv, &other, &ts })
        for (const std::string& ip : *v)
            if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
    return out;
}

// Whether `ip` is one the adapters say this PC has *and* a phone could use: the gate for the
// older name-resolution sources, which know nothing about adapters.
inline bool is_candidate_ip(const std::vector<Adapter>& adapters, const std::string& ip)
{
    const std::vector<std::string> all = candidate_ips(adapters);
    return std::find(all.begin(), all.end(), ip) != all.end();
}

// What `ips` carries once the hub knows whether Tailscale Serve publishes it. With Serve up the
// phone reaches the hub by the ts.net name (urls.remote), and a raw tailnet address can only be
// tried over https, which fails TLS because the certificate covers the name alone - so it goes.
// Without Serve the tailnet address is kept (it stays last, see candidate_ips): plain http to the
// hub's own port is the one thing that works on it.
inline std::vector<std::string> advertised_ips(std::vector<std::string> ips, bool serve_https_available)
{
    if (serve_https_available)
        ips.erase(std::remove_if(ips.begin(), ips.end(), is_tailscale_range), ips.end());
    return ips;
}

// The `ips` a /pair answer carries. The app builds an origin from each entry on the scheme and
// port of the URL the phone *scanned*; when that URL is the Serve origin (https, 443) every such
// origin is https://<ip>:443, which nothing answers (the hub's listener is plain http on its own
// port). urls.lan already carries the real LAN origin whole, so a request that arrived through
// Serve gets an empty list rather than candidates that cannot connect.
inline std::vector<std::string> ips_for_pair(std::vector<std::string> ips, bool request_via_serve_https)
{
    if (request_via_serve_https) ips.clear();
    return ips;
}

} // namespace HubAddresses
} // namespace Slic3r

#endif
