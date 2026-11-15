#include "kokaku_lobby.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace kokaku::json {
namespace {

void append_utf8(std::string &out, unsigned code) {
    if (code < 0x80u) out += static_cast<char>(code);
    else if (code < 0x800u) {
        out += static_cast<char>(0xC0u | (code >> 6));
        out += static_cast<char>(0x80u | (code & 0x3Fu));
    } else if (code < 0x10000u) {
        out += static_cast<char>(0xE0u | (code >> 12));
        out += static_cast<char>(0x80u | ((code >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (code & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (code >> 18));
        out += static_cast<char>(0x80u | ((code >> 12) & 0x3Fu));
        out += static_cast<char>(0x80u | ((code >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (code & 0x3Fu));
    }
}

struct Parser {
    std::string_view text;
    std::size_t at{};
    int depth{};

    void skip() {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) ++at;
    }
    bool literal(std::string_view word) {
        if (text.substr(at, word.size()) != word) return false;
        at += word.size();
        return true;
    }
    bool hex4(unsigned &code) {
        if (at + 4 > text.size()) return false;
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text[at++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') code |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string &out) {
        if (at >= text.size() || text[at] != '"') return false;
        ++at;
        out.clear();
        while (at < text.size()) {
            const char c = text[at++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20u) return false;
            if (c != '\\') { out += c; continue; }
            if (at >= text.size()) return false;
            switch (text[at++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned code = 0;
                if (!hex4(code)) return false;
                if (code >= 0xD800u && code <= 0xDBFFu && text.substr(at, 2) == "\\u") {
                    at += 2;
                    unsigned low = 0;
                    if (!hex4(low)) return false;
                    code = low >= 0xDC00u && low <= 0xDFFFu ? 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u) : 0xFFFDu;
                } else if (code >= 0xD800u && code <= 0xDFFFu) {
                    code = 0xFFFDu;
                }
                append_utf8(out, code);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool value(Value &out) {
        if (depth >= 16) return false;
        ++depth;
        const bool ok = value_body(out);
        --depth;
        return ok;
    }
    bool value_body(Value &out) {
        skip();
        if (at >= text.size()) return false;
        out = Value{};
        const char c = text[at];
        if (c == '{') {
            ++at;
            out.kind = Value::Kind::Object;
            skip();
            if (at < text.size() && text[at] == '}') { ++at; return true; }
            for (;;) {
                skip();
                std::string key;
                if (!string(key)) return false;
                skip();
                if (at >= text.size() || text[at++] != ':') return false;
                Value member;
                if (!value(member)) return false;
                out.object.emplace_back(std::move(key), std::move(member));
                skip();
                if (at >= text.size()) return false;
                if (text[at] == ',') { ++at; continue; }
                if (text[at] == '}') { ++at; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++at;
            out.kind = Value::Kind::Array;
            skip();
            if (at < text.size() && text[at] == ']') { ++at; return true; }
            for (;;) {
                Value item;
                if (!value(item)) return false;
                out.array.push_back(std::move(item));
                skip();
                if (at >= text.size()) return false;
                if (text[at] == ',') { ++at; continue; }
                if (text[at] == ']') { ++at; return true; }
                return false;
            }
        }
        if (c == '"') { out.kind = Value::Kind::String; return string(out.string); }
        if (c == 't') { out.kind = Value::Kind::Bool; out.boolean = true; return literal("true"); }
        if (c == 'f') { out.kind = Value::Kind::Bool; return literal("false"); }
        if (c == 'n') return literal("null");
        const std::size_t begin = at;
        while (at < text.size() && ((text[at] >= '0' && text[at] <= '9') || text[at] == '-' || text[at] == '+' ||
                                    text[at] == '.' || text[at] == 'e' || text[at] == 'E')) ++at;
        if (at == begin) return false;
        const auto result = std::from_chars(text.data() + begin, text.data() + at, out.number);
        out.kind = Value::Kind::Number;
        return result.ec == std::errc{} && result.ptr == text.data() + at;
    }
};

} // namespace

const Value *Value::find(std::string_view key) const {
    if (kind != Kind::Object) return nullptr;
    for (const auto &[name, value] : object)
        if (name == key) return &value;
    return nullptr;
}

std::string Value::text(std::string_view key, std::string fallback) const {
    const Value *value = find(key);
    return value && value->kind == Kind::String ? value->string : fallback;
}

long long Value::integer(std::string_view key, long long fallback) const {
    const Value *value = find(key);
    if (!value || value->kind != Kind::Number || !(value->number >= -9.0e15 && value->number <= 9.0e15)) return fallback;
    return static_cast<long long>(value->number);
}

bool Value::flag(std::string_view key, bool fallback) const {
    const Value *value = find(key);
    return value && value->kind == Kind::Bool ? value->boolean : fallback;
}

bool parse(std::string_view text, Value &out) {
    if (text.size() > 256u * 1024u) return false;
    Parser parser{text};
    if (!parser.value(out)) return false;
    parser.skip();
    return parser.at == text.size();
}

std::string quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (byte < 0x20u) {
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\u%04x", byte);
            out += escape;
        } else out += c;
    }
    out += '"';
    return out;
}

} // namespace kokaku::json

namespace kokaku::lobby {
namespace {

unsigned long long tick_ms() {
    return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Names come from other players: keep what the menu font can draw.
std::string printable(const std::string &text, std::size_t limit) {
    std::string out;
    for (const char c : text)
        if (c >= 0x20 && c < 0x7f && out.size() < limit) out += c;
    return out;
}

bool token(const std::string &text, std::size_t limit) {
    if (text.empty() || text.size() > limit) return false;
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
    });
}

bool parse_peer(const json::Value &value, Peer &peer) {
    const auto mac = net::parse_mac(value.text("mac"));
    if (!mac) return false;
    peer.mac = *mac;
    peer.nick = printable(value.text("nick"), 24);
    peer.host = value.flag("host");
    if (const json::Value *list = value.find("c"); list && list->kind == json::Value::Kind::Array) {
        for (const auto &entry : list->array) {
            if (entry.kind != json::Value::Kind::Array || entry.array.size() != 2 ||
                entry.array[0].kind != json::Value::Kind::String || entry.array[1].kind != json::Value::Kind::Number) continue;
            const auto ip = net::parse_ipv4(entry.array[0].string);
            const double port = entry.array[1].number;
            if (!ip || !(port >= 1 && port <= 65535) || peer.candidates.size() >= 8) continue;
            peer.candidates.push_back(net::Endpoint{*ip, static_cast<std::uint16_t>(port)});
        }
    }
    return true;
}

} // namespace

std::string hello_message(const Identity &identity) {
    return "{\"t\":\"hello\",\"v\":" + std::to_string(protocol_version) + ",\"net\":" + std::to_string(identity.net_version) +
           ",\"mac\":" + json::quote(net::mac_text(identity.mac)) + ",\"nick\":" + json::quote(printable(identity.nickname, 24)) + "}";
}

std::string candidates_message(const std::vector<net::Endpoint> &candidates) {
    std::string out = "{\"t\":\"cand\",\"c\":[";
    bool first = true;
    for (const auto &candidate : candidates) {
        if (!candidate.valid()) continue;
        if (!first) out += ',';
        first = false;
        out += "[" + json::quote(net::ip_text(candidate.ip)) + "," + std::to_string(candidate.port) + "]";
    }
    return out + "]}";
}

std::string host_message(const std::string &id, const std::string &key, const std::string &name,
                         const std::vector<int> &rules, int max_players, const std::string &state) {
    std::string out = "{\"t\":\"host\",\"id\":" + json::quote(id) + ",\"key\":" + json::quote(key) +
                      ",\"name\":" + json::quote(printable(name, 32)) + ",\"rules\":[";
    for (std::size_t i = 0; i < rules.size(); ++i) out += (i ? "," : "") + std::to_string(rules[i]);
    return out + "],\"max\":" + std::to_string(max_players) + ",\"state\":" + json::quote(state) + "}";
}

std::string join_message(const std::string &id, const std::string &key) {
    return "{\"t\":\"join\",\"id\":" + json::quote(id) + (key.empty() ? "" : ",\"key\":" + json::quote(key)) + "}";
}

bool parse_message(std::string_view text, Event &event) {
    json::Value message;
    if (!json::parse(text, message) || message.kind != json::Value::Kind::Object) return false;
    const std::string type = message.text("t");
    event = Event{};
    if (type == "welcome") {
        if (message.integer("v") != protocol_version) return false;
        event.kind = Event::Kind::Connected;
        event.text = printable(message.text("ip"), 45);
        event.online = static_cast<int>(std::clamp<long long>(message.integer("online"), 0, 100000));
        return true;
    }
    if (type == "rooms") {
        event.kind = Event::Kind::Rooms;
        event.online = static_cast<int>(std::clamp<long long>(message.integer("online"), 0, 100000));
        if (const json::Value *list = message.find("rooms"); list && list->kind == json::Value::Kind::Array) {
            for (const auto &entry : list->array) {
                Room room{};
                room.id = entry.text("id");
                if (!token(room.id, 32) || event.rooms.size() >= 64) continue;
                room.name = printable(entry.text("name"), 32);
                room.nick = printable(entry.text("nick"), 24);
                room.state = entry.text("state");
                if (room.state != "open" && room.state != "playing") continue;
                room.players = static_cast<int>(std::clamp<long long>(entry.integer("players", 1), 1, 16));
                room.max = static_cast<int>(std::clamp<long long>(entry.integer("max", 6), 1, 16));
                room.age = static_cast<int>(std::clamp<long long>(entry.integer("age"), 0, 1'000'000));
                if (const json::Value *rules = entry.find("rules"); rules && rules->kind == json::Value::Kind::Array)
                    for (const auto &rule : rules->array)
                        if (room.rules.size() < 8)
                            room.rules.push_back(rule.kind == json::Value::Kind::Number && rule.number >= 0 && rule.number <= 255
                                                 ? static_cast<int>(rule.number) : 0);
                event.rooms.push_back(std::move(room));
            }
        }
        return true;
    }
    if (type == "hosted" || type == "closed") {
        event.kind = type == "hosted" ? Event::Kind::Hosted : Event::Kind::RoomClosed;
        event.id = message.text("id");
        return token(event.id, 32);
    }
    if (type == "joined") {
        event.kind = Event::Kind::Joined;
        event.id = message.text("id");
        event.key = message.text("key");
        event.text = printable(message.text("name"), 32);
        if (!token(event.id, 32) || !token(event.key, 32)) return false;
        if (const json::Value *list = message.find("peers"); list && list->kind == json::Value::Kind::Array)
            for (const auto &entry : list->array) {
                Peer peer{};
                if (event.peers.size() < 16 && parse_peer(entry, peer)) event.peers.push_back(std::move(peer));
            }
        return true;
    }
    if (type == "join_failed") {
        event.kind = Event::Kind::JoinFailed;
        event.id = printable(message.text("id"), 32);
        event.text = printable(message.text("reason"), 16);
        return true;
    }
    if (type == "peer") {
        Peer peer{};
        if (!parse_peer(message, peer)) return false;
        event.kind = Event::Kind::PeerUpdate;
        event.mac = peer.mac;
        event.peers.push_back(std::move(peer));
        return true;
    }
    if (type == "gone") {
        const auto mac = net::parse_mac(message.text("mac"));
        if (!mac) return false;
        event.kind = Event::Kind::PeerGone;
        event.mac = *mac;
        return true;
    }
    if (type == "error") {
        event.kind = Event::Kind::Error;
        event.text = printable(message.text("code"), 16);
        return true;
    }
    return false;
}

bool parse_url(const std::string &text, Url &url) {
    std::string rest;
    if (text.rfind("https://", 0) == 0 || text.rfind("wss://", 0) == 0) {
        url.secure = true;
        rest = text.substr(text.find("://") + 3);
    } else if (text.rfind("http://", 0) == 0 || text.rfind("ws://", 0) == 0) {
        url.secure = false;
        rest = text.substr(text.find("://") + 3);
    } else {
        return false;
    }
    std::string path;
    if (const auto slash = rest.find('/'); slash != std::string::npos) {
        path = rest.substr(slash);
        rest.resize(slash);
    }
    while (!path.empty() && path.back() == '/') path.pop_back();
    unsigned port = url.secure ? 443u : 80u;
    if (const auto colon = rest.rfind(':'); colon != std::string::npos) {
        const std::string digits = rest.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        port = static_cast<unsigned>(std::stoul(digits));
        rest.resize(colon);
    }
    const auto valid_host = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
    };
    const auto valid_path = [](char c) { return c > 0x20 && c < 0x7f && c != '?' && c != '#'; };
    if (rest.empty() || rest.size() > 253 || port == 0 || port > 65535 || !std::all_of(rest.begin(), rest.end(), valid_host) ||
        path.size() > 128 || !std::all_of(path.begin(), path.end(), valid_path)) return false;
    path += "/ws";
    url.host.assign(rest.begin(), rest.end());
    url.path.assign(path.begin(), path.end());
    url.port = static_cast<unsigned short>(port);
    return true;
}

Client::~Client() { stop(); }

void Client::start(const std::string &url, const Identity &identity) {
    if (running_.load()) return;
    if (!parse_url(url, url_)) return;
    identity_ = identity;
    running_.store(true);
    connection_ = std::thread([this] { connection_loop(); });
    sender_ = std::thread([this] { sender_loop(); });
}

void Client::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard lock(mutex_);
        wake_.notify_all();
    }
    close_socket();
    if (connection_.joinable()) connection_.join();
    if (sender_.joinable()) sender_.join();
    online_.store(false);
}

void Client::send(std::string message) {
    if (!online_.load()) return;
    std::lock_guard lock(mutex_);
    if (outgoing_.size() >= 64) return;
    outgoing_.push_back(std::move(message));
    wake_.notify_all();
}

bool Client::poll(Event &event) {
    std::lock_guard lock(mutex_);
    if (events_.empty()) return false;
    event = std::move(events_.front());
    events_.pop_front();
    return true;
}

void Client::push(Event event) {
    std::lock_guard lock(mutex_);
    if (events_.size() >= 256) events_.pop_front();
    events_.push_back(std::move(event));
}

#ifdef _WIN32

void Client::close_socket() {
    std::lock_guard lock(socket_mutex_);
    if (socket_) {
        // Aborts the receive pending on the connection thread.
        WinHttpCloseHandle(static_cast<HINTERNET>(socket_));
        socket_ = nullptr;
    }
    if (request_) {
        // Aborts a handshake still waiting for the server.
        WinHttpCloseHandle(static_cast<HINTERNET>(request_));
        request_ = nullptr;
    }
}

// Why the list could not be reached, for the log.
static std::string failure_text(DWORD code) {
    const char *meaning = code == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "the address was not found"
        : code == ERROR_WINHTTP_CANNOT_CONNECT ? "no connection to the server"
        : code == ERROR_WINHTTP_TIMEOUT ? "the server did not answer in time"
        : code == ERROR_WINHTTP_SECURE_FAILURE ? "the secure connection was refused"
        : code == ERROR_WINHTTP_CONNECTION_ERROR ? "the connection broke"
        : "network error";
    return std::string(meaning) + ", code " + std::to_string(code);
}

void Client::connection_loop() {
    unsigned backoff = 1000;
    std::string reported; // the failure the game was already told about
    while (running_.load()) {
        DWORD code = 0;
        std::string failure;
        HINTERNET session = WinHttpOpen(L"KokakuNative", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            session = WinHttpOpen(L"KokakuNative", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET connect = nullptr, request = nullptr, socket = nullptr;
        if (session) {
            WinHttpSetTimeouts(session, 8000, 8000, 8000, 15000);
            connect = WinHttpConnect(session, url_.host.c_str(), url_.port, 0);
        }
        if (connect)
            request = WinHttpOpenRequest(connect, L"GET", url_.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, url_.secure ? WINHTTP_FLAG_SECURE : 0);
        if (!request) code = GetLastError();
        {
            std::lock_guard lock(socket_mutex_);
            request_ = request;
        }
        if (!request) {
            failure = failure_text(code);
        } else if (running_.load()) {
            if (WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
                WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0, size = sizeof(status);
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                        &status, &size, WINHTTP_NO_HEADER_INDEX) && status == 101) {
                    socket = WinHttpWebSocketCompleteUpgrade(request, 0);
                    if (!socket) failure = failure_text(GetLastError());
                } else {
                    failure = "the server answered with status " + std::to_string(status);
                }
            } else {
                failure = failure_text(GetLastError());
            }
        }
        {
            // stop() may have closed the request to abort the handshake.
            std::lock_guard lock(socket_mutex_);
            if (request_) WinHttpCloseHandle(static_cast<HINTERNET>(request_));
            request_ = nullptr;
        }
        bool welcomed = false;
        if (socket && running_.load()) {
            {
                std::lock_guard lock(socket_mutex_);
                socket_ = socket;
            }
            last_pong_ms_.store(tick_ms());
            {
                std::lock_guard lock(mutex_);
                outgoing_.clear();
                outgoing_.push_back(hello_message(identity_));
                wake_.notify_all();
            }
            std::string message;
            std::vector<char> buffer(8192);
            for (;;) {
                DWORD read = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
                if (WinHttpWebSocketReceive(socket, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &type) != NO_ERROR ||
                    type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
                if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
                    continue;
                message.append(buffer.data(), read);
                if (message.size() > 256u * 1024u) break;
                if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;
                if (message == "pong") {
                    last_pong_ms_.store(tick_ms());
                } else {
                    Event event{};
                    if (parse_message(message, event)) {
                        if (event.kind == Event::Kind::Connected) {
                            welcomed = true;
                            last_pong_ms_.store(tick_ms());
                            online_.store(true);
                        }
                        // A refused version cannot recover by reconnecting quickly.
                        if (event.kind == Event::Kind::Error && event.text == "version") backoff = 30000;
                        push(std::move(event));
                    }
                }
                message.clear();
            }
        }
        const bool was_online = online_.exchange(false);
        {
            std::lock_guard lock(socket_mutex_);
            // Otherwise stop() or the sender already closed it.
            if (socket_) {
                WinHttpCloseHandle(static_cast<HINTERNET>(socket_));
                socket_ = nullptr;
            }
        }
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
        {
            std::lock_guard lock(mutex_);
            outgoing_.clear();
        }
        if (was_online) {
            Event disconnected{};
            disconnected.kind = Event::Kind::Disconnected;
            push(std::move(disconnected));
        }
        // A list that cannot be reached is said once, not at every attempt.
        if (socket && !welcomed && failure.empty()) failure = "the connection closed before the welcome";
        if (welcomed) {
            reported.clear();
        } else if (running_.load() && !failure.empty() && failure != reported) {
            reported = failure;
            Event unreachable{};
            unreachable.kind = Event::Kind::Error;
            unreachable.text = "unreachable (" + failure + ")";
            push(std::move(unreachable));
        }
        if (welcomed && backoff < 30000) backoff = 1000;
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(backoff), [this] { return !running_.load(); });
        if (!welcomed) backoff = std::min(backoff * 2u, 30000u);
    }
}

void Client::sender_loop() {
    unsigned long long last_ping = 0;
    while (running_.load()) {
        std::string message;
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::seconds(1), [this] { return !running_.load() || !outgoing_.empty(); });
            if (!running_.load()) break;
            if (!outgoing_.empty()) {
                message = std::move(outgoing_.front());
                outgoing_.pop_front();
            }
        }
        const auto now = tick_ms();
        if (message.empty()) {
            if (!online_.load()) continue;
            // The lobby drops connections that stop pinging; a missing pong
            // means our own link is dead even if TCP has not noticed yet.
            if (now - last_pong_ms_.load() > 70000) { close_socket(); continue; }
            if (now - last_ping < 20000) continue;
            last_ping = now;
            message = "ping";
        }
        std::lock_guard lock(socket_mutex_);
        if (!socket_) continue;
        if (WinHttpWebSocketSend(static_cast<HINTERNET>(socket_), WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                 message.data(), static_cast<DWORD>(message.size())) != NO_ERROR) {
            WinHttpCloseHandle(static_cast<HINTERNET>(socket_));
            socket_ = nullptr;
        }
    }
}

#else

void Client::close_socket() {}
void Client::connection_loop() {}
void Client::sender_loop() {}

#endif

bool run_lobby_self_tests(std::string &error) {
    const auto require = [](bool condition, const char *message) { if (!condition) throw std::runtime_error(message); };
    try {
        json::Value value;
        // Unicode escapes, a surrogate pair included, spelled without putting
        // the escape itself in this source file.
        const std::string u = std::string(1, '\\') + "u";
        require(json::parse(R"({"a":1,"b":[true,false,null,-2.5e1],"c":"x\"y\\z\n)" + u + "00e8" + u + "d83d" + u + "de00" +
                            R"(","d":{"e":{}}})", value), "Valid JSON rejected");
        require(value.integer("a") == 1 && value.find("b") && value.find("b")->array.size() == 4 &&
                value.find("b")->array[0].boolean && value.find("b")->array[3].number == -25.0, "JSON numbers or arrays");
        require(value.text("c") == "x\"y\\z\n\xc3\xa8\xf0\x9f\x98\x80", "JSON string escapes");
        require(value.find("d") && value.find("d")->find("e") && !value.find("zz") && value.text("a", "fallback") == "fallback" &&
                value.integer("c", 7) == 7 && value.flag("a", true), "JSON lookups");
        for (const char *bad : {"", "{", "[1,]", "{\"a\":}", "{\"a\" 1}", "\"unterminated", "tru", "{\"a\":1} x", "\"bad\\q\"",
                                "[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]", "01x", "\"ctrl\x01\""})
            require(!json::parse(bad, value), "Malformed JSON accepted");
        require(json::quote("a\"b\\c\n") == "\"a\\\"b\\\\c\\u000a\"", "JSON quoting");
        require(json::parse(json::quote("tab\there \"q\""), value) && value.string == "tab\there \"q\"", "Quote roundtrip");

        Url url;
        require(parse_url("https://kokaku-lobby.example.workers.dev", url) && url.secure && url.port == 443 &&
                url.host == L"kokaku-lobby.example.workers.dev" && url.path == L"/ws", "HTTPS URL");
        require(parse_url("http://127.0.0.1:8787/", url) && !url.secure && url.port == 8787 && url.host == L"127.0.0.1" &&
                url.path == L"/ws", "Local URL");
        require(parse_url("wss://example.org:8443/lobby/v1/", url) && url.secure && url.port == 8443 && url.path == L"/lobby/v1/ws",
                "URL with a base path");
        for (const char *bad : {"", "example.org", "ftp://example.org", "https://", "https://exa mple.org", "https://example.org:0",
                                "https://example.org:99999", "https://example.org:12ab", "https://example.org/a?b"})
            require(!parse_url(bad, url), "Invalid lobby URL accepted");

        const Identity identity{net::Mac{2, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}, "Major \"K\"\x01", 3};
        require(json::parse(hello_message(identity), value) && value.text("t") == "hello" && value.integer("v") == 1 &&
                value.integer("net") == 3 && value.text("mac") == "02:aa:bb:cc:dd:ee" && value.text("nick") == "Major \"K\"",
                "Hello message");
        require(json::parse(candidates_message({{0xC0A80105u, 3658}, {}, {0x01020304u, 40000}}), value) &&
                value.find("c")->array.size() == 2 && value.find("c")->array[0].array[0].string == "192.168.1.5" &&
                value.find("c")->array[1].array[1].number == 40000, "Candidate message");
        require(json::parse(host_message("0123456789abcdef", "fedcba9876543210", "Section 9", {0, 1, 2}, 6, "open"), value) &&
                value.text("id") == "0123456789abcdef" && value.text("key") == "fedcba9876543210" && value.text("name") == "Section 9" &&
                value.find("rules")->array.size() == 3 && value.integer("max") == 6 && value.text("state") == "open", "Host message");
        require(json::parse(join_message("abc", ""), value) && !value.find("key") && json::parse(join_message("abc", "k1"), value) &&
                value.text("key") == "k1", "Join message");

        Event event{};
        require(parse_message(R"({"t":"welcome","v":1,"ip":"203.0.113.9","online":4})", event) &&
                event.kind == Event::Kind::Connected && event.text == "203.0.113.9" && event.online == 4, "Welcome event");
        require(!parse_message(R"({"t":"welcome","v":2})", event), "Welcome from another protocol accepted");
        require(parse_message(R"({"t":"rooms","online":9,"rooms":[
            {"id":"aa11","name":"Sec)" + u + R"(00e8tion 9<b>","nick":"Motoko","rules":[0,1,2,3,3,1,1,9,9],"players":2,"max":6,"state":"open","age":12},
            {"id":"bad id","name":"x","state":"open"},
            {"id":"cc33","name":"y","state":"closed"},
            {"id":"dd44","name":"Playing","nick":"B","rules":["x",300],"players":99,"max":0,"state":"playing","age":-5}]})", event) &&
                event.kind == Event::Kind::Rooms && event.online == 9 && event.rooms.size() == 2, "Room list filtering");
        require(event.rooms[0].id == "aa11" && event.rooms[0].name == "Section 9<b>" && event.rooms[0].nick == "Motoko" &&
                event.rooms[0].rules.size() == 8 && event.rooms[0].rules[2] == 2 && event.rooms[0].players == 2 &&
                event.rooms[0].max == 6 && event.rooms[0].age == 12, "Room fields");
        require(event.rooms[1].state == "playing" && event.rooms[1].players == 16 && event.rooms[1].max == 1 &&
                event.rooms[1].age == 0 && event.rooms[1].rules == std::vector<int>({0, 0}), "Room values are clamped");
        require(parse_message(R"({"t":"joined","id":"aa11","key":"0011223344556677","name":"Room","peers":[
            {"mac":"02:00:00:00:00:01","nick":"Host","host":true,"c":[["192.168.1.5",3658],["300.1.1.1",1],["1.2.3.4",0],["1.2.3.4",70000],"x",["8.8.4.4",40001]]},
            {"mac":"nonsense","nick":"x","c":[]},
            {"mac":"02:00:00:00:00:02","nick":"Member","host":false,"c":[]}]})", event) &&
                event.kind == Event::Kind::Joined && event.id == "aa11" && event.key == "0011223344556677" && event.peers.size() == 2,
                "Joined event");
        require(event.peers[0].host && event.peers[0].nick == "Host" && event.peers[0].candidates.size() == 2 &&
                event.peers[0].candidates[0] == net::Endpoint{0xC0A80105u, 3658} &&
                event.peers[0].candidates[1] == net::Endpoint{0x08080404u, 40001} && !event.peers[1].host, "Peer candidates");
        require(parse_message(R"({"t":"peer","mac":"02:00:00:00:00:03","nick":"New","host":false,"c":[["10.0.0.2",3658]]})", event) &&
                event.kind == Event::Kind::PeerUpdate && event.mac == net::Mac{2, 0, 0, 0, 0, 3} && event.peers.size() == 1, "Peer event");
        require(parse_message(R"({"t":"gone","mac":"02:00:00:00:00:03"})", event) && event.kind == Event::Kind::PeerGone &&
                event.mac == net::Mac{2, 0, 0, 0, 0, 3}, "Gone event");
        require(parse_message(R"({"t":"join_failed","id":"aa11","reason":"full"})", event) &&
                event.kind == Event::Kind::JoinFailed && event.text == "full", "Join failure event");
        require(parse_message(R"({"t":"closed","id":"aa11"})", event) && event.kind == Event::Kind::RoomClosed &&
                parse_message(R"({"t":"hosted","id":"aa11"})", event) && event.kind == Event::Kind::Hosted &&
                parse_message(R"({"t":"error","code":"rate"})", event) && event.kind == Event::Kind::Error && event.text == "rate",
                "Simple events");
        for (const char *bad : {"pong", "[]", "{}", R"({"t":"unknown"})", R"({"t":"gone","mac":"zz"})",
                                R"({"t":"joined","id":"a b","key":"k"})", R"({"t":"peer","nick":"x"})"})
            require(!parse_message(bad, event), "Unusable lobby message accepted");
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

