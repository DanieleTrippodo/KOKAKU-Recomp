// Protocol tests against a running lobby (default: `npm run dev` on 127.0.0.1:8787).
//   LOBBY_URL=https://kokaku-lobby.example.workers.dev node test/lobby.test.mjs
import WebSocket from "ws";

const base = (process.env.LOBBY_URL || "http://127.0.0.1:8787").replace(/\/$/, "");
const wsUrl = base.replace(/^http/, "ws") + "/ws";
// A local server sees loopback clients and accepts loopback candidates; a
// deployed one sees the real public address.
const local = /127\.0\.0\.1|localhost/.test(base);
const host4 = local ? "127.0.0.1" : "192.168.50.2";

let failures = 0;
let passed = 0;
function check(condition, label) {
  if (condition) {
    passed += 1;
    console.log(`  ok    ${label}`);
  } else {
    failures += 1;
    console.log(`  FAIL  ${label}`);
  }
}

class Client {
  constructor(name) {
    this.name = name;
    this.queue = [];
    this.waiters = [];
    this.closed = false;
  }
  async open(headers = {}) {
    this.ws = new WebSocket(wsUrl, { headers });
    this.ws.on("message", (data) => {
      const text = data.toString();
      const message = text === "pong" ? { t: "pong" } : JSON.parse(text);
      const index = this.waiters.findIndex((waiter) => waiter.match(message));
      if (index >= 0) this.waiters.splice(index, 1)[0].resolve(message);
      else this.queue.push(message);
    });
    this.ws.on("close", () => {
      this.closed = true;
    });
    await new Promise((resolve, reject) => {
      this.ws.once("open", resolve);
      this.ws.once("error", reject);
    });
    return this;
  }
  send(message) {
    this.ws.send(typeof message === "string" ? message : JSON.stringify(message));
  }
  // Next message of the given type, or null after the timeout.
  next(type, timeout = 2000, where = () => true) {
    const match = (message) => message.t === type && where(message);
    const index = this.queue.findIndex(match);
    if (index >= 0) return Promise.resolve(this.queue.splice(index, 1)[0]);
    return new Promise((resolve) => {
      const waiter = { match, resolve };
      this.waiters.push(waiter);
      setTimeout(() => {
        const at = this.waiters.indexOf(waiter);
        if (at >= 0) {
          this.waiters.splice(at, 1);
          resolve(null);
        }
      }, timeout);
    });
  }
  async hello(mac, nick, net = 1) {
    this.send({ t: "hello", v: 1, net, mac, nick });
    return this.next("welcome");
  }
  close() {
    this.ws.close();
  }
}

