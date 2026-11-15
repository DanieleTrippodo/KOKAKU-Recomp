#include "kokaku_net.hpp"
#include "kokaku_lobby.hpp"
#include "kokaku_online.hpp"
#include "kokaku_replica_timing.hpp"
#include "psprecomp/common.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <sstream>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace kokaku {

namespace {

// Used when the INI does not name a lobby. "off" in the INI disables it.
// The service is the Cloudflare Worker in lobby/ (see lobby/README.md).
constexpr const char *default_lobby_url = "https://kokaku-lobby.masterfirefoxdani.workers.dev";

NetworkConfiguration g_network_config{};
std::mutex g_net_mutex;

std::atomic<bool> g_net_inited{false};
std::atomic<bool> g_adhoc_inited{false};
std::atomic<bool> g_adhocctl_inited{false};

std::unique_ptr<net::Transport> g_transport;
bool g_transport_failed{};
std::mutex g_transport_mutex;

struct PdpSocketRecord {
    std::uint16_t port{};
    std::uint32_t buffer_size{};
    std::uint32_t flags{};
};
std::unordered_map<int, PdpSocketRecord> g_pdp_sockets;
int g_next_socket_id = 1;
std::uint64_t g_sent_packets{}, g_received_packets{};

struct AdhocHandlerRecord {
    std::uint32_t function{};
    std::uint32_t argument{};
    std::int32_t thread_uid{};
};
std::unordered_map<int, AdhocHandlerRecord> g_adhoc_handlers;
int g_next_handler_id = 1;

std::deque<std::pair<std::uint32_t, std::uint32_t>> g_queued_adhoc_events;
std::atomic<bool> g_adhoc_events_pending{false};

std::ofstream *network_trace() {
    static std::ofstream output;
    static const bool enabled = [] {
        const char *path = std::getenv("PSPRECOMP_NET_TRACE");
        if (path && *path) output.open(path);
        return output.is_open();
    }();
    return enabled ? &output : nullptr;
}

std::uint64_t trace_time_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Both ends hash the same fields, so a sent datagram can be matched with its
// arrival in the other instance's trace.
void trace_packet(char direction, const std::uint8_t *source, unsigned sport, unsigned dport,
                  std::span<const std::uint8_t> payload) {
    auto *out = network_trace();
    if (!out) return;
    std::uint64_t hash = 14695981039346656037ull;
    const auto mix = [&](std::uint8_t byte) { hash = (hash ^ byte) * 1099511628211ull; };
    for (int i = 0; i < 6; ++i) mix(source[i]);
    mix(static_cast<std::uint8_t>(sport >> 8)); mix(static_cast<std::uint8_t>(sport));
    mix(static_cast<std::uint8_t>(dport >> 8)); mix(static_cast<std::uint8_t>(dport));
    for (auto byte : payload) mix(byte);
    *out << direction << ',' << trace_time_us() << ',' << hash << ',' << payload.size() + 32u << '\n';
}

// Opt-in payload dump for protocol analysis: PSPRECOMP_NET_DUMP=<file>.
void dump_payload(char direction, const std::uint8_t *mac, unsigned sport, unsigned dport,
                  const std::uint8_t *payload, std::size_t size) {
    static std::ofstream output;
    static const bool enabled = [] {
        const char *path = std::getenv("PSPRECOMP_NET_DUMP");
        if (path && *path) output.open(path);
        return output.is_open();
    }();
    if (!enabled) return;
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) { hex += digits[payload[i] >> 4]; hex += digits[payload[i] & 15]; }
    char peer[18];
    std::snprintf(peer, sizeof(peer), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    output << direction << ',' << trace_time_us() << ',' << peer << ',' << sport << ',' << dport << ','
           << size << ',' << hex << '\n';
    output.flush();
}

std::string trimmed(std::string text) {
    text.erase(0, text.find_first_not_of(" \t\r\n"));
    text.erase(text.find_last_not_of(" \t\r\n") + 1);
    return text;
}

std::string lowercase(std::string text) {
    for (char &c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return text;
}

bool truthy(const std::string &value) {
    const auto text = lowercase(value);
    return text == "true" || text == "1" || text == "yes" || text == "on";
}

// A stable, anonymous default so rooms of different players can be told apart
// before anyone has chosen a name.
std::string default_nickname() {
    std::uint32_t hash = 2166136261u;
#ifdef _WIN32
    char guid[64]{};
    DWORD size = sizeof(guid);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid",
                     RRF_RT_REG_SZ, nullptr, guid, &size) == ERROR_SUCCESS) {
        for (const char c : std::string(guid)) hash = (hash ^ static_cast<std::uint8_t>(c)) * 16777619u;
    } else
#endif
    {
        hash = std::random_device{}();
    }
    char name[16];
    std::snprintf(name, sizeof(name), "Agent-%04u", (hash ^ (hash >> 16)) % 10000u);
    return name;
}

std::string clean_nickname(const std::string &text) {
    std::string out;
    for (const char c : text)
        if (c >= 0x20 && c < 0x7f && out.size() < 16) out += c;
    return trimmed(out);
}

void close_guest_sockets_locked() {
    if (g_transport) g_transport->pdp_close_all();
    g_pdp_sockets.clear();
    g_next_socket_id = 1;
}

} // namespace

NetworkConfiguration &network_config() {
    return g_network_config;
}

net::Transport *network_transport() {
    std::lock_guard lock(g_transport_mutex);
    if (g_transport || g_transport_failed || !g_network_config.enabled) return g_transport.get();
    std::string error;
    const auto port = static_cast<std::uint16_t>(g_network_config.game_port + g_network_config.port_offset);
    auto socket = net::open_udp(port, true, error);
    if (!socket) {
        g_transport_failed = true;
        std::cerr << "[network] No UDP socket for multiplayer: " << error << '\n';
        return nullptr;
    }
    if (socket->local_port() != port)
        std::cerr << "[network] UDP port " << port << " is busy, using " << socket->local_port() << '\n';
    // Diagnostics: PSPRECOMP_NET_IMPAIR=delay[,jitter[,loss%]] plays as if the
    // other players were that far away.
    if (const char *text = std::getenv("PSPRECOMP_NET_IMPAIR"); text && *text) {
        const auto impairment = net::parse_impairment(text);
        if (!impairment) {
            std::cerr << "[network] PSPRECOMP_NET_IMPAIR must be delay[,jitter[,loss%]], not '" << text << "'\n";
        } else if (impairment->delay_ms || impairment->jitter_ms || impairment->loss > 0) {
            socket = net::open_impaired(std::move(socket), *impairment);
            std::cerr << "[network] Diagnostics: datagrams leave " << impairment->delay_ms << " ms late (+-"
                      << impairment->jitter_ms << " ms), " << impairment->loss * 100.0 << "% are lost\n";
        }
    }
    net::Mac mac{};
    std::copy(g_network_config.mac.begin(), g_network_config.mac.end(), mac.begin());
    g_transport = std::make_unique<net::Transport>(std::move(socket), mac);
    g_transport->start();
    std::cerr << "[network] Transport on UDP port " << g_transport->local_port() << '\n';
    return g_transport.get();
}

bool network_guest_sockets_open() {
    std::lock_guard lock(g_net_mutex);
    return !g_pdp_sockets.empty();
}

