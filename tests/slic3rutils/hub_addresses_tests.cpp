// Which of the PC's addresses the hub advertises to the phone (slic3r/Utils/HubAddresses.hpp).
// Everything is driven with fake adapter lists, several of them copied from a real PC that has
// WSL, a Hyper-V Default Switch, Tailscale and two VPN clients installed (the one whose iPhone
// log showed https://100.x, 172.22.144.1:443, 172.17.16.1:443 and 10.255.233.231:443 as
// candidates), so no network, adapter or socket is needed.

#include <catch2/catch.hpp>

#include "slic3r/Utils/HubAddresses.hpp"

#include <string>
#include <vector>

using namespace Slic3r::HubAddresses;

namespace {

Adapter make(const std::string& name, const std::string& desc, unsigned type, bool up, std::vector<std::string> ips,
             bool gateway = false)
{
    Adapter a;
    a.name        = name;
    a.description = desc;
    a.if_type     = type;
    a.up          = up;
    a.loopback    = type == IF_LOOPBACK;
    a.has_gateway = gateway;
    a.ipv4        = std::move(ips);
    return a;
}

// The Windows PC from the bug report, as GetAdaptersAddresses lists it.
std::vector<Adapter> owner_pc()
{
    return {
        make("vEthernet (WSL (Hyper-V firewall))", "Hyper-V Virtual Ethernet Adapter #2", IF_ETHERNET, true, { "172.17.16.1" }),
        make("vEthernet (Default Switch)", "Hyper-V Virtual Ethernet Adapter", IF_ETHERNET, true, { "172.27.96.1" }),
        make("NordLynx", "NordLynx Tunnel", 53, false, { "169.254.5.254", "10.5.0.2" }),
        make("Local Area Connection* 10", "Microsoft Wi-Fi Direct Virtual Adapter #2", IF_IEEE80211, false, { "169.254.189.137" }),
        make("Tailscale", "Tailscale Tunnel", 53, true, { "100.78.10.92" }),
        make("NGI VPN", "NGI VPN", IF_PPP, true, { "10.255.233.248" }),
        make("Ethernet", "Intel(R) Ethernet Controller (3) I225-V", IF_ETHERNET, false, { "169.254.196.236" }),
        make("Wi-Fi", "Intel(R) Wi-Fi 6E AX210 160MHz", IF_IEEE80211, true, { "10.0.0.131" }, /*gateway*/ true),
        make("Loopback Pseudo-Interface 1", "Software Loopback Interface 1", IF_LOOPBACK, true, { "127.0.0.1" }),
    };
}

} // namespace

TEST_CASE("the owner's PC advertises its Wi-Fi address and the Tailscale one, nothing virtual", "[HubAddresses]")
{
    const std::vector<std::string> ips = candidate_ips(owner_pc(), "10.0.0.131");
    REQUIRE(ips == std::vector<std::string>{ "10.0.0.131", "100.78.10.92" });
}

TEST_CASE("WSL, Hyper-V, Docker, VirtualBox and VMware adapters are never candidates", "[HubAddresses]")
{
    const std::vector<Adapter> adapters = {
        make("vEthernet (WSL)", "Hyper-V Virtual Ethernet Adapter", IF_ETHERNET, true, { "172.22.144.1" }),
        make("vEthernet (Default Switch)", "Hyper-V Virtual Ethernet Adapter #3", IF_ETHERNET, true, { "172.27.96.1" }),
        make("vEthernet (DockerNAT)", "Hyper-V Virtual Ethernet Adapter #4", IF_ETHERNET, true, { "10.0.75.1" }),
        make("Ethernet 2", "VirtualBox Host-Only Ethernet Adapter", IF_ETHERNET, true, { "192.168.56.1" }),
        make("VMware Network Adapter VMnet8", "VMware Virtual Ethernet Adapter for VMnet8", IF_ETHERNET, true, { "192.168.126.1" }),
        make("Bluetooth Network Connection", "Bluetooth Device (Personal Area Network)", IF_ETHERNET, true, { "192.168.44.1" }),
        make("Ethernet", "Realtek PCIe GbE Family Controller", IF_ETHERNET, true, { "192.168.1.20" }, true),
    };
    REQUIRE(candidate_ips(adapters) == std::vector<std::string>{ "192.168.1.20" });
}

