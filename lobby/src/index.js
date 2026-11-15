// KokakuNative lobby: public room list and peer introduction for hole punching.
//
// One Durable Object holds every WebSocket. It never relays game traffic: it
// lists rooms and tells the members of a room each other's UDP candidates.
// All per-connection state lives in the WebSocket attachment, so the object
// can hibernate between messages (Workers Free plan friendly).
import { DurableObject } from "cloudflare:workers";

const PROTOCOL = 1;
const MAX_MESSAGE_BYTES = 2048;
const MAX_ROOMS = 200;
const MAX_CONNECTIONS = 1000;
const MAX_PER_ADDRESS = 16;
const MAX_CANDIDATES = 8;
const MAX_PLAYERS = 6;
const STALE_MS = 90_000;
const RATE_BURST = 40;
const RATE_PER_SECOND = 4;
const STATES = new Set(["open", "playing", "closed"]);

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/ws" || url.pathname === "/rooms") {
      return env.LOBBY.get(env.LOBBY.idFromName("lobby-v1")).fetch(request);
    }
    if (url.pathname === "/") {
      return new Response(`KokakuNative lobby, protocol ${PROTOCOL}\n`, {
        headers: { "content-type": "text/plain; charset=utf-8" },
      });
    }
    return new Response("Not found\n", { status: 404 });
  },
};

const isHex = (value, length) => typeof value === "string" && value.length === length && /^[0-9a-f]+$/.test(value);
const isMac = (value) => typeof value === "string" && /^[0-9a-f]{2}(:[0-9a-f]{2}){5}$/.test(value);

// Printable text only; the game renders names with a fixed font.
function cleanText(value, limit, fallback) {
  if (typeof value !== "string") return fallback;
  const text = value.replace(/[^\x20-\x7e]/g, "").trim().slice(0, limit);
  return text || fallback;
}

function parseIpv4(text) {
  if (typeof text !== "string") return null;
  const match = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(text);
  if (!match) return null;
  const parts = match.slice(1).map(Number);
  return parts.every((part) => part <= 255) ? parts : null;
}

const isLoopback = (ip) => ip[0] === 127;
const isPrivate = (ip) =>
  ip[0] === 10 || (ip[0] === 172 && ip[1] >= 16 && ip[1] <= 31) || (ip[0] === 192 && ip[1] === 168) ||
  (ip[0] === 100 && ip[1] >= 64 && ip[1] <= 127);
const isUnusable = (ip) => ip[0] === 0 || ip[0] >= 224 || (ip[0] === 169 && ip[1] === 254);

// Members only ever punch towards these addresses. Private ranges are always
// allowed (same-LAN play); loopback only for a local test server; at most two
// public addresses, which is what STUN can legitimately report.
function cleanCandidates(value, remoteIp) {
  if (!Array.isArray(value)) return [];
  const local = !remoteIp || remoteIp === "::1" || remoteIp.startsWith("127.");
  const result = [];
  let publicCount = 0;
  for (const entry of value) {
    if (!Array.isArray(entry) || entry.length !== 2) continue;
    const ip = parseIpv4(entry[0]);
    const port = entry[1];
    if (!ip || !Number.isInteger(port) || port < 1 || port > 65535 || isUnusable(ip)) continue;
    if (isLoopback(ip)) {
      if (!local) continue;
    } else if (!isPrivate(ip)) {
      if (entry[0] !== remoteIp && ++publicCount > 2) continue;
    }
    if (result.some((item) => item[0] === entry[0] && item[1] === port)) continue;
    result.push([entry[0], port]);
    if (result.length === MAX_CANDIDATES) break;
  }
  return result;
}

function cleanRules(value) {
  if (!Array.isArray(value)) return [];
  return value.slice(0, 8).map((item) => (Number.isInteger(item) && item >= 0 && item <= 255 ? item : 0));
}