void init_network_config(const std::filesystem::path &ini_path, int argc, char **argv) {
    g_network_config = NetworkConfiguration{};
    g_network_config.lobby_url = default_lobby_url;
    // An automatic identity must differ on separate PCs as well as local instances.
    std::random_device random;
    for (auto &byte : g_network_config.mac) byte = static_cast<std::uint8_t>(random());
    g_network_config.mac[0] = (g_network_config.mac[0] & 0xFCu) | 0x02u;
    const auto set_mac = [](const std::string &text) {
        if (const auto mac = net::parse_mac(text)) std::copy(mac->begin(), mac->end(), g_network_config.mac.begin());
    };
    // 1. Read defaults from INI file if present
    std::ifstream file(ini_path);
    if (file.is_open()) {
        std::string line;
        bool in_net_section = false;
        while (std::getline(file, line)) {
            auto comment = line.find(';');
            if (comment != std::string::npos) line = line.substr(0, comment);
            line = trimmed(line);
            if (line.empty()) continue;
            if (line.starts_with("[") && line.ends_with("]")) {
                in_net_section = lowercase(line) == "[network]";
                continue;
            }
            const auto eq = line.find('=');
            if (!in_net_section || eq == std::string::npos) continue;
            const std::string key = lowercase(trimmed(line.substr(0, eq)));
            const std::string val = trimmed(line.substr(eq + 1));
            if (key == "enabled") g_network_config.enabled = truthy(val);
            else if (key == "online") g_network_config.online = truthy(val);
            else if (key == "nickname") g_network_config.nickname = val;
            else if (key == "mac") { if (lowercase(val) != "auto") set_mac(val); }
            else if (key == "lobby") {
                // Empty keeps the built-in address.
                if (!val.empty()) g_network_config.lobby_url = lowercase(val) == "off" ? std::string{} : val;
            }
            else if (key == "localdiscovery") g_network_config.local_discovery = truthy(val);
            else if (key == "port") {
                const int port = std::atoi(val.c_str());
                if (port > 0 && port < 65536) g_network_config.game_port = static_cast<std::uint16_t>(port);
            } else if (key == "stun") {
                g_network_config.stun_servers.clear();
                std::istringstream list(val);
                for (std::string item; std::getline(list, item, ',');)
                    if (item = trimmed(item); !item.empty() && lowercase(item) != "off") g_network_config.stun_servers.push_back(item);
            } else if (key == "portoffset") {
                g_network_config.port_offset = static_cast<std::uint16_t>(std::atoi(val.c_str()));
            }
        }
    }

    // 2. Override via CLI arguments
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--port-offset" && i + 1 < argc) {
            g_network_config.port_offset = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--mac" && i + 1 < argc) {
            set_mac(argv[++i]);
        } else if (arg == "--net-nickname" && i + 1 < argc) {
            g_network_config.nickname = argv[++i];
        } else if (arg == "--lobby" && i + 1 < argc) {
            g_network_config.lobby_url = argv[++i];
        } else if (arg == "--lan-only") {
            g_network_config.online = false;
        } else if (arg == "--disable-net") {
            g_network_config.enabled = false;
        }
    }

    if (lobby::Url url; !g_network_config.lobby_url.empty() && !lobby::parse_url(g_network_config.lobby_url, url)) {
        std::cerr << "[network] Ignoring the lobby address '" << g_network_config.lobby_url
                  << "': expected https://host[:port]\n";
        g_network_config.lobby_url.clear();
    }
    g_network_config.nickname = clean_nickname(g_network_config.nickname);
    if (g_network_config.nickname.empty() || g_network_config.nickname == "MajorKusanagi")
        g_network_config.nickname = default_nickname();
    if (unsigned(g_network_config.game_port) + g_network_config.port_offset > 65535u ||
        unsigned(g_network_config.discovery_port) + g_network_config.port_offset > 65535u)
        g_network_config.port_offset = 0;

    std::cerr << "[network] Configured: Enabled=" << g_network_config.enabled
              << " Online=" << g_network_config.online
              << " Nickname='" << g_network_config.nickname
              << "' MAC=" << net::mac_text(g_network_config.mac)
              << " PortOffset=" << g_network_config.port_offset
              << " Lobby=" << (g_network_config.lobby_url.empty() ? "none" : g_network_config.lobby_url) << '\n';
}

void queue_adhocctl_event(std::uint32_t event, std::uint32_t error) {
    std::lock_guard<std::mutex> lock(g_net_mutex);
    g_queued_adhoc_events.push_back({event, error});
    g_adhoc_events_pending.store(true, std::memory_order_release);
}

void trace_network_frame(std::uint64_t vblank, std::uint64_t guest_us) {
    if (auto *out = network_trace())
        *out << 'F' << ',' << trace_time_us() << ',' << vblank << ',' << guest_us << '\n';
}

namespace {

// Playback clocks of the remote players, by controller. Guest thread only.
std::unordered_map<std::uint32_t, ReplicaClock> g_replica_clocks;

struct ReplicaHistory {
    std::uint32_t now{}, step{}, oldest{}, newest{};
    std::uint32_t last_sample{};   // address of the newest sample
};

// The sample history of a remote player controller, when it can be interpolated.
bool read_replica_history(const psprecomp::GuestMemory &memory, std::uint32_t controller, ReplicaHistory &history) {
    if (!memory.contains(controller, 0x50u) || memory.load32(controller) != 0x08A84F30u) return false;
    const auto samples = memory.load32(controller + 0x10u);
    const auto stride = memory.load32(controller + 0x14u);
    const auto count = memory.load32(controller + 0x1cu);
    if (stride != 0x40u || count < 2u || count > 4u || !memory.contains(samples, count * stride)) return false;
    history.last_sample = samples + (count - 1u) * stride;
    history.oldest = memory.load32(samples + 0x24u);
    history.newest = memory.load32(history.last_sample + 0x24u);
    if (static_cast<std::int32_t>(history.newest - history.oldest) < 0) return false;
    // +0x44 is the session clock of this frame, +0x40 that of the previous one.
    history.now = memory.load32(controller + 0x44u);
    const auto step = history.now - memory.load32(controller + 0x40u);
    history.step = step > 0u && step <= 100u ? step : 17u;
    return true;
}

} // namespace

void update_replica_playback_clock(psprecomp::GuestMemory &memory, std::uint32_t controller) {
    ReplicaHistory history{};
    if (!read_replica_history(memory, controller, history)) return;
    // The original adaptive cursor advances at one third speed while its
    // four-sample history fills. That delay persists even on a low-latency LAN.
    // Retain the original interpolation curve, using a bounded session time.
    auto &clock = g_replica_clocks[controller];
    const auto cursor = replica_playback_time(clock, history.now, history.step, history.oldest, history.newest);
    memory.store32(controller + 0x48u, cursor);
    if (auto *out = network_trace())
        *out << 'C' << ',' << trace_time_us() << ',' << controller << ',' << history.now << ',' << cursor << ','
             << clock.delay << ',' << history.oldest << ',' << history.newest << '\n';
}

