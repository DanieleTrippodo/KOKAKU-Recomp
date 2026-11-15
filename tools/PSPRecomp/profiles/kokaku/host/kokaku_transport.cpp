#include "kokaku_transport.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <mmsystem.h>
#include <mswsock.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
inline int closesocket(SOCKET s) { return ::close(s); }
#endif

namespace kokaku::net {
namespace {

constexpr std::uint64_t peer_silence_ms = 10'000;
constexpr std::uint64_t ping_interval_ms = 2'000;
constexpr std::uint64_t relay_after_ms = 4'000;
constexpr std::size_t max_candidates = 12;
constexpr std::size_t max_payload = 65'000;
constexpr std::uint32_t stun_cookie = 0x2112A442u;

std::uint64_t steady_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t random_u64() {
    static std::mutex mutex;
    static std::mt19937_64 engine{std::random_device{}() ^ (std::uint64_t(std::random_device{}()) << 32)};
    std::lock_guard lock(mutex);
    return engine();
}

void ensure_sockets() {
#ifdef _WIN32
    static const bool started = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
}

void put16(std::uint8_t *out, std::uint16_t value) { out[0] = static_cast<std::uint8_t>(value >> 8); out[1] = static_cast<std::uint8_t>(value); }
std::uint16_t get16(const std::uint8_t *in) { return static_cast<std::uint16_t>((in[0] << 8) | in[1]); }
void put64(std::uint8_t *out, std::uint64_t value) { for (int i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(value >> (56 - i * 8)); }
std::uint64_t get64(const std::uint8_t *in) { std::uint64_t value{}; for (int i = 0; i < 8; ++i) value = (value << 8) | in[i]; return value; }

sockaddr_in to_sockaddr(const Endpoint &endpoint) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(endpoint.ip);
    address.sin_port = htons(endpoint.port);
    return address;
}

class UdpSocket final : public Datagram {
public:
    static std::unique_ptr<UdpSocket> open(std::uint16_t port, bool allow_fallback, bool shared, std::string &error) {
        ensure_sockets();
        for (int attempt = 0; attempt < 2; ++attempt) {
            const SOCKET handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (handle == INVALID_SOCKET) { error = "socket() failed"; return nullptr; }
            int yes = 1;
            setsockopt(handle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&yes), sizeof(yes));
            if (shared) setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));
            int buffer = 512 * 1024;
            setsockopt(handle, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&buffer), sizeof(buffer));
#ifdef _WIN32
            // A punch towards a port that is not open yet comes back as ICMP
            // port-unreachable; it must not turn into WSAECONNRESET on receive.
            BOOL reset = FALSE;
            DWORD returned = 0;
            WSAIoctl(handle, SIO_UDP_CONNRESET, &reset, sizeof(reset), nullptr, 0, &returned, nullptr, nullptr);
#endif
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            local.sin_port = htons(attempt == 0 ? port : 0);
            if (::bind(handle, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0) {
                socklen_t length = sizeof(local);
                getsockname(handle, reinterpret_cast<sockaddr *>(&local), &length);
                return std::unique_ptr<UdpSocket>(new UdpSocket(handle, ntohs(local.sin_port)));
            }
            closesocket(handle);
            if (!allow_fallback) break;
        }
        error = "UDP port " + std::to_string(port) + " is not available";
        return nullptr;
    }

    ~UdpSocket() override { close(); }

    bool send(const Endpoint &to, std::span<const std::uint8_t> data) override {
        const SOCKET handle = handle_.load();
        if (handle == INVALID_SOCKET) return false;
        const auto address = to_sockaddr(to);
        return ::sendto(handle, reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()), 0,
                        reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == static_cast<int>(data.size());
    }

    int receive(Endpoint &from, std::span<std::uint8_t> buffer, int wait_ms) override {
        const SOCKET handle = handle_.load();
        if (handle == INVALID_SOCKET) return -1;
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(handle, &readable);
        timeval timeout{wait_ms / 1000, (wait_ms % 1000) * 1000};
        const int ready = ::select(static_cast<int>(handle) + 1, &readable, nullptr, nullptr, &timeout);
        if (ready == 0) return 0;
        if (ready < 0) return handle_.load() == INVALID_SOCKET ? -1 : 0;
        sockaddr_in sender{};
        socklen_t length = sizeof(sender);
        const int bytes = ::recvfrom(handle, reinterpret_cast<char *>(buffer.data()), static_cast<int>(buffer.size()), 0,
                                     reinterpret_cast<sockaddr *>(&sender), &length);
        if (bytes <= 0) return handle_.load() == INVALID_SOCKET ? -1 : 0;
        from.ip = ntohl(sender.sin_addr.s_addr);
        from.port = ntohs(sender.sin_port);
        return bytes;
    }

    [[nodiscard]] std::uint16_t local_port() const override { return port_; }

    void close() override {
        const SOCKET handle = handle_.exchange(INVALID_SOCKET);
        if (handle != INVALID_SOCKET) closesocket(handle);
    }

private:
    UdpSocket(SOCKET handle, std::uint16_t port) : handle_(handle), port_(port) {}
    std::atomic<SOCKET> handle_;
    std::uint16_t port_;
};

constexpr std::size_t delay_line_limit = 8192;

// Sends leave from a thread of their own, when the DelayLine says so.
class ImpairedSocket final : public Datagram {
public:
    ImpairedSocket(std::unique_ptr<Datagram> inner, const Impairment &impairment)
        : inner_(std::move(inner)), line_(impairment, random_u64()) {
#ifdef _WIN32
        timeBeginPeriod(1); // waits of a millisecond instead of the usual 15.6
#endif
        thread_ = std::thread([this] { run(); });
    }

    ~ImpairedSocket() override { close(); }

    bool send(const Endpoint &to, std::span<const std::uint8_t> data) override {
        std::lock_guard lock(mutex_);
        if (closed_) return false;
        // A lost datagram looks sent, as it would on a real route.
        line_.push(steady_ms(), to, data);
        wake_.notify_one();
        return true;
    }

    int receive(Endpoint &from, std::span<std::uint8_t> buffer, int wait_ms) override {
        return inner_->receive(from, buffer, wait_ms);
    }

    [[nodiscard]] std::uint16_t local_port() const override { return inner_->local_port(); }

    void close() override {
        {
            std::lock_guard lock(mutex_);
            if (closed_) return;
            closed_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
        // Whatever still waits, a goodbye usually, leaves at once.
        Endpoint to{};
        std::vector<std::uint8_t> data;
        while (line_.pop(~std::uint64_t{0}, to, data)) inner_->send(to, data);
        inner_->close();
#ifdef _WIN32
        timeEndPeriod(1);
#endif
    }

private:
    void run() {
        std::unique_lock lock(mutex_);
        while (!closed_) {
            const auto now = steady_ms();
            Endpoint to{};
            std::vector<std::uint8_t> data;
            if (line_.pop(now, to, data)) {
                lock.unlock();
                inner_->send(to, data);
                lock.lock();
                continue;
            }
            if (const auto due = line_.next_due()) wake_.wait_for(lock, std::chrono::milliseconds(*due - now));
            else wake_.wait(lock);
        }
    }

    std::unique_ptr<Datagram> inner_;
    std::mutex mutex_;
    std::condition_variable wake_;
    DelayLine line_;
    bool closed_{};
    std::thread thread_;
};

} // namespace

std::string ip_text(std::uint32_t ip) {
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u);
    return text;
}

std::string endpoint_text(const Endpoint &endpoint) {
    return ip_text(endpoint.ip) + ':' + std::to_string(endpoint.port);
}

std::optional<std::uint32_t> parse_ipv4(std::string_view text) {
    std::uint32_t value = 0;
    int parts = 0;
    std::size_t index = 0;
    while (index <= text.size()) {
        unsigned part = 0;
        std::size_t digits = 0;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            part = part * 10u + static_cast<unsigned>(text[index] - '0');
            if (++digits > 3 || part > 255u) return std::nullopt;
            ++index;
        }
        if (digits == 0) return std::nullopt;
        value = (value << 8) | part;
        ++parts;
        if (index == text.size()) break;
        if (text[index] != '.' || parts == 4) return std::nullopt;
        ++index;
    }
    return parts == 4 ? std::optional<std::uint32_t>(value) : std::nullopt;
}

std::string mac_text(const Mac &mac) {
    char text[18];
    std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return text;
}

std::optional<Mac> parse_mac(std::string_view text) {
    if (text.size() != 17) return std::nullopt;
    Mac mac{};
    for (std::size_t i = 0; i < 6; ++i) {
        unsigned value = 0;
        for (std::size_t j = 0; j < 2; ++j) {
            const char c = text[i * 3 + j];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
            else return std::nullopt;
        }
        if (i < 5 && text[i * 3 + 2] != ':') return std::nullopt;
        mac[i] = static_cast<std::uint8_t>(value);
    }
    return mac;
}

std::vector<LocalAddress> local_ipv4_addresses() {
    std::vector<LocalAddress> result;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<std::uint8_t> storage(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    auto *adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data());
    ULONG status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        storage.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data());
        status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    }
    if (status != NO_ERROR) return result;
    for (auto *adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto *unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto ip = ntohl(reinterpret_cast<sockaddr_in *>(unicast->Address.lpSockaddr)->sin_addr.s_addr);
            if ((ip >> 24) == 127u || (ip >> 16) == 0xA9FEu || ip == 0u) continue;
            const unsigned prefix = std::min<unsigned>(unicast->OnLinkPrefixLength, 32u);
            const std::uint32_t mask = prefix == 0u ? 0u : ~std::uint32_t{0} << (32u - prefix);
            result.push_back({ip, ip | ~mask});
        }
    }