bool run_lobby_live_test(const std::string &url, int idle_seconds, std::string &error) {
    const auto require = [](bool condition, const char *message) { if (!condition) throw std::runtime_error(message); };
    try {
        Client host, guest;
        const auto wait = [](Client &client, Event::Kind kind, Event &event, int seconds = 10) {
            for (int i = 0; i < seconds * 100; ++i) {
                while (client.poll(event))
                    if (event.kind == kind) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        };
        Event event{};
        host.start(url, Identity{net::Mac{2, 0x4b, 0x4e, 0, 0, 1}, "LiveHost", 9001});
        guest.start(url, Identity{net::Mac{2, 0x4b, 0x4e, 0, 0, 2}, "LiveGuest", 9001});
        require(host.running() && guest.running(), "Lobby URL was not accepted");
        require(wait(host, Event::Kind::Connected, event) && host.online(), "Host did not connect to the lobby");
        std::cout << "lobby sees this machine as " << event.text << " (" << event.online << " online)\n";
        require(wait(guest, Event::Kind::Connected, event) && guest.online(), "Guest did not connect to the lobby");
        if (idle_seconds > 0) {
            // An idle connection must survive on pings alone.
            std::this_thread::sleep_for(std::chrono::seconds(idle_seconds));
            bool dropped = false;
            while (host.poll(event)) dropped |= event.kind == Event::Kind::Disconnected;
            require(!dropped && host.online() && guest.online(), "Idle lobby connection was lost");
        }
        const std::string id = "4c495645" + std::to_string(10000000 + tick_ms() % 89999999);
        const std::string key = "00112233aabbccdd";
        host.send(candidates_message({{0xC0A80132u, 3658}}));
        host.send(host_message(id, key, "Live test room", {0, 1, 2, 3, 3, 1, 1}, 6, "open"));
        require(wait(host, Event::Kind::Hosted, event) && event.id == id, "Room was not created");
        guest.send("{\"t\":\"list\"}");
        require(wait(guest, Event::Kind::Rooms, event), "Room list did not arrive");
        const auto listed = std::find_if(event.rooms.begin(), event.rooms.end(), [&](const Room &room) { return room.id == id; });
        require(listed != event.rooms.end() && listed->name == "Live test room" && listed->nick == "LiveHost" &&
                listed->players == 1 && listed->rules.size() == 7, "Room is not listed correctly");
        guest.send(candidates_message({{0xC0A80133u, 4658}}));
        guest.send(join_message(id, ""));
        require(wait(guest, Event::Kind::Joined, event) && event.key == key && event.peers.size() == 1 && event.peers[0].host &&
                event.peers[0].candidates.size() == 1 && event.peers[0].candidates[0] == net::Endpoint{0xC0A80132u, 3658},
                "Join did not return the host");
        require(wait(host, Event::Kind::PeerUpdate, event) && event.mac == net::Mac{2, 0x4b, 0x4e, 0, 0, 2} &&
                event.peers[0].candidates.size() == 1, "Host was not told about the guest");
        guest.stop();
        require(wait(host, Event::Kind::PeerGone, event) && event.mac == net::Mac{2, 0x4b, 0x4e, 0, 0, 2},
                "Host was not told that the guest left");
        host.send("{\"t\":\"unhost\"}");
        host.stop();
        require(!host.online() && !host.running(), "Client did not stop");
        // A list that is not there is reported, once, with the reason.
        Client nowhere;
        nowhere.start("http://127.0.0.1:9", Identity{net::Mac{2, 0x4b, 0x4e, 0, 0, 3}, "LiveNowhere", 9001});
        require(wait(nowhere, Event::Kind::Error, event, 20) && event.text.starts_with("unreachable (") && !nowhere.online(),
                "An unreachable lobby was not reported");
        std::cout << "a closed port is reported as: " << event.text << '\n';
        // Long enough for the next attempt to fail the same way.
        std::this_thread::sleep_for(std::chrono::milliseconds(4500));
        bool repeated = false;
        while (nowhere.poll(event)) repeated |= event.kind == Event::Kind::Error;
        nowhere.stop();
        require(!repeated, "The same lobby failure was reported again");
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

} // namespace kokaku::lobby