std::uint32_t replica_keep_history(psprecomp::GuestMemory &memory, std::uint32_t controller, std::uint32_t spent) {
    // `spent` is the game's own finding: no new sample, cursor past the newest one.
    if (!spent) return 0u;
    ReplicaHistory history{};
    const auto clock = g_replica_clocks.find(controller);
    if (clock == g_replica_clocks.end() || !read_replica_history(memory, controller, history)) return spent;
    const bool clear = replica_history_spent(clock->second, history.now, history.newest);
    if (auto *out = network_trace())
        *out << (clear ? 'X' : 'K') << ',' << trace_time_us() << ',' << controller << ',' << history.now << ','
             << history.newest << ',' << std::bit_cast<float>(memory.load32(history.last_sample + 0x20u)) << ','
             << memory.load16(history.last_sample + 0x30u) << ',' << memory.load16(history.last_sample + 0x32u) << '\n';
    return clear ? spent : 0u;
}

void trace_network_player(const psprecomp::GuestMemory &memory, std::uint32_t player,
                          std::uint64_t vblank, std::uint64_t guest_us) {
    auto *out = network_trace();
    if (!out || !network_guest_sockets_open()) return;
    static std::uint32_t previous_player{};
    static std::vector<std::uint32_t> replicas;
    if (player != previous_player || replicas.empty()) {
        replicas.clear();
        const auto &bytes = memory.bytes();
        for (std::size_t offset = 0; offset + 0x94 <= bytes.size(); offset += 4) {
            std::uint32_t vtable;
            std::memcpy(&vtable, bytes.data() + offset, 4);
            if (vtable == 0x08A84C60u) replicas.push_back(0x08000000u + offset);
        }
        previous_player = player;
    }
    const auto now = trace_time_us();
    const auto write_actor = [&](char kind, std::uint32_t address) {
        const auto x = memory.load32(address + 0x40), y = memory.load32(address + 0x44), z = memory.load32(address + 0x48);
        if (kind == 'R' && x == 0 && y == 0 && z == 0) return;
        *out << kind << ',' << now << ',' << vblank << ',' << guest_us << ',' << address << ','
             << std::setprecision(9) << std::bit_cast<float>(x) << ',' << std::bit_cast<float>(y) << ','
             << std::bit_cast<float>(z) << ',' << memory.load32(address + 0x90) << '\n';
    };
    write_actor('L', player);
    for (auto address : replicas)
        if (memory.load32(address) == 0x08A84C60u) write_actor('R', address);
}

// The game called sceNetTerm (it does so between the lobby and a match).
// Guest-visible state goes away; the transport and its peers stay.
static void guest_network_terminated() {
    online_adhoc_disconnected();
    std::lock_guard<std::mutex> lock(g_net_mutex);
    close_guest_sockets_locked();
    if (g_sent_packets || g_received_packets)
        std::cerr << "[network] PDP session: sent=" << g_sent_packets << " received=" << g_received_packets << '\n';
    g_sent_packets = g_received_packets = 0;
    if (auto *out = network_trace()) out->flush();
    g_replica_clocks.clear();
    g_adhoc_handlers.clear();
    g_queued_adhoc_events.clear();
    g_adhoc_events_pending.store(false, std::memory_order_release);
    g_next_handler_id = 1;
    g_adhoc_inited = false;
    g_adhocctl_inited = false;
    g_net_inited = false;
}

void shutdown_network() {
    guest_network_terminated();
    online_shutdown();
    std::unique_ptr<net::Transport> transport;
    {
        std::lock_guard lock(g_transport_mutex);
        transport = std::move(g_transport);
        g_transport_failed = false;
    }
    if (transport) transport->stop();
}

// Forward declare PSPRecomp internal callback queuing from kokaku_profile.cpp
extern void kokaku_enqueue_guest_callback(std::int32_t uid, std::uint32_t function,
                                          std::uint32_t a0, std::uint32_t a1, std::uint32_t a2);

void update_network_events(psprecomp::Runtime &, psprecomp::AllegrexContext &) {
    if (!g_adhoc_events_pending.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_net_mutex);
    while (!g_queued_adhoc_events.empty()) {
        const auto [event, error] = g_queued_adhoc_events.front();
        g_queued_adhoc_events.pop_front();

        for (const auto &[handler_id, handler] : g_adhoc_handlers) {
            std::cerr << "[network] Dispatching Adhocctl Event " << event
                      << " (err=" << error << ") to guest callback="
                      << psprecomp::hex32(handler.function)
                      << " thread=" << handler.thread_uid << '\n';

            kokaku_enqueue_guest_callback(handler.thread_uid, handler.function,
                                          event, error, handler.argument);
        }
    }
    g_adhoc_events_pending.store(false, std::memory_order_release);
}