#else
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return result;
    for (auto *entry = list; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || !(entry->ifa_flags & IFF_UP) ||
            (entry->ifa_flags & IFF_LOOPBACK)) continue;
        const auto ip = ntohl(reinterpret_cast<sockaddr_in *>(entry->ifa_addr)->sin_addr.s_addr);
        const auto mask = entry->ifa_netmask ? ntohl(reinterpret_cast<sockaddr_in *>(entry->ifa_netmask)->sin_addr.s_addr) : 0u;
        result.push_back({ip, ip | ~mask});
    }
    freeifaddrs(list);
#endif
    return result;
}

std::optional<Endpoint> resolve_ipv4(const std::string &host, std::uint16_t port) {
    ensure_sockets();
    if (const auto literal = parse_ipv4(host)) return Endpoint{*literal, port};
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) return std::nullopt;
    std::optional<Endpoint> result;
    for (auto *entry = found; entry; entry = entry->ai_next) {
        if (entry->ai_family != AF_INET || !entry->ai_addr) continue;
        result = Endpoint{ntohl(reinterpret_cast<sockaddr_in *>(entry->ai_addr)->sin_addr.s_addr), port};
        break;
    }
    freeaddrinfo(found);
    return result;
}

std::unique_ptr<Datagram> open_udp(std::uint16_t preferred_port, bool allow_fallback, std::string &error) {
    return UdpSocket::open(preferred_port, allow_fallback, false, error);
}

std::optional<Impairment> parse_impairment(std::string_view text) {
    double fields[3]{};
    std::size_t count = 0, begin = 0;
    for (;;) {
        const auto end = text.find(',', begin);
        const auto part = text.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        if (count == 3 || part.size() > 8) return std::nullopt;
        double value = 0, scale = 1;
        bool fraction = false, digits = false;
        for (const char c : part) {
            if (c == '.' && !fraction) { fraction = true; continue; }
            if (c < '0' || c > '9') return std::nullopt;
            digits = true;
            if (fraction) { scale /= 10; value += (c - '0') * scale; }
            else value = value * 10 + (c - '0');
        }
        if (!digits) return std::nullopt;
        fields[count++] = value;
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    if (fields[0] > 5'000 || fields[1] > 5'000 || fields[2] > 100) return std::nullopt;
    return Impairment{static_cast<unsigned>(fields[0]), static_cast<unsigned>(fields[1]), fields[2] / 100.0};
}

DelayLine::DelayLine(const Impairment &impairment, std::uint64_t seed) : impairment_(impairment), random_(seed) {}

bool DelayLine::push(std::uint64_t now_ms, const Endpoint &to, std::span<const std::uint8_t> data) {
    if (impairment_.loss > 0 && std::uniform_real_distribution<double>(0.0, 1.0)(random_) < impairment_.loss) return false;
    if (queue_.size() >= delay_line_limit) return false;
    std::int64_t delay = impairment_.delay_ms;
    if (impairment_.jitter_ms) {
        const auto jitter = static_cast<std::int64_t>(impairment_.jitter_ms);
        delay += std::uniform_int_distribution<std::int64_t>(-jitter, jitter)(random_);
    }
    queue_.push_back(Item{now_ms + static_cast<std::uint64_t>(std::max<std::int64_t>(delay, 0)), to, {data.begin(), data.end()}});
    return true;
}

bool DelayLine::pop(std::uint64_t now_ms, Endpoint &to, std::vector<std::uint8_t> &data) {
    // Only the oldest datagram can leave, so a short delay drawn after a long
    // one waits behind it.
    if (queue_.empty() || queue_.front().due > now_ms) return false;
    to = queue_.front().to;
    data = std::move(queue_.front().data);
    queue_.pop_front();
    return true;
}

std::optional<std::uint64_t> DelayLine::next_due() const {
    if (queue_.empty()) return std::nullopt;
    return queue_.front().due;
}

std::unique_ptr<Datagram> open_impaired(std::unique_ptr<Datagram> socket, const Impairment &impairment) {
    if (!socket) return nullptr;
    return std::make_unique<ImpairedSocket>(std::move(socket), impairment);
}

const char *nat_type_name(NatType type) {
    switch (type) {
    case NatType::Open: return "Open";
    case NatType::Cone: return "Moderate";
    case NatType::Symmetric: return "Strict";
    default: return "Unknown";
    }
}

namespace wire {

std::vector<std::uint8_t> encode(const Header &header, std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> packet(header_size + payload.size());
    std::memcpy(packet.data(), "KNP2", 4);
    packet[4] = static_cast<std::uint8_t>(header.type);
    packet[5] = header.flags;
    std::memcpy(packet.data() + 6, header.source.data(), 6);
    std::memcpy(packet.data() + 12, header.destination.data(), 6);
    put16(packet.data() + 18, header.source_port);
    put16(packet.data() + 20, header.destination_port);
    std::memcpy(packet.data() + 22, header.session.data(), 8);
    put16(packet.data() + 30, static_cast<std::uint16_t>(payload.size()));
    if (!payload.empty()) std::memcpy(packet.data() + header_size, payload.data(), payload.size());
    return packet;
}

bool decode(std::span<const std::uint8_t> packet, Header &header, std::span<const std::uint8_t> &payload) {
    if (packet.size() < header_size || std::memcmp(packet.data(), "KNP2", 4) != 0) return false;
    const auto type = packet[4];
    if (type < static_cast<std::uint8_t>(Type::Data) || type > static_cast<std::uint8_t>(Type::Bye)) return false;
    header.type = static_cast<Type>(type);
    header.flags = packet[5];
    std::memcpy(header.source.data(), packet.data() + 6, 6);
    std::memcpy(header.destination.data(), packet.data() + 12, 6);
    header.source_port = get16(packet.data() + 18);
    header.destination_port = get16(packet.data() + 20);
    std::memcpy(header.session.data(), packet.data() + 22, 8);
    header.length = get16(packet.data() + 30);
    if (header.length != packet.size() - header_size) return false;
    payload = packet.subspan(header_size);
    return true;
}

} // namespace wire

Transport::Transport(std::unique_ptr<Datagram> socket, const Mac &mac) : socket_(std::move(socket)), mac_(mac) {
    now_ms_.store(steady_ms());
}

Transport::~Transport() { stop(); }

void Transport::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void Transport::stop() {
    running_.store(false);
    {
        std::lock_guard lock(mutex_);
        for (const auto &peer : peers_)
            if (peer.connected) send_control(peer, peer.active, wire::Type::Bye, 0);
    }
    if (socket_) socket_->close();
    if (thread_.joinable()) thread_.join();
}

void Transport::run() {
    std::vector<std::uint8_t> buffer(65536);
    while (running_.load(std::memory_order_relaxed)) {
        Endpoint from{};
        const int bytes = socket_->receive(from, buffer, 10);
        if (bytes < 0) break;
        const auto now = steady_ms();
        now_ms_.store(now, std::memory_order_relaxed);
        std::lock_guard lock(mutex_);
        if (bytes > 0) handle(from, std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(bytes)), now);
        if (now - last_service_ms_ >= 20u) {
            last_service_ms_ = now;
            service(now);
        }
    }
}

void Transport::pump(std::uint64_t now) {
    now_ms_.store(now);
    std::vector<std::uint8_t> buffer(65536);
    for (;;) {
        Endpoint from{};
        const int bytes = socket_->receive(from, buffer, 0);
        if (bytes <= 0) break;
        std::lock_guard lock(mutex_);
        handle(from, std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(bytes)), now);
    }
    std::lock_guard lock(mutex_);
    service(now);
}

void Transport::set_session(const SessionKey &key, bool relay_for_members) {
    std::lock_guard lock(mutex_);
    session_ = key;
    relay_for_members_ = relay_for_members;
}

SessionKey Transport::session() const {
    std::lock_guard lock(mutex_);
    return session_;
}

Transport::Peer *Transport::find_peer(const Mac &mac) {
    for (auto &peer : peers_)
        if (peer.info.mac == mac) return &peer;
    return nullptr;
}

void Transport::learn_candidate(Peer &peer, const Endpoint &endpoint) {
    auto &candidates = peer.info.candidates;
    if (!endpoint.valid() || candidates.size() >= max_candidates ||
        std::find(candidates.begin(), candidates.end(), endpoint) != candidates.end()) return;
    candidates.push_back(endpoint);
}

void Transport::set_peer(const PeerDescription &description) {
    if (description.mac == mac_ || description.mac == broadcast_mac) return;
    std::lock_guard lock(mutex_);
    const auto now = now_ms_.load();
    if (Peer *peer = find_peer(description.mac)) {
        peer->info.nickname = description.nickname;
        peer->info.host = description.host;
        peer->departed = false;
        bool added = false;
        for (const auto &candidate : description.candidates) {
            const auto before = peer->info.candidates.size();
            learn_candidate(*peer, candidate);
            added |= peer->info.candidates.size() != before;
        }
        // New addresses deserve the fast punch cadence again.
        if (added && !peer->connected) {
            peer->punch_started_ms = now;
            peer->last_punch_ms = 0;
        }
        return;
    }
    Peer peer{};
    peer.info.mac = description.mac;
    peer.info.nickname = description.nickname;
    peer.info.host = description.host;
    for (const auto &candidate : description.candidates) learn_candidate(peer, candidate);
    peer.nonce = random_u64() | 1u;
    peer.punch_started_ms = now;
    peers_.push_back(std::move(peer));
}

