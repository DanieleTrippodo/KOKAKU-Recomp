#include "kokaku_online.hpp"
#include "kokaku_lobby.hpp"
#include "kokaku_net.hpp"
#include "kokaku_transport.hpp"
#include "psprecomp/common.hpp"
#include "vcs_runtime_log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace kokaku {
namespace {

// ULUS10020 multiplayer objects.
constexpr std::uint32_t dme_pointer = 0x08ac7358u;        // CKoDmeP2P *
constexpr std::uint32_t dme_thread_pointer = 0x08ac7360u; // CDmeTh *
constexpr std::uint32_t session_pointer = 0x08aa8a3cu;
constexpr std::uint32_t dme_app_name = 0x38u;             // "Kokaku PSP Lobby", "...LobbyEnd", "Kokaku PSP Game..."
constexpr std::uint32_t dme_room_state = 0x5030u;         // 12 in a room, 9-10 joined but not confirmed
constexpr std::uint32_t thread_active = 0xb0u;
constexpr std::uint32_t session_server = 0xd7cu;
constexpr std::uint32_t session_rules = 0x1094u;          // seven 32-bit room rules
constexpr std::size_t rule_count = 7;

constexpr std::uint32_t button_up = 0x10u, button_down = 0x40u, button_refresh = 0x1000u, button_confirm = 0x4000u;
// What the search page returns while it stays on screen; anything else is the
// page that replaces it (18: room found, 11: none found, the previous one on Back).
constexpr std::uint32_t page_stay = 0xffffffffu;
// The search and Create pages call their hooks once per game frame. Measured at
// 30 and at 60 FPS, the display counter advances at most 3 times between two
// calls, and it does not advance at all while the whole game is stalled. This
// much silence therefore means the page is gone without having said so.
constexpr std::uint64_t hook_silence_vblanks = 30;

constexpr int max_players = 6;
constexpr std::uint64_t lan_fresh_ms = 3'000;
constexpr std::uint64_t lobby_answer_ms = 8'000;
constexpr std::uint64_t join_timeout_ms = 12'000;
constexpr std::uint64_t list_interval_ms = 3'000;
constexpr std::uint64_t idle_linger_ms = 30'000;
constexpr std::uint64_t message_ms = 5'000;
// A member the list does not know (any more): asked again soon a few times,
// then at long intervals for as long as the room lasts.
constexpr std::uint64_t rejoin_soon_ms = 2'000, rejoin_later_ms = 15'000;
constexpr int rejoin_attempts = 8;
// Leaving a room is noticed quickly; a match start or end tears the game's
// network down for a while without anyone having left.
constexpr std::uint64_t room_grace_ms = 3'000, match_grace_ms = 60'000;

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool trace_enabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("PSPRECOMP_ONLINE_TRACE");
        return value && *value == '1';
    }();
    return enabled;
}