void install_network_hle(psprecomp::Runtime &runtime) {
    // 1. sceWlanDrv: sceWlanGetSwitchState
    runtime.register_hle("sceWlanDrv", 0xD7763699u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        const std::uint32_t state = g_network_config.enabled ? 1u : 0u;
        ctx.set_gpr(2, state);
    });

    // 2. sceNet: sceNetInit
    runtime.register_hle("sceNet", 0x39AF39A6u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        g_net_inited.store(true);
        std::cerr << "[network] sceNetInit called\n";
        ctx.set_gpr(2, 0u);
    });

    // 3. sceNet: sceNetTerm
    runtime.register_hle("sceNet", 0x281928A9u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        guest_network_terminated();
        std::cerr << "[network] sceNetTerm called\n";
        ctx.set_gpr(2, 0u);
    });

    // 4. sceNet: sceNetGetLocalEtherAddr
    runtime.register_hle("sceNet", 0x0BF0A3AEu, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        const std::uint32_t addr_ptr = ctx.gpr[4];
        if (addr_ptr != 0u && rt.memory().contains(addr_ptr, 6u)) {
            rt.memory().copy_in(addr_ptr, std::span<const std::uint8_t>(g_network_config.mac.data(), 6u));
            std::cerr << "[network] sceNetGetLocalEtherAddr -> " << net::mac_text(g_network_config.mac) << "\n";
            ctx.set_gpr(2, 0u);
        } else {
            ctx.set_gpr(2, 0x80410180u);
        }
    });

    // 5. sceNet: sceNetEtherNtostr
    runtime.register_hle("sceNet", 0x89360950u, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        const std::uint32_t mac_ptr = ctx.gpr[4];
        const std::uint32_t str_ptr = ctx.gpr[5];
        if (mac_ptr != 0u && str_ptr != 0u && rt.memory().contains(mac_ptr, 6u) && rt.memory().contains(str_ptr, 18u)) {
            net::Mac mac{};
            rt.memory().copy_out(mac_ptr, std::span<std::uint8_t>(mac.data(), 6u));
            const std::string formatted = net::mac_text(mac);
            rt.memory().copy_in(str_ptr, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(formatted.c_str()), formatted.size() + 1));
        }
        ctx.set_gpr(2, 0u);
    });

    // 6. sceNetAdhoc: sceNetAdhocInit
    runtime.register_hle("sceNetAdhoc", 0xE1D621D7u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        g_adhoc_inited.store(true);
        std::cerr << "[network] sceNetAdhocInit\n";
        ctx.set_gpr(2, 0u);
    });

    // 7. sceNetAdhoc: sceNetAdhocTerm
    runtime.register_hle("sceNetAdhoc", 0xA62C6F57u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        {
            std::lock_guard<std::mutex> lock(g_net_mutex);
            close_guest_sockets_locked();
        }
        g_adhoc_inited.store(false);
        std::cerr << "[network] sceNetAdhocTerm\n";
        ctx.set_gpr(2, 0u);
    });

    // 8. sceNetAdhocctl: sceNetAdhocctlInit
    runtime.register_hle("sceNetAdhocctl", 0xE26F226Eu, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        g_adhocctl_inited.store(true);
        std::cerr << "[network] sceNetAdhocctlInit\n";
        ctx.set_gpr(2, 0u);
    });

    // 9. sceNetAdhocctl: sceNetAdhocctlTerm
    runtime.register_hle("sceNetAdhocctl", 0x9D689E13u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        online_adhoc_disconnected();
        {
            std::lock_guard<std::mutex> lock(g_net_mutex);
            g_adhoc_handlers.clear();
            g_queued_adhoc_events.clear();
            g_adhoc_events_pending.store(false, std::memory_order_release);
        }
        g_adhocctl_inited.store(false);
        std::cerr << "[network] sceNetAdhocctlTerm\n";
        ctx.set_gpr(2, 0u);
    });

    // 10. sceNetAdhocctl: sceNetAdhocctlAddHandler
    runtime.register_hle("sceNetAdhocctl", 0x20B317A0u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        std::lock_guard<std::mutex> lock(g_net_mutex);
        const std::uint32_t func_ptr = ctx.gpr[4];
        const std::uint32_t arg = ctx.gpr[5];

        const int id = g_next_handler_id++;
        AdhocHandlerRecord rec{};
        rec.function = func_ptr;
        rec.argument = arg;
        rec.thread_uid = static_cast<std::int32_t>(psprecomp::runtime_thread_uid());
        g_adhoc_handlers[id] = rec;

        std::cerr << "[network] sceNetAdhocctlAddHandler id=" << id
                  << " callback=" << psprecomp::hex32(func_ptr)
                  << " arg=" << psprecomp::hex32(arg)
                  << " thread_uid=" << rec.thread_uid << '\n';
        ctx.set_gpr(2, static_cast<std::uint32_t>(id));
    });

    // 11. sceNetAdhocctl: sceNetAdhocctlDelHandler
    runtime.register_hle("sceNetAdhocctl", 0x6402490Bu, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        std::lock_guard<std::mutex> lock(g_net_mutex);
        const int id = static_cast<int>(ctx.gpr[4]);
        g_adhoc_handlers.erase(id);
        std::cerr << "[network] sceNetAdhocctlDelHandler id=" << id << '\n';
        ctx.set_gpr(2, 0u);
    });

    // 12. sceNetAdhocctl: sceNetAdhocctlConnect
    runtime.register_hle("sceNetAdhocctl", 0x0AD043EDu, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        const std::uint32_t group_ptr = ctx.gpr[4];
        std::string group = "DEFAULT";
        if (group_ptr != 0u && rt.memory().contains(group_ptr, 8u)) {
            char gbuf[9]{};
            rt.memory().copy_out(group_ptr, std::span<std::uint8_t>(reinterpret_cast<std::uint8_t *>(gbuf), 8u));
            gbuf[8] = '\0';
            if (gbuf[0] != '\0') group = gbuf;
        }

        std::cerr << "[network] sceNetAdhocctlConnect group='" << group << "'\n";
        online_adhoc_connected(group);

        // Queue ADHOCCTL_EVENT_CONNECT to notify the game that group connection succeeded
        queue_adhocctl_event(ADHOCCTL_EVENT_CONNECT, 0u);
        ctx.set_gpr(2, 0u);
        // The post-import hook may start the callback on this very thread.
        // Its saved continuation must return to the caller, not re-run Connect.
        ctx.pc = ctx.gpr[31];
    });

    // 13. sceNetAdhocctl: sceNetAdhocctlDisconnect
    runtime.register_hle("sceNetAdhocctl", 0x34401D65u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        std::cerr << "[network] sceNetAdhocctlDisconnect\n";
        online_adhoc_disconnected();
        queue_adhocctl_event(ADHOCCTL_EVENT_DISCONNECT, 0u);
        ctx.set_gpr(2, 0u);
        ctx.pc = ctx.gpr[31];
    });

    // 14. sceNetAdhoc: sceNetAdhocPdpCreate
    // Every PDP socket is a virtual port on the shared transport socket.
    runtime.register_hle("sceNetAdhoc", 0x6F92741Bu, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        std::uint16_t psp_port = static_cast<std::uint16_t>(ctx.gpr[5]);
        const std::uint32_t bufsize = ctx.gpr[6];
        const std::uint32_t flags = ctx.gpr[7];
        if (!rt.memory().contains(ctx.gpr[4], 6)) { ctx.set_gpr(2, 0x80410702u); return; }
        if (bufsize == 0 || bufsize > 16 * 1024 * 1024) { ctx.set_gpr(2, 0x80410704u); return; }
        net::Transport *transport = network_transport();
        if (!transport) {
            std::cerr << "[network] PdpCreate: no transport\n";
            ctx.set_gpr(2, SCE_NET_ADHOC_ERROR_PORT_IN_USE);
            return;
        }
        std::lock_guard<std::mutex> lock(g_net_mutex);
        if (psp_port == 0) {
            // The PSP assigns a free port; keep clear of the game's fixed ones.
            for (std::uint16_t candidate = 49152; candidate != 0 && psp_port == 0; ++candidate)
                if (!transport->pdp_is_open(candidate)) psp_port = candidate;
        }
        if (psp_port == 0 || !transport->pdp_open(psp_port, bufsize)) {
            std::cerr << "[network] PdpCreate: port=" << psp_port << " is in use\n";
            ctx.set_gpr(2, SCE_NET_ADHOC_ERROR_PORT_IN_USE);
            return;
        }
        const int id = g_next_socket_id++;
        g_pdp_sockets[id] = PdpSocketRecord{psp_port, bufsize, flags};
        std::cerr << "[network] sceNetAdhocPdpCreate id=" << id
                  << " psp_port=" << psp_port
                  << " host_port=" << transport->local_port()
                  << " bufsize=" << bufsize << '\n';
        ctx.set_gpr(2, static_cast<std::uint32_t>(id));
    });

    // 15. sceNetAdhoc: sceNetAdhocPdpDelete
    runtime.register_hle("sceNetAdhoc", 0x7F27BB5Eu, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        std::lock_guard<std::mutex> lock(g_net_mutex);
        const int id = static_cast<int>(ctx.gpr[4]);
        const auto it = g_pdp_sockets.find(id);
        if (it != g_pdp_sockets.end()) {
            if (g_transport) g_transport->pdp_close(it->second.port);
            g_pdp_sockets.erase(it);
            std::cerr << "[network] sceNetAdhocPdpDelete id=" << id << '\n';
            ctx.set_gpr(2, 0u);
        } else {
            ctx.set_gpr(2, SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID);
        }
    });

    // 16. sceNetAdhoc: sceNetAdhocPdpSend (EABI: the fifth argument is in t0)
    runtime.register_hle("sceNetAdhoc", 0xABED3790u, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        const int id = static_cast<int>(ctx.gpr[4]);
        const auto mac_ptr = ctx.gpr[5], data_ptr = ctx.gpr[7], len = ctx.gpr[8];
        const auto dport = static_cast<std::uint16_t>(ctx.gpr[6]);
        std::lock_guard<std::mutex> lock(g_net_mutex);
        const auto it = g_pdp_sockets.find(id);
        if (it == g_pdp_sockets.end() || !g_transport) { ctx.set_gpr(2, SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID); return; }
        if (!rt.memory().contains(mac_ptr, 6) || !rt.memory().contains(data_ptr, len) ||
            len == 0 || len > 65475 || dport == 0) { ctx.set_gpr(2, 0x80410705u); return; }
        net::Mac dest{};
        rt.memory().copy_out(mac_ptr, dest);
        std::vector<std::uint8_t> payload(len);
        rt.memory().copy_out(data_ptr, payload);
        const auto sport = it->second.port;
        dump_payload('T', dest.data(), sport, dport, payload.data(), len);
        const auto result = g_transport->pdp_send(sport, dest, dport, payload);
        const bool broadcast = dest == net::broadcast_mac;
        // PSP broadcast with no other stations succeeds; unknown unicast does not.
        ctx.set_gpr(2, result == net::Transport::SendResult::Failed ? SCE_NET_ADHOC_ERROR_WOULD_BLOCK
                       : result == net::Transport::SendResult::NoRoute && !broadcast ? 0x80410702u : 0u);
        if (result != net::Transport::SendResult::Sent) return;
        trace_packet('T', g_network_config.mac.data(), sport, dport, payload);
        if (++g_sent_packets <= 4 || g_sent_packets % 1000 == 0)
            std::cerr << "[network] PDP sent=" << g_sent_packets << " bytes=" << len << " sport=" << sport << " dport=" << dport << '\n';
    });

    // 17. sceNetAdhoc: sceNetAdhocPdpRecv
    runtime.register_hle("sceNetAdhoc", 0xDFE53E03u, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        const int id = static_cast<int>(ctx.gpr[4]);
        const auto mac_ptr = ctx.gpr[5], port_ptr = ctx.gpr[6], data_ptr = ctx.gpr[7], len_ptr = ctx.gpr[8];
        std::lock_guard<std::mutex> lock(g_net_mutex);
        const auto it = g_pdp_sockets.find(id);
        if (it == g_pdp_sockets.end() || !g_transport) { ctx.set_gpr(2, SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID); return; }
        if (!rt.memory().contains(mac_ptr, 6) || !rt.memory().contains(port_ptr, 2) ||
            !rt.memory().contains(len_ptr, 4)) { ctx.set_gpr(2, 0x80410711u); return; }
        const auto capacity = rt.memory().load32(len_ptr);
        if (capacity == 0 || !rt.memory().contains(data_ptr, capacity)) {
            ctx.set_gpr(2, 0x80410705u); return;
        }
        const auto pending = g_transport->pdp_peek(it->second.port);
        if (!pending) { ctx.set_gpr(2, SCE_NET_ADHOC_ERROR_WOULD_BLOCK); return; }
        if (*pending > capacity) {
            rt.memory().store32(len_ptr, static_cast<std::uint32_t>(*pending));
            ctx.set_gpr(2, 0x80410705u);
            return; // Preserve the datagram for a larger-buffer retry.
        }
        net::ReceivedDatagram datagram{};
        if (!g_transport->pdp_receive(it->second.port, datagram)) { ctx.set_gpr(2, SCE_NET_ADHOC_ERROR_WOULD_BLOCK); return; }
        const auto len = static_cast<std::uint32_t>(datagram.payload.size());
        rt.memory().copy_in(mac_ptr, datagram.source);
        rt.memory().store16(port_ptr, datagram.source_port);
        rt.memory().copy_in(data_ptr, datagram.payload);
        rt.memory().store32(len_ptr, len);
        ctx.set_gpr(2, 0u);
        dump_payload('R', datagram.source.data(), datagram.source_port, it->second.port, datagram.payload.data(), len);
        trace_packet('P', datagram.source.data(), datagram.source_port, it->second.port, datagram.payload);
        if (++g_received_packets <= 4 || g_received_packets % 1000 == 0)
            std::cerr << "[network] PDP received=" << g_received_packets << " bytes=" << len
                      << " source=" << net::mac_text(datagram.source) << ':' << datagram.source_port << '\n';
    });

    // 18. sceNetAdhoc: sceNetAdhocGetPdpStat
    runtime.register_hle("sceNetAdhoc", 0xC7C1FC57u, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        std::lock_guard<std::mutex> lock(g_net_mutex);
        const std::uint32_t len_ptr = ctx.gpr[4];
        const std::uint32_t buf_ptr = ctx.gpr[5];

        if (len_ptr == 0u || !rt.memory().contains(len_ptr, 4u)) {
            ctx.set_gpr(2, 0x80410705u);
            return;
        }

        const std::uint32_t count = static_cast<std::uint32_t>(g_pdp_sockets.size());
        const std::uint32_t required_bytes = count * sizeof(SceNetAdhocPdpStat);

        if (buf_ptr == 0u) {
            rt.memory().store32(len_ptr, required_bytes);
            ctx.set_gpr(2, 0u);
            return;
        }

        const auto capacity = rt.memory().load32(len_ptr);
        const auto written_count = std::min<std::uint32_t>(count, capacity / sizeof(SceNetAdhocPdpStat));
        if (!rt.memory().contains(buf_ptr, written_count * sizeof(SceNetAdhocPdpStat))) {
            ctx.set_gpr(2, 0x80410711u); return;
        }
        std::uint32_t offset = 0;
        int idx = 0;
        for (const auto &[id, sock] : g_pdp_sockets) {
            if (idx == static_cast<int>(written_count)) break;
            SceNetAdhocPdpStat stat{};
            stat.next = (idx + 1 < static_cast<int>(written_count)) ? (buf_ptr + offset + sizeof(SceNetAdhocPdpStat)) : 0u;
            stat.id = id;
            std::memcpy(stat.laddr, g_network_config.mac.data(), 6);
            stat.lport = sock.port;
            stat.rcv_sb_cc = g_transport ? static_cast<std::uint32_t>(g_transport->pdp_queued_bytes(sock.port)) : 0u;

            if (rt.memory().contains(buf_ptr + offset, sizeof(SceNetAdhocPdpStat))) {
                rt.memory().copy_in(buf_ptr + offset, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(&stat), sizeof(stat)));
            }
            offset += sizeof(SceNetAdhocPdpStat);
            ++idx;
        }

        rt.memory().store32(len_ptr, offset);
        ctx.set_gpr(2, 0u);
    });

    // 19. Clean stubs for Apctl / Inet / Resolver / ApDialogDummy
    auto net_stub_ok = [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); };
    runtime.register_hle("sceNetApDialogDummy", 0xF213BE65u, net_stub_ok);
    runtime.register_hle("sceNetApctl", 0xB3EDD0ECu, net_stub_ok);
    runtime.register_hle("sceNetApctl", 0x2BEFDF23u, net_stub_ok);
    runtime.register_hle("sceNetApctl", 0x5963991Bu, net_stub_ok);
    runtime.register_hle("sceNetApctl", 0x24FE91A1u, net_stub_ok);
    runtime.register_hle("sceNetApctl", 0x5DEAC81Bu, net_stub_ok);
    runtime.register_hle("sceNetInet", 0xA9ED66B9u, net_stub_ok);
    runtime.register_hle("sceNetResolver", 0x6138194Au, net_stub_ok);
}