const hex16 = () => [...crypto.getRandomValues(new Uint8Array(8))].map((b) => b.toString(16).padStart(2, "0")).join("");
const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function main() {
  console.log(`Lobby under test: ${base}`);

  console.log("HTTP");
  const index = await fetch(base + "/");
  check(index.ok && (await index.text()).includes("KokakuNative lobby"), "index answers");
  check((await fetch(base + "/nope")).status === 404, "unknown path is 404");
  check((await fetch(base + "/ws")).status === 426, "plain GET on /ws is refused");

  console.log("Handshake");
  const stranger = await new Client("stranger").open();
  stranger.send({ t: "list" });
  check((await stranger.next("error"))?.code === "hello", "requests before hello are refused");
  stranger.send({ t: "hello", v: 99, net: 1, mac: "02:00:00:00:00:99", nick: "x" });
  check((await stranger.next("error"))?.code === "version", "unknown protocol version is refused");
  stranger.send("{not json");
  check((await stranger.next("error"))?.code === "bad", "malformed JSON is refused");
  stranger.send("ping");
  check((await stranger.next("pong")) !== null, "ping is answered");
  stranger.close();

  console.log("Hosting and listing");
  const host = await new Client("host").open();
  const welcome = await host.hello("02:00:00:00:00:01", "Motoko<script>è");
  check(welcome && welcome.v === 1 && typeof welcome.ip === "string", "welcome carries version and address");
  host.send({ t: "cand", c: [[host4, 3658], ["10.0.0.5", 3658], ["224.0.0.1", 1], ["1.2.3.4", 70000], "junk"] });
  const id = hex16(), key = hex16();
  host.send({ t: "host", id, key, name: "Section 9", rules: [0, 1, 2, 3, 3, 1, 1], max: 6, state: "open" });
  check((await host.next("hosted"))?.id === id, "room is created");

  const viewer = await new Client("viewer").open();
  await viewer.hello("02:00:00:00:00:02", "Batou");
  viewer.send({ t: "list" });
  let rooms = await viewer.next("rooms");
  let mine = rooms?.rooms.find((room) => room.id === id);
  check(mine && mine.name === "Section 9" && mine.players === 1 && mine.max === 6 && mine.state === "open",
        "room is listed with its details");
  check(mine && mine.nick === "Motoko<script>" , "nickname is reduced to printable ASCII");
  check(mine && JSON.stringify(mine.rules) === "[0,1,2,3,3,1,1]", "rules are passed through");
  check(mine && !JSON.stringify(mine).includes(host4) && mine.key === undefined, "list reveals no address or key");
  const http = await (await fetch(base + "/rooms")).json();
  check(http.rooms.some((room) => room.id === id), "HTTP room list matches");

  const other = await new Client("other-build").open();
  await other.hello("02:00:00:00:00:03", "Saito", 2);
  other.send({ t: "list" });
  rooms = await other.next("rooms");
  check(rooms && !rooms.rooms.some((room) => room.id === id), "other netcode versions do not see the room");
  other.send({ t: "join", id });
  check((await other.next("join_failed"))?.reason === "version", "other netcode versions cannot join");
  other.close();

  console.log("Joining");
  viewer.send({ t: "cand", c: [["192.168.1.20", 4658], ["8.8.8.8", 40000]] });
  viewer.send({ t: "join", id });
  const joined = await viewer.next("joined");
  check(joined && joined.key === key && joined.peers.length === 1 && joined.peers[0].host === true,
        "joiner receives the key and the host");
  check(joined && JSON.stringify(joined.peers[0].c) === JSON.stringify([[host4, 3658], ["10.0.0.5", 3658]]),
        "host candidates are validated and delivered");
  const introduced = await host.next("peer");
  check(introduced && introduced.mac === "02:00:00:00:00:02" && introduced.host === false &&
        introduced.c.length === 2, "host learns the joiner");

  const third = await new Client("third").open();
  await third.hello("02:00:00:00:00:04", "Togusa");
  third.send({ t: "join", id });
  const joinedThird = await third.next("joined");
  check(joinedThird && joinedThird.peers.length === 2, "second joiner receives host and first member");
  check((await viewer.next("peer"))?.mac === "02:00:00:00:00:04", "existing member learns the newcomer");
  check((await host.next("peer"))?.mac === "02:00:00:00:00:04", "host learns the newcomer");
  viewer.send({ t: "list" });
  rooms = await viewer.next("rooms");
  check(rooms?.rooms.find((room) => room.id === id)?.players === 3, "player count follows the members");

  third.send({ t: "cand", c: [["192.168.1.30", 3658]] });
  const update = await host.next("peer");
  check(update && update.mac === "02:00:00:00:00:04" && update.c[0][0] === "192.168.1.30",
        "candidate changes reach the room");
  await viewer.next("peer");

  console.log("Room states");
  host.send({ t: "host", id, key, name: "Section 9", rules: [0, 1, 2, 3, 3, 1, 1], max: 6, state: "playing" });
  await host.next("hosted");
  const late = await new Client("late").open();
  await late.hello("02:00:00:00:00:05", "Paz");
  late.send({ t: "join", id });
  check((await late.next("join_failed"))?.reason === "playing", "a running match cannot be joined");
  late.send({ t: "join", id, key });
  check((await late.next("joined")) !== null, "a returning member with the key can rejoin");
  await host.next("peer");
  late.send({ t: "leave" });
  check((await host.next("gone"))?.mac === "02:00:00:00:00:05", "leaving is announced");
  late.send({ t: "list" });
  rooms = await late.next("rooms");
  check(rooms?.rooms.find((room) => room.id === id)?.state === "playing", "running rooms stay listed as playing");
  host.send({ t: "host", id, key, name: "Section 9", rules: [0, 1, 2, 3, 3, 1, 1], max: 3, state: "open" });
  await host.next("hosted");
  late.send({ t: "join", id });
  check((await late.next("join_failed"))?.reason === "full", "a full room refuses newcomers");
  const squatter = hex16();
  late.send({ t: "host", id, key: squatter, name: "Fake", rules: [], max: 6, state: "open" });
  check((await late.next("error"))?.code === "taken", "another connection cannot take over a room id");
  late.close();

  console.log("Disconnects");
  third.close();
  check((await host.next("gone"))?.mac === "02:00:00:00:00:04", "a dropped member is announced to the host");
  check((await viewer.next("gone", 2000, (message) => message.mac === "02:00:00:00:00:04")) !== null,
        "a dropped member is announced to members");
  host.close();
  check((await viewer.next("closed"))?.id === id, "members learn that the room closed");
  await pause(200);
  viewer.send({ t: "list" });
  rooms = await viewer.next("rooms");
  check(rooms && !rooms.rooms.some((room) => room.id === id), "closed room leaves the list");

  console.log("Host reconnect");
  const again = await new Client("host-again").open();
  await again.hello("02:00:00:00:00:01", "Motoko");
  again.send({ t: "cand", c: [[host4, 3658]] });
  again.send({ t: "host", id, key, name: "Section 9", rules: [], max: 6, state: "open" });
  await again.next("hosted");
  check((await again.next("peer"))?.mac === "02:00:00:00:00:02", "reconnected host gets its members back");
  check((await viewer.next("peer"))?.host === true, "members get the reconnected host");
  again.send({ t: "unhost" });
  check((await viewer.next("closed"))?.id === id, "unhost closes the room");

  console.log("Reconnecting over a dead connection");
  // The old connections are still open, as after a network change the
  // service has not noticed yet.
  const roomId = hex16(), roomKey = hex16();
  const oldHost = await new Client("old-host").open();
  await oldHost.hello("02:00:00:00:00:21", "Host");
  oldHost.send({ t: "host", id: roomId, key: roomKey, name: "Keeps going", rules: [], max: 6, state: "playing" });
  await oldHost.next("hosted");
  const oldMember = await new Client("old-member").open();
  await oldMember.hello("02:00:00:00:00:22", "Member");
  oldMember.send({ t: "join", id: roomId, key: roomKey });
  await oldMember.next("joined");
  await oldHost.next("peer");
  const watcher = await new Client("watcher").open();
  await watcher.hello("02:00:00:00:00:23", "Watcher");
  watcher.send({ t: "join", id: roomId, key: roomKey });
  await watcher.next("joined");
  await oldHost.next("peer");
  await oldMember.next("peer");

  const newMember = await new Client("new-member").open();
  await newMember.hello("02:00:00:00:00:22", "Member");
  newMember.send({ t: "join", id: roomId, key: roomKey });
  const back = await newMember.next("joined");
  check(back && back.peers.length === 2 && !back.peers.some((peer) => peer.mac === "02:00:00:00:00:22"),
        "a returning member does not meet their own old connection");
  check((await watcher.next("peer"))?.mac === "02:00:00:00:00:22", "the room is told where the member is now");
  check((await watcher.next("gone", 700)) === null, "replacing a dead connection is not announced as leaving");
  watcher.send({ t: "list" });
  rooms = await watcher.next("rooms");
  check(rooms?.rooms.find((room) => room.id === roomId)?.players === 3, "a reconnected member is counted once");

  const newHost = await new Client("new-host").open();
  await newHost.hello("02:00:00:00:00:21", "Host");
  newHost.send({ t: "host", id: roomId, key: roomKey, name: "Keeps going", rules: [], max: 6, state: "playing" });
  check((await newHost.next("hosted"))?.id === roomId, "a returning host takes its room back with the key");
  check((await watcher.next("closed", 700)) === null, "the room is not announced as closed");
  check((await watcher.next("peer"))?.host === true, "members are told where the host is now");
  watcher.send({ t: "list" });
  rooms = await watcher.next("rooms");
  const kept = rooms?.rooms.filter((room) => room.id === roomId) || [];
  check(kept.length === 1 && kept[0].players === 3 && kept[0].state === "playing", "the room is listed once, with everyone");
  await pause(300);
  check(oldHost.closed && oldMember.closed, "the replaced connections are closed");
  for (const client of [newHost, newMember, watcher]) client.close();
  await pause(200);

  console.log("Rate limit");
  for (let i = 0; i < 60; i += 1) viewer.send({ t: "list" });
  let limited = false;
  for (let i = 0; i < 60 && !limited; i += 1) limited = (await viewer.next("error", 300))?.code === "rate";
  check(limited, "floods are rate limited");
  viewer.close();
  again.close();

  if (local) {
    // Only a local server lets the test choose the address it appears to come from.
    console.log("Connections per address");
    const address = { "CF-Connecting-IP": "203.0.113.50" };
    const crowd = [];
    for (let i = 0; i < 16; i += 1) crowd.push(await new Client(`crowd${i}`).open(address));
    let refused = false;
    try {
      await new Client("one-too-many").open(address);
    } catch {
      refused = true;
    }
    check(refused, "the seventeenth connection from one address is refused");
    const elsewhere = await new Client("elsewhere").open({ "CF-Connecting-IP": "203.0.113.51" });
    check((await elsewhere.hello("02:00:00:00:00:60", "Other"))?.ip === "203.0.113.51", "other addresses are unaffected");
    elsewhere.send({ t: "cand", c: [["203.0.113.51", 3658], ["127.0.0.1", 3658], ["198.51.100.1", 1], ["198.51.100.2", 2], ["198.51.100.3", 3]] });
    const probeId = hex16();
    elsewhere.send({ t: "host", id: probeId, key: hex16(), name: "Probe", rules: [], max: 6, state: "open" });
    await elsewhere.next("hosted");
    const joiner = await new Client("joiner").open({ "CF-Connecting-IP": "203.0.113.52" });
    await joiner.hello("02:00:00:00:00:61", "Joiner");
    joiner.send({ t: "join", id: probeId });
    const answer = await joiner.next("joined");
    check(answer && JSON.stringify(answer.peers[0].c) ===
          JSON.stringify([["203.0.113.51", 3658], ["198.51.100.1", 1], ["198.51.100.2", 2]]),
          "remote clients cannot advertise loopback or many foreign addresses");
    for (const client of [...crowd, elsewhere, joiner]) client.close();
  }

  await pause(200);
  console.log(failures ? `\n${failures} check(s) FAILED` : `\nAll ${passed} lobby checks passed`);
  process.exit(failures ? 1 : 0);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