std::string hex16(std::uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

std::uint64_t random_u64() {
    static std::mt19937_64 engine{std::random_device{}() ^ (std::uint64_t(std::random_device{}()) << 32) ^ now_ms()};
    return engine();
}

bool is_hex16(const std::string &text) {
    return text.size() == 16 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// Printable ASCII without the beacon separator.
std::string clean(const std::string &text, std::size_t limit) {
    std::string out;
    for (const char c : text)
        if (c >= 0x20 && c < 0x7f && out.size() < limit) out += c;
    return out;
}

net::SessionKey key_from_hex(const std::string &text) {
    net::SessionKey key{};
    if (!is_hex16(text)) return key;
    for (std::size_t i = 0; i < 8; ++i) key[i] = static_cast<std::uint8_t>(std::stoul(text.substr(i * 2, 2), nullptr, 16));
    return key;
}

net::SessionKey key_from_group(const std::string &group) {
    net::SessionKey key{};
    std::memcpy(key.data(), group.data(), std::min<std::size_t>(group.size(), key.size()));
    return key;
}

enum class GamePhase { None, Menu, Room, Starting, Match };

struct GameNet {
    bool exists{};
    std::uint32_t state{};
    bool active{};
    bool server{};
    std::string app;
};

GameNet read_game(const psprecomp::GuestMemory &memory) {
    GameNet game{};
    const auto dme = memory.load32(dme_pointer);
    if (!dme || !memory.contains(dme, dme_room_state + 4u)) return game;
    game.exists = true;
    game.state = memory.load32(dme + dme_room_state);
    for (std::uint32_t i = 0; i < 40u; ++i) {
        const char c = static_cast<char>(memory.load8(dme + dme_app_name + i));
        if (!c) break;
        game.app += c;
    }
    const auto thread = memory.load32(dme_thread_pointer);
    game.active = thread && memory.contains(thread, thread_active + 4u) && memory.load32(thread + thread_active) == 1u;
    const auto session = memory.load32(session_pointer);
    game.server = session && memory.contains(session, session_server + 4u) && memory.load32(session + session_server) == 1u;
    return game;
}

GamePhase classify(const GameNet &game) {
    if (!game.exists) return GamePhase::None;
    if (game.app.starts_with("Kokaku PSP Game")) return GamePhase::Match;
    if (game.app == "Kokaku PSP LobbyEnd") return GamePhase::Starting;
    if (game.app == "Kokaku PSP Lobby" && game.active &&
        (game.state == 12u || (!game.server && (game.state == 9u || game.state == 10u)))) return GamePhase::Room;
    return GamePhase::Menu;
}

std::vector<int> read_rules(const psprecomp::GuestMemory &memory) {
    std::vector<int> rules;
    const auto session = memory.load32(session_pointer);
    if (!session || !memory.contains(session, session_rules + rule_count * 4u)) return rules;
    for (std::size_t i = 0; i < rule_count; ++i) {
        const auto value = memory.load32(session + session_rules + static_cast<std::uint32_t>(i * 4u));
        rules.push_back(value <= 255u ? static_cast<int>(value) : 0);
    }
    return rules;
}

// Local network announcement. Tab separated:
// KOKAKU2 mac nick port net kind room [key state players max rules name]
struct Beacon {
    net::Mac mac{};
    std::string nick;
    std::uint16_t port{};
    int net{};
    char kind{};          // G legacy group member, H room host, M room member
    std::string room;
    std::string key, state, name;
    int players{}, max{};
    std::vector<int> rules;
};

std::string encode_beacon(const Beacon &beacon) {
    std::ostringstream out;
    out << "KOKAKU2\t" << net::mac_text(beacon.mac) << '\t' << clean(beacon.nick, 16) << '\t' << beacon.port << '\t'
        << beacon.net << '\t' << beacon.kind << '\t' << clean(beacon.room, 16);
    if (beacon.kind == 'H') {
        out << '\t' << clean(beacon.key, 16) << '\t' << clean(beacon.state, 8) << '\t' << beacon.players << '\t' << beacon.max << '\t';
        for (std::size_t i = 0; i < beacon.rules.size() && i < 8; ++i) out << (i ? "," : "") << beacon.rules[i];
        out << '\t' << clean(beacon.name, 32);
    }
    return out.str();
}

bool parse_beacon(const std::string &payload, Beacon &beacon) {
    if (payload.size() > 512) return false;
    std::vector<std::string> fields;
    std::size_t begin = 0;
    for (;;) {
        const auto end = payload.find('\t', begin);
        fields.push_back(payload.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (fields.size() < 7 || fields[0] != "KOKAKU2" || fields[5].size() != 1) return false;
    const auto mac = net::parse_mac(fields[1]);
    const int port = std::atoi(fields[3].c_str());
    if (!mac || port < 1 || port > 65535) return false;
    beacon = Beacon{};
    beacon.mac = *mac;
    beacon.nick = clean(fields[2], 16);
    beacon.port = static_cast<std::uint16_t>(port);
    beacon.net = std::atoi(fields[4].c_str());
    beacon.kind = fields[5][0];
    beacon.room = clean(fields[6], 16);
    if (beacon.room.empty()) return false;
    if (beacon.kind == 'G' || beacon.kind == 'M') return true;
    if (beacon.kind != 'H' || fields.size() < 13 || !is_hex16(beacon.room) || !is_hex16(fields[7])) return false;
    beacon.key = fields[7];
    beacon.state = fields[8] == "playing" ? "playing" : fields[8] == "open" ? "open" : "closed";
    beacon.players = std::clamp(std::atoi(fields[9].c_str()), 1, 16);
    beacon.max = std::clamp(std::atoi(fields[10].c_str()), 1, 16);
    std::istringstream rules(fields[11]);
    for (std::string item; std::getline(rules, item, ',') && beacon.rules.size() < 8;)
        beacon.rules.push_back(std::clamp(std::atoi(item.c_str()), 0, 255));
    beacon.name = clean(fields[12], 32);
    return true;
}

struct LanRoom {
    std::string id, key, name, nick, state;
    std::vector<int> rules;
    int players{}, max{};
    net::Mac host{};
    net::Endpoint endpoint{};
    std::uint64_t seen_ms{};
};

struct KnownPeer {
    net::Mac mac{};
    std::string nick;
    bool host{};
    bool from_lobby{};
    std::vector<net::Endpoint> lobby_candidates;
    net::Endpoint lan_endpoint{};
    std::uint64_t lan_seen_ms{};
};

struct Row {
    std::string id, key, name, nick, state;
    std::vector<int> rules;
    int players{}, max{};
    bool lan{}, internet{};
    net::Mac lan_host{};
    net::Endpoint lan_endpoint{};
};

struct Session {
    bool active{};
    bool hosting{};
    bool via_lobby{};   // a member the lobby knows about
    std::string id, key, name, state{"closed"};
    std::vector<int> rules;
    net::Mac host{};
};

enum class JoinStage { Idle, Requested, Introduced, Ready, Failed };
enum class Farewell { None, Leave, Unhost };

struct State {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool worker_started{};
    std::atomic<bool> running{};
    bool wake_requested{};
    bool test_mode{};

    // Guest side.
    std::uint64_t frame{};
    bool legacy_connected{};
    std::string legacy_group;
    GamePhase phase{GamePhase::None};
    bool join_ui{};
    std::uint64_t join_seen_frame{};
    bool join_found{};
    std::uint32_t traced_result{};
    std::uint32_t previous_buttons{};
    bool creating{};
    std::uint64_t create_seen_frame{};
    std::uint64_t last_alive_ms{};
    std::uint64_t grace_ms{};
    std::uint64_t rules_ms{};
    std::uint64_t test_clock_ms{};

    // Network side.
    lobby::Client lobby;
    net::LanDiscovery discovery;
    std::uint64_t needed_until_ms{};
    std::uint16_t local_port{};
    bool lobby_online{};
    std::string public_ip;
    std::vector<net::Endpoint> published;
    bool publish{};
    std::vector<lobby::Room> net_rooms;
    int online_count{};
    std::uint64_t list_sent_ms{};
    bool list_now{};
    std::vector<LanRoom> lan_rooms;
    std::vector<KnownPeer> peers;
    net::NatType nat{net::NatType::Unknown};
    bool mapped{};
    net::NatType logged_nat{net::NatType::Unknown};
    net::Endpoint logged_mapped{};
    std::vector<std::pair<net::Mac, int>> links;   // last reported state per peer

    Session session;
    bool announce{};
    bool apply_session{};
    bool rejoin{};
    std::uint64_t rejoin_at_ms{};   // when to ask the list again after it turned this member away
    int rejoin_soon{};              // quick attempts left before the slow ones
    Farewell farewell{Farewell::None};

    // Room list.
    bool browser{};
    bool rows_dirty{};
    std::vector<Row> rows;
    int cursor{};
    std::string cursor_id;
    JoinStage join{JoinStage::Idle};
    Row target;
    std::uint64_t join_started_ms{};
    bool join_sent{};
    std::string message;
    std::uint64_t message_until_ms{};
};

State g;

std::uint64_t clock_ms() {
    return g.test_mode ? g.test_clock_ms : now_ms();
}

// A menu page that stopped calling its hook has been left.
bool hook_silent(std::uint64_t seen_frame) {
    return g.frame - seen_frame > hook_silence_vblanks;
}

void mark_ready_locked() {
    g.join = JoinStage::Ready;
    g.last_alive_ms = clock_ms();
    g.grace_ms = match_grace_ms;
}

bool online_mode() {
    const auto &config = network_config();
    return config.enabled && config.online;
}

void log(const std::string &text) {
    // One insertion, so lines from several threads do not interleave.
    std::cerr << "[online] " + text + "\n";
    // The log file is what a player can send along when a connection fails;
    // these lines are few and must survive a game that is closed abruptly.
    vcs::runtime_log_line("online: " + text);
    vcs::runtime_log_flush();
}

// An address as the log shows it. A public one keeps its first half only:
// enough to tell networks apart, not enough to find a player.
std::string log_address(const net::Endpoint &endpoint) {
    const auto ip = endpoint.ip;
    const bool local = (ip >> 24) == 10u || (ip >> 20) == 0xAC1u || (ip >> 16) == 0xC0A8u || (ip >> 16) == 0xA9FEu ||
                       (ip >> 24) == 127u || (ip >> 22) == (0x64400000u >> 22);
    if (local) return net::endpoint_text(endpoint);
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255u) + ".x.x:" + std::to_string(endpoint.port);
}

void wake_worker_locked() {
    g.wake_requested = true;
    g.wake.notify_all();
}

void say_locked(const std::string &text, std::uint64_t duration = message_ms) {
    g.message = text;
    g.message_until_ms = clock_ms() + duration;
}

// Drop the current room: the lobby is told, the transport forgets its peers.
void end_session_locked(const char *reason) {
    if (!g.session.active) return;
    log(std::string(g.session.hosting ? "Room closed: " : "Left room: ") + reason);
    if (g.session.hosting) g.farewell = Farewell::Unhost;
    else if (g.session.via_lobby) g.farewell = Farewell::Leave;
    g.session = Session{};
    g.peers.clear();
    g.announce = false;
    g.rejoin = false;
    g.rejoin_at_ms = 0;
    g.rejoin_soon = 0;
    g.apply_session = true;
    wake_worker_locked();
}

// A member that the list turned away asks again when the time has come.
void due_rejoin_locked(std::uint64_t now) {
    if (!g.rejoin_at_ms || now < g.rejoin_at_ms) return;
    g.rejoin_at_ms = 0;
    g.rejoin = true;
}

void fail_join_locked(const std::string &text) {
    g.join = JoinStage::Failed;
    g.join_sent = false;
    end_session_locked("join failed");
    say_locked(text);
    log("Join failed: " + text);
}

// The search page is gone: a room was found, none was, or the player went
// back. The room list goes with it; only a room the game found is kept.
void close_search_locked(const char *why) {
    const bool entering = g.join == JoinStage::Ready && g.join_found;
    if (trace_enabled())
        log("vblank=" + std::to_string(g.frame) + " search page left (" + why + "), entering=" + std::to_string(entering));
    g.join_ui = false;
    g.browser = false;
    if (!entering && !g.session.hosting) end_session_locked("search cancelled");
    g.join = JoinStage::Idle;
    g.join_sent = false;
    g.message.clear();
}

void rebuild_rows_locked(std::uint64_t now) {
    std::vector<Row> rows;
    const auto fresh = [&](const LanRoom &room) { return now - room.seen_ms < lan_fresh_ms; };
    for (const auto &room : g.net_rooms) {
        Row row{};
        row.id = room.id; row.name = room.name; row.nick = room.nick; row.state = room.state;
        row.rules = room.rules; row.players = room.players; row.max = room.max; row.internet = true;
        for (const auto &lan : g.lan_rooms)
            if (lan.id == room.id && fresh(lan)) {
                row.lan = true; row.key = lan.key; row.lan_host = lan.host; row.lan_endpoint = lan.endpoint;
            }
        rows.push_back(std::move(row));
    }
    for (const auto &lan : g.lan_rooms) {
        if (!fresh(lan) || lan.state == "closed") continue;
        if (std::any_of(rows.begin(), rows.end(), [&](const Row &row) { return row.id == lan.id; })) continue;
        Row row{};
        row.id = lan.id; row.key = lan.key; row.name = lan.name; row.nick = lan.nick; row.state = lan.state;
        row.rules = lan.rules; row.players = lan.players; row.max = lan.max; row.lan = true;
        row.lan_host = lan.host; row.lan_endpoint = lan.endpoint;
        rows.push_back(std::move(row));
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if ((a.state == "open") != (b.state == "open")) return a.state == "open";
        if (a.lan != b.lan) return a.lan;
        return false;
    });
    g.rows = std::move(rows);
    int cursor = -1;
    for (std::size_t i = 0; i < g.rows.size(); ++i)
        if (g.rows[i].id == g.cursor_id) cursor = static_cast<int>(i);
    if (cursor < 0) cursor = std::clamp(g.cursor, 0, std::max(0, static_cast<int>(g.rows.size()) - 1));
    g.cursor = cursor;
    g.cursor_id = g.rows.empty() ? std::string{} : g.rows[static_cast<std::size_t>(g.cursor)].id;
    g.rows_dirty = false;
}

KnownPeer &known_peer_locked(const net::Mac &mac) {
    for (auto &peer : g.peers)
        if (peer.mac == mac) return peer;
    g.peers.push_back(KnownPeer{});
    g.peers.back().mac = mac;
    return g.peers.back();
}

std::string make_beacon() {
    const auto &config = network_config();
    std::lock_guard lock(g.mutex);
    if (!g.local_port) return {};
    Beacon beacon{};
    std::copy(config.mac.begin(), config.mac.end(), beacon.mac.begin());
    beacon.nick = config.nickname;
    beacon.port = g.local_port;
    beacon.net = netcode_version;
    if (!config.online) {
        if (!g.legacy_connected) return {};
        beacon.kind = 'G';
        beacon.room = g.legacy_group;
    } else if (g.session.active) {
        beacon.room = g.session.id;
        beacon.kind = g.session.hosting ? 'H' : 'M';
        if (g.session.hosting) {
            beacon.key = g.session.key;
            beacon.state = g.session.state;
            beacon.name = g.session.name;
            beacon.rules = g.session.rules;
            beacon.players = 1 + static_cast<int>(g.peers.size());
            beacon.max = max_players;
        }
    } else {
        return {};
    }
    return encode_beacon(beacon);
}

void on_beacon(const std::string &payload, std::uint32_t ip) {
    const auto &config = network_config();
    Beacon beacon{};
    if (!parse_beacon(payload, beacon) || beacon.net != netcode_version ||
        std::equal(beacon.mac.begin(), beacon.mac.end(), config.mac.begin())) return;
    const net::Endpoint endpoint{ip, beacon.port};
    const auto now = now_ms();
    std::lock_guard lock(g.mutex);
    if (beacon.kind == 'H' && config.online) {
        LanRoom *room = nullptr;
        for (auto &known : g.lan_rooms)
            if (known.id == beacon.room) room = &known;
        if (!room) {
            if (g.lan_rooms.size() >= 32) return;
            g.lan_rooms.push_back(LanRoom{});
            room = &g.lan_rooms.back();
            g.rows_dirty = true;
        }
        g.rows_dirty |= room->state != beacon.state || room->players != beacon.players || room->rules != beacon.rules;
        room->id = beacon.room; room->key = beacon.key; room->name = beacon.name; room->nick = beacon.nick;
        room->state = beacon.state; room->rules = beacon.rules; room->players = beacon.players; room->max = beacon.max;
        room->host = beacon.mac; room->endpoint = endpoint; room->seen_ms = now;
    }
    const bool member = config.online
        ? g.session.active && beacon.room == g.session.id && (beacon.kind == 'H' || beacon.kind == 'M')
        : g.legacy_connected && beacon.kind == 'G' && beacon.room == g.legacy_group;
    if (!member) return;
    if (g.peers.size() >= 16 && std::none_of(g.peers.begin(), g.peers.end(), [&](const KnownPeer &p) { return p.mac == beacon.mac; }))
        return;
    KnownPeer &peer = known_peer_locked(beacon.mac);
    if (peer.nick.empty()) peer.nick = beacon.nick;
    peer.host |= beacon.kind == 'H';
    peer.lan_endpoint = endpoint;
    peer.lan_seen_ms = now;
}

void on_lobby_event(const lobby::Event &event) {
    std::lock_guard lock(g.mutex);
    switch (event.kind) {
    case lobby::Event::Kind::Connected:
        g.lobby_online = true;
        g.public_ip = event.text;
        g.online_count = event.online;
        g.publish = true;
        g.list_now = true;
        if (g.session.active && g.session.hosting) g.announce = true;
        if (g.session.active && !g.session.hosting && g.session.via_lobby) {
            g.rejoin = true;
            g.rejoin_at_ms = 0;
            g.rejoin_soon = rejoin_attempts;
        }
        log("Lobby connected (" + std::to_string(event.online) + " online)");
        break;
    case lobby::Event::Kind::Disconnected:
        g.lobby_online = false;
        g.net_rooms.clear();
        g.rows_dirty = true;
        log("Lobby connection lost");
        break;
    case lobby::Event::Kind::Rooms:
        g.net_rooms = event.rooms;
        g.online_count = event.online;
        g.rows_dirty = true;
        break;
    case lobby::Event::Kind::Joined: {
        const bool pending = g.join == JoinStage::Requested && event.id == g.target.id;
        const bool returning = g.session.active && !g.session.hosting && event.id == g.session.id;
        if (!pending && !returning) break;
        if (returning && !pending) {
            log("Listed in the room again");
            g.rejoin_at_ms = 0;
            g.rejoin_soon = 0;
        }
        if (pending) {
            g.session = Session{};
            g.session.active = true;
            g.session.via_lobby = true;
            g.session.id = event.id;
            g.session.key = event.key;
            g.session.name = event.text.empty() ? g.target.name : event.text;
            g.session.rules = g.target.rules;
            g.peers.clear();
            g.apply_session = true;
            g.join = JoinStage::Introduced;
            log("The list introduced " + std::to_string(event.peers.size()) + " player(s) of the room");
        }
        for (const auto &listed : event.peers) {
            KnownPeer &peer = known_peer_locked(listed.mac);
            peer.nick = listed.nick;
            peer.host = listed.host;
            peer.from_lobby = true;
            peer.lobby_candidates = listed.candidates;
            if (listed.host) g.session.host = listed.mac;
        }
        // The same host seen on the local network is the shortest path.
        if (pending && g.target.lan && g.target.lan_host == g.session.host) {
            KnownPeer &host = known_peer_locked(g.session.host);
            host.lan_endpoint = g.target.lan_endpoint;
            host.lan_seen_ms = now_ms();
        }
        break;
    }
    case lobby::Event::Kind::JoinFailed:
        if (g.join == JoinStage::Requested && event.id == g.target.id) {
            fail_join_locked(event.text == "full" ? "That room is full."
                             : event.text == "playing" ? "That match has already started."
                             : event.text == "version" ? "That room uses a different game version."
                             : "That room no longer exists.");
            g.list_now = true;
        } else if (g.session.active && !g.session.hosting && event.id == g.session.id) {
            // The room is not on the list, or not yet: when the list itself
            // restarts, a member can be back before its host. The players may
            // all still be there, and a newcomer learns about this one only
            // from the list, so keep asking: soon at first, then now and then.
            if (g.rejoin_soon == rejoin_attempts) log("The room is not on the list yet: asking again");
            g.rejoin_at_ms = now_ms() + (g.rejoin_soon > 0 ? rejoin_soon_ms : rejoin_later_ms);
            if (g.rejoin_soon > 0) --g.rejoin_soon;
        }
        break;
    case lobby::Event::Kind::PeerUpdate:
        if (g.session.active && !event.peers.empty() && g.peers.size() < 16) {
            const bool known = std::any_of(g.peers.begin(), g.peers.end(), [&](const KnownPeer &p) { return p.mac == event.mac; });
            KnownPeer &peer = known_peer_locked(event.mac);
            peer.nick = event.peers[0].nick;
            peer.host = event.peers[0].host;
            peer.from_lobby = true;
            peer.lobby_candidates = event.peers[0].candidates;
            if (!known)
                log("The list introduced " + peer.nick + " (" + std::to_string(peer.lobby_candidates.size()) + " addresses)");
        }
        break;
    case lobby::Event::Kind::PeerGone:
        for (auto &peer : g.peers)
            if (peer.mac == event.mac) peer.from_lobby = false;
        break;
    case lobby::Event::Kind::Error:
        if (event.text == "version") say_locked("The online list needs a newer version of the game.", 15'000);
        log("Lobby error: " + event.text);
        break;
    default:
        break;
    }
}

std::vector<net::Endpoint> gather_candidates(const net::Transport &transport, const std::vector<net::LocalAddress> &locals,
                                             const std::string &public_ip) {
    std::vector<net::Endpoint> candidates;
    const auto add = [&](const net::Endpoint &endpoint) {
        if (endpoint.valid() && candidates.size() < 8 &&
            std::find(candidates.begin(), candidates.end(), endpoint) == candidates.end()) candidates.push_back(endpoint);
    };
    const auto port = transport.local_port();
    const auto mapped = transport.mapped_endpoint();
    add(mapped);
    // Without a STUN answer, a port-preserving router still maps our port to itself.
    if (const auto seen = net::parse_ipv4(public_ip)) add(net::Endpoint{*seen, mapped.valid() ? mapped.port : port});
    for (std::size_t i = 0; i < locals.size() && i < 4; ++i) add(net::Endpoint{locals[i].ip, port});
    return candidates;
}

void step(net::Transport &transport, const std::vector<net::LocalAddress> &locals, std::uint64_t now) {
    const auto &config = network_config();
    std::lock_guard lock(g.mutex);
    g.local_port = transport.local_port();
    g.nat = transport.nat_type();
    const auto mapped = transport.mapped_endpoint();
    g.mapped = mapped.valid();
    if (g.nat != g.logged_nat || !(mapped == g.logged_mapped)) {
        g.logged_nat = g.nat;
        g.logged_mapped = mapped;
        log("Internet address " + (mapped.valid() ? log_address(mapped) : std::string("not known")) + ", router type " +
            net::nat_type_name(g.nat));
    }

    if (g.farewell != Farewell::None) {
        g.lobby.send(g.farewell == Farewell::Unhost ? "{\"t\":\"unhost\"}" : "{\"t\":\"leave\"}");
        g.farewell = Farewell::None;
    }
    if (g.apply_session) {
        transport.clear_peers();
        if (!config.online) transport.set_session(key_from_group(g.legacy_group), false);
        else if (g.session.active) transport.set_session(key_from_hex(g.session.key), g.session.hosting);
        else {
            // No room: nothing that still arrives may reach the game.
            net::SessionKey idle{};
            const auto value = random_u64() | 1u;
            std::memcpy(idle.data(), &value, idle.size());
            transport.set_session(idle, false);
        }
        g.apply_session = false;
    }

    if (config.online) {
        const auto candidates = gather_candidates(transport, locals, g.public_ip);
        if (g.lobby.online() && (g.publish || candidates != g.published)) {
            g.lobby.send(lobby::candidates_message(candidates));
            g.published = candidates;
            g.publish = false;
        }
        if (g.announce && g.session.active && g.session.hosting && g.lobby.online()) {
            g.lobby.send(lobby::host_message(g.session.id, g.session.key, g.session.name, g.session.rules, max_players,
                                             g.session.state));
            g.announce = false;
        }
        due_rejoin_locked(now);
        if (g.rejoin && g.session.active && !g.session.hosting && g.lobby.online()) {
            g.lobby.send(lobby::join_message(g.session.id, g.session.key));
            g.rejoin = false;
        }
        if (g.browser && g.lobby.online() && (g.list_now || now - g.list_sent_ms >= list_interval_ms)) {
            g.lobby.send("{\"t\":\"list\"}");
            g.list_sent_ms = now;
            g.list_now = false;
        }

        if (g.join == JoinStage::Requested && !g.join_sent) {
            if (g.target.internet && g.lobby.online()) {
                g.lobby.send(lobby::join_message(g.target.id, {}));
                g.join_sent = true;
            } else if (g.target.lan && is_hex16(g.target.key)) {
                // A room known only from the local network.
                g.session = Session{};
                g.session.active = true;
                g.session.id = g.target.id;
                g.session.key = g.target.key;
                g.session.name = g.target.name;
                g.session.rules = g.target.rules;
                g.session.host = g.target.lan_host;
                g.peers.clear();
                KnownPeer &host = known_peer_locked(g.target.lan_host);
                host.nick = g.target.nick;
                host.host = true;
                host.lan_endpoint = g.target.lan_endpoint;
                host.lan_seen_ms = now;
                transport.clear_peers();
                transport.set_session(key_from_hex(g.session.key), false);
                g.join = JoinStage::Introduced;
            } else {
                fail_join_locked("The online list is not reachable right now.");
            }
        }
        if (g.join == JoinStage::Requested && now - g.join_started_ms > lobby_answer_ms)
            fail_join_locked("The online list did not answer.");
    }

    // Peers: whoever the lobby lists or the local network announces.
    for (auto it = g.peers.begin(); it != g.peers.end();) {
        const bool lan = it->lan_endpoint.valid() && now - it->lan_seen_ms < lan_fresh_ms;
        if (!it->from_lobby && !lan) {
            transport.remove_peer(it->mac, true);
            it = g.peers.erase(it);
            continue;
        }
        net::PeerDescription description{it->mac, it->nick, it->lobby_candidates, it->host};
        if (lan) description.candidates.push_back(it->lan_endpoint);
        transport.set_peer(description);
        ++it;
    }

    if (g.join == JoinStage::Introduced) {
        if (transport.peer_reachable(g.session.host)) {
            mark_ready_locked();
            say_locked("Connected. Entering the room...");
            log("Connected to the host of '" + g.session.name + "'");
        } else if (now - g.join_started_ms > join_timeout_ms) {
            fail_join_locked(g.nat == net::NatType::Symmetric
                ? "Could not reach the host: your router uses a strict NAT."
                : "Could not reach the host: a router or firewall is in the way.");
        }
    }

    // The log is where a player finds out why someone could not be reached.
    std::vector<std::pair<net::Mac, int>> links;
    for (const auto &peer : transport.peers()) {
        const int state = peer.connected ? 2 : peer.relayed ? 1 : 0;
        links.emplace_back(peer.mac, state);
        const auto known = std::find_if(g.links.begin(), g.links.end(), [&](const auto &link) { return link.first == peer.mac; });
        if (known != g.links.end() && known->second == state) continue;
        const std::string name = peer.nickname.empty() ? net::mac_text(peer.mac) : peer.nickname;
        if (state == 2) log("Direct connection with " + name + " at " + log_address(peer.endpoint));
        else if (state == 1) log("No direct connection with " + name + ": the host forwards");
        else if (known != g.links.end()) log("Connection with " + name + " lost");
    }
    g.links = std::move(links);

    const auto before = g.lan_rooms.size();
    std::erase_if(g.lan_rooms, [&](const LanRoom &room) { return now - room.seen_ms >= lan_fresh_ms; });
    if (g.lan_rooms.size() != before) g.rows_dirty = true;
    if (g.rows_dirty) rebuild_rows_locked(now);
}

void worker_loop() {
    const auto &config = network_config();
    lobby::Identity identity{};
    std::copy(config.mac.begin(), config.mac.end(), identity.mac.begin());
    identity.nickname = config.nickname;
    identity.net_version = netcode_version;
    std::vector<net::LocalAddress> locals;
    std::uint64_t locals_ms = 0, stun_ms = 0;
    bool stun_ready = false;
    net::Transport *started = nullptr;
    // DNS can block for many seconds without a network. It runs on its own
    // thread, which is simply abandoned if the game exits first.
    struct Lookup {
        std::mutex mutex;
        bool done{};
        std::vector<net::Endpoint> servers;
    };
    std::shared_ptr<Lookup> lookup;
    while (g.running.load()) {
        const auto now = now_ms();
        bool needed = false;
        {
            std::lock_guard lock(g.mutex);
            if (g.legacy_connected || g.browser || g.join_ui || g.creating || g.session.active)
                g.needed_until_ms = now + idle_linger_ms;
            needed = now < g.needed_until_ms || g.farewell != Farewell::None;
        }
        net::Transport *transport = needed ? network_transport() : nullptr;
        if (transport) started = transport;
        if (!needed && started && stun_ready) {
            // Back in single player: stop asking the public STUN servers.
            started->set_stun_servers({});
            stun_ready = false;
            stun_ms = 0;
        }
        if (transport) {
            if (locals_ms == 0 || now - locals_ms >= 5'000) {
                locals = net::local_ipv4_addresses();
                std::vector<std::uint32_t> addresses;
                for (const auto &local : locals) addresses.push_back(local.ip);
                transport->set_local_addresses(std::move(addresses));
                locals_ms = now;
            }
            // Only the internet list needs our public address.
            if (config.online && !config.lobby_url.empty() && !stun_ready && !lookup &&
                (stun_ms == 0 || now - stun_ms >= 30'000)) {
                stun_ms = now;
                lookup = std::make_shared<Lookup>();
                std::thread([lookup, names = config.stun_servers] {
                    std::vector<net::Endpoint> servers;
                    for (const auto &entry : names) {
                        const auto colon = entry.rfind(':');
                        const int port = colon == std::string::npos ? 3478 : std::atoi(entry.c_str() + colon + 1);
                        if (port < 1 || port > 65535) continue;
                        if (const auto resolved = net::resolve_ipv4(entry.substr(0, colon), static_cast<std::uint16_t>(port)))
                            servers.push_back(*resolved);
                    }
                    std::lock_guard lock(lookup->mutex);
                    lookup->servers = std::move(servers);
                    lookup->done = true;
                }).detach();
            }
            if (lookup) {
                std::lock_guard lock(lookup->mutex);
                if (lookup->done) {
                    if (!lookup->servers.empty()) {
                        transport->set_stun_servers(lookup->servers);
                        stun_ready = true;
                    }
                    lookup.reset();
                }
            }
        }
        const bool want_lobby = needed && config.online && !config.lobby_url.empty();
        if (want_lobby && !g.lobby.running()) g.lobby.start(config.lobby_url, identity);
        if (!want_lobby && g.lobby.running()) {
            g.lobby.stop();
            std::lock_guard lock(g.mutex);
            g.lobby_online = false;
            g.net_rooms.clear();
            g.published.clear();
            g.rows_dirty = true;
        }
        // The original LAN mode has no other way to find players.
        if (needed && transport && !g.discovery.running() && (config.local_discovery || !config.online))
            g.discovery.start(config.discovery_port, config.port_offset, make_beacon, on_beacon);
        if (!needed && g.discovery.running()) g.discovery.stop();

        lobby::Event event{};
        while (g.lobby.poll(event)) on_lobby_event(event);
        if (transport) step(*transport, locals, now);
        else if (needed) {
            std::lock_guard lock(g.mutex);
            if (g.join == JoinStage::Requested || g.join == JoinStage::Introduced)
                fail_join_locked("Networking is not available on this PC.");
        }

        std::unique_lock lock(g.mutex);
        g.wake.wait_for(lock, std::chrono::milliseconds(100), [] { return !g.running.load() || g.wake_requested; });
        g.wake_requested = false;
    }
    // Give a queued goodbye a moment to leave before the socket closes.
    if (g.lobby.online()) {
        bool farewell = false;
        {
            std::lock_guard lock(g.mutex);
            farewell = g.farewell != Farewell::None;
            if (farewell) g.lobby.send(g.farewell == Farewell::Unhost ? "{\"t\":\"unhost\"}" : "{\"t\":\"leave\"}");
            g.farewell = Farewell::None;
        }
        if (farewell) std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    g.lobby.stop();
    g.discovery.stop();
}

void ensure_worker_locked() {
    if (g.test_mode || g.worker_started) { wake_worker_locked(); return; }
    g.worker_started = true;
    g.running.store(true);
    g.worker = std::thread(worker_loop);
}

std::string rules_log(const std::vector<int> &rules) {
    std::string raw;
    for (const int rule : rules) raw += (raw.empty() ? "" : ",") + std::to_string(rule);
    return online_rules_text(rules) + " [" + raw + "]";
}

void begin_hosting_locked(const psprecomp::GuestMemory &memory) {
    if (!(g.session.active && g.session.hosting)) {
        g.session = Session{};
        g.session.active = true;
        g.session.hosting = true;
        g.session.id = hex16(random_u64());
        g.session.key = hex16(random_u64());
        std::copy(network_config().mac.begin(), network_config().mac.end(), g.session.host.begin());
        g.peers.clear();
        g.apply_session = true;
    }
    g.session.name = network_config().nickname + "'s room";
    g.session.rules = read_rules(memory);
    g.session.state = "open";
    g.last_alive_ms = clock_ms();
    g.grace_ms = room_grace_ms;
    g.announce = true;
    log("Hosting room '" + g.session.name + "': " + rules_log(g.session.rules));
    ensure_worker_locked();
}

void set_host_state_locked(const char *state) {
    if (!g.session.hosting || g.session.state == state) return;
    g.session.state = state;
    g.announce = true;
    log(std::string("Room is now ") + state);
    wake_worker_locked();
}

} // namespace

namespace {

// Room rules as the game stores them: seven option indices.
constexpr const char *rule_modes[] = {"Player Team Battle", "Player Battle", "Tachikoma Team Battle", "Tachikoma Battle"};
constexpr const char *rule_modes_short[] = {"Team Battle", "Battle", "Tachi Team", "Tachi Battle"};
constexpr const char *rule_maps[] = {"Uptown", "Public Facility", "Hotel Roof", "Sewer", "Container Pier", "Missile Silo",
                                     "Army Facility", "Underground Arena"};
constexpr int rule_minutes[] = {5, 10, 15, 20, 30, 60};
constexpr int rule_frags[] = {1, 5, 10, 15, 20, 30, 0};   // 0: no limit
constexpr int rule_friendly_fire[] = {0, 25, 50, 75, 100, 200};

template <typename T, std::size_t N>
const T *rule_entry(const T (&table)[N], const std::vector<int> &rules, std::size_t field) {
    if (field >= rules.size() || rules[field] < 0 || static_cast<std::size_t>(rules[field]) >= N) return nullptr;
    return &table[static_cast<std::size_t>(rules[field])];
}

std::string rule_map(const std::vector<int> &rules) {
    const auto *name = rule_entry(rule_maps, rules, 1);
    return name ? *name : "";
}

std::string rule_mode(const std::vector<int> &rules) {
    const auto *name = rule_entry(rule_modes_short, rules, 0);
    return name ? *name : "";
}

} // namespace

std::string online_rules_text(const std::vector<int> &rules) {
    if (rules.size() < rule_count) return {};
    std::string text;
    const auto add = [&](const std::string &part) {
        if (part.empty()) return;
        if (!text.empty()) text += ", ";
        text += part;
    };
    if (const auto *mode = rule_entry(rule_modes, rules, 0)) add(*mode);
    add(rule_map(rules));
    if (const auto *minutes = rule_entry(rule_minutes, rules, 2)) add(std::to_string(*minutes) + " min");
    if (const auto *frags = rule_entry(rule_frags, rules, 3)) add(*frags ? std::to_string(*frags) + " frags" : "no frag limit");
    if (const auto *fire = rule_entry(rule_friendly_fire, rules, 4)) add("FF " + std::to_string(*fire) + "%");
    if (rules[5] == 1) add("Lock On");
    if (rules[6] == 1) add("Map rotation");
    return text;
}

void online_adhoc_connected(const std::string &group) {
    if (!network_config().enabled) return;
    std::lock_guard lock(g.mutex);
    g.legacy_connected = true;
    g.legacy_group = group;
    if (!network_config().online) g.apply_session = true;
    ensure_worker_locked();
}

void online_adhoc_disconnected() {
    std::lock_guard lock(g.mutex);
    if (!g.legacy_connected) return;
    g.legacy_connected = false;
    if (!network_config().online) {
        g.peers.clear();
        g.apply_session = true;
    }
    wake_worker_locked();
}

void online_shutdown() {
    std::thread worker;
    {
        std::lock_guard lock(g.mutex);
        end_session_locked("shutting down");
        g.running.store(false);
        g.wake.notify_all();
        worker = std::move(g.worker);
        g.worker_started = false;
    }
    if (worker.joinable()) worker.join();
    std::lock_guard lock(g.mutex);
    g.legacy_connected = false;
    g.browser = g.join_ui = g.creating = false;
    g.join = JoinStage::Idle;
    g.farewell = Farewell::None;
    g.peers.clear();
    g.lan_rooms.clear();
    g.net_rooms.clear();
    g.rows.clear();
    g.published.clear();
    g.needed_until_ms = 0;
    g.lobby_online = false;
    g.apply_session = false;
    g.local_port = 0;
}

bool online_join_hold(psprecomp::GuestMemory &memory, std::uint32_t ui) {
    if (!online_mode()) return false;
    std::lock_guard lock(g.mutex);
    g.join_seen_frame = g.frame;
    if (!g.join_ui) {
        if (trace_enabled()) log("vblank=" + std::to_string(g.frame) + " search page opened");
        g.join_ui = true;
        g.join_found = false;
        g.traced_result = ~0u;
        g.previous_buttons = ~0u; // the press that chose Join must not act on the list
        // The game only ever searches because the player chose Join or
        // Re-Search, so whatever room was current before is over.
        end_session_locked("searching for another room");
        g.browser = true;
        g.cursor = 0;
        g.cursor_id.clear();
        g.join = JoinStage::Idle;
        g.message.clear();
        g.list_now = true;
        g.rows_dirty = true;
        ensure_worker_locked();
    }
    if (g.join == JoinStage::Ready) return false;

    std::uint32_t buttons = 0;
    if (memory.contains(ui, 0x24u)) {
        const auto root = memory.load32(ui + 0x20u);
        if (root && memory.contains(root, 0x44u)) buttons = memory.load32(root + 0x40u);
    }
    const auto pressed = buttons & ~g.previous_buttons;
    g.previous_buttons = buttons;
    if (g.join == JoinStage::Requested || g.join == JoinStage::Introduced) return true;

    const int count = static_cast<int>(g.rows.size());
    if (count > 0) {
        if (pressed & button_up) g.cursor = (g.cursor + count - 1) % count;
        if (pressed & button_down) g.cursor = (g.cursor + 1) % count;
        g.cursor = std::clamp(g.cursor, 0, count - 1);
        g.cursor_id = g.rows[static_cast<std::size_t>(g.cursor)].id;
    }
    if (pressed & button_refresh) {
        g.list_now = true;
        wake_worker_locked();
    }
    if ((pressed & button_confirm) && count > 0) {
        const Row &row = g.rows[static_cast<std::size_t>(g.cursor)];
        if (row.state != "open") say_locked("That match has already started.", 3'000);
        else if (row.players >= row.max) say_locked("That room is full.", 3'000);
        else {
            g.target = row;
            g.join = JoinStage::Requested;
            g.join_sent = false;
            g.join_started_ms = now_ms();
            say_locked("Connecting to " + row.name + "...", 60'000);
            log("Joining '" + row.name + "'");
            wake_worker_locked();
        }
    }
    return true;
}

void online_join_result(psprecomp::GuestMemory &, std::uint32_t result) {
    if (!online_mode()) return;
    std::lock_guard lock(g.mutex);
    if (trace_enabled() && result != g.traced_result) {
        g.traced_result = result;
        log("vblank=" + std::to_string(g.frame) + " search result=" + std::to_string(result));
    }
    if (!g.join_ui || (result != 1u && result != 2u)) return;
    if (result == 1u) g.join_found = true;
    // The game searched and found nothing: whatever we joined is not a room any more.
    else if (g.session.active && !g.session.hosting) end_session_locked("the game found no room");
}

void online_join_page(psprecomp::GuestMemory &, std::uint32_t page) {
    if (!online_mode() || page == page_stay) return;
    std::lock_guard lock(g.mutex);
    if (g.join_ui) close_search_locked(("page " + std::to_string(page)).c_str());
}

void online_create_result(psprecomp::GuestMemory &memory, std::uint32_t result) {
    if (!online_mode()) return;
    std::lock_guard lock(g.mutex);
    g.create_seen_frame = g.frame;
    if (result == 6u) {
        if (!g.creating) {
            g.creating = true;
            // Creating a room while a member of another one means that one is over.
            if (g.session.active && !g.session.hosting) end_session_locked("creating a room");
            ensure_worker_locked();
        }
    } else {
        g.creating = false;
        if (result == 7u) begin_hosting_locked(memory);
    }
}

void online_frame(psprecomp::GuestMemory &memory, std::uint64_t vblank) {
    if (!online_mode()) return;
    std::lock_guard lock(g.mutex);
    g.frame = vblank;
    const auto now = clock_ms();
    // The search page normally says where it hands over (online_join_page);
    // this covers a menu that is torn down from outside.
    if (g.join_ui && hook_silent(g.join_seen_frame)) close_search_locked("silence");
    if (g.creating && hook_silent(g.create_seen_frame)) g.creating = false;
    if (!g.session.active) { g.phase = GamePhase::None; return; }

    const GameNet game = read_game(memory);
    const GamePhase phase = classify(game);
    if (trace_enabled() && phase != g.phase)
        log("vblank=" + std::to_string(vblank) + " phase=" + std::to_string(static_cast<int>(phase)) +
            " app='" + game.app + "' state=" + std::to_string(game.state));
    g.phase = phase;
    switch (phase) {
    case GamePhase::Room:
        g.last_alive_ms = now;
        g.grace_ms = room_grace_ms;
        set_host_state_locked("open");
        if (g.session.hosting && now - g.rules_ms >= 1'000u) {
            g.rules_ms = now;
            if (auto rules = read_rules(memory); !rules.empty() && rules != g.session.rules) {
                g.session.rules = std::move(rules);
                g.announce = true;
            }
        }
        break;
    case GamePhase::Starting:
    case GamePhase::Match:
        g.last_alive_ms = now;
        g.grace_ms = match_grace_ms;
        set_host_state_locked("playing");
        break;
    default:
        if (g.join_ui || g.creating) break;
        // Between maps and while a match loads the game has no network
        // objects at all. Seeing the multiplayer menu's objects after a match
        // means the Result screen was left with Exit: the room is over.
        if (phase == GamePhase::Menu && g.grace_ms != room_grace_ms) {
            g.grace_ms = room_grace_ms;
            g.last_alive_ms = now;
        }
        if (g.grace_ms == room_grace_ms) set_host_state_locked("closed");
        if (now - g.last_alive_ms > g.grace_ms)
            end_session_locked(g.session.hosting ? "the host left" : "the player left");
        break;
    }
}

bool online_view(OnlineView &view) {
    if (!online_mode()) return false;
    const auto &config = network_config();
    std::lock_guard lock(g.mutex);
    view = OnlineView{};
    const auto now = clock_ms();
    const char *quality = g.nat == net::NatType::Open ? "Your connection: open"
        : g.nat == net::NatType::Cone ? "Your connection: good"
        : g.nat == net::NatType::Symmetric ? "Your connection: strict NAT, some rooms may not be reachable"
        : "";
    if (g.browser) {
        view.browser = true;
        view.cursor = g.cursor;
        for (const auto &row : g.rows) {
            OnlineRoomRow out{};
            out.name = row.name;
            out.map = rule_map(row.rules);
            out.mode = rule_mode(row.rules);
            out.players = std::to_string(row.players) + "/" + std::to_string(row.max);
            out.tag = row.state != "open" ? "IN GAME" : row.lan ? "LAN" : "NET";
            out.joinable = row.state == "open" && row.players < row.max;
            view.rows.push_back(std::move(out));
        }
        if (!g.rows.empty()) view.details = online_rules_text(g.rows[static_cast<std::size_t>(g.cursor)].rules);
        if (config.lobby_url.empty()) view.headline = "Local network";
        else if (!g.lobby_online) view.headline = "Internet list: connecting...";
        else view.headline = "Internet: " + std::to_string(g.online_count) + " online";
        if (!g.message.empty() && now < g.message_until_ms) view.message = g.message;
        view.footer = quality;
        view.busy = g.join == JoinStage::Requested || g.join == JoinStage::Introduced || g.join == JoinStage::Ready;
        return true;
    }
    if (g.session.active && g.phase == GamePhase::Room) {
        const int players = 1 + static_cast<int>(g.peers.size());
        if (g.session.hosting)
            view.banner = (config.lobby_url.empty() ? std::string("LAN room")
                           : g.lobby_online ? std::string("Listed online") : std::string("Online list unreachable")) +
                          "  " + std::to_string(players) + "/" + std::to_string(max_players);
        else
            view.banner = g.session.name;
        return true;
    }
    return false;
}

bool run_online_self_tests(std::string &error) {
    const auto require = [](bool condition, const char *message) { if (!condition) throw std::runtime_error(message); };
    const auto saved = network_config();
    try {
        online_shutdown();
        g.test_mode = true;
        network_config() = NetworkConfiguration{};
        network_config().nickname = "Tester";
        network_config().mac = {2, 0, 0, 0, 0, 1};

        // Local network announcements.
        Beacon beacon{};
        beacon.mac = net::Mac{2, 0, 0, 0, 0, 7};
        beacon.nick = "Ba\ttou";
        beacon.port = 4658;
        beacon.net = netcode_version;
        beacon.kind = 'H';
        beacon.room = "00112233445566aa";
        beacon.key = "ffeeddccbbaa9988";
        beacon.state = "open";
        beacon.players = 2;
        beacon.max = 6;
        beacon.rules = {0, 1, 2, 3, 3, 1, 1};
        beacon.name = "Batou's\troom";
        Beacon parsed{};
        require(parse_beacon(encode_beacon(beacon), parsed) && parsed.mac == beacon.mac && parsed.nick == "Batou" &&
                parsed.port == 4658 && parsed.kind == 'H' && parsed.room == beacon.room && parsed.key == beacon.key &&
                parsed.state == "open" && parsed.players == 2 && parsed.max == 6 && parsed.rules == beacon.rules &&
                parsed.name == "Batou'sroom", "Host beacon roundtrip");
        beacon.kind = 'M';
        require(parse_beacon(encode_beacon(beacon), parsed) && parsed.kind == 'M' && parsed.room == beacon.room &&
                parsed.key.empty(), "Member beacon roundtrip");
        beacon.kind = 'G';
        beacon.room = "DEFAULT";
        require(parse_beacon(encode_beacon(beacon), parsed) && parsed.kind == 'G' && parsed.room == "DEFAULT", "Group beacon roundtrip");
        for (const char *bad : {"", "KOKAKU_BEACON\tDEFAULT\tnick\t02:00:00:00:00:01\t0", "KOKAKU2\tnotamac\tn\t1\t2\tG\tx",
                                "KOKAKU2\t02:00:00:00:00:01\tn\t0\t2\tG\tx", "KOKAKU2\t02:00:00:00:00:01\tn\t1\t2\tG\t",
                                "KOKAKU2\t02:00:00:00:00:01\tn\t1\t2\tH\tshort\tkey\topen\t1\t6\t0\tname",
                                "KOKAKU2\t02:00:00:00:00:01\tn\t1\t2\tH\t00112233445566aa\tffeeddccbbaa9988\topen"})
            require(!parse_beacon(bad, parsed), "Malformed beacon accepted");
        require(key_from_hex("0102030405060708") == net::SessionKey{1, 2, 3, 4, 5, 6, 7, 8} &&
                key_from_hex("nothex") == net::SessionKey{} &&
                key_from_group("DEFAULT") == net::SessionKey{'D', 'E', 'F', 'A', 'U', 'L', 'T', 0}, "Session keys");

        // What the game's own state means.
        require(classify(GameNet{}) == GamePhase::None &&
                classify(GameNet{true, 0, false, false, "Kokaku PSP Lobby"}) == GamePhase::Menu &&
                classify(GameNet{true, 17, true, false, "Kokaku PSP Lobby"}) == GamePhase::Menu &&
                classify(GameNet{true, 12, true, true, "Kokaku PSP Lobby"}) == GamePhase::Room &&
                classify(GameNet{true, 12, false, true, "Kokaku PSP Lobby"}) == GamePhase::Menu &&
                classify(GameNet{true, 9, true, false, "Kokaku PSP Lobby"}) == GamePhase::Room &&
                classify(GameNet{true, 9, true, true, "Kokaku PSP Lobby"}) == GamePhase::Menu &&
                classify(GameNet{true, 12, true, true, "Kokaku PSP LobbyEnd"}) == GamePhase::Starting &&
                classify(GameNet{true, 0, false, true, "Kokaku PSP GameU36c27bd4 M0"}) == GamePhase::Match, "Game phase");

        // The log keeps local addresses whole and half of a public one.
        require(log_address({0xC0A80105u, 3658}) == "192.168.1.5:3658" && log_address({0x0A000001u, 5}) == "10.0.0.1:5" &&
                log_address({0xAC100001u, 5}) == "172.16.0.1:5" && log_address({0xAC1F0001u, 5}) == "172.31.0.1:5" &&
                log_address({0x64400001u, 1}) == "100.64.0.1:1" && log_address({0xA9FE0102u, 7}) == "169.254.1.2:7",
                "Local address was hidden in the log");
        require(log_address({0x5D29AABBu, 40000}) == "93.41.x.x:40000" && log_address({0xAC0F0001u, 5}) == "172.15.x.x:5" &&
                log_address({0xAC200001u, 5}) == "172.32.x.x:5" && log_address({0x64800001u, 1}) == "100.128.x.x:1" &&
                log_address({0xC0A90105u, 9}) == "192.169.x.x:9", "Public address was written to the log");

        psprecomp::GuestMemory memory;
        constexpr std::uint32_t dme = 0x09000000u, thread = 0x09010000u, session = 0x09020000u, ui = 0x09030000u, root = 0x09031000u;
        g.test_clock_ms = 1'000'000;
        const auto game = [&](const char *app, std::uint32_t state, bool active, bool server) {
            memory.store32(dme_pointer, app ? dme : 0u);
            memory.store32(dme_thread_pointer, thread);
            memory.store32(session_pointer, session);
            memory.store32(dme + dme_room_state, state);
            for (std::uint32_t i = 0; i < 40u; ++i) memory.store8(dme + dme_app_name + i, 0);
            for (std::uint32_t i = 0; app && app[i]; ++i) memory.store8(dme + dme_app_name + i, static_cast<std::uint8_t>(app[i]));
            memory.store32(thread + thread_active, active ? 1u : 0u);
            memory.store32(session + session_server, server ? 1u : 0u);
        };
        const std::array<std::uint32_t, 7> stored{0, 1, 2, 3, 3, 1, 1};
        for (std::size_t i = 0; i < stored.size(); ++i) memory.store32(session + session_rules + static_cast<std::uint32_t>(i * 4), stored[i]);
        memory.store32(ui + 0x20u, root);
        // One game frame: 16 ms and two steps of the display counter.
        std::uint64_t vblank = 100;
        const auto frames = [&](std::uint64_t count) {
            for (std::uint64_t i = 0; i < count; ++i) {
                g.test_clock_ms += 16;
                online_frame(memory, ++vblank);
                online_frame(memory, ++vblank);
            }
        };
        const auto seconds = [&](unsigned count) { frames(count * 63u); };
        // One frame of the search page while it stays on screen.
        const auto search_frame = [&] {
            const bool held = online_join_hold(memory, ui);
            online_join_result(memory, 0);
            online_join_page(memory, page_stay);
            frames(1);
            return held;
        };
        const auto press = [&](std::uint32_t buttons) {
            memory.store32(root + 0x40u, buttons);
            const bool held = search_frame();
            memory.store32(root + 0x40u, 0);
            search_frame();
            return held;
        };
        // The game's own search succeeds: the page hands over to "Game room is found".
        const auto found = [&] {
            online_join_hold(memory, ui);
            online_join_result(memory, 1);
            online_join_page(memory, 18);
        };
        const auto rows = [&](std::initializer_list<Row> list) {
            std::lock_guard lock(g.mutex);
            g.net_rooms.clear();
            for (const auto &row : list) {
                lobby::Room room{};
                room.id = row.id; room.name = row.name; room.state = row.state; room.players = row.players; room.max = row.max;
                g.net_rooms.push_back(room);
            }
            rebuild_rows_locked(now_ms());
        };
        const auto row = [](const char *id, const char *name, const char *state, int players) {
            Row value{};
            value.id = id; value.name = name; value.state = state; value.players = players; value.max = 6; value.internet = true;
            return value;
        };

        // Hosting: created, listed, started, finished, left.
        game("Kokaku PSP Lobby", 13, false, false);
        frames(1);
        online_create_result(memory, 6);
        require(g.creating && !g.session.active, "Create in progress opened a room");
        // A Create page that vanishes half-way stops counting, after a moment.
        for (std::uint64_t i = 0; i < hook_silence_vblanks; ++i) online_frame(memory, ++vblank);
        require(g.creating, "Create was forgotten too early");
        online_frame(memory, ++vblank);
        require(!g.creating, "A vanished Create page kept creating");
        online_create_result(memory, 6);
        game("Kokaku PSP Lobby", 12, true, true);
        online_create_result(memory, 7);
        require(g.session.active && g.session.hosting && is_hex16(g.session.id) && is_hex16(g.session.key) &&
                g.session.id != g.session.key && g.session.state == "open" && g.session.name == "Tester's room" &&
                g.session.rules == std::vector<int>({0, 1, 2, 3, 3, 1, 1}) && g.announce && g.apply_session, "Room was not created");
        const std::string room_id = g.session.id;
        OnlineView view{};
        frames(2);
        require(online_view(view) && !view.browser && view.banner == "LAN room  1/6", "Host banner");
        g.announce = false;
        game("Kokaku PSP LobbyEnd", 12, true, true);
        frames(1);
        require(g.session.state == "playing" && g.announce && !online_view(view), "Match start did not close the room to newcomers");
        game(nullptr, 0, false, true);
        seconds(20);
        require(g.session.active && g.session.state == "playing", "Loading a match dropped the room");
        game("Kokaku PSP GameU36c27bd4 M0", 0, true, true);
        seconds(120);
        require(g.session.active && g.session.state == "playing", "Match dropped the room");
        // With map rotation the next map starts without passing through the menu.
        game(nullptr, 0, false, true);
        seconds(5);
        game("Kokaku PSP GameU34dd38ce M1", 0, true, true);
        seconds(120);
        require(g.session.active && g.session.id == room_id && g.session.state == "playing", "Map rotation dropped the room");
        // Exit on the Result screen leads back to the multiplayer menu: over.
        game(nullptr, 0, false, true);
        seconds(5);
        game("Kokaku PSP Lobby", 0xffffffffu, false, true);
        g.announce = false;
        frames(1);
        require(g.session.active && g.session.state == "closed" && g.announce, "A finished match stayed listed as running");
        seconds(4);
        require(!g.session.active && g.farewell == Farewell::Unhost && g.apply_session, "Returning to the menu after a match kept the room");
        g.farewell = Farewell::None;
        // A new room after that is a different room.
        online_create_result(memory, 6);
        game("Kokaku PSP Lobby", 12, true, true);
        online_create_result(memory, 7);
        frames(2);
        require(g.session.active && g.session.hosting && g.session.id != room_id && g.session.state == "open", "Second room");
        const std::string second_id = g.session.id;
        // The host backs out to the menu: unlisted at once, forgotten shortly after.
        game("Kokaku PSP Lobby", 12, false, true);
        g.announce = false;
        frames(1);
        require(g.session.active && g.session.state == "closed" && g.announce, "A room without its host stayed listed");
        seconds(2);
        require(g.session.active, "A room was dropped before its grace period");
        // Created again within that moment: still the same room, open again.
        online_create_result(memory, 6);
        game("Kokaku PSP Lobby", 12, true, true);
        online_create_result(memory, 7);
        frames(2);
        require(g.session.active && g.session.id == second_id && g.session.state == "open", "Room was not reopened");
        game("Kokaku PSP Lobby", 12, false, true);
        seconds(4);
        require(!g.session.active && g.farewell == Farewell::Unhost && g.apply_session, "Host leaving did not end the room");
        g.farewell = Farewell::None;

        // Join: the list replaces the blind search.
        game("Kokaku PSP Lobby", 0, false, false);
        rows({row("aaaaaaaaaaaaaaaa", "Open room", "open", 2), row("bbbbbbbbbbbbbbbb", "Second room", "open", 6),
              row("cccccccccccccccc", "Running", "playing", 3)});
        memory.store32(root + 0x40u, button_confirm); // still held from choosing Join
        require(online_join_hold(memory, ui) && g.browser && g.join == JoinStage::Idle, "Join did not open the room list");
        online_join_result(memory, 0);
        frames(1);
        require(online_join_hold(memory, ui) && g.join == JoinStage::Idle, "The press that chose Join selected a room");
        memory.store32(root + 0x40u, 0);
        online_join_hold(memory, ui);
        frames(1);
        require(online_view(view) && view.browser && view.rows.size() == 3 && view.cursor == 0 && view.rows[0].name == "Open room" &&
                view.rows[0].players == "2/6" && view.rows[0].tag == "NET" && view.rows[0].joinable && !view.rows[1].joinable &&
                view.rows[2].tag == "IN GAME" && !view.busy && view.headline == "Local network", "Room list view");
        require(press(button_down) && g.cursor == 1 && press(button_confirm) && g.join == JoinStage::Idle &&
                g.message == "That room is full.", "A full room was joinable");
        // A game frame may span several steps of the display counter (30 FPS, a
        // slow frame): the list stays open as long as the page keeps polling.
        for (int i = 0; i < 10; ++i) {
            g.test_clock_ms += 33;
            for (int j = 0; j < 6; ++j) online_frame(memory, ++vblank);
            require(online_join_hold(memory, ui) && g.browser && g.cursor == 1, "A slow frame closed the room list");
            online_join_page(memory, page_stay);
        }
        // A page that vanishes without handing over (the menu torn down) is
        // noticed after a moment of silence, not before.
        for (std::uint64_t i = 0; i < hook_silence_vblanks; ++i) online_frame(memory, ++vblank);
        require(g.browser && g.join_ui, "A short silence closed the room list");
        online_frame(memory, ++vblank);
        require(!g.browser && !g.join_ui && !online_view(view), "A vanished search page kept the room list open");
        require(online_join_hold(memory, ui) && g.browser && g.cursor == 0, "Room list did not reopen after the silence");
        frames(1);
        press(0);
        require(press(button_down) && g.cursor == 1, "Navigation after reopening");
        require(press(button_down) && g.cursor == 2 && press(button_confirm) && g.join == JoinStage::Idle, "A running match was joinable");
        require(press(button_down) && g.cursor == 0 && press(button_up) && g.cursor == 2 && press(button_up) && press(button_up) &&
                g.cursor == 0, "List navigation");
        require(press(button_confirm) && g.join == JoinStage::Requested && g.target.id == "aaaaaaaaaaaaaaaa", "Selecting a room");
        require(press(button_down) && g.cursor == 0 && online_view(view) && view.busy, "Navigation while connecting");
        // The lobby answers with the room key and its host.
        lobby::Event joined{};
        joined.kind = lobby::Event::Kind::Joined;
        joined.id = "aaaaaaaaaaaaaaaa";
        joined.key = "0102030405060708";
        joined.text = "Open room";
        lobby::Peer host{};
        host.mac = net::Mac{2, 0, 0, 0, 0, 9};
        host.host = true;
        host.candidates = {net::Endpoint{0x01020304u, 3658}};
        joined.peers.push_back(host);
        on_lobby_event(joined);
        require(g.session.active && !g.session.hosting && g.session.via_lobby && g.session.key == "0102030405060708" &&
                g.session.host == host.mac && g.peers.size() == 1 && g.join == JoinStage::Introduced && g.apply_session,
                "Lobby answer did not start the session");
        require(online_join_hold(memory, ui), "Search released before the host was reachable");
        mark_ready_locked(); // what the worker does once the host answers
        require(!online_join_hold(memory, ui), "Search was not released once connected");
        online_join_result(memory, 0);
        online_join_page(memory, page_stay);
        frames(1);
        require(online_view(view) && view.browser && view.busy, "The list closed while the game was still searching");
        // The game finds the room: its own page takes over at once.
        found();
        require(!g.browser && !g.join_ui && g.session.active && g.join == JoinStage::Idle && !online_view(view),
                "The room list still covers the page of the found room");
        game("Kokaku PSP Lobby", 9, true, false);
        frames(10);
        require(g.session.active && g.phase == GamePhase::Room, "Found room was not kept");
        // Re-Search from that screen: the room just found is dropped, the list returns.
        game("Kokaku PSP Lobby", 0, false, false);
        require(online_join_hold(memory, ui) && g.browser && !g.session.active && g.farewell == Farewell::Leave,
                "Re-Search kept the previous room");
        g.farewell = Farewell::None;
        frames(1);
        press(0);
        require(press(button_confirm) && g.join == JoinStage::Requested, "Joining again after Re-Search");
        on_lobby_event(joined);
        mark_ready_locked();
        // Found again, this time noticed only by the silence of the page.
        online_join_hold(memory, ui);
        online_join_result(memory, 1);
        game("Kokaku PSP Lobby", 9, true, false);
        frames(10);
        require(g.browser && g.session.active, "The list closed before the silence limit");
        frames(10);
        require(g.session.active && !g.browser && !g.join_ui, "Room was not found again");
        game("Kokaku PSP Lobby", 12, true, false);
        frames(10);
        require(online_view(view) && view.banner == "Open room", "Member banner");
        // Someone else joins and leaves.
        lobby::Event update{};
        update.kind = lobby::Event::Kind::PeerUpdate;
        update.mac = net::Mac{2, 0, 0, 0, 0, 10};
        update.peers.push_back(lobby::Peer{update.mac, "Third", {net::Endpoint{0x05060708u, 3658}}, false});
        on_lobby_event(update);
        require(g.peers.size() == 2 && g.peers[1].from_lobby, "New member was not added");
        update.kind = lobby::Event::Kind::PeerGone;
        on_lobby_event(update);
        require(g.peers.size() == 2 && !g.peers[1].from_lobby, "Departed member was dropped while possibly still playing");
        // The list comes back after a restart: the member asks to be listed in
        // its room again, and keeps asking while the list does not know the
        // room, because the host may only be a moment behind.
        lobby::Event back{};
        back.kind = lobby::Event::Kind::Connected;
        on_lobby_event(back);
        require(g.rejoin && g.session.active && g.session.via_lobby, "A member did not ask for its room after reconnecting");
        g.rejoin = false; // what the worker does once the request is sent
        lobby::Event gone{};
        gone.kind = lobby::Event::Kind::JoinFailed;
        gone.id = "aaaaaaaaaaaaaaaa";
        gone.text = "gone";
        on_lobby_event(gone);
        require(g.session.active && g.session.via_lobby && !g.rejoin && g.rejoin_at_ms != 0,
                "A member turned away once gave up its place on the list");
        const auto asked = g.rejoin_at_ms;
        due_rejoin_locked(asked - 1);
        require(!g.rejoin && g.rejoin_at_ms == asked, "A member asked again too early");
        due_rejoin_locked(asked);
        require(g.rejoin && g.rejoin_at_ms == 0, "A member never asked again");
        g.rejoin = false;
        // Soon for the first attempts, at long intervals after that, never not at all.
        for (int i = 0; i < rejoin_attempts + 2; ++i) {
            const auto before = now_ms();
            on_lobby_event(gone);
            const auto wait = g.rejoin_at_ms - before;
            require(i < rejoin_attempts - 1 ? wait <= rejoin_soon_ms + 100u : wait >= rejoin_later_ms,
                    "A member turned away asks again at the wrong pace");
        }
        on_lobby_event(joined);
        require(g.rejoin_at_ms == 0 && g.session.active && g.session.via_lobby && g.join == JoinStage::Idle && g.peers.size() == 2,
                "Being listed again did not settle the member");
        // Through a match: still a member while it runs and while maps change.
        game("Kokaku PSP LobbyEnd", 12, true, false);
        frames(5);
        game(nullptr, 0, false, false);
        seconds(20);
        game("Kokaku PSP GameU36c27bd4 M0", 0, true, false);
        seconds(120);
        require(g.session.active && !online_view(view), "Member lost the room during the match");
        // Exit on the Result screen: back in the menu, out of the room.
        game("Kokaku PSP Lobby", 0xffffffffu, false, false);
        seconds(2);
        require(g.session.active, "Member was dropped before the grace period");
        seconds(2);
        require(!g.session.active && g.farewell == Farewell::Leave && g.peers.empty(), "Leaving a finished match did not end the session");
        g.farewell = Farewell::None;
        // Join after a match shows the list like any other time.
        require(online_join_hold(memory, ui) && g.browser, "Join after a match did not open the room list");
        frames(1);
        press(0);
        require(press(button_confirm) && g.join == JoinStage::Requested, "Joining again after a match");
        on_lobby_event(joined);
        mark_ready_locked();
        found();
        game("Kokaku PSP Lobby", 12, true, false);
        frames(10);
        require(g.session.active && !g.join_ui, "Room was not entered again");
        // Backing out of the room.
        game("Kokaku PSP Lobby", 12, false, false);
        seconds(4);
        require(!g.session.active && g.farewell == Farewell::Leave && g.peers.empty(), "Leaving the room did not end the session");
        g.farewell = Farewell::None;

        // Cancelling the list, and a search that finds nothing.
        game("Kokaku PSP Lobby", 0, false, false);
        require(online_join_hold(memory, ui) && g.browser, "Room list did not reopen");
        online_join_result(memory, 0);
        online_join_page(memory, page_stay);
        frames(1);
        require(g.browser && g.join_ui, "The room list closed while its page stayed");
        // Back: the page returns the one it came from, and the list is gone with it.
        online_join_hold(memory, ui);
        online_join_result(memory, 0);
        online_join_page(memory, 0);
        require(!g.browser && !g.join_ui && !g.session.active && !online_view(view), "Back did not close the room list at once");
        online_join_hold(memory, ui);
        frames(1);
        press(0);
        require(press(button_confirm) && g.join == JoinStage::Requested, "Second join attempt");
        on_lobby_event(joined);
        mark_ready_locked();
        require(!online_join_hold(memory, ui), "Second search was not released");
        online_join_result(memory, 2);
        require(!g.session.active && g.farewell == Farewell::Leave, "A room the game could not find was kept");
        online_join_page(memory, 11);
        require(!g.browser && !g.join_ui, "The list covers the page that says no room was found");
        g.farewell = Farewell::None;
        // Back pressed after connecting, while the game itself was still searching.
        online_join_hold(memory, ui);
        frames(1);
        press(0);
        press(button_confirm);
        on_lobby_event(joined);
        mark_ready_locked();
        require(!online_join_hold(memory, ui) && g.session.active, "Third search was not released");
        online_join_result(memory, 0);
        online_join_page(memory, 0);
        require(!g.session.active && g.farewell == Farewell::Leave && !g.browser, "A search cancelled after connecting kept the room");
        g.farewell = Farewell::None;
        // A refused join leaves the list usable.
        online_join_hold(memory, ui);
        frames(1);
        press(0);
        press(button_confirm);
        lobby::Event refused{};
        refused.kind = lobby::Event::Kind::JoinFailed;
        refused.id = "aaaaaaaaaaaaaaaa";
        refused.text = "full";
        on_lobby_event(refused);
        require(g.join == JoinStage::Failed && g.message == "That room is full." && !g.session.active && g.browser, "Refused join");
        require(press(button_down) && g.cursor == 1, "List was not usable after a refused join");
        online_join_hold(memory, ui);
        online_join_page(memory, 0);
        require(!g.browser, "Back after a refused join");

        // Creating a room while a member of another ends that membership.
        online_join_hold(memory, ui);
        frames(1);
        press(0);
        press(button_confirm);
        on_lobby_event(joined);
        mark_ready_locked();
        found();
        game("Kokaku PSP Lobby", 12, true, false);
        frames(10);
        require(g.session.active && !g.session.hosting, "Setup for create-after-join");
        game("Kokaku PSP Lobby", 13, false, false);
        online_create_result(memory, 6);
        require(!g.session.active && g.farewell == Farewell::Leave, "Creating a room kept the old membership");
        game("Kokaku PSP Lobby", 12, true, true);
        online_create_result(memory, 7);
        require(g.session.active && g.session.hosting, "Create after join");

        // LAN-only mode keeps the original behaviour: no list, no hooks.
        online_shutdown();
        network_config().online = false;
        require(!online_join_hold(memory, ui) && !online_view(view), "LAN-only mode showed the room list");
        online_create_result(memory, 7);
        require(!g.session.active, "LAN-only mode created an online room");
        online_adhoc_connected("DEFAULT");
        require(g.legacy_connected && g.legacy_group == "DEFAULT" && g.apply_session, "LAN group was not joined");
        online_adhoc_disconnected();
        require(!g.legacy_connected, "LAN group was not left");

        require(online_rules_text({0, 0, 2, 3, 3, 1, 1}) == "Player Team Battle, Uptown, 15 min, 15 frags, FF 75%, Lock On, Map rotation" &&
                online_rules_text({3, 7, 5, 6, 5, 0, 0}) == "Tachikoma Battle, Underground Arena, 60 min, no frag limit, FF 200%" &&
                online_rules_text({9, 9, 9, 9, 9, 9, 9}).empty() && online_rules_text({1, 2}).empty() &&
                rule_map({1, 4}) == "Container Pier" && rule_mode({2, 0}) == "Tachi Team" && rule_map({}).empty(), "Rules text");
        online_shutdown();
        g.test_mode = false;
        network_config() = saved;
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        online_shutdown();
        g.test_mode = false;
        network_config() = saved;
        return false;
    }
}

} // namespace kokaku
