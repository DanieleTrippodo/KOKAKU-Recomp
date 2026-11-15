#pragma once

// Peer-to-peer datagram transport for the emulated PSP ad hoc network.
//
// Every guest PDP socket is multiplexed over one host UDP socket. That socket
// outlives the guest's own network start/stop cycles, so NAT mappings and
// connected peers survive the teardown the game performs between the lobby and
// a match. Peers are introduced from outside (LAN beacons or the online lobby)
// as a MAC plus candidate addresses; the transport proves a working path to
// each of them by exchanging punch packets and then carries PDP datagrams.

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kokaku::net {

using Mac = std::array<std::uint8_t, 6>;
using SessionKey = std::array<std::uint8_t, 8>;
inline constexpr Mac broadcast_mac{0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

struct Endpoint {
    std::uint32_t ip{};   // host byte order
    std::uint16_t port{};
    friend bool operator==(const Endpoint &, const Endpoint &) = default;
    [[nodiscard]] bool valid() const noexcept { return ip != 0u && port != 0u; }
};

std::string ip_text(std::uint32_t ip);
std::string endpoint_text(const Endpoint &endpoint);
std::optional<std::uint32_t> parse_ipv4(std::string_view text);
std::string mac_text(const Mac &mac);
std::optional<Mac> parse_mac(std::string_view text);

struct LocalAddress {
    std::uint32_t ip{};
    std::uint32_t broadcast{};
};
// IPv4 addresses of the adapters that are up, without loopback.
std::vector<LocalAddress> local_ipv4_addresses();
// Blocking DNS lookup; call it away from the game thread.
std::optional<Endpoint> resolve_ipv4(const std::string &host, std::uint16_t port);

// Datagram I/O. The real implementation is a UDP socket; tests substitute a
// simulated network with address translators in between.
class Datagram {
public:
    virtual ~Datagram() = default;
    virtual bool send(const Endpoint &to, std::span<const std::uint8_t> data) = 0;
    // Bytes received, 0 when nothing arrived within the wait, negative once closed.
    virtual int receive(Endpoint &from, std::span<std::uint8_t> buffer, int wait_ms) = 0;
    [[nodiscard]] virtual std::uint16_t local_port() const = 0;
    virtual void close() = 0;
};

// Binds the preferred port, or any free one when it is taken and fallback is allowed.
std::unique_ptr<Datagram> open_udp(std::uint16_t preferred_port, bool allow_fallback, std::string &error);

// Diagnostics: what a distant player looks like from here. Everything that is
// sent leaves late, and some of it never does.
struct Impairment {
    unsigned delay_ms{};    // added to every datagram
    unsigned jitter_ms{};   // plus or minus, evenly spread
    double loss{};          // 0..1, each datagram on its own
};
// "delay[,jitter[,loss percent]]", for example "80,15,1".
std::optional<Impairment> parse_impairment(std::string_view text);

// Decides when each delayed datagram leaves. Nothing overtakes what was sent
// before it, as on a single route.
class DelayLine {
public:
    DelayLine(const Impairment &impairment, std::uint64_t seed);
    // False when the datagram is lost.
    bool push(std::uint64_t now_ms, const Endpoint &to, std::span<const std::uint8_t> data);
    // The next datagram whose time has come.
    bool pop(std::uint64_t now_ms, Endpoint &to, std::vector<std::uint8_t> &data);
    // When the next one leaves; empty while nothing waits.
    [[nodiscard]] std::optional<std::uint64_t> next_due() const;
    [[nodiscard]] std::size_t waiting() const noexcept { return queue_.size(); }

private:
    struct Item {
        std::uint64_t due{};
        Endpoint to{};
        std::vector<std::uint8_t> data;
    };
    Impairment impairment_;
    std::mt19937_64 random_;
    std::deque<Item> queue_;
};

// The same socket with its sends going through a DelayLine. Receiving is not
// touched: the other side delays its own half of the way.
std::unique_ptr<Datagram> open_impaired(std::unique_ptr<Datagram> socket, const Impairment &impairment);

enum class NatType { Unknown, Open, Cone, Symmetric };
const char *nat_type_name(NatType type);

struct PeerDescription {
    Mac mac{};
    std::string nickname;
    std::vector<Endpoint> candidates;
    bool host{};
};

struct PeerStatus {
    Mac mac{};
    std::string nickname;
    bool host{};
    bool connected{};   // a direct path answered
    bool relayed{};     // no direct path, but the room host forwards for us
    Endpoint endpoint{};
    int rtt_ms{-1};
};

struct ReceivedDatagram {
    Mac source{};
    std::uint16_t source_port{};
    std::vector<std::uint8_t> payload;
};

// Wire envelope, exposed for tests and diagnostics.
namespace wire {
inline constexpr std::size_t header_size = 32;
enum class Type : std::uint8_t { Data = 1, Punch = 2, PunchAck = 3, Ping = 4, Pong = 5, Relay = 6, Bye = 7 };
inline constexpr std::uint8_t flag_relayed = 1;
struct Header {
    Type type{};
    std::uint8_t flags{};
    Mac source{};
    Mac destination{};
    std::uint16_t source_port{};
    std::uint16_t destination_port{};
    SessionKey session{};
    std::uint16_t length{};
};
std::vector<std::uint8_t> encode(const Header &header, std::span<const std::uint8_t> payload);
// The payload span points into `packet`.
bool decode(std::span<const std::uint8_t> packet, Header &header, std::span<const std::uint8_t> &payload);
} // namespace wire

class Transport {
public:
    Transport(std::unique_ptr<Datagram> socket, const Mac &mac);
    ~Transport();
    Transport(const Transport &) = delete;
    Transport &operator=(const Transport &) = delete;

    // Production: a background thread receives and runs the timers.
    void start();
    void stop();
    // Tests: handle everything already queued on the socket, then run the timers.
    void pump(std::uint64_t now_ms);

    // Packets carrying another key are ignored. A room host also forwards
    // datagrams between two members that cannot reach each other directly.
    void set_session(const SessionKey &key, bool relay_for_members);
    [[nodiscard]] SessionKey session() const;
    void set_peer(const PeerDescription &peer);
    // With keep_while_alive the peer stays until its path goes silent: the
    // lobby losing sight of a player must not cut a match in progress.
    void remove_peer(const Mac &mac, bool keep_while_alive);
    void clear_peers();
    [[nodiscard]] std::vector<PeerStatus> peers() const;
    [[nodiscard]] bool peer_reachable(const Mac &mac) const;

    bool pdp_open(std::uint16_t port, std::size_t buffer_bytes);
    void pdp_close(std::uint16_t port);
    void pdp_close_all();
    [[nodiscard]] bool pdp_is_open(std::uint16_t port) const;
    [[nodiscard]] std::size_t pdp_open_count() const;
    enum class SendResult { Sent, NoRoute, Failed };
    SendResult pdp_send(std::uint16_t source_port, const Mac &destination, std::uint16_t destination_port,
                        std::span<const std::uint8_t> payload);
    // Size of the next datagram; empty when nothing is queued.
    [[nodiscard]] std::optional<std::size_t> pdp_peek(std::uint16_t port) const;
    bool pdp_receive(std::uint16_t port, ReceivedDatagram &out);
    [[nodiscard]] std::size_t pdp_queued_bytes(std::uint16_t port) const;
    // Delivers a datagram as if a peer had sent it (loopback sends).
    void pdp_inject(std::uint16_t port, ReceivedDatagram datagram);

    void set_stun_servers(std::vector<Endpoint> servers);
    void set_local_addresses(std::vector<std::uint32_t> addresses);
    [[nodiscard]] Endpoint mapped_endpoint() const;
    [[nodiscard]] NatType nat_type() const;
    [[nodiscard]] std::uint16_t local_port() const;
    [[nodiscard]] const Mac &mac() const noexcept { return mac_; }

    struct Counters {
        std::uint64_t data_sent{}, data_received{}, relayed{}, rejected{}, overflowed{};
    };
    [[nodiscard]] Counters counters() const;

private:
    struct Peer {
        PeerDescription info;
        Endpoint active{};
        bool connected{};
        bool departed{};
        std::uint64_t punch_started_ms{}, last_punch_ms{}, last_receive_ms{}, last_ping_ms{};
        std::uint64_t nonce{};
        int rtt_ms{-1};
    };
    struct PdpPort {
        std::deque<ReceivedDatagram> queue;
        std::size_t bytes{};
        std::size_t limit{};
    };
    struct StunServer {
        Endpoint address{};
        std::array<std::uint8_t, 12> transaction{};
        Endpoint mapped{};
        std::uint64_t last_request_ms{};
        unsigned unanswered{};
        bool requested{};
    };

    void run();
    void handle(const Endpoint &from, std::span<const std::uint8_t> packet, std::uint64_t now);
    bool handle_stun(const Endpoint &from, std::span<const std::uint8_t> packet, std::uint64_t now);
    void service(std::uint64_t now);
    void service_stun(std::uint64_t now);
    void send_control(const Peer &peer, const Endpoint &to, wire::Type type, std::uint64_t value);
    void learn_candidate(Peer &peer, const Endpoint &endpoint);
    void deliver(std::uint16_t port, ReceivedDatagram &&datagram);
    [[nodiscard]] const Peer *relay_for(const Peer &peer) const;
    [[nodiscard]] Peer *find_peer(const Mac &mac);
    void update_nat_type();

    std::unique_ptr<Datagram> socket_;
    const Mac mac_;
    mutable std::mutex mutex_;
    SessionKey session_{};
    bool relay_for_members_{};
    std::vector<Peer> peers_;
    std::unordered_map<std::uint16_t, PdpPort> ports_;
    std::vector<StunServer> stun_;
    std::vector<std::uint32_t> local_addresses_;
    Endpoint mapped_{};
    NatType nat_type_{NatType::Unknown};
    Counters counters_{};
    std::uint64_t last_service_ms_{};
    std::atomic<std::uint64_t> now_ms_{};
    std::atomic<bool> running_{};
    std::thread thread_;
};

// UDP broadcast announcements on the local network.
class LanDiscovery {
public:
    using MakeBeacon = std::function<std::string()>;
    using OnBeacon = std::function<void(const std::string &payload, std::uint32_t sender_ip)>;
    LanDiscovery() = default;
    ~LanDiscovery();
    LanDiscovery(const LanDiscovery &) = delete;
    LanDiscovery &operator=(const LanDiscovery &) = delete;

    // An empty beacon string means "listen only".
    bool start(std::uint16_t base_port, std::uint16_t port_offset, MakeBeacon make_beacon, OnBeacon on_beacon);
    void stop();
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

private:
    void run(std::uint16_t base_port, std::uint16_t port_offset);
    std::unique_ptr<Datagram> socket_;
    MakeBeacon make_beacon_;
    OnBeacon on_beacon_;
    std::atomic<bool> running_{};
    std::thread thread_;
};

bool run_transport_self_tests(std::string &error);

} // namespace kokaku::net