TEST_CASE("a Hyper-V external switch that carries the default gateway is the real LAN and is kept", "[HubAddresses]")
{
    const std::vector<Adapter> adapters = {
        make("vEthernet (External)", "Hyper-V Virtual Ethernet Adapter", IF_ETHERNET, true, { "192.168.1.50" }, /*gateway*/ true),
        make("vEthernet (Default Switch)", "Hyper-V Virtual Ethernet Adapter #2", IF_ETHERNET, true, { "172.27.96.1" }),
    };
    REQUIRE(candidate_ips(adapters, "192.168.1.50") == std::vector<std::string>{ "192.168.1.50" });
}

TEST_CASE("loopback, link-local, down and VPN-client adapters are never candidates", "[HubAddresses]")
{
    const std::vector<Adapter> adapters = {
        make("Loopback", "Software Loopback Interface 1", IF_LOOPBACK, true, { "127.0.0.1" }),
        make("Ethernet", "Intel(R) Ethernet", IF_ETHERNET, true, { "169.254.10.4" }), // link-local only: no DHCP lease
        make("Wi-Fi 2", "USB Wi-Fi", IF_IEEE80211, false, { "192.168.8.2" }),         // down
        make("NordLynx", "NordLynx Tunnel", 53, true, { "10.5.0.2" }),
        make("OpenVPN", "TAP-Windows Adapter V9", IF_ETHERNET, true, { "10.8.0.6" }),
        make("Corp", "Cisco AnyConnect", IF_TUNNEL, true, { "10.99.0.5" }),
        make("NGI VPN", "NGI VPN", IF_PPP, true, { "10.255.233.231" }),
        make("Wi-Fi", "Intel(R) Wi-Fi 6E AX210", IF_IEEE80211, true, { "192.168.1.9" }),
    };
    REQUIRE(candidate_ips(adapters) == std::vector<std::string>{ "192.168.1.9" });
}

TEST_CASE("an adapter called Local Area Connection is not mistaken for a loopback", "[HubAddresses]")
{
    const std::vector<Adapter> adapters = {
        make("Local Area Connection", "Intel(R) PRO/1000", IF_ETHERNET, true, { "192.168.0.10" }, true),
    };
    REQUIRE(candidate_ips(adapters) == std::vector<std::string>{ "192.168.0.10" });
}

TEST_CASE("candidates are ordered: default route, other private LAN addresses, other LAN, Tailscale", "[HubAddresses]")
{
    const std::vector<Adapter> adapters = {
        make("Tailscale", "Tailscale Tunnel", 53, true, { "100.78.10.92" }),
        make("Wi-Fi", "Intel(R) Wi-Fi", IF_IEEE80211, true, { "192.168.1.9" }),
        make("Ethernet", "Intel(R) Ethernet", IF_GIGABIT, true, { "203.0.113.7", "10.0.0.4" }, true),
    };
    // The Ethernet address carries the default route, so it leads even though Wi-Fi is listed first.
    REQUIRE(candidate_ips(adapters, "10.0.0.4") ==
            std::vector<std::string>{ "10.0.0.4", "192.168.1.9", "203.0.113.7", "100.78.10.92" });
    // Without a known default route: private LAN addresses first, in system order, then the rest.
    REQUIRE(candidate_ips(adapters) == std::vector<std::string>{ "192.168.1.9", "10.0.0.4", "203.0.113.7", "100.78.10.92" });
}

TEST_CASE("Tailscale is last even when it holds the default route (an exit node)", "[HubAddresses]")
{
    const std::vector<Adapter> adapters = {
        make("Tailscale", "Tailscale Tunnel", 53, true, { "100.78.10.92" }, true),
        make("Wi-Fi", "Intel(R) Wi-Fi", IF_IEEE80211, true, { "10.0.0.131" }),
    };
    REQUIRE(candidate_ips(adapters, "100.78.10.92") == std::vector<std::string>{ "10.0.0.131", "100.78.10.92" });
}

TEST_CASE("duplicates across adapters drop", "[HubAddresses]")
{
    const std::vector<Adapter> adapters = {
        make("Wi-Fi", "Intel(R) Wi-Fi", IF_IEEE80211, true, { "192.168.1.9", "192.168.1.9" }),
        make("Ethernet", "Intel(R) Ethernet", IF_ETHERNET, true, { "192.168.1.9" }),
    };
    REQUIRE(candidate_ips(adapters) == std::vector<std::string>{ "192.168.1.9" });
}

TEST_CASE("no adapters, or none usable, gives nothing", "[HubAddresses]")
{
    REQUIRE(candidate_ips({}).empty());
    REQUIRE(candidate_ips({ make("lo", "", 0, true, { "127.0.0.1" }) }).empty());
}

// ---- POSIX: interface names only, no ifType / description ----