void Transport::remove_peer(const Mac &mac, bool keep_while_alive) {
    std::lock_guard lock(mutex_);
    for (auto it = peers_.begin(); it != peers_.end(); ++it) {
        if (it->info.mac != mac) continue;
        if (keep_while_alive && it->connected) {
            it->departed = true;
        } else {
            if (it->connected) send_control(*it, it->active, wire::Type::Bye, 0);
            peers_.erase(it);
        }
        return;
    }
}

void Transport::clear_peers() {
    std::lock_guard lock(mutex_);
    for (const auto &peer : peers_)
        if (peer.connected) send_control(peer, peer.active, wire::Type::Bye, 0);
    peers_.clear();
}

const Transport::Peer *Transport::relay_for(const Peer &peer) const {
    if (relay_for_members_ || peer.connected || peer.info.host) return nullptr;
    if (now_ms_.load() - peer.punch_started_ms < relay_after_ms) return nullptr;
    for (const auto &candidate : peers_)
        if (candidate.info.host && candidate.connected) return &candidate;
    return nullptr;
}

std::vector<PeerStatus> Transport::peers() const {
    std::lock_guard lock(mutex_);
    std::vector<PeerStatus> result;
    result.reserve(peers_.size());
    for (const auto &peer : peers_) {
        PeerStatus status{};
        status.mac = peer.info.mac;
        status.nickname = peer.info.nickname;
        status.host = peer.info.host;
        status.connected = peer.connected;
        status.relayed = relay_for(peer) != nullptr;
        status.endpoint = peer.active;
        status.rtt_ms = peer.rtt_ms;
        result.push_back(std::move(status));
    }
    return result;
}

bool Transport::peer_reachable(const Mac &mac) const {
    std::lock_guard lock(mutex_);
    for (const auto &peer : peers_)
        if (peer.info.mac == mac) return peer.connected || relay_for(peer) != nullptr;
    return false;
}

bool Transport::pdp_open(std::uint16_t port, std::size_t buffer_bytes) {
    std::lock_guard lock(mutex_);
    if (port == 0 || ports_.contains(port)) return false;
    ports_[port].limit = std::max<std::size_t>(buffer_bytes, 2048);
    return true;
}

void Transport::pdp_close(std::uint16_t port) {
    std::lock_guard lock(mutex_);
    ports_.erase(port);
}

void Transport::pdp_close_all() {
    std::lock_guard lock(mutex_);
    ports_.clear();
}

bool Transport::pdp_is_open(std::uint16_t port) const {
    std::lock_guard lock(mutex_);
    return ports_.contains(port);
}

std::size_t Transport::pdp_open_count() const {
    std::lock_guard lock(mutex_);
    return ports_.size();
}

void Transport::deliver(std::uint16_t port, ReceivedDatagram &&datagram) {
    const auto found = ports_.find(port);
    if (found == ports_.end()) { ++counters_.rejected; return; }
    auto &queue = found->second;
    if (queue.bytes + datagram.payload.size() > queue.limit) { ++counters_.overflowed; return; }
    queue.bytes += datagram.payload.size();
    queue.queue.push_back(std::move(datagram));
    ++counters_.data_received;
}

void Transport::pdp_inject(std::uint16_t port, ReceivedDatagram datagram) {
    std::lock_guard lock(mutex_);
    deliver(port, std::move(datagram));
}

Transport::SendResult Transport::pdp_send(std::uint16_t source_port, const Mac &destination,
                                         std::uint16_t destination_port, std::span<const std::uint8_t> payload) {
    if (payload.size() > max_payload) return SendResult::Failed;
    std::lock_guard lock(mutex_);
    if (destination == mac_) {
        deliver(destination_port, ReceivedDatagram{mac_, source_port, {payload.begin(), payload.end()}});
        return SendResult::Sent;
    }
    wire::Header header{};
    header.type = wire::Type::Data;
    header.source = mac_;
    header.destination = destination;
    header.source_port = source_port;
    header.destination_port = destination_port;
    header.session = session_;
    const bool broadcast = destination == broadcast_mac;
    std::vector<std::uint8_t> direct;
    bool routed = false, failed = false;
    for (const auto &peer : peers_) {
        if (!broadcast && peer.info.mac != destination) continue;
        if (peer.connected) {
            if (direct.empty()) direct = wire::encode(header, payload);
            failed |= !socket_->send(peer.active, direct);
            routed = true;
        } else if (const Peer *relay = relay_for(peer)) {
            wire::Header forwarded = header;
            forwarded.type = wire::Type::Relay;
            forwarded.destination = peer.info.mac;
            failed |= !socket_->send(relay->active, wire::encode(forwarded, payload));
            routed = true;
        }
    }
    if (routed && !failed) ++counters_.data_sent;
    return failed ? SendResult::Failed : routed ? SendResult::Sent : SendResult::NoRoute;
}

std::optional<std::size_t> Transport::pdp_peek(std::uint16_t port) const {
    std::lock_guard lock(mutex_);
    const auto found = ports_.find(port);
    if (found == ports_.end() || found->second.queue.empty()) return std::nullopt;
    return found->second.queue.front().payload.size();
}

bool Transport::pdp_receive(std::uint16_t port, ReceivedDatagram &out) {
    std::lock_guard lock(mutex_);
    const auto found = ports_.find(port);
    if (found == ports_.end() || found->second.queue.empty()) return false;
    out = std::move(found->second.queue.front());
    found->second.queue.pop_front();
    found->second.bytes -= out.payload.size();
    return true;
}

std::size_t Transport::pdp_queued_bytes(std::uint16_t port) const {
    std::lock_guard lock(mutex_);
    const auto found = ports_.find(port);
    return found == ports_.end() ? 0u : found->second.bytes;
}

void Transport::send_control(const Peer &peer, const Endpoint &to, wire::Type type, std::uint64_t value) {
    if (!to.valid()) return;
    wire::Header header{};
    header.type = type;
    header.source = mac_;
    header.destination = peer.info.mac;
    header.session = session_;
    std::array<std::uint8_t, 8> payload{};
    put64(payload.data(), value);
    socket_->send(to, wire::encode(header, payload));
}

void Transport::handle(const Endpoint &from, std::span<const std::uint8_t> packet, std::uint64_t now) {
    if (handle_stun(from, packet, now)) return;
    wire::Header header{};
    std::span<const std::uint8_t> payload;
    if (!wire::decode(packet, header, payload) || header.session != session_ || header.source == mac_) {
        ++counters_.rejected;
        return;
    }
    Peer *peer = find_peer(header.source);
    if (!peer) { ++counters_.rejected; return; }
    const bool broadcast = header.destination == broadcast_mac;
    if (header.type == wire::Type::Relay) {
        // A member asks the room host to pass a datagram to another member.
        Peer *target = relay_for_members_ && !broadcast && header.destination != mac_ ? find_peer(header.destination) : nullptr;
        peer->last_receive_ms = now;
        if (!target || !target->connected) { ++counters_.rejected; return; }
        header.type = wire::Type::Data;
        header.flags |= wire::flag_relayed;
        socket_->send(target->active, wire::encode(header, payload));
        ++counters_.relayed;
        return;
    }
    if (!broadcast && header.destination != mac_) { ++counters_.rejected; return; }
    const bool relayed = (header.flags & wire::flag_relayed) != 0;
    if (!relayed) {
        peer->last_receive_ms = now;
        learn_candidate(*peer, from);
        // A router may give an established peer a new public port. Its own
        // traffic shows where it is now; probes do not count, they arrive on
        // every path at once.
        if (peer->connected && !(from == peer->active) &&
            (header.type == wire::Type::Data || header.type == wire::Type::Ping || header.type == wire::Type::Pong))
            peer->active = from;
    }
    const std::uint64_t value = payload.size() >= 8 ? get64(payload.data()) : 0;
    switch (header.type) {
    case wire::Type::Punch:
        send_control(*peer, from, wire::Type::PunchAck, value);
        // Try the path the peer just opened, without echoing every packet.
        if (!peer->connected && now - peer->last_punch_ms >= 100u) peer->last_punch_ms = 0;
        break;
    case wire::Type::PunchAck:
        if (value == peer->nonce && !peer->connected) {
            peer->connected = true;
            peer->active = from;
            peer->last_ping_ms = 0;
        }
        break;
    case wire::Type::Ping:
        send_control(*peer, from, wire::Type::Pong, value);
        break;
    case wire::Type::Pong:
        if (value <= now) peer->rtt_ms = static_cast<int>(std::min<std::uint64_t>(now - value, 9999));
        break;
    case wire::Type::Data:
        deliver(header.destination_port,
                ReceivedDatagram{header.source, header.source_port, {payload.begin(), payload.end()}});
        break;
    case wire::Type::Bye:
        peer->connected = false;
        peer->active = {};
        peer->rtt_ms = -1;
        peer->punch_started_ms = now;
        break;
    default:
        break;
    }
}

