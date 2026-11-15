#pragma once

// Client for the online room list (lobby/src/index.js). One WebSocket carries
// the room list and the introductions between the members of a room; game
// traffic never goes through it.

#include "kokaku_transport.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kokaku::json {

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind{Kind::Null};
    bool boolean{};
    double number{};
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    [[nodiscard]] const Value *find(std::string_view key) const;
    [[nodiscard]] std::string text(std::string_view key, std::string fallback = {}) const;
    [[nodiscard]] long long integer(std::string_view key, long long fallback = 0) const;
    [[nodiscard]] bool flag(std::string_view key, bool fallback = false) const;
};

bool parse(std::string_view text, Value &out);
std::string quote(std::string_view text);

} // namespace kokaku::json

namespace kokaku::lobby {

inline constexpr int protocol_version = 1;

struct Room {
    std::string id, name, nick, state;
    std::vector<int> rules;
    int players{}, max{}, age{};
};

struct Peer {
    net::Mac mac{};
    std::string nick;
    std::vector<net::Endpoint> candidates;
    bool host{};
};

struct Event {
    enum class Kind { Connected, Disconnected, Rooms, Hosted, Joined, JoinFailed, PeerUpdate, PeerGone, RoomClosed, Error };
    Kind kind{};
    std::string id;       // room
    std::string key;      // Joined
    std::string text;     // public address, failure reason, error code or room name
    std::vector<Room> rooms;
    std::vector<Peer> peers;
    net::Mac mac{};
    int online{};
};

struct Identity {
    net::Mac mac{};
    std::string nickname;
    int net_version{};
};

// Outgoing messages.
std::string hello_message(const Identity &identity);
std::string candidates_message(const std::vector<net::Endpoint> &candidates);
std::string host_message(const std::string &id, const std::string &key, const std::string &name,
                         const std::vector<int> &rules, int max_players, const std::string &state);
std::string join_message(const std::string &id, const std::string &key);
// Incoming text to an event; false for messages the game does not act on.
bool parse_message(std::string_view text, Event &event);

struct Url {
    std::wstring host, path;
    unsigned short port{};
    bool secure{};
};
// Accepts http(s):// and ws(s):// bases; the WebSocket endpoint is <base>/ws.
bool parse_url(const std::string &text, Url &url);

class Client {
public:
    Client() = default;
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    void start(const std::string &url, const Identity &identity);
    void stop();
    [[nodiscard]] bool running() const noexcept { return running_.load(); }
    // True between the server's welcome and the loss of the connection.
    [[nodiscard]] bool online() const noexcept { return online_.load(); }
    // Queued for the sender thread; discarded while offline.
    void send(std::string message);
    bool poll(Event &event);

private:
    void connection_loop();
    void sender_loop();
    void push(Event event);
    void close_socket();

    Url url_{};
    Identity identity_{};
    std::atomic<bool> running_{}, online_{};
    std::thread connection_, sender_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> outgoing_;
    std::deque<Event> events_;
    // The handshake request, then the WebSocket. Closing whichever is current
    // aborts the blocking call of the connection thread.
    void *request_{}, *socket_{};
    std::mutex socket_mutex_;
    std::atomic<unsigned long long> last_pong_ms_{};
};

bool run_lobby_self_tests(std::string &error);
// Talks to a running lobby; used by diagnostics/online scripts.
bool run_lobby_live_test(const std::string &url, int idle_seconds, std::string &error);

} // namespace kokaku::lobby