export class Lobby extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    // Answered by the runtime without waking the object.
    this.ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
  }

  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/rooms") {
      const sockets = this.sweep();
      return Response.json({ rooms: this.roomList(sockets, null), online: sockets.length });
    }
    if (request.headers.get("Upgrade") !== "websocket") {
      return new Response("Expected a WebSocket upgrade\n", { status: 426 });
    }
    const sockets = this.ctx.getWebSockets();
    if (sockets.length >= MAX_CONNECTIONS) {
      return new Response("Lobby is full\n", { status: 503 });
    }
    const ip = request.headers.get("CF-Connecting-IP") || "";
    if (ip && sockets.filter((ws) => this.state(ws)?.ip === ip).length >= MAX_PER_ADDRESS) {
      return new Response("Too many connections from this address\n", { status: 429 });
    }
    const pair = new WebSocketPair();
    this.ctx.acceptWebSocket(pair[1]);
    pair[1].serializeAttachment({
      ip,
      seen: Date.now(),
      tokens: RATE_BURST,
      refill: Date.now(),
    });
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  state(ws) {
    try {
      return ws.deserializeAttachment() || null;
    } catch {
      return null;
    }
  }

  send(ws, message) {
    try {
      ws.send(JSON.stringify(message));
    } catch {
      // The peer is gone; its close event performs the cleanup.
    }
  }

  alive(ws, st, now) {
    const auto = this.ctx.getWebSocketAutoResponseTimestamp(ws);
    const last = Math.max(st.seen || 0, auto ? auto.getTime() : 0);
    return now - last < STALE_MS;
  }

  // Drop connections that stopped pinging (crash, sleep, cable pulled) and
  // return the live ones as [socket, state].
  sweep() {
    const now = Date.now();
    const live = [];
    for (const ws of this.ctx.getWebSockets()) {
      const st = this.state(ws);
      if (!st) continue;
      if (!this.alive(ws, st, now)) {
        this.drop(ws, st);
        try {
          ws.close(1001, "stale");
        } catch {
          // Already closed.
        }
        continue;
      }
      live.push([ws, st]);
    }
    return live;
  }

  findRoom(sockets, id) {
    return sockets.find(([, st]) => st.room && st.room.id === id) || null;
  }

  members(sockets, id) {
    return sockets.filter(([, st]) => st.member === id);
  }

  roomList(sockets, net) {
    const now = Date.now();
    const rooms = [];
    for (const [, st] of sockets) {
      if (!st.room || st.room.state === "closed") continue;
      if (net !== null && st.net !== net) continue;
      rooms.push({
        id: st.room.id,
        name: st.room.name,
        nick: st.nick,
        rules: st.room.rules,
        players: 1 + this.members(sockets, st.room.id).length,
        max: st.room.max,
        state: st.room.state,
        age: Math.max(0, Math.round((now - st.room.created) / 1000)),
      });
    }
    rooms.sort((a, b) => (a.state === b.state ? a.age - b.age : a.state === "open" ? -1 : 1));
    return rooms;
  }

  peerView(st, host) {
    return { mac: st.mac, nick: st.nick, c: st.cand || [], host };
  }

  // A connection that died without closing is still here when its owner
  // comes back on a new one. The old one leaves quietly: nobody is told that
  // the player or the room went away.
  retire(ws, st) {
    st.room = null;
    st.member = null;
    try {
      ws.serializeAttachment(st);
      ws.close(1000, "replaced");
    } catch {
      // Already closed.
    }
  }

  // Remove whatever this connection contributed to a room and tell the others.
  drop(ws, st) {
    const others = this.ctx.getWebSockets().filter((other) => other !== ws);
    const pairs = others.map((other) => [other, this.state(other)]).filter(([, state]) => state);
    if (st.room) {
      for (const [other] of this.members(pairs, st.room.id)) this.send(other, { t: "closed", id: st.room.id });
    }
    if (st.member) {
      const host = this.findRoom(pairs, st.member);
      const targets = this.members(pairs, st.member);
      if (host) targets.push(host);
      for (const [other] of targets) this.send(other, { t: "gone", mac: st.mac });
    }
    st.room = null;
    st.member = null;
  }

  webSocketMessage(ws, raw) {
    const st = this.state(ws);
    if (!st) return;
    const now = Date.now();
    st.tokens = Math.min(RATE_BURST, st.tokens + ((now - st.refill) / 1000) * RATE_PER_SECOND);
    st.refill = now;
    st.seen = now;
    if (st.tokens < 1) {
      this.send(ws, { t: "error", code: "rate" });
      ws.serializeAttachment(st);
      return;
    }
    st.tokens -= 1;
    let message = null;
    if (typeof raw === "string" && raw.length <= MAX_MESSAGE_BYTES) {
      try {
        message = JSON.parse(raw);
      } catch {
        message = null;
      }
    }
    if (!message || typeof message !== "object" || typeof message.t !== "string") {
      this.send(ws, { t: "error", code: "bad" });
      ws.serializeAttachment(st);
      return;
    }
    this.handle(ws, st, message);
    ws.serializeAttachment(st);
  }

  handle(ws, st, message) {
    if (message.t === "hello") {
      if (message.v !== PROTOCOL || !isMac(message.mac) || !Number.isInteger(message.net)) {
        this.send(ws, { t: "error", code: "version", v: PROTOCOL });
        return;
      }
      st.mac = message.mac;
      st.net = message.net;
      st.nick = cleanText(message.nick, 24, "Player");
      st.ready = true;
      const sockets = this.sweep();
      this.send(ws, { t: "welcome", v: PROTOCOL, ip: st.ip, online: sockets.length });
      return;
    }
    if (!st.ready) {
      this.send(ws, { t: "error", code: "hello" });
      return;
    }
    switch (message.t) {
      case "cand": {
        st.cand = cleanCandidates(message.c, st.ip);
        const room = st.room ? st.room.id : st.member;
        if (!room) return;
        // Addresses changed mid-session (new NAT mapping): let the room retry.
        const sockets = this.sweep().filter(([other]) => other !== ws);
        const targets = this.members(sockets, room);
        const host = this.findRoom(sockets, room);
        if (host) targets.push(host);
        for (const [other] of targets) this.send(other, { t: "peer", ...this.peerView(st, !!st.room) });
        return;
      }
      case "host": {
        if (!isHex(message.id, 16) || !isHex(message.key, 16)) {
          this.send(ws, { t: "error", code: "bad" });
          return;
        }
        const sockets = this.sweep();
        const existing = this.findRoom(sockets, message.id);
        let created = st.room && st.room.id === message.id ? st.room.created : Date.now();
        if (existing && existing[0] !== ws) {
          // Only the room key proves that this is the same host.
          if (existing[1].room.key !== message.key) {
            this.send(ws, { t: "error", code: "taken" });
            return;
          }
          created = existing[1].room.created;
          this.retire(existing[0], existing[1]);
        }
        if (!st.room && sockets.filter(([, other]) => other.room).length >= MAX_ROOMS) {
          this.send(ws, { t: "error", code: "full" });
          return;
        }
        if (st.member) this.drop(ws, st);
        if (st.room && st.room.id !== message.id) this.drop(ws, st);
        st.room = {
          id: message.id,
          key: message.key,
          name: cleanText(message.name, 32, `${st.nick}'s room`),
          rules: cleanRules(message.rules),
          max: Number.isInteger(message.max) ? Math.min(Math.max(message.max, 2), MAX_PLAYERS) : MAX_PLAYERS,
          state: STATES.has(message.state) ? message.state : "open",
          created,
        };
        this.send(ws, { t: "hosted", id: st.room.id });
        // A host that reconnects finds its members still attached.
        for (const [other, member] of this.members(sockets, st.room.id)) {
          if (other === ws) continue;
          this.send(ws, { t: "peer", ...this.peerView(member, false) });
          this.send(other, { t: "peer", ...this.peerView(st, true) });
        }
        return;
      }
      case "unhost": {
        if (st.room) this.drop(ws, st);
        return;
      }
      case "list": {
        const sockets = this.sweep();
        this.send(ws, { t: "rooms", rooms: this.roomList(sockets, st.net), online: sockets.length });
        return;
      }
      case "join": {
        const sockets = this.sweep();
        const host = typeof message.id === "string" ? this.findRoom(sockets, message.id) : null;
        const fail = (reason) => this.send(ws, { t: "join_failed", id: message.id, reason });
        if (!host || host[0] === ws) return fail("gone");
        const room = host[1].room;
        if (host[1].net !== st.net) return fail("version");
        // The same player on a new connection takes the place of the old one.
        for (const [other, member] of this.members(sockets, room.id)) {
          if (other !== ws && member.mac === st.mac) this.retire(other, member);
        }
        const members = this.members(sockets, room.id).filter(([other]) => other !== ws);
        // The room key proves an earlier membership: reconnecting players get
        // back in even while the match is running.
        const returning = message.key === room.key;
        if (!returning && room.state !== "open") return fail(room.state);
        if (members.length + 1 >= room.max && !returning) return fail("full");
        if (st.room) this.drop(ws, st);
        if (st.member && st.member !== room.id) this.drop(ws, st);
        st.member = room.id;
        const peers = [this.peerView(host[1], true), ...members.map(([, member]) => this.peerView(member, false))];
        this.send(ws, { t: "joined", id: room.id, key: room.key, name: room.name, peers });
        const mine = { t: "peer", ...this.peerView(st, false) };
        this.send(host[0], mine);
        for (const [other] of members) this.send(other, mine);
        return;
      }
      case "leave": {
        if (st.member) this.drop(ws, st);
        return;
      }
      default:
        this.send(ws, { t: "error", code: "bad" });
    }
  }

  webSocketClose(ws) {
    const st = this.state(ws);
    if (st) this.drop(ws, st);
    try {
      ws.close(1000, "bye");
    } catch {
      // Already closed.
    }
  }

  webSocketError(ws) {
    const st = this.state(ws);
    if (st) this.drop(ws, st);
  }
}