void Transport::service(std::uint64_t now) {
    for (auto it = peers_.begin(); it != peers_.end();) {
        Peer &peer = *it;
        if (peer.connected && now - peer.last_receive_ms > peer_silence_ms) {
            peer.connected = false;
            peer.active = {};
            peer.rtt_ms = -1;
            peer.punch_started_ms = now;
            peer.last_punch_ms = 0;
        }
        if (peer.departed && !peer.connected) {
            it = peers_.erase(it);
            continue;
        }
        if (peer.connected) {
            if (peer.last_ping_ms == 0 || now - peer.last_ping_ms >= ping_interval_ms) {
                peer.last_ping_ms = now;
                send_control(peer, peer.active, wire::Type::Ping, now);
            }
        } else {
            const auto elapsed = now - peer.punch_started_ms;
            const std::uint64_t interval = elapsed < 3'000 ? 200 : elapsed < 10'000 ? 500 : elapsed < 60'000 ? 1'000 : 5'000;
            if (peer.last_punch_ms == 0 || now - peer.last_punch_ms >= interval) {
                peer.last_punch_ms = now;
                for (const auto &candidate : peer.info.candidates)
                    send_control(peer, candidate, wire::Type::Punch, peer.nonce);
            }
        }
        ++it;
    }
    service_stun(now);
}

void Transport::set_stun_servers(std::vector<Endpoint> servers) {
    std::lock_guard lock(mutex_);
    stun_.clear();
    for (const auto &address : servers) {
        if (!address.valid()) continue;
        StunServer server{};
        server.address = address;
        stun_.push_back(server);
    }
    update_nat_type();
}

void Transport::set_local_addresses(std::vector<std::uint32_t> addresses) {
    std::lock_guard lock(mutex_);
    local_addresses_ = std::move(addresses);
    update_nat_type();
}

void Transport::service_stun(std::uint64_t now) {
    for (auto &server : stun_) {
        const bool settled = server.mapped.valid() && server.unanswered == 0;
        const std::uint64_t interval = settled ? 20'000 : server.unanswered < 6 ? 700 : 30'000;
        if (server.requested && now - server.last_request_ms < interval) continue;
        if (server.unanswered >= 6 && server.mapped.valid()) {
            server.mapped = {};
            update_nat_type();
        }
        std::array<std::uint8_t, 20> request{};
        request[1] = 0x01; // Binding Request
        request[4] = 0x21; request[5] = 0x12; request[6] = 0xA4; request[7] = 0x42;
        const auto a = random_u64(), b = random_u64();
        for (int i = 0; i < 8; ++i) server.transaction[i] = static_cast<std::uint8_t>(a >> (i * 8));
        for (int i = 0; i < 4; ++i) server.transaction[8 + i] = static_cast<std::uint8_t>(b >> (i * 8));
        std::memcpy(request.data() + 8, server.transaction.data(), 12);
        socket_->send(server.address, request);
        server.requested = true;
        server.last_request_ms = now;
        ++server.unanswered;
    }
}

bool Transport::handle_stun(const Endpoint &from, std::span<const std::uint8_t> packet, std::uint64_t) {
    StunServer *server = nullptr;
    for (auto &candidate : stun_)
        if (candidate.address == from) server = &candidate;
    if (!server || packet.size() < 20 || std::memcmp(packet.data(), "KNP2", 4) == 0) return false;
    if (packet[4] != 0x21 || packet[5] != 0x12 || packet[6] != 0xA4 || packet[7] != 0x42) return false;
    if (get16(packet.data()) != 0x0101u || std::memcmp(packet.data() + 8, server->transaction.data(), 12) != 0) return true;
    const std::size_t length = std::min<std::size_t>(get16(packet.data() + 2), packet.size() - 20);
    Endpoint mapped{};
    bool exclusive = false;
    for (std::size_t offset = 20; offset + 4 <= 20 + length;) {
        const auto type = get16(packet.data() + offset);
        const std::size_t size = get16(packet.data() + offset + 2);
        const std::uint8_t *value = packet.data() + offset + 4;
        if (offset + 4 + size > packet.size()) break;
        if (size >= 8 && value[1] == 0x01 && (type == 0x0020u || (type == 0x0001u && !exclusive))) {
            std::uint16_t port = get16(value + 2);
            std::uint32_t ip = (std::uint32_t(value[4]) << 24) | (std::uint32_t(value[5]) << 16) |
                               (std::uint32_t(value[6]) << 8) | value[7];
            if (type == 0x0020u) {
                port ^= static_cast<std::uint16_t>(stun_cookie >> 16);
                ip ^= stun_cookie;
                exclusive = true;
            }
            mapped = Endpoint{ip, port};
        }
        offset += 4 + ((size + 3) & ~std::size_t{3});
    }
    if (mapped.valid()) {
        server->mapped = mapped;
        server->unanswered = 0;
        update_nat_type();
    }
    return true;
}

void Transport::update_nat_type() {
    std::vector<Endpoint> seen;
    for (const auto &server : stun_)
        if (server.mapped.valid()) seen.push_back(server.mapped);
    if (seen.empty()) {
        mapped_ = {};
        nat_type_ = NatType::Unknown;
        return;
    }
    mapped_ = seen.front();
    const bool same = std::all_of(seen.begin(), seen.end(), [&](const Endpoint &e) { return e == seen.front(); });
    const bool local = std::find(local_addresses_.begin(), local_addresses_.end(), mapped_.ip) != local_addresses_.end();
    if (!same) nat_type_ = NatType::Symmetric;
    else if (local && mapped_.port == socket_->local_port()) nat_type_ = NatType::Open;
    else nat_type_ = seen.size() >= 2 ? NatType::Cone : NatType::Unknown;
}

Endpoint Transport::mapped_endpoint() const {
    std::lock_guard lock(mutex_);
    return mapped_;
}

NatType Transport::nat_type() const {
    std::lock_guard lock(mutex_);
    return nat_type_;
}

std::uint16_t Transport::local_port() const { return socket_->local_port(); }

Transport::Counters Transport::counters() const {
    std::lock_guard lock(mutex_);
    return counters_;
}

LanDiscovery::~LanDiscovery() { stop(); }

bool LanDiscovery::start(std::uint16_t base_port, std::uint16_t port_offset, MakeBeacon make_beacon, OnBeacon on_beacon) {
    if (running_.load()) return true;
    std::string error;
    auto socket = UdpSocket::open(static_cast<std::uint16_t>(base_port + port_offset), false, true, error);
    if (!socket) return false;
    socket_ = std::move(socket);
    make_beacon_ = std::move(make_beacon);
    on_beacon_ = std::move(on_beacon);
    running_.store(true);
    thread_ = std::thread([this, base_port, port_offset] { run(base_port, port_offset); });
    return true;
}

void LanDiscovery::stop() {
    running_.store(false);
    if (socket_) socket_->close();
    if (thread_.joinable()) thread_.join();
    socket_.reset();
}

void LanDiscovery::run(std::uint16_t base_port, std::uint16_t) {
    std::vector<std::uint8_t> buffer(2048);
    std::vector<LocalAddress> adapters;
    std::uint64_t last_beacon = 0, last_adapters = 0;
    while (running_.load(std::memory_order_relaxed)) {
        const auto now = steady_ms();
        if (last_adapters == 0 || now - last_adapters >= 5'000) {
            adapters = local_ipv4_addresses();
            last_adapters = now;
        }
        if (last_beacon == 0 || now - last_beacon >= 500) {
            last_beacon = now;
            const std::string beacon = make_beacon_ ? make_beacon_() : std::string{};
            if (!beacon.empty()) {
                const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t *>(beacon.data()), beacon.size());
                socket_->send(Endpoint{0xffffffffu, base_port}, bytes);
                // The limited broadcast leaves through one adapter only; reach
                // every attached network explicitly.
                for (const auto &adapter : adapters)
                    if (adapter.broadcast != 0xffffffffu) socket_->send(Endpoint{adapter.broadcast, base_port}, bytes);
                // Several instances on one PC listen on offset ports.
                for (std::uint16_t offset : {0, 1000, 2000, 3000})
                    socket_->send(Endpoint{0x7f000001u, static_cast<std::uint16_t>(base_port + offset)}, bytes);
            }
        }
        Endpoint from{};
        const int bytes = socket_->receive(from, buffer, 20);
        if (bytes < 0) break;
        if (bytes > 0 && on_beacon_)
            on_beacon_(std::string(reinterpret_cast<const char *>(buffer.data()), static_cast<std::size_t>(bytes)), from.ip);
    }
}

// ---------------------------------------------------------------------------
// Self tests: transports talk across a simulated internet with address
// translators, so hole punching is exercised without real routers.
namespace {

struct SimNat {
    enum class Kind { FullCone, RestrictedCone, PortRestricted, Symmetric };
    struct Mapping {
        Endpoint inside{};
        Endpoint remote{};              // Symmetric: one mapping per destination
        std::uint16_t outside_port{};
        std::vector<Endpoint> contacted;
    };
    Kind kind{Kind::PortRestricted};
    std::uint32_t public_ip{};
    std::uint16_t next_port{40000};
    std::vector<Mapping> mappings;
};

class SimNetwork;

class SimSocket final : public Datagram {
public:
    SimSocket(SimNetwork &network, Endpoint address, SimNat *nat);
    ~SimSocket() override;
    bool send(const Endpoint &to, std::span<const std::uint8_t> data) override;
    int receive(Endpoint &from, std::span<std::uint8_t> buffer, int) override {
        if (closed) return -1;
        if (inbox.empty()) return 0;
        auto [sender, data] = std::move(inbox.front());
        inbox.pop_front();
        from = sender;
        std::memcpy(buffer.data(), data.data(), std::min(buffer.size(), data.size()));
        return static_cast<int>(std::min(buffer.size(), data.size()));
    }
    [[nodiscard]] std::uint16_t local_port() const override { return address.port; }
    void close() override { closed = true; }