namespace {
Adapter posix(const std::string& name, std::vector<std::string> ips, bool up = true)
{
    Adapter a;
    a.name     = name;
    a.up       = up;
    a.loopback = name == "lo";
    a.ipv4     = std::move(ips);
    return a;
}
} // namespace

TEST_CASE("Linux: docker0, br-*, veth*, virbr* and friends are skipped, real NICs and tailscale0 kept", "[HubAddresses]")
{
    const std::vector<Adapter> adapters = {
        posix("lo", { "127.0.0.1" }),
        posix("docker0", { "172.17.0.1" }),
        posix("br-3f2a9c0d1e", { "172.18.0.1" }),
        posix("veth1a2b3c", { "169.254.7.1" }),
        posix("virbr0", { "192.168.122.1" }),
        posix("vboxnet0", { "192.168.56.1" }),
        posix("vmnet8", { "192.168.126.1" }),
        posix("lxdbr0", { "10.20.30.1" }),
        posix("cni0", { "10.244.0.1" }),
        posix("wg0", { "10.7.0.2" }),
        posix("tun0", { "10.8.0.6" }),
        posix("tailscale0", { "100.78.10.92" }),
        posix("eth0", { "192.168.1.30" }),
        posix("wlan0", { "192.168.1.31" }, /*up*/ false),
        posix("enp3s0", { "10.0.0.8" }),
    };
    REQUIRE(candidate_ips(adapters) == std::vector<std::string>{ "192.168.1.30", "10.0.0.8", "100.78.10.92" });
}

TEST_CASE("macOS-style names: utun / awdl / bridge are skipped", "[HubAddresses]")
{
    const std::vector<Adapter> adapters = {
        posix("lo0", { "127.0.0.1" }),
        posix("awdl0", { "169.254.9.9" }),
        posix("bridge100", { "192.168.64.1" }),
        posix("utun3", { "10.9.0.2" }),
        posix("en0", { "192.168.1.5" }),
    };
    REQUIRE(candidate_ips(adapters) == std::vector<std::string>{ "192.168.1.5" });
}

// ---- Tailscale Serve: never a raw tailnet address over https ----

TEST_CASE("with Tailscale Serve publishing the hub the raw tailnet address is dropped", "[HubAddresses]")
{
    const std::vector<std::string> ips = { "10.0.0.131", "100.78.10.92" };
    REQUIRE(advertised_ips(ips, /*serve_https_available*/ true) == std::vector<std::string>{ "10.0.0.131" });
}

TEST_CASE("without Serve the tailnet address stays, after the LAN ones", "[HubAddresses]")
{
    const std::vector<std::string> ips = { "10.0.0.131", "100.78.10.92" };
    REQUIRE(advertised_ips(ips, false) == ips);
}

TEST_CASE("only the 100.64.0.0/10 range counts as a tailnet address", "[HubAddresses]")
{
    REQUIRE_FALSE(is_tailscale_range("100.63.255.255"));
    REQUIRE(is_tailscale_range("100.64.0.0"));
    REQUIRE(is_tailscale_range("100.78.10.92"));
    REQUIRE(is_tailscale_range("100.127.255.255"));
    REQUIRE_FALSE(is_tailscale_range("100.128.0.1"));
    REQUIRE_FALSE(is_tailscale_range("10.100.64.1"));
    REQUIRE_FALSE(is_tailscale_range("not an address"));
}

TEST_CASE("a /pair answered through Serve carries no bare hosts", "[HubAddresses]")
{
    // The app would join each to https://<scanned host>:443 - candidates that cannot answer. The
    // same list is kept for a request on the LAN listener, where it is joined to the working
    // scheme and port.
    const std::vector<std::string> ips = { "10.0.0.131" };
    REQUIRE(ips_for_pair(ips, /*via serve https*/ true).empty());
    REQUIRE(ips_for_pair(ips, false) == ips);
}

TEST_CASE("the whole story for the owner's PC with Serve on", "[HubAddresses]")
{
    // What the iPhone log should have shown: no 172.x / 10.255.x virtual-adapter candidates, no
    // https://100.78.10.92, and nothing bare-host-derived from the Serve origin at all.
    const std::vector<std::string> lan = candidate_ips(owner_pc(), "10.0.0.131");
    const std::vector<std::string> ips = advertised_ips(lan, true);
    REQUIRE(ips == std::vector<std::string>{ "10.0.0.131" });
    REQUIRE(ips_for_pair(ips, true).empty());
    REQUIRE(ips_for_pair(ips, false) == std::vector<std::string>{ "10.0.0.131" });
}