bool run_net_self_tests(std::string &error) {
    const auto saved = g_network_config;
    auto require = [](bool ok, const char *message) { if (!ok) throw std::runtime_error(message); };
    try {
        // Remote player playback: one call per frame with the session clock and
        // both ends of the sample history.
        {
            ReplicaClock clock{};
            require(replica_playback_time(clock, 1000u, 17u, 850u, 950u) == 920u && clock.delay == 80u,
                    "Replica playback lost its bounded delay");
            require(replica_playback_time(clock, 1017u, 17u, 940u, 950u) == 940u, "Replica playback precedes available history");
            ReplicaClock gap{};
            require(replica_playback_time(gap, 3000u, 17u, 850u, 950u) == 950u, "Packet gap extrapolated beyond received history");
            ReplicaClock wrap{};
            require(replica_playback_time(wrap, 40u, 17u, 0xffffff00u, 20u) == 0xffffffd8u, "Replica timestamp wrap");
        }
        {
            // A player walking: a sample every 50 ms, each with its own delay on
            // the way. Frames are 17 ms apart and the game keeps four samples.
            struct Result {
                std::uint32_t largest_delay{}, last_delay{}, held{}, backwards{}, beyond{}, off_target{}, cleared{};
            };
            constexpr std::uint32_t never = 0xffffffffu;   // a sample lost on the way
            // `adaptive` false keeps the distance at its local network value.
            const auto walk = [](std::uint32_t start, std::uint32_t samples, bool adaptive, const auto &lateness) {
                ReplicaClock clock{};
                Result result{};
                std::deque<std::uint32_t> history;
                std::uint32_t next = 0, previous = start - 17u, cursor = 0;
                bool playing = false;
                for (std::uint32_t now = start; next < samples || now - history.back() < 200u; now += 17u) {
                    bool arrived = false;
                    while (next < samples) {
                        const auto late = lateness(next);
                        if (late != never) {
                            // In order, as on one route: a late sample holds back the ones behind it.
                            if (static_cast<std::int32_t>(now - (start + next * 50u + late)) < 0) break;
                            history.push_back(start + next * 50u);
                            if (history.size() > 4u) history.pop_front();
                            arrived = true;
                        }
                        ++next;
                    }
                    if (history.size() >= 2u) {
                        // What the game asks before interpolating when nothing new came
                        // and its cursor, one step further by then, is past the newest sample.
                        if (!arrived && playing && replica_later(cursor + (now - previous), history.back()) &&
                            replica_history_spent(clock, now, history.back())) ++result.cleared;
                        if (!adaptive) clock.delay = replica_delay_min;
                        const auto time = replica_playback_time(clock, now, now - previous, history.front(), history.back());
                        if (playing && replica_later(cursor, time)) ++result.backwards;
                        if (replica_later(time, history.back()) || replica_later(history.front(), time)) ++result.beyond;
                        if (time == history.back() && next < samples) ++result.held;
                        if (time != now - clock.delay && time != history.front() && time != history.back() && time != cursor)
                            ++result.off_target;
                        result.largest_delay = std::max(result.largest_delay, clock.delay);
                        result.last_delay = clock.delay;
                        cursor = time;
                        playing = true;
                    }
                    previous = now;
                }
                return result;
            };
            // As on a local network: every sample 10 ms after it was taken.
            const auto local = walk(5'000u, 200u, true, [](std::uint32_t) { return 10u; });
            require(local.largest_delay == replica_delay_min && local.held == 0u && local.backwards == 0u && local.beyond == 0u &&
                    local.off_target == 0u && local.cleared == 0u, "Regular samples changed the local network playback");
            // A constant delay does not show in the session clock; a little more
            // lateness, and the clock wrapping on the way, change nothing either.
            const auto steady = walk(0xfffff000u, 200u, true, [](std::uint32_t) { return 25u; });
            require(steady.largest_delay == replica_delay_min && steady.held == 0u && steady.backwards == 0u && steady.beyond == 0u,
                    "A steady connection changed the playback, or the clock wrap did");
            // One sample 70 ms late: the player waits for it without the history
            // being cleared, then plays a little further behind and recovers.
            const auto late = walk(5'000u, 400u, true, [](std::uint32_t index) { return index == 40u ? 80u : 10u; });
            require(late.held >= 1u && late.held <= 6u && late.cleared == 0u && late.backwards == 0u && late.beyond == 0u,
                    "A late sample cleared the history or moved the player back");
            require(late.largest_delay > 100u && late.largest_delay <= 150u && late.last_delay == replica_delay_min,
                    "A late sample did not widen the playback distance for a while");
            // One sample lost: the next one comes 100 ms after the previous.
            const auto single = walk(5'000u, 400u, true, [](std::uint32_t index) { return index == 40u ? never : 10u; });
            require(single.held >= 1u && single.held <= 5u && single.cleared == 0u && single.backwards == 0u &&
                    single.largest_delay > 90u && single.largest_delay <= 125u, "A lost sample was not ridden out");
            // An uneven connection: the distance widens until samples stop being
            // late, where the fixed distance keeps running out of them.
            const auto jitter = [](std::uint32_t index) { return 10u + (index * 37u) % 41u; };
            const auto uneven = walk(5'000u, 400u, true, jitter), fixed = walk(5'000u, 400u, false, jitter);
            require(uneven.cleared == 0u && uneven.backwards == 0u && uneven.beyond == 0u &&
                    uneven.largest_delay > replica_delay_min && uneven.largest_delay <= 140u,
                    "Uneven arrival was not absorbed by a wider playback distance");
            require(fixed.held >= 40u && uneven.held * 4u < fixed.held, "A wider playback distance did not reduce the waiting");
            const auto settled = walk(5'000u, 400u, true, [&](std::uint32_t index) { return index < 60u ? jitter(index) : 20u; });
            require(settled.largest_delay > replica_delay_min && settled.last_delay == replica_delay_min,
                    "The playback distance did not shrink back");
            // Ten samples in a row lost: the history is given up, as for a player
            // who stopped, and nothing else goes wrong.
            const auto outage = walk(5'000u, 100u, true, [](std::uint32_t index) { return index >= 50u && index < 60u ? never : 10u; });
            require(outage.cleared >= 1u && outage.largest_delay == replica_delay_min && outage.backwards == 0u && outage.beyond == 0u,
                    "A long outage was not handled like a stop");
        }
        {
            // A player who stopped: the history is given up after a moment.
            ReplicaClock clock{};
            replica_playback_time(clock, 1000u, 17u, 850u, 950u);
            require(!replica_history_spent(clock, 1040u, 950u) && !replica_history_spent(clock, 950u + 80u + replica_hold_ms, 950u),
                    "A history that just ran out was cleared at once");
            require(replica_history_spent(clock, 951u + 80u + replica_hold_ms, 950u) && replica_history_spent(ReplicaClock{}, 1000u, 950u),
                    "A history that ran out long ago was kept");
            // Samples half a second apart are not the cadence of a walk.
            ReplicaClock idle{};
            replica_playback_time(idle, 1000u, 17u, 400u, 900u);
            replica_playback_time(idle, 1500u, 17u, 900u, 1400u);
            require(idle.delay == replica_delay_min, "Idle refreshes widened the playback distance");
            // Timestamps that go back belong to another history, and so does
            // whatever comes after a long silence.
            ReplicaClock again{};
            replica_playback_time(again, 1000u, 17u, 850u, 950u);
            again.delay = 200u;
            require(replica_playback_time(again, 1017u, 17u, 300u, 400u) == 400u && again.delay == replica_delay_min,
                    "A new history inherited the old playback distance");
            ReplicaClock silent{};
            replica_playback_time(silent, 1000u, 17u, 850u, 950u);
            silent.delay = 200u;
            replica_playback_time(silent, 2500u, 17u, 2300u, 2400u);
            require(silent.delay == replica_delay_min, "The playback distance survived a long silence");
            // However late a sample is, the distance has a ceiling.
            ReplicaClock capped{};
            replica_playback_time(capped, 1000u, 17u, 300u, 350u);
            replica_playback_time(capped, 1017u, 17u, 350u, 400u);
            require(capped.delay == replica_delay_max, "The playback distance has no ceiling");
            // A distance that widens by more than the clock advanced leaves the
            // player where it is: it never walks backwards along its path.
            ReplicaClock waiting{};
            require(replica_playback_time(waiting, 1030u, 17u, 850u, 950u) == 950u, "Playback did not wait at the newest sample");
            require(replica_playback_time(waiting, 1040u, 1u, 850u, 1000u) == 950u && waiting.delay > replica_delay_min,
                    "A wider playback distance moved the player back");
        }
        {
            psprecomp::GuestMemory memory;
            constexpr auto controller = 0x08810000u, samples = 0x08811000u;
            memory.store32(controller, 0x08A84F30u);
            memory.store32(controller + 0x10u, samples);
            memory.store32(controller + 0x14u, 0x40u);
            memory.store32(controller + 0x1cu, 2u);
            memory.store32(controller + 0x40u, 983u);
            memory.store32(controller + 0x44u, 1000u);
            memory.store32(samples + 0x24u, 850u);
            memory.store32(samples + 0x64u, 950u);
            update_replica_playback_clock(memory, controller);
            require(memory.load32(controller + 0x48u) == 920u, "Remote controller clock was not updated");
            // The game finds its cursor past the newest sample: not cleared yet, then cleared.
            require(replica_keep_history(memory, controller, 0u) == 0u, "A history in use was reported as spent");
            memory.store32(controller + 0x40u, 1040u);
            memory.store32(controller + 0x44u, 1057u);
            require(replica_keep_history(memory, controller, 1u) == 0u, "A late sample cleared the remote history");
            memory.store32(controller + 0x44u, 951u + 80u + replica_hold_ms);
            require(replica_keep_history(memory, controller, 1u) == 1u, "A stopped player kept its history");
            require(replica_keep_history(memory, controller + 0x100u, 1u) == 1u, "An unknown controller overrode the game");
            memory.store32(controller + 0x1cu, 5u);
            memory.store32(controller + 0x44u, 2000u);
            update_replica_playback_clock(memory, controller);
            require(memory.load32(controller + 0x48u) == 920u, "Invalid history count was accepted");
            require(replica_keep_history(memory, controller, 1u) == 1u, "An unreadable history overrode the game");
        }
        shutdown_network();

        // Configuration: defaults, INI keys, command line.
        {
            const auto directory = std::filesystem::temp_directory_path() /
                ("kokaku-net-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directory(directory);
            const auto ini = directory / "net.ini";
            char program[] = "test", lan[] = "--lan-only", lobby[] = "--lobby", url[] = "http://127.0.0.1:1";
            char *plain[] = {program}, *flags[] = {program, lan, lobby, url};
            init_network_config(directory / "missing.ini", 1, plain);
            require(g_network_config.enabled && g_network_config.online && g_network_config.game_port == 3658 &&
                    g_network_config.port_offset == 0 && g_network_config.stun_servers.size() == 2 &&
                    g_network_config.nickname.starts_with("Agent-") && g_network_config.nickname.size() == 10 &&
                    (g_network_config.mac[0] & 3u) == 2u, "Default network configuration");
            const std::string generated = g_network_config.nickname;
            init_network_config(directory / "missing.ini", 1, plain);
            require(g_network_config.nickname == generated, "Default nickname is not stable");
            { std::ofstream out(ini); out << "[Display]\nNickname=Wrong\n[network]\nenabled = TRUE\nOnline=false ; note\n"
                  "Nickname=  Togusa\x01\tX  \nMAC=02:AA:bb:cc:dd:ee\nPortOffset=1000\nPort=4000\n"
                  "Lobby=https://lobby.example\nStun=a.example:3478, off ,b.example:19302\nMode=LAN\n"; }
            init_network_config(ini, 1, plain);
            require(g_network_config.enabled && !g_network_config.online && g_network_config.nickname == "TogusaX" &&
                    net::mac_text(g_network_config.mac) == "02:aa:bb:cc:dd:ee" && g_network_config.port_offset == 1000 &&
                    g_network_config.game_port == 4000 && g_network_config.lobby_url == "https://lobby.example" &&
                    g_network_config.stun_servers == std::vector<std::string>({"a.example:3478", "b.example:19302"}),
                    "INI network keys");
            { std::ofstream out(ini); out << "[Network]\nNickname=MajorKusanagi\nMAC=auto\nLobby=off\nPortOffset=65000\nMAC=junk\n"; }
            init_network_config(ini, 4, flags);
            require(!g_network_config.online && g_network_config.lobby_url == "http://127.0.0.1:1" &&
                    g_network_config.nickname == generated && g_network_config.port_offset == 0 &&
                    (g_network_config.mac[0] & 3u) == 2u, "Command line or fallbacks");
            init_network_config(ini, 1, plain);
            require(g_network_config.lobby_url.empty(), "Lobby=off did not disable the lobby");
            { std::ofstream out(ini); out << "[Network]\nLobby=\n"; }
            init_network_config(ini, 1, plain);
            require(g_network_config.lobby_url == default_lobby_url, "An empty Lobby key did not keep the built-in address");
            { std::ofstream out(ini); out << "[Network]\nLobby=lobby.example without scheme\n"; }
            init_network_config(ini, 1, plain);
            require(g_network_config.lobby_url.empty(), "An unusable lobby address was kept");
            std::filesystem::remove_all(directory);
        }

        g_network_config = NetworkConfiguration{};
        g_network_config.online = false;
        g_network_config.port_offset = 1000;
        net::Mac mac{};
        require(net::parse_mac("02:aa:bb:cc:dd:ee") && !net::parse_mac("102:00:00:00:00:01") &&
                !net::parse_mac("02:00:00:00:00:01junk"), "MAC parser accepted invalid identity");
        psprecomp::Runtime rt;
        install_network_hle(rt);
        constexpr std::uint32_t base = 0x08810000u;
        auto call = [&](const char *lib, std::uint32_t nid, std::initializer_list<std::uint32_t> args) {
            psprecomp::AllegrexContext ctx{};
            unsigned reg = 4;
            for (auto value : args) ctx.gpr[reg++] = value;
            rt.invoke_import(lib, nid, ctx);
            require(!rt.stopped(), "HLE import stopped the runtime");
            return ctx.gpr[2];
        };
        require(call("sceWlanDrv", 0xD7763699u, {}) == 1, "Enabled WLAN");
        g_network_config.enabled = false;
        require(call("sceWlanDrv", 0xD7763699u, {}) == 0, "Disabled WLAN");
        require(call("sceNetAdhoc", 0x6F92741Bu, {base, 0, 65523, 0}) == SCE_NET_ADHOC_ERROR_PORT_IN_USE && !network_transport(),
                "A disabled network opened a socket");
        g_network_config.enabled = true;
        call("sceNet", 0x39AF39A6u, {});
        call("sceNetAdhoc", 0xE1D621D7u, {});
        call("sceNet", 0x0BF0A3AEu, {base});
        rt.memory().copy_out(base, mac);
        require(std::equal(mac.begin(), mac.end(), g_network_config.mac.begin()), "Local MAC was not reported");
        require(!network_guest_sockets_open(), "Guest sockets open before PdpCreate");
        auto s1 = call("sceNetAdhoc", 0x6F92741Bu, {base, 0, 65523, 0});
        auto s2 = call("sceNetAdhoc", 0x6F92741Bu, {base, 0, 65523, 0});
        require(s1 < 0x80000000u && s2 < 0x80000000u && s1 != s2 && network_guest_sockets_open(), "HLE socket creation");
        const auto p1 = g_pdp_sockets.at(s1).port, p2 = g_pdp_sockets.at(s2).port;
        require(p1 != p2 && p1 >= 49152 && p2 >= 49152, "Automatic PDP ports");
        require(call("sceNetAdhoc", 0x6F92741Bu, {base, p1, 4096, 0}) == SCE_NET_ADHOC_ERROR_PORT_IN_USE, "Duplicate port allowed");
        require(call("sceNetAdhoc", 0x6F92741Bu, {base, 3658, 0, 0}) == 0x80410704u, "Zero buffer accepted");
        const auto fixed = call("sceNetAdhoc", 0x6F92741Bu, {base, 3658, 65523, 0});
        require(fixed < 0x80000000u && g_pdp_sockets.at(fixed).port == 3658, "Fixed PDP port");
        const std::array<std::uint8_t,5> data{0, 1, 127, 128, 255};
        rt.memory().copy_in(base+64, data);
        // A datagram to a port nobody has open is dropped, not an error.
        const auto closed = call("sceNetAdhoc", 0x6F92741Bu, {base, 0, 4096, 0});
        const auto closed_port = g_pdp_sockets.at(closed).port;
        require(call("sceNetAdhoc", 0x7F27BB5Eu, {closed, 0}) == 0 &&
                call("sceNetAdhoc", 0x7F27BB5Eu, {closed, 0}) == SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID, "PDP delete");
        require(call("sceNetAdhoc", 0xABED3790u, {s2,base,closed_port,base+64,5,0,1}) == 0, "Unreachable PDP send");
        require(call("sceNetAdhoc", 0xABED3790u, {s1,base,p2,base+64,5,0,1}) == 0, "HLE PDP send EABI");
        require(network_transport()->pdp_peek(p2) == 5u, "HLE packet not delivered");
        rt.memory().store32(base+32, 1);
        require(call("sceNetAdhoc", 0xDFE53E03u, {s2,base+16,base+24,base+128,base+32,0,1}) == 0x80410705u &&
                rt.memory().load32(base+32) == data.size(), "Short receive did not retain packet");
        rt.memory().store32(base+256, 60);
        require(call("sceNetAdhoc", 0xC7C1FC57u, {base+256,base+256+4}) == 0 && rt.memory().load32(base+256) == 60,
                "PDP stat with three sockets");
        bool counted = false;
        for (std::uint32_t entry = base+260; entry < base+320; entry += 20)
            counted |= rt.memory().load16(entry+14) == p2 && rt.memory().load32(entry+16) == 5;
        require(counted, "PDP stat did not report the queued bytes");
        rt.memory().store32(base+32, 32);
        require(call("sceNetAdhoc", 0xDFE53E03u, {s2,base+16,base+24,base+128,base+32,0,1}) == 0, "HLE PDP receive EABI");
        std::array<std::uint8_t,5> received{};
        rt.memory().copy_out(base+128, received);
        std::array<std::uint8_t,6> received_mac{};
        rt.memory().copy_out(base+16, received_mac);
        require(received == data && received_mac == g_network_config.mac && rt.memory().load16(base+24) == p1 &&
                rt.memory().load32(base+32) == data.size(), "HLE payload or source identity/port incorrect");
        require(call("sceNetAdhoc", 0xDFE53E03u, {s2,base+16,base+24,base+128,base+32,0,1}) == SCE_NET_ADHOC_ERROR_WOULD_BLOCK, "Empty socket did not return WOULD_BLOCK");
        require(call("sceNetAdhoc", 0xABED3790u, {s1,base,p2,base+64,0xffffffffu,0,1}) == 0x80410705u, "Invalid send size accepted");
        require(call("sceNetAdhoc", 0xABED3790u, {999,base,p2,base+64,5,0,1}) == SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID &&
                call("sceNetAdhoc", 0xDFE53E03u, {999,base+16,base+24,base+128,base+32,0,1}) == SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID,
                "Unknown socket id accepted");
        // Unknown unicast fails, broadcast to an empty network succeeds.
        const std::array<std::uint8_t,6> nobody{2, 9, 9, 9, 9, 9}, everyone{255, 255, 255, 255, 255, 255};
        rt.memory().copy_in(base+96, nobody);
        rt.memory().copy_in(base+104, everyone);
        require(call("sceNetAdhoc", 0xABED3790u, {s1,base+96,p2,base+64,5,0,1}) == 0x80410702u &&
                call("sceNetAdhoc", 0xABED3790u, {s1,base+104,p2,base+64,5,0,1}) == 0, "Destinations without a route");
        rt.memory().store32(base+256, 20);
        rt.memory().store32(base+280, 0x12345678u);
        require(call("sceNetAdhoc", 0xC7C1FC57u, {base+256,base+256+4}) == 0, "PDP stat");
        // Only one 20-byte entry fits; no next pointer outside caller capacity.
        require(rt.memory().load32(base+260) == 0 && rt.memory().load32(base+256) == 20 &&
                rt.memory().load32(base+280) == 0x12345678u, "PDP stat exceeded caller capacity");
        // The game stops its network between lobby and match; the transport
        // (and with it every NAT mapping) has to outlive that.
        net::Transport *before = network_transport();
        const auto host_port = before->local_port();
        call("sceNetAdhoc", 0xA62C6F57u, {});
        require(g_pdp_sockets.empty() && before->pdp_open_count() == 0, "AdhocTerm leaked sockets");
        call("sceNet", 0x281928A9u, {});
        require(network_transport() == before && before->local_port() == host_port && !network_guest_sockets_open(),
                "sceNetTerm destroyed the transport");
        const auto again = call("sceNetAdhoc", 0x6F92741Bu, {base, 3658, 65523, 0});
        require(again < 0x80000000u && g_pdp_sockets.at(again).port == 3658, "PDP port not reusable after restart");
        shutdown_network();
        require(g_pdp_sockets.empty(), "Shutdown leaked sockets");
        g_network_config = saved;
        std::cerr << "[self-test] Network HLE EABI, identity, virtual ports, bounds and lifecycle PASSED\n";
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        shutdown_network();
        g_network_config = saved;
        return false;
    }
}

} // namespace kokaku