    SimNetwork &network;
    Endpoint address;
    SimNat *nat;
    std::deque<std::pair<Endpoint, std::vector<std::uint8_t>>> inbox;
    bool closed{};
    bool muted{};   // cable pulled: nothing leaves or arrives
};

class SimNetwork {
public:
    using Service = std::function<void(const Endpoint &from, std::span<const std::uint8_t> data)>;
    std::vector<SimSocket *> sockets;
    std::vector<std::pair<Endpoint, Service>> services;
    std::uint64_t forwarded{};

    SimSocket *inside(const SimNat *nat, const Endpoint &address) {
        for (auto *socket : sockets)
            if (socket->nat == nat && socket->address == address) return socket;
        return nullptr;
    }

    static void push(SimSocket *socket, const Endpoint &from, std::span<const std::uint8_t> data) {
        if (socket && !socket->closed && !socket->muted) socket->inbox.emplace_back(from, std::vector<std::uint8_t>(data.begin(), data.end()));
    }

    // A packet travelling on the public side towards `to`.
    void internet(const Endpoint &from, const Endpoint &to, std::span<const std::uint8_t> data) {
        ++forwarded;
        for (auto &[address, service] : services)
            if (address == to) { service(from, data); return; }
        if (auto *direct = inside(nullptr, to)) { push(direct, from, data); return; }
        for (auto *socket : sockets) {
            SimNat *nat = socket->nat;
            if (!nat || nat->public_ip != to.ip) continue;
            for (auto &mapping : nat->mappings) {
                if (mapping.outside_port != to.port) continue;
                bool allowed = false;
                switch (nat->kind) {
                case SimNat::Kind::FullCone: allowed = true; break;
                case SimNat::Kind::RestrictedCone:
                    allowed = std::any_of(mapping.contacted.begin(), mapping.contacted.end(),
                                          [&](const Endpoint &e) { return e.ip == from.ip; });
                    break;
                case SimNat::Kind::PortRestricted:
                    allowed = std::find(mapping.contacted.begin(), mapping.contacted.end(), from) != mapping.contacted.end();
                    break;
                case SimNat::Kind::Symmetric: allowed = mapping.remote == from; break;
                }
                if (allowed) push(inside(nat, mapping.inside), from, data);
                return;
            }
            return;
        }
    }

    void route(SimSocket &sender, const Endpoint &to, std::span<const std::uint8_t> data) {
        if (sender.muted || sender.closed) return;
        if (!sender.nat) { internet(sender.address, to, data); return; }
        if (auto *neighbour = inside(sender.nat, to)) { push(neighbour, sender.address, data); return; }
        SimNat &nat = *sender.nat;
        SimNat::Mapping *mapping = nullptr;
        for (auto &candidate : nat.mappings)
            if (candidate.inside == sender.address && (nat.kind != SimNat::Kind::Symmetric || candidate.remote == to))
                mapping = &candidate;
        if (!mapping) {
            nat.mappings.push_back({sender.address, nat.kind == SimNat::Kind::Symmetric ? to : Endpoint{}, nat.next_port++, {}});
            mapping = &nat.mappings.back();
        }
        if (std::find(mapping->contacted.begin(), mapping->contacted.end(), to) == mapping->contacted.end())
            mapping->contacted.push_back(to);
        if (to.ip == nat.public_ip) return; // no hairpin, like most home routers
        internet(Endpoint{nat.public_ip, mapping->outside_port}, to, data);
    }

    void add_stun(const Endpoint &address) {
        services.emplace_back(address, [this, address](const Endpoint &from, std::span<const std::uint8_t> request) {
            if (request.size() < 20 || get16(request.data()) != 0x0001u) return;
            std::array<std::uint8_t, 32> response{};
            response[0] = 0x01; response[1] = 0x01;
            put16(response.data() + 2, 12);
            std::memcpy(response.data() + 4, request.data() + 4, 16);
            put16(response.data() + 20, 0x0020);
            put16(response.data() + 22, 8);
            response[25] = 0x01;
            put16(response.data() + 26, static_cast<std::uint16_t>(from.port ^ (stun_cookie >> 16)));
            const std::uint32_t ip = from.ip ^ stun_cookie;
            response[28] = static_cast<std::uint8_t>(ip >> 24); response[29] = static_cast<std::uint8_t>(ip >> 16);
            response[30] = static_cast<std::uint8_t>(ip >> 8); response[31] = static_cast<std::uint8_t>(ip);
            internet(address, from, response);
        });
    }
};

SimSocket::SimSocket(SimNetwork &owner, Endpoint local, SimNat *translator) : network(owner), address(local), nat(translator) {
    network.sockets.push_back(this);
}
SimSocket::~SimSocket() { std::erase(network.sockets, this); }
bool SimSocket::send(const Endpoint &to, std::span<const std::uint8_t> data) {
    if (closed) return false;
    network.route(*this, to, data);
    return true;
}

constexpr Endpoint ep(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port) {
    return Endpoint{(std::uint32_t(a) << 24) | (std::uint32_t(b) << 16) | (std::uint32_t(c) << 8) | d, port};
}

struct SimNode {
    std::unique_ptr<Transport> transport;
    SimSocket *socket{};
    Mac mac{};
    [[nodiscard]] PeerStatus status(const SimNode &other) const {
        for (const auto &peer : transport->peers())
            if (peer.mac == other.mac) return peer;
        return {};
    }
    [[nodiscard]] bool knows(const SimNode &other) const {
        for (const auto &peer : transport->peers())
            if (peer.mac == other.mac) return true;
        return false;
    }
};

} // namespace

