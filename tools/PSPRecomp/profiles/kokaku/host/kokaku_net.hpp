#pragma once

#include "kokaku_transport.hpp"
#include "psprecomp/runtime.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kokaku {

constexpr std::uint32_t SCE_NET_ADHOC_ERROR_WOULD_BLOCK = 0x80410709u;
constexpr std::uint32_t SCE_NET_ADHOC_ERROR_PORT_IN_USE  = 0x8041070Au;
constexpr std::uint32_t SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID = 0x80410701u;

constexpr std::uint32_t ADHOCCTL_EVENT_ERROR        = 0u;
constexpr std::uint32_t ADHOCCTL_EVENT_CONNECT      = 1u;
constexpr std::uint32_t ADHOCCTL_EVENT_DISCONNECT   = 2u;
constexpr std::uint32_t ADHOCCTL_EVENT_SCAN         = 3u;
constexpr std::uint32_t ADHOCCTL_EVENT_GAME         = 4u;
constexpr std::uint32_t ADHOCCTL_EVENT_DISCOVER     = 5u;

#pragma pack(push, 1)
struct SceNetAdhocPdpStat {
    std::uint32_t next;
    std::int32_t id;
    std::uint8_t laddr[6];
    std::uint16_t lport;
    std::uint32_t rcv_sb_cc;
};
#pragma pack(pop)

// Bumped whenever two builds could no longer play together; the online list
// only shows rooms of the same value.
inline constexpr int netcode_version = 2;

struct NetworkConfiguration {
    bool enabled{true};
    // Online: rooms are picked from a list (internet lobby plus the local
    // network). Off: the original behaviour, every instance on the LAN is a peer.
    bool online{true};
    std::string nickname{"MajorKusanagi"};
    std::array<std::uint8_t, 6> mac{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    std::string lobby_url;
    // Announce and look for rooms on the local network as well.
    bool local_discovery{true};
    std::vector<std::string> stun_servers{"stun.cloudflare.com:3478", "stun.l.google.com:19302"};
    std::uint16_t game_port{3658};
    std::uint16_t port_offset{0};
    std::uint16_t discovery_port{50050};
};

// Access configuration
NetworkConfiguration &network_config();

// Initialize configuration from file / CLI arguments
void init_network_config(const std::filesystem::path &ini_path, int argc, char **argv);

// Register all HLE networking functions
void install_network_hle(psprecomp::Runtime &runtime);

// Release every network resource before tearing down the runtime.
void shutdown_network();

// The host UDP socket shared by every emulated PDP socket. Created on first
// use; null when networking is disabled or no UDP port could be opened.
net::Transport *network_transport();
// True while the game has at least one PDP socket open.
bool network_guest_sockets_open();

// Queue a guest adhocctl event to be fired into the guest callback
void queue_adhocctl_event(std::uint32_t event, std::uint32_t error);

// Update routine called from main thread / vblank
void update_network_events(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx);

// Run network self-test suite
bool run_net_self_tests(std::string &error);

// Opt-in CSV diagnostics; timestamps share the Windows monotonic clock.
void trace_network_frame(std::uint64_t vblank, std::uint64_t guest_us);
void trace_network_player(const psprecomp::GuestMemory &memory, std::uint32_t player,
                          std::uint64_t vblank, std::uint64_t guest_us);

} // namespace kokaku