bool run_transport_self_tests(std::string &error) {
    const auto require = [](bool condition, const char *message) { if (!condition) throw std::runtime_error(message); };
    try {
        // Text helpers.
        require(ip_text(0xC0A80105u) == "192.168.1.5" && parse_ipv4("192.168.1.5") == 0xC0A80105u, "IPv4 text roundtrip");
        require(!parse_ipv4("256.1.1.1") && !parse_ipv4("1.2.3") && !parse_ipv4("1.2.3.4.5") && !parse_ipv4("1..2.3") &&
                !parse_ipv4("a.b.c.d") && !parse_ipv4(""), "Invalid IPv4 accepted");
        const Mac sample{0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
        require(mac_text(sample) == "02:aa:bb:cc:dd:ee" && parse_mac("02:AA:bb:cc:dd:ee") == sample, "MAC text roundtrip");
        require(!parse_mac("02:aa:bb:cc:dd") && !parse_mac("02-aa-bb-cc-dd-ee") && !parse_mac("02:aa:bb:cc:dd:eg"), "Invalid MAC accepted");

        // Envelope.
        {
            wire::Header header{};
            header.type = wire::Type::Data;
            header.flags = wire::flag_relayed;
            header.source = sample;
            header.destination = broadcast_mac;
            header.source_port = 3658;
            header.destination_port = 40001;
            header.session = SessionKey{1, 2, 3, 4, 5, 6, 7, 8};
            const std::array<std::uint8_t, 5> body{0, 1, 127, 128, 255};
            auto packet = wire::encode(header, body);
            wire::Header parsed{};
            std::span<const std::uint8_t> payload;
            require(packet.size() == wire::header_size + 5 && wire::decode(packet, parsed, payload), "Envelope did not decode");
            require(parsed.type == header.type && parsed.flags == header.flags && parsed.source == header.source &&
                    parsed.destination == header.destination && parsed.source_port == 3658 &&
                    parsed.destination_port == 40001 && parsed.session == header.session && parsed.length == 5 &&
                    std::equal(payload.begin(), payload.end(), body.begin(), body.end()), "Envelope fields changed");
            auto truncated = packet; truncated.pop_back();
            auto magic = packet; magic[3] = '1';
            auto type = packet; type[4] = 99;
            require(!wire::decode(truncated, parsed, payload) && !wire::decode(magic, parsed, payload) &&
                    !wire::decode(type, parsed, payload) &&
                    !wire::decode(std::span<const std::uint8_t>(packet.data(), 10), parsed, payload), "Malformed envelope accepted");
        }

        std::uint64_t now = 1000;
        const SessionKey key{'K', 'O', 'K', 'A', 'K', 'U', '0', '1'};
        const auto node = [&](SimNetwork &network, std::uint8_t id, Endpoint address, SimNat *nat, bool host = false) {
            auto socket = std::make_unique<SimSocket>(network, address, nat);
            SimNode result{};
            result.socket = socket.get();
            result.mac = Mac{0x02, 0, 0, 0, 0, id};
            result.transport = std::make_unique<Transport>(std::move(socket), result.mac);
            result.transport->pump(now);
            result.transport->set_session(key, host);
            return result;
        };
        const auto run = [&](std::initializer_list<SimNode *> nodes, std::uint64_t duration) {
            for (const auto end = now + duration; now < end; now += 20)
                for (auto *item : nodes) item->transport->pump(now);
        };
        const auto introduce = [](SimNode &to, const SimNode &about, std::vector<Endpoint> candidates, bool host = false) {
            to.transport->set_peer(PeerDescription{about.mac, "peer", std::move(candidates), host});
        };
        const std::array<std::uint8_t, 4> hello{'h', 'e', 'y', '!'};

        // Two public machines: connection, data, ports, limits.
        {
            SimNetwork network;
            auto a = node(network, 1, ep(1, 0, 0, 1, 3658), nullptr), b = node(network, 2, ep(1, 0, 0, 2, 3658), nullptr);
            require(a.transport->pdp_send(3658, broadcast_mac, 3658, hello) == Transport::SendResult::NoRoute &&
                    a.transport->pdp_send(3658, b.mac, 3658, hello) == Transport::SendResult::NoRoute, "Send without peers found a route");
            introduce(a, b, {b.socket->address});
            introduce(b, a, {a.socket->address});
            require(!a.status(b).connected, "Peer connected before any exchange");
            run({&a, &b}, 400);
            require(a.status(b).connected && b.status(a).connected && a.status(b).endpoint == b.socket->address,
                    "Public peers did not connect");
            run({&a, &b}, 2500);
            require(a.status(b).rtt_ms >= 0 && b.status(a).rtt_ms >= 0, "Round trip time was not measured");
            require(a.transport->pdp_open(3658, 65536) && b.transport->pdp_open(3658, 2048) &&
                    !a.transport->pdp_open(3658, 65536) && !a.transport->pdp_open(0, 100), "PDP port bookkeeping");
            require(a.transport->pdp_send(3658, b.mac, 3658, hello) == Transport::SendResult::Sent, "Unicast send");
            require(!b.transport->pdp_peek(3658), "Datagram arrived before the network ran");
            run({&a, &b}, 40);
            ReceivedDatagram received{};
            require(b.transport->pdp_peek(3658) == 4u && b.transport->pdp_queued_bytes(3658) == 4u, "Peek did not report the datagram");
            require(b.transport->pdp_receive(3658, received) && received.source == a.mac && received.source_port == 3658 &&
                    std::equal(received.payload.begin(), received.payload.end(), hello.begin(), hello.end()),
                    "Unicast payload or source identity changed");
            require(!b.transport->pdp_receive(3658, received) && b.transport->pdp_queued_bytes(3658) == 0u, "Queue not drained");
            require(b.transport->pdp_send(3658, broadcast_mac, 3658, hello) == Transport::SendResult::Sent, "Broadcast send");
            run({&a, &b}, 40);
            require(a.transport->pdp_receive(3658, received) && received.source == b.mac, "Broadcast not delivered");
            require(b.transport->pdp_peek(3658) == std::nullopt, "Broadcast came back to its sender");
            // Closed destination port: dropped, never queued elsewhere.
            const auto rejected = b.transport->counters().rejected;
            a.transport->pdp_send(3658, b.mac, 4000, hello);
            run({&a, &b}, 40);
            require(b.transport->counters().rejected == rejected + 1 && !b.transport->pdp_peek(3658), "Closed port accepted data");
            // Receive buffer limit (2048 bytes on b).
            const std::vector<std::uint8_t> large(1000, 0x5a);
            for (int i = 0; i < 3; ++i) a.transport->pdp_send(3658, b.mac, 3658, large);
            run({&a, &b}, 40);
            require(b.transport->pdp_queued_bytes(3658) == 2000u && b.transport->counters().overflowed == 1u, "Receive buffer limit");
            require(a.transport->pdp_send(3658, b.mac, 3658, std::vector<std::uint8_t>(70000)) == Transport::SendResult::Failed,
                    "Oversized datagram accepted");
            // A datagram to oneself is delivered locally.
            require(a.transport->pdp_send(3658, a.mac, 3658, hello) == Transport::SendResult::Sent &&
                    a.transport->pdp_receive(3658, received) && received.source == a.mac, "Loopback send");
            // Another room on the same addresses is ignored.
            auto stranger = node(network, 9, ep(1, 0, 0, 9, 3658), nullptr);
            stranger.transport->set_session(SessionKey{'O', 'T', 'H', 'E', 'R'}, false);
            introduce(stranger, a, {a.socket->address});
            introduce(a, stranger, {stranger.socket->address});
            run({&a, &b, &stranger}, 1000);
            require(!a.status(stranger).connected && !stranger.status(a).connected, "Different session keys connected");
            // The other side's router moves it to a new address mid-session.
            b.socket->address = ep(1, 0, 0, 22, 3700);
            run({&a, &b}, 2500);
            require(a.status(b).connected && a.status(b).endpoint == b.socket->address, "Peer address change was not followed");
            a.transport->pdp_send(3658, b.mac, 3658, hello);
            run({&a, &b}, 40);
            require(b.transport->pdp_peek(3658).has_value(), "No data after the peer address changed");
            // Goodbye is immediate; silence takes the timeout.
            b.transport->remove_peer(a.mac, false);
            run({&a, &b}, 40);
            require(!b.knows(a) && !a.status(b).connected, "Goodbye did not disconnect the other side");
            introduce(b, a, {a.socket->address});
            run({&a, &b}, 600);
            require(a.status(b).connected && b.status(a).connected, "Peers did not reconnect after goodbye");
            b.socket->muted = true;
            run({&a, &b}, 9000);
            require(a.status(b).connected, "Peer dropped before the silence timeout");
            run({&a, &b}, 2000);
            require(!a.status(b).connected, "Silent peer still connected");
            b.socket->muted = false;
            run({&a, &b}, 2500);
            require(a.status(b).connected && b.status(a).connected, "Peer did not recover after silence");
            // A player the lobby lost sight of stays while its path is alive.
            a.transport->remove_peer(b.mac, true);
            run({&a, &b}, 3000);
            require(a.knows(b) && a.status(b).connected, "Departed but alive peer was dropped");
            while (b.transport->pdp_receive(3658, received)) {}
            a.transport->pdp_send(3658, b.mac, 3658, hello);
            run({&a, &b}, 40);
            require(b.transport->pdp_receive(3658, received) && received.source == a.mac, "Departed peer lost its route");
            b.socket->muted = true;
            run({&a, &b}, 11000);
            require(!a.knows(b), "Departed silent peer was kept");
        }

        // Address discovery and NAT classification.
        {
            SimNetwork network;
            const auto stun1 = ep(9, 9, 9, 1, 3478), stun2 = ep(9, 9, 9, 2, 3478);
            network.add_stun(stun1);
            network.add_stun(stun2);
            SimNat cone{SimNat::Kind::PortRestricted, 0x05000001u}, symmetric{SimNat::Kind::Symmetric, 0x06000001u};
            auto open = node(network, 1, ep(1, 0, 0, 1, 3658), nullptr);
            auto behind = node(network, 2, ep(192, 168, 1, 10, 3658), &cone);
            auto strict = node(network, 3, ep(192, 168, 1, 10, 3658), &symmetric);
            auto single = node(network, 4, ep(1, 0, 0, 4, 3658), nullptr);
            auto offline = node(network, 5, ep(1, 0, 0, 5, 3658), nullptr);
            for (auto *item : {&open, &behind, &strict}) item->transport->set_stun_servers({stun1, stun2});
            open.transport->set_local_addresses({open.socket->address.ip});
            single.transport->set_stun_servers({stun1});
            offline.transport->set_stun_servers({ep(9, 9, 9, 3, 3478)});
            require(open.transport->nat_type() == NatType::Unknown && !open.transport->mapped_endpoint().valid(), "NAT known before STUN");
            run({&open, &behind, &strict, &single, &offline}, 200);
            require(open.transport->nat_type() == NatType::Open && open.transport->mapped_endpoint() == open.socket->address, "Open NAT type");
            require(behind.transport->nat_type() == NatType::Cone && behind.transport->mapped_endpoint() == ep(5, 0, 0, 1, 40000), "Cone NAT type");
            require(strict.transport->nat_type() == NatType::Symmetric && strict.transport->mapped_endpoint().ip == 0x06000001u, "Symmetric NAT type");
            require(single.transport->nat_type() == NatType::Unknown && single.transport->mapped_endpoint() == single.socket->address,
                    "One STUN answer must not classify the NAT");
            require(!offline.transport->mapped_endpoint().valid(), "Unanswered STUN produced an address");
            // The mapping is refreshed, and forgotten once the servers go quiet.
            const auto requests = network.forwarded;
            run({&behind}, 21'000);
            require(network.forwarded > requests && behind.transport->mapped_endpoint().valid(), "STUN mapping was not refreshed");
            network.services.clear();
            run({&behind}, 60'000);
            require(!behind.transport->mapped_endpoint().valid() && behind.transport->nat_type() == NatType::Unknown, "Lost STUN mapping was kept");
        }

        const auto mapped_candidates = [](SimNode &item) {
            return std::vector<Endpoint>{item.socket->address, item.transport->mapped_endpoint()};
        };
        const auto exchange = [&](SimNetwork &network, SimNode &a, SimNode &b) {
            (void)network;
            introduce(a, b, mapped_candidates(b));
            introduce(b, a, mapped_candidates(a));
        };
        const auto delivers = [&](SimNode &from, SimNode &to, std::initializer_list<SimNode *> all) {
            to.transport->pdp_open(3658, 65536);
            ReceivedDatagram drained{};
            while (to.transport->pdp_receive(3658, drained)) {}
            from.transport->pdp_send(3658, to.mac, 3658, hello);
            run(all, 60);
            return to.transport->pdp_receive(3658, drained) && drained.source == from.mac;
        };

        // Two home routers: both sides punch towards the STUN address.
        {
            SimNetwork network;
            const auto stun1 = ep(9, 9, 9, 1, 3478), stun2 = ep(9, 9, 9, 2, 3478);
            network.add_stun(stun1); network.add_stun(stun2);
            SimNat nat1{SimNat::Kind::PortRestricted, 0x05000001u}, nat2{SimNat::Kind::PortRestricted, 0x06000001u};
            auto a = node(network, 1, ep(192, 168, 1, 10, 3658), &nat1), b = node(network, 2, ep(192, 168, 1, 10, 3658), &nat2);
            for (auto *item : {&a, &b}) item->transport->set_stun_servers({stun1, stun2});
            run({&a, &b}, 200);
            exchange(network, a, b);
            run({&a, &b}, 1000);
            require(a.status(b).connected && b.status(a).connected, "Port-restricted routers did not punch through");
            require(a.status(b).endpoint == b.transport->mapped_endpoint(), "Connected through an unexpected address");
            require(delivers(a, b, {&a, &b}) && delivers(b, a, {&a, &b}), "No data across punched routers");
            // The hole stays open across a long idle period.
            run({&a, &b}, 120'000);
            require(a.status(b).connected && delivers(a, b, {&a, &b}), "Idle connection was lost");
        }

        // Same router, no hairpin: the private addresses must win.
        {
            SimNetwork network;
            const auto stun1 = ep(9, 9, 9, 1, 3478), stun2 = ep(9, 9, 9, 2, 3478);
            network.add_stun(stun1); network.add_stun(stun2);
            SimNat nat{SimNat::Kind::PortRestricted, 0x05000001u};
            auto a = node(network, 1, ep(192, 168, 1, 10, 3658), &nat), b = node(network, 2, ep(192, 168, 1, 11, 3658), &nat);
            for (auto *item : {&a, &b}) item->transport->set_stun_servers({stun1, stun2});
            run({&a, &b}, 200);
            exchange(network, a, b);
            run({&a, &b}, 1000);
            require(a.status(b).connected && a.status(b).endpoint == b.socket->address && delivers(a, b, {&a, &b}),
                    "Players behind one router did not connect over the LAN");
        }

        // A strict (symmetric) router reaches cone routers that filter by
        // address only, through the port the other side observes.
        for (const auto kind : {SimNat::Kind::FullCone, SimNat::Kind::RestrictedCone}) {
            SimNetwork network;
            const auto stun1 = ep(9, 9, 9, 1, 3478), stun2 = ep(9, 9, 9, 2, 3478);
            network.add_stun(stun1); network.add_stun(stun2);
            SimNat strict{SimNat::Kind::Symmetric, 0x05000001u}, cone{kind, 0x06000001u};
            auto a = node(network, 1, ep(192, 168, 1, 10, 3658), &strict), b = node(network, 2, ep(192, 168, 1, 10, 3658), &cone);
            for (auto *item : {&a, &b}) item->transport->set_stun_servers({stun1, stun2});
            run({&a, &b}, 200);
            exchange(network, a, b);
            run({&a, &b}, 1500);
            require(a.status(b).connected && b.status(a).connected && delivers(a, b, {&a, &b}) && delivers(b, a, {&a, &b}),
                    "Strict router did not reach an address-filtering router");
        }

        // Strict against port-restricted has no direct path. With a host both
        // can reach, the host forwards for that pair only.
        {
            SimNetwork network;
            const auto stun1 = ep(9, 9, 9, 1, 3478), stun2 = ep(9, 9, 9, 2, 3478);
            network.add_stun(stun1); network.add_stun(stun2);
            SimNat strict{SimNat::Kind::Symmetric, 0x05000001u}, restricted{SimNat::Kind::PortRestricted, 0x06000001u};
            auto host = node(network, 1, ep(1, 0, 0, 1, 3658), nullptr, true);
            auto a = node(network, 2, ep(192, 168, 1, 10, 3658), &strict), b = node(network, 3, ep(192, 168, 1, 10, 3658), &restricted);
            for (auto *item : {&host, &a, &b}) item->transport->set_stun_servers({stun1, stun2});
            host.transport->set_local_addresses({host.socket->address.ip});
            run({&host, &a, &b}, 200);
            introduce(a, host, mapped_candidates(host), true);
            introduce(b, host, mapped_candidates(host), true);
            introduce(host, a, mapped_candidates(a));
            introduce(host, b, mapped_candidates(b));
            exchange(network, a, b);
            run({&host, &a, &b}, 1500);
            require(host.status(a).connected && host.status(b).connected && a.status(host).connected && b.status(host).connected,
                    "Members did not reach the host");
            require(!a.status(b).connected && !b.status(a).connected, "Strict and port-restricted routers connected directly");
            require(!a.status(b).relayed && !a.transport->peer_reachable(b.mac), "Relay was used before direct attempts had time");
            run({&host, &a, &b}, 4000);
            require(a.status(b).relayed && b.status(a).relayed && a.transport->peer_reachable(b.mac), "Host relay was not offered");
            require(delivers(a, b, {&host, &a, &b}) && delivers(b, a, {&host, &a, &b}), "No data through the host relay");
            require(host.transport->counters().relayed == 2u, "Host did not count the forwarded datagrams");
            // Broadcast: the host hears it directly, the unreachable member through it.
            host.transport->pdp_open(3658, 65536);
            ReceivedDatagram received{};
            while (b.transport->pdp_receive(3658, received)) {}
            a.transport->pdp_send(3658, broadcast_mac, 3658, hello);
            run({&host, &a, &b}, 60);
            require(host.transport->pdp_receive(3658, received) && received.source == a.mac &&
                    b.transport->pdp_receive(3658, received) && received.source == a.mac &&
                    !host.transport->pdp_receive(3658, received) && !b.transport->pdp_receive(3658, received),
                    "Broadcast did not reach every member exactly once");
            // Only the host forwards, and only between its connected members.
            wire::Header forged{};
            forged.type = wire::Type::Relay;
            forged.source = host.mac;
            forged.destination = b.mac;
            forged.session = key;
            const auto rejected = a.transport->counters().rejected;
            host.socket->send(host.status(a).endpoint, wire::encode(forged, hello));
            run({&host, &a, &b}, 60);
            require(a.transport->counters().relayed == 0u && a.transport->counters().rejected == rejected + 1u &&
                    !b.transport->pdp_receive(3658, received), "A member forwarded traffic");
        }

        // Forwarding is the room host's job alone, even for a member that
        // could reach both sides.
        {
            SimNetwork network;
            auto a = node(network, 1, ep(1, 0, 0, 1, 3658), nullptr), b = node(network, 2, ep(1, 0, 0, 2, 3658), nullptr),
                 c = node(network, 3, ep(1, 0, 0, 3, 3658), nullptr);
            for (auto *from : {&a, &b, &c})
                for (auto *to : {&a, &b, &c})
                    if (from != to) introduce(*from, *to, {to->socket->address});
            run({&a, &b, &c}, 600);
            require(a.status(b).connected && a.status(c).connected && b.status(c).connected, "Three public peers did not connect");
            b.transport->pdp_open(3658, 65536);
            wire::Header request{};
            request.type = wire::Type::Relay;
            request.source = c.mac;
            request.destination = b.mac;
            request.source_port = 3658;
            request.destination_port = 3658;
            request.session = key;
            c.socket->send(a.socket->address, wire::encode(request, hello));
            run({&a, &b, &c}, 60);
            ReceivedDatagram received{};
            require(a.transport->counters().relayed == 0u && !b.transport->pdp_receive(3658, received), "A member forwarded traffic");
            a.transport->set_session(key, true);
            c.socket->send(a.socket->address, wire::encode(request, hello));
            run({&a, &b, &c}, 60);
            require(a.transport->counters().relayed == 1u && b.transport->pdp_receive(3658, received) && received.source == c.mac &&
                    received.payload.size() == hello.size(), "A host did not forward for its members");
        }

        // New candidates for a known peer restart the fast attempts.
        {
            SimNetwork network;
            auto a = node(network, 1, ep(1, 0, 0, 1, 3658), nullptr), b = node(network, 2, ep(1, 0, 0, 2, 3658), nullptr);
            introduce(a, b, {ep(1, 0, 0, 77, 3658)});
            introduce(b, a, {a.socket->address});
            run({&a, &b}, 300);
            // b's punches arrive from its real address, which a learns on its own.
            require(a.status(b).connected && a.status(b).endpoint == b.socket->address, "Observed source address was not used");
            auto c = node(network, 3, ep(1, 0, 0, 3, 3658), nullptr);
            introduce(a, c, {ep(1, 0, 0, 78, 3658)});
            run({&a, &c}, 70'000);
            require(!a.status(c).connected, "Connected to a wrong address");
            introduce(a, c, {c.socket->address});
            introduce(c, a, {a.socket->address});
            run({&a, &c}, 400);
            require(a.status(c).connected, "Updated candidates were not tried promptly");
            // Only an answer that echoes our own punch proves a path.
            SimSocket raw(network, ep(1, 0, 0, 50, 3658), nullptr);
            SimNode ghost{};
            ghost.mac = Mac{0x02, 0, 0, 0, 0, 50};
            introduce(a, ghost, {raw.address});
            wire::Header answer{};
            answer.type = wire::Type::PunchAck;
            answer.source = ghost.mac;
            answer.destination = a.mac;
            answer.session = key;
            const std::array<std::uint8_t, 8> guess{1, 2, 3, 4, 5, 6, 7, 8};
            raw.send(a.socket->address, wire::encode(answer, guess));
            run({&a}, 300);
            require(!a.status(ghost).connected, "A punch answer with the wrong value connected a peer");
            std::span<const std::uint8_t> nonce;
            wire::Header punch{};
            require(!raw.inbox.empty() && wire::decode(raw.inbox.front().second, punch, nonce) &&
                    punch.type == wire::Type::Punch && punch.destination == ghost.mac && nonce.size() == 8,
                    "No punch was sent to the candidate");
            raw.send(a.socket->address, wire::encode(answer, nonce));
            run({&a}, 60);
            require(a.status(ghost).connected && a.status(ghost).endpoint == raw.address, "A correct punch answer was ignored");
        }

        // Real sockets on loopback, with the background threads.
        {
            std::string socket_error;
            auto first = open_udp(0, true, socket_error), second = open_udp(0, true, socket_error);
            require(first && second, "Could not open loopback UDP sockets");
            const Endpoint first_address{0x7f000001u, first->local_port()}, second_address{0x7f000001u, second->local_port()};
            Transport a(std::move(first), Mac{2, 0, 0, 0, 1, 1}), b(std::move(second), Mac{2, 0, 0, 0, 1, 2});
            a.set_session(key, false);
            b.set_session(key, false);
            a.pdp_open(3658, 65536);
            b.pdp_open(3658, 65536);
            a.start();
            b.start();
            a.set_peer(PeerDescription{b.mac(), "b", {second_address}, false});
            b.set_peer(PeerDescription{a.mac(), "a", {first_address}, false});
            const auto wait_until = [](const std::function<bool()> &done) {
                for (int i = 0; i < 300 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return done();
            };
            require(wait_until([&] { return a.peer_reachable(b.mac()) && b.peer_reachable(a.mac()); }), "Loopback sockets did not connect");
            require(a.pdp_send(3658, b.mac(), 3658, hello) == Transport::SendResult::Sent, "Loopback socket send");
            ReceivedDatagram received{};
            require(wait_until([&] { return b.pdp_peek(3658).has_value(); }) && b.pdp_receive(3658, received) &&
                    received.source == a.mac() && received.payload.size() == 4, "Loopback socket receive");
            a.stop();
            require(wait_until([&] { const auto peers = b.peers(); return !peers.empty() && !peers[0].connected; }),
                    "Stopping a transport did not say goodbye");
            b.stop();
        }

        // Diagnostics: the imitation of a distant player.
        {
            const auto parsed = parse_impairment("80,15,1.5");
            require(parsed && parsed->delay_ms == 80u && parsed->jitter_ms == 15u && parsed->loss > 0.0149 && parsed->loss < 0.0151,
                    "Impairment text");
            const auto plain = parse_impairment("120");
            require(plain && plain->delay_ms == 120u && plain->jitter_ms == 0u && plain->loss == 0.0, "Impairment with a delay only");
            require(!parse_impairment("") && !parse_impairment("80,") && !parse_impairment("a") && !parse_impairment("80,1,2,3") &&
                    !parse_impairment("9000") && !parse_impairment("80,0,101") && !parse_impairment("-5") && !parse_impairment("."),
                    "Invalid impairment text accepted");

            const Endpoint first_target = ep(1, 2, 3, 4, 3658), second_target = ep(5, 6, 7, 8, 9);
            Endpoint to{};
            std::vector<std::uint8_t> data;
            const auto numbered = [](unsigned index) {
                return std::array<std::uint8_t, 2>{static_cast<std::uint8_t>(index >> 8), static_cast<std::uint8_t>(index)};
            };
            const auto number = [&] { return data.size() == 2 ? static_cast<unsigned>((data[0] << 8) | data[1]) : ~0u; };

            DelayLine fixed(Impairment{100, 0, 0.0}, 1);
            require(!fixed.next_due() && !fixed.pop(1'000'000, to, data), "Empty delay line returned a datagram");
            require(fixed.push(1'000, first_target, numbered(1)) && fixed.push(1'010, second_target, numbered(2)) &&
                    fixed.waiting() == 2u, "Delay line refused a datagram");
            require(fixed.next_due() == 1'100u && !fixed.pop(1'099, to, data), "Datagram left before its delay");
            require(fixed.pop(1'100, to, data) && to == first_target && number() == 1u, "Delayed datagram was changed");
            require(!fixed.pop(1'109, to, data) && fixed.pop(1'110, to, data) && to == second_target && number() == 2u &&
                    fixed.waiting() == 0u, "Second delayed datagram left at the wrong time");

            // Jitter spreads the delay within its bounds.
            DelayLine jittered(Impairment{50, 30, 0.0}, 7);
            for (unsigned i = 0; i < 400; ++i) jittered.push(10'000 + i * 100u, first_target, numbered(i));
            std::uint64_t lowest = ~std::uint64_t{0}, highest = 0;
            unsigned expected = 0;
            for (std::uint64_t now = 10'000; now < 10'000 + 400 * 100 + 200; ++now) {
                while (jittered.pop(now, to, data)) {
                    require(number() == expected, "Jitter reordered datagrams sent apart");
                    const auto delay = now - (10'000 + expected * 100u);
                    lowest = std::min(lowest, delay);
                    highest = std::max(highest, delay);
                    ++expected;
                }
            }
            require(expected == 400u && lowest >= 20u && lowest <= 30u && highest >= 70u && highest <= 80u, "Jitter outside its bounds");

            // A burst keeps its order: nothing overtakes what was sent before.
            DelayLine burst(Impairment{50, 30, 0.0}, 3);
            for (unsigned i = 0; i < 50; ++i) burst.push(5'000, first_target, numbered(i));
            expected = 0;
            for (std::uint64_t now = 5'000; now <= 5'080; ++now)
                while (burst.pop(now, to, data)) require(number() == expected++, "A burst was reordered");
            require(expected == 50u, "A burst was not delivered within the largest delay");

            // Loss is a share of the datagrams, and the line has an end.
            DelayLine lossy(Impairment{0, 0, 0.25}, 11);
            unsigned kept = 0;
            for (unsigned i = 0; i < 4'000; ++i) {
                if (!lossy.push(100, first_target, numbered(i))) continue;
                ++kept;
                require(lossy.pop(100, to, data) && number() == i, "A datagram without delay did not leave at once");
            }
            require(kept > 2'800u && kept < 3'200u, "Loss rate is off");
            DelayLine full(Impairment{1'000, 0, 0.0}, 1);
            unsigned accepted = 0;
            for (unsigned i = 0; i < delay_line_limit + 10; ++i) accepted += full.push(0, first_target, numbered(i)) ? 1u : 0u;
            require(accepted == delay_line_limit, "Delay line grew without limit");
        }
        // The same over real sockets: the round trip takes both delays.
        {
            std::string socket_error;
            auto first = open_udp(0, true, socket_error), second = open_udp(0, true, socket_error);
            require(first && second, "Could not open loopback UDP sockets");
            const Endpoint first_address{0x7f000001u, first->local_port()}, second_address{0x7f000001u, second->local_port()};
            const Impairment slow{40, 0, 0.0};
            Transport a(open_impaired(std::move(first), slow), Mac{2, 0, 0, 0, 2, 1});
            Transport b(open_impaired(std::move(second), slow), Mac{2, 0, 0, 0, 2, 2});
            require(a.local_port() == first_address.port && b.local_port() == second_address.port, "Impaired socket hides its port");
            a.set_session(key, false);
            b.set_session(key, false);
            a.pdp_open(3658, 65536);
            b.pdp_open(3658, 65536);
            a.start();
            b.start();
            a.set_peer(PeerDescription{b.mac(), "b", {second_address}, false});
            b.set_peer(PeerDescription{a.mac(), "a", {first_address}, false});
            const auto wait_until = [](const std::function<bool()> &done) {
                for (int i = 0; i < 500 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return done();
            };
            const auto rtt = [&] { const auto peers = a.peers(); return peers.empty() ? -1 : peers[0].rtt_ms; };
            require(wait_until([&] { return rtt() >= 0; }), "Impaired sockets did not connect");
            require(rtt() >= 79 && rtt() < 1'000, "Round trip does not show the delay of both directions");
            const auto sent_at = steady_ms();
            require(a.pdp_send(3658, b.mac(), 3658, hello) == Transport::SendResult::Sent, "Impaired socket send");
            ReceivedDatagram received{};
            require(wait_until([&] { return b.pdp_peek(3658).has_value(); }) && steady_ms() - sent_at >= 39u &&
                    b.pdp_receive(3658, received) && received.source == a.mac() && received.payload.size() == 4,
                    "Impaired socket receive");
            // The goodbye must not wait in the line of a socket that is closing.
            a.stop();
            require(wait_until([&] { const auto peers = b.peers(); return !peers.empty() && !peers[0].connected; }),
                    "A delayed goodbye was lost");
            b.stop();
        }
        // The fixed port falls back to a free one only when allowed.
        {
            std::string socket_error;
            auto holder = open_udp(0, true, socket_error);
            require(holder != nullptr, "Could not open a UDP socket");
            auto fallback = open_udp(holder->local_port(), true, socket_error);
            require(fallback && fallback->local_port() != holder->local_port(), "Busy port did not fall back");
            require(!open_udp(holder->local_port(), false, socket_error) && !socket_error.empty(), "Busy port was shared");
        }
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

} // namespace kokaku::net
