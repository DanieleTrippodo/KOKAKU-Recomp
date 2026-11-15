#include "kokaku_pc_balance.hpp"
#include "kokaku_mouse.hpp"
#include "generated_units.hpp"
#include "psprecomp/runtime.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace kokaku {
namespace {
std::atomic<bool> enabled{true};
std::uint32_t active_player{}; // Accessed only by guest execution, like guest memory.
unsigned damage_fraction{5}; // Tenths; round the cumulative damage to nearest HP.
unsigned companion_fraction{10}; // Twentieths; retain 5% even for weak attacks.
std::unordered_map<std::uint32_t, float> next_reposition;
constexpr std::uint32_t player_vtable = 0x08a74358u;
constexpr std::uint32_t run_mode = 0x08aa8d60u, session_data_pointer = 0x08aa8a3cu;
constexpr std::uint32_t soldier_vtable = 0x08a8d440u, soldier_ai_vtable = 0x08a8d760u;
unsigned reduced_hp(unsigned original) { return std::max(1u, (original * 2u + 2u) / 5u); }
bool trace_enabled() {
    static const bool trace = [] { const char *value = std::getenv("PSPRECOMP_TRACE_PC_BALANCE");
        return value && value[0] == '1'; }();
    return trace;
}
bool campaign_session(psprecomp::GuestMemory &memory) {
    // run_mode is a debug setting, NOT the campaign/multiplayer selector.
    // The native lobby sets session+0xdc8 at 0x0881093c. CGame checks the
    // same field at 0x08814d48 to create CUI_MultiPlayInGame before play.
    if (!memory.contains(run_mode, 4) || memory.load32(run_mode) > 1u ||
        !memory.contains(session_data_pointer, 4)) return false;
    const auto session = memory.load32(session_data_pointer);
    return session && memory.contains(session, 0xdcc) && memory.load32(session + 0xdc8) == 0;
}
bool campaign_active(psprecomp::GuestMemory &memory) {
    if (!active_player || !campaign_session(memory) ||
        !memory.contains(active_player, 0x114) || memory.load32(active_player) != player_vtable) return false;
    const auto parameters = memory.load32(active_player + 0x110);
    if (!memory.contains(parameters, 0x80)) return false;
    const auto original = memory.load32(parameters + 0x7c);
    return original && original <= 1'000'000u && memory.load32(active_player + 0x94) == reduced_hp(original);
}
bool hostile_actor(psprecomp::GuestMemory &memory, std::uint32_t actor) {
    if (!actor || actor == active_player || !memory.contains(actor, 0x114)) return false;
    const auto vtable = memory.load32(actor);
    // Admit the original CActor health layout and its known affiliation methods.
    // 0x08886d8c compares team +0xd0; -1 is neutral. The humanoid override
    // 0x0888d9b0 also has a special allegiance state at +0xc40, excluded here.
    if (vtable < 0x08a70000u || vtable >= 0x08ab0000u || !memory.contains(vtable, 0x144) ||
        memory.load32(vtable + 0x88) != 0x088842ecu) return false;
    const auto affiliation = memory.load32(vtable + 0x140);
    if (affiliation == 0x0888d9b0u) {
        if (!memory.contains(actor, 0xc41) || memory.load8(actor + 0xc40) != 0) return false;
    } else if (affiliation != 0x08886d8cu) return false;
    const auto team = memory.load32(actor + 0xd0);
    return team != 0xffffffffu && team != memory.load32(active_player + 0xd0);
}
bool mobile_battle(psprecomp::GuestMemory &memory, std::uint32_t state) {
    if (!campaign_active(memory) || !memory.contains(state, 0xdc)) return false;
    const auto type = memory.load32(state);
    // Native patrol and assault battle machines have different lateral states.
    if (type != 0x08a8e1d0u && type != 0x08a8db10u) return false;
    const auto ai = memory.load32(state + 0x3c), actor = memory.load32(state + 0x38);
    if (!memory.contains(ai, 0x108) || memory.load32(ai) != soldier_ai_vtable ||
        memory.load32(ai + 0x30) != actor || !hostile_actor(memory, actor) ||
        memory.load32(actor) != soldier_vtable || memory.load32(actor + 0x90) == 0 ||
        memory.load32(actor + 0xd4) != 0) return false;
    const auto target = memory.load32(ai + 0x104);
    return target == active_player || (target && is_local_controlled_actor(memory, target));
}
float game_seconds(psprecomp::GuestMemory &memory) {
    const auto clock = memory.load32(0x08ac5d80);
    return memory.contains(clock, 0x68) ? std::bit_cast<float>(memory.load32(clock + 0x64)) : -1.f;
}
}
void set_pc_balance_enabled(bool value) { enabled.store(value); }
bool pc_balance_enabled() { return enabled.load(); }

bool apply_pc_balance(psprecomp::GuestMemory &memory, std::uint32_t player, bool selected) {
    // ULUS10020 CPlayer::Init, return block 0x0882332c. The base initializer
    // 0x08880c64 copies param.Life (+0x7c) into current/max HP (+0x90/+0x94).
    // CPlayer then restores carried HP at 0x088232c4. Reduce the maximum and
    // clamp that carried value, without charging the player another 60% at
    // each map transition. Native healing at 0x08886fd0 respects this maximum.
    // Check the native multiplayer flag before any health write.
    if (!memory.contains(player, 0x114) || memory.load32(player) != player_vtable) return false;
    active_player = 0;
    damage_fraction = 5;
    companion_fraction = 10;
    next_reposition.clear();
    if (!selected || !campaign_session(memory)) return false;
    const auto parameters = memory.load32(player + 0x110);
    if (!memory.contains(parameters, 0x80)) return false;
    const auto original = memory.load32(parameters + 0x7c);
    if (!original || original > 1'000'000u) return false;
    const auto reduced = reduced_hp(original);
    const auto maximum = memory.load32(player + 0x94);
    const auto current = memory.load32(player + 0x90);
    if ((maximum != original && maximum != reduced) || current > original) return false;
    memory.store32(player + 0x94, reduced);
    memory.store32(player + 0x90, std::min(current, reduced));
    active_player = player;
    if (trace_enabled()) std::fprintf(stderr, "[pc-balance] player=%08x hp=%u/%u original=%u\n",
                                     player, std::min(current, reduced), reduced, original);
    return true;
}

std::uint32_t pc_balance_damage(psprecomp::GuestMemory &memory, std::uint32_t target,
                               std::uint32_t source, std::uint32_t damage) {
    if (!damage || damage > 1'000'000u || !campaign_active(memory)) return damage;
    const auto root = memory.load32(0x08aa8ddc);
    // Only the player's companion, never an enemy vehicle or a piloted one.
    if (memory.contains(root, 0xb8) && memory.load32(root + 0x94) == active_player &&
        memory.contains(active_player, 0x58c) && source &&
        source == memory.load32(active_player + 0x588) && memory.contains(source, 0xd4) &&
        memory.load32(source) == 0x08a88678u &&
        memory.load32(source + 0xd0) == memory.load32(active_player + 0xd0) &&
        !is_local_controlled_actor(memory, source)) {
        const auto scaled = damage + companion_fraction;
        const auto adjusted = scaled / 20u;
        companion_fraction = scaled % 20u;
        static unsigned traces{};
        if (trace_enabled() && traces++ < 128) std::fprintf(stderr,
            "[pc-balance] companion-damage source=%08x target=%08x original=%u adjusted=%u\n",
            source, target, damage, adjusted);
        return adjusted;
    }
    if (target != active_player || !hostile_actor(memory, source)) return damage;
    const auto scaled = damage * 16u + damage_fraction;
    const auto adjusted = scaled / 10u;
    damage_fraction = scaled % 10u;
    static unsigned traces{};
    if (trace_enabled() && traces++ < 32) std::fprintf(stderr,
        "[pc-balance] damage source=%08x target=%08x original=%u adjusted=%u\n", source, target, damage, adjusted);
    return adjusted;
}

void pc_balance_reposition(psprecomp::GuestMemory &memory, std::uint32_t state) {
    if (!mobile_battle(memory, state) || memory.load32(state + 0x10) != 4) return;
    const float now = game_seconds(memory);
    if (!std::isfinite(now) || now < 0) return;
    const auto actor = memory.load32(state + 0x38);
    const float rest = 0.9f + float((actor >> 4u) % 7u) * 0.08f;
    auto [entry, inserted] = next_reposition.try_emplace(state, now + rest);
    auto &next = entry->second;
    if (memory.load32(state + 4) != 0) { next = now + rest; return; }
    if (inserted || now < next) return;
    // Respect native non-idle transitions (damage, path orders, retreat, etc.).
    if (memory.load8(state + 0x18) && memory.load32(state + 0x1c) != 0) return;
    const auto lateral = memory.load32(state) == 0x08a8e1d0u ? 1u : 2u;
    memory.store32(state + 0x1c, lateral);
    memory.store8(state + 0x18, 1);
    next = now + rest;
    static unsigned traces{};
    if (trace_enabled() && traces++ < 128) std::fprintf(stderr,
        "[pc-balance] reposition actor=%08x state=%08x lateral=%u time=%.3f\n", actor, state, lateral, now);
}

bool pc_balance_keep_strafing(psprecomp::GuestMemory &memory, std::uint32_t state) {
    // Patrol's lateral state normally cancels as soon as sight returns. Keep
    // its native timer, direction, collision checks and ordinary firing path.
    return mobile_battle(memory, state) && memory.load32(state) == 0x08a8e1d0u &&
        memory.load32(state + 4) == 1 && memory.load32(state + 0x10) == 4;
}

void pc_balance_trace_shot(psprecomp::GuestMemory &memory, std::uint32_t hit) {
    static unsigned traces{};
    // CWeapon's emission path, after creating the CHitInfo. The earlier
    // CCharacter::Shoot return value only acknowledges a held fire request.
    if (!trace_enabled() || traces >= 256 || !hit || !memory.contains(hit, 0x18)) return;
    const auto actor = memory.load32(hit + 0x14);
    if (!actor || !memory.contains(actor, 0x10a4) || memory.load32(actor) != soldier_vtable) return;
    const auto ai = actor + 0xc60, battle = memory.load32(ai + 0x440);
    if (memory.load32(ai) != soldier_ai_vtable || !memory.contains(battle, 8)) return;
    const auto motion = memory.load32(actor + 0x78);
    if (!memory.contains(motion, 0x50)) return;
    ++traces;
    std::fprintf(stderr, "[pc-balance-probe] shot actor=%08x state=%u time=%.3f x=%.3f z=%.3f\n",
        actor, memory.load32(battle + 4), game_seconds(memory),
        std::bit_cast<float>(memory.load32(motion + 0x40)), std::bit_cast<float>(memory.load32(motion + 0x48)));
}
std::uint32_t pc_balance_hit_damage(psprecomp::GuestMemory &memory, std::uint32_t target,
                                   std::uint32_t hit, std::uint32_t damage, unsigned source_offset) {
    // Ordinary projectile CHitInfo: source +0x14 (setter 0x0894f624).
    // The alternate hit descriptor used at 0x08884bfc has source +0.
    if ((source_offset != 0 && source_offset != 20) || !memory.contains(hit, source_offset + 4)) return damage;
    return pc_balance_damage(memory, target, memory.load32(hit + source_offset), damage);
}
float pc_balance_attack_pause(psprecomp::GuestMemory &memory, std::uint32_t ai, float pause) {
    if (!std::isfinite(pause) || pause <= 0 || !campaign_active(memory) ||
        !memory.contains(ai, 0x34) || memory.load32(ai) != soldier_ai_vtable) return pause;
    const auto actor = memory.load32(ai + 0x30);
    if (!hostile_actor(memory, actor) || memory.load32(actor) != soldier_vtable) return pause;
    // CZakoAI::attack at 0x08910d50 constructs a rest interval (+0x5c4).
    // Shorten only that pause, once at assignment; burst length, weapon fire
    // rate, accuracy, animations and sight/memory tests remain native.
    const auto adjusted = pause * 0.7f;
    static unsigned traces{};
    if (trace_enabled() && traces++ < 32) std::fprintf(stderr,
        "[pc-balance] attack-pause actor=%08x original=%.4f adjusted=%.4f\n", actor, pause, adjusted);
    return adjusted;
}

bool run_pc_balance_tests(std::string &error) {
    try {
        psprecomp::Runtime runtime;
        struct RestoreState {
            bool selected = pc_balance_enabled();
            std::uint32_t player = active_player;
            unsigned fraction = damage_fraction, companion = companion_fraction;
            decltype(next_reposition) movement = next_reposition;
            ~RestoreState() { set_pc_balance_enabled(selected); active_player = player; damage_fraction = fraction; companion_fraction = companion; next_reposition = std::move(movement); }
        } restore;
        auto &memory = runtime.memory();
        constexpr std::uint32_t player = 0x08001000, parameters = 0x08002000, session = 0x08012000;
        const auto require = [](bool ok, const char *reason) { if (!ok) throw std::runtime_error(reason); };
        const auto prepare = [&](unsigned hp, unsigned maximum, unsigned mode = 0, unsigned type = player_vtable) {
            memory.store32(run_mode, mode); memory.store32(player, type);
            memory.store32(session_data_pointer, session); memory.store32(session + 0xdc8, 0);
            memory.store32(player + 0x110, parameters); memory.store32(parameters + 0x7c, maximum);
            memory.store32(player + 0x90, hp); memory.store32(player + 0x94, maximum);
        };
        for (unsigned maximum : {1u, 100u, 200u, 250u, 300u, 400u}) {
            prepare(maximum, maximum);
            const auto expected = std::max(1u, (maximum * 2u + 2u) / 5u);
            require(apply_pc_balance(memory, player, true), "Campaign player rejected");
            require(memory.load32(player + 0x90) == expected && memory.load32(player + 0x94) == expected,
                    "Initial or maximum HP does not match 40 percent");
            require(memory.load32(parameters + 0x7c) == maximum, "Shared original character parameters changed");
            require(apply_pc_balance(memory, player, true) && memory.load32(player + 0x90) == expected,
                    "Repeated initialization compounded the reduction");
        }
        // A wounded player carries absolute HP between areas, including zero.
        for (unsigned hp : {0u, 1u, 100u, 187u, 200u, 250u}) {
            prepare(hp, 250);
            require(apply_pc_balance(memory, player, true) && memory.load32(player + 0x90) == std::min(hp, 100u),
                    "Transition healed, resurrected or repeatedly penalized the player");
        }
        prepare(100, 250);
        require(!apply_pc_balance(memory, player, false) && memory.load32(player + 0x90) == 100 &&
                memory.load32(player + 0x94) == 250, "Disabled mode changed original health");
        for (unsigned mode : {2u, 3u, 4u, 5u, 0xffffffffu}) {
            prepare(250, 250, mode);
            require(!apply_pc_balance(memory, player, true) && memory.load32(player + 0x94) == 250,
                    "Debug or unknown run mode was modified");
        }
        for (unsigned type : {0u, 0x08a88678u, 0x08a8ce58u}) {
            prepare(250, 250, 0, type);
            require(!apply_pc_balance(memory, player, true) && memory.load32(player + 0x90) == 250,
                    "Non-player actor was modified");
        }
        prepare(100, 250); memory.store32(player + 0x94, 999);
        require(!apply_pc_balance(memory, player, true) && memory.load32(player + 0x90) == 100,
                "Unexpected health layout partially patched");
        prepare(250, 250); memory.store32(player + 0x110, 0xffffffffu);
        require(!apply_pc_balance(memory, player, true), "Invalid character parameters accepted");
        require(!apply_pc_balance(memory, 0xffffffffu, true), "Invalid player accepted");
        // Exercise the compiled guest hook and its real recovery clamp, so a
        // missing generated patch or a stale generated object fails this test.
        prepare(250, 250);
        psprecomp::AllegrexContext context{};
        context.pc = 0x0882332c; context.gpr[21] = player; context.gpr[29] = 0x08004000;
        const bool previous = pc_balance_enabled();
        set_pc_balance_enabled(true);
        psprecomp::recomp_unit_0007(runtime, context);
        set_pc_balance_enabled(previous);
        require(memory.load32(player + 0x90) == 100 && memory.load32(player + 0x94) == 100,
                "Generated CPlayer initialization hook missing");
        // Real multiplayer uses run_mode=0 too. Exercise its actual flag
        // through the compiled hook, with the 150 HP seen in the real round.
        prepare(150, 150); memory.store32(session + 0xdc8, 1);
        context = {}; context.pc = 0x0882332c; context.gpr[21] = player; context.gpr[29] = 0x08004000;
        set_pc_balance_enabled(true);
        psprecomp::recomp_unit_0007(runtime, context);
        require(memory.load32(player + 0x90) == 150 && memory.load32(player + 0x94) == 150,
                "Multiplayer initialization reduced original health");
        prepare(250, 250); memory.store32(session_data_pointer, 0);
        require(!apply_pc_balance(memory, player, true), "Missing session information enabled balance");
        prepare(250, 250); require(apply_pc_balance(memory, player, true), "Campaign did not resume after multiplayer");
        memory.store32(player + 0x90, 230); // health after a recovery pickup
        context = {}; context.pc = 0x08886fd0;
        context.gpr[17] = player; context.gpr[2] = 230; context.gpr[29] = 0x08004000;
        psprecomp::recomp_unit_0032(runtime, context);
        require(memory.load32(player + 0x90) == 100, "Original recovery code exceeded the reduced maximum");
        constexpr std::uint32_t soldier = 0x08008000, ai = 0x0800a000, hit = 0x0800b000;
        memory.store32(player + 0xd0, 0);
        memory.store32(soldier, soldier_vtable); memory.store32(soldier + 0xd0, 1);
        memory.store8(soldier + 0xc40, 0);
        memory.store32(soldier_vtable + 0x88, 0x088842ec);
        memory.store32(soldier_vtable + 0x140, 0x0888d9b0);
        memory.store32(ai, soldier_ai_vtable); memory.store32(ai + 0x30, soldier);
        memory.store32(hit, soldier);
        memory.store32(hit + 20, soldier);
        const auto close = [](float a, float b) { return std::fabs(a - b) < 0.00001f; };
        for (unsigned damage : {0u, 1u, 5u, 10u, 20u, 100u, 1'000'000u}) {
            require(pc_balance_damage(memory, player, soldier, damage) == (damage * 16u + 5u) / 10u,
                    "Hostile damage does not match 160 percent");
        }
        require(pc_balance_damage(memory, player, soldier, 0xffffffffu) == 0xffffffffu,
                "Negative damage/sentinel was amplified");
        unsigned weak_damage{};
        for (unsigned i = 0; i < 10; ++i) weak_damage += pc_balance_damage(memory, player, soldier, 4);
        require(weak_damage == 64, "Rounding erased the damage boost on weak attacks");
        for (unsigned source : {0u, player, 0xffffffffu})
            require(pc_balance_damage(memory, player, source, 20) == 20, "Self or unattributed damage changed");
        require(pc_balance_damage(memory, soldier, player, 20) == 20, "Outgoing player damage changed");
        require(pc_balance_hit_damage(memory, player, 0xffffffffu, 20) == 20, "Invalid hit accepted");
        for (unsigned team : {0u, 0xffffffffu}) {
            memory.store32(soldier + 0xd0, team);
            require(pc_balance_damage(memory, player, soldier, 20) == 20 &&
                    pc_balance_attack_pause(memory, ai, 2) == 2, "Ally or neutral actor was boosted");
        }
        memory.store32(soldier + 0xd0, 1); memory.store8(soldier + 0xc40, 1);
        require(pc_balance_damage(memory, player, soldier, 20) == 20 &&
                pc_balance_attack_pause(memory, ai, 2) == 2, "Special allegiance actor was boosted");
        memory.store8(soldier + 0xc40, 0);
        require(close(pc_balance_attack_pause(memory, ai, 2), 1.4f), "Attack pause not reduced by 30 percent");
        for (float interval : {0.0f, -1.0f, std::numeric_limits<float>::infinity()})
            require(pc_balance_attack_pause(memory, ai, interval) == interval, "Invalid pause changed");
        memory.store32(ai, 0);
        require(pc_balance_attack_pause(memory, ai, 2) == 2, "Unknown AI changed");
        memory.store32(ai, soldier_ai_vtable);
        // Exercise all three compiled damage subtraction sites. Unregistered virtual
        // reaction targets return to the dispatcher after the HP write.
        context = {}; context.pc = 0x08884c68; context.gpr[16] = player;
        context.gpr[17] = 10; context.gpr[20] = hit; context.gpr[29] = 0x08004000;
        psprecomp::recomp_unit_0032(runtime, context);
        require(memory.load32(player + 0x90) == 84, "Generated hit damage hook missing or applied twice");
        context = {}; context.pc = 0x08884400; context.gpr[17] = player;
        context.gpr[19] = soldier; context.gpr[21] = 10; context.gpr[3] = memory.load32(player + 0x90);
        context.gpr[29] = 0x08004000;
        psprecomp::recomp_unit_0032(runtime, context);
        require(memory.load32(player + 0x90) == 68, "Generated direct damage hook missing or applied twice");
        context = {}; context.pc = 0x088846e0; context.gpr[16] = player;
        context.gpr[17] = 10; context.gpr[18] = hit; context.gpr[29] = 0x08004000;
        psprecomp::recomp_unit_0032(runtime, context);
        require(memory.load32(player + 0x90) == 52, "Generated projectile damage hook missing or applied twice");
        constexpr std::uint32_t root = 0x0800c000, vehicle = 0x0800d000;
        memory.store32(0x08aa8ddc, root); memory.store32(root + 0x94, player);
        memory.store32(root + 0xb4, player); memory.store32(player + 0x588, vehicle);
        memory.store32(vehicle, 0x08a88678); memory.store32(vehicle + 0xd0, 0);
        require(pc_balance_damage(memory, soldier, vehicle, 100) == 5,
                "Autonomous Tachikoma damage was not reduced by 95 percent");
        unsigned companion_total{};
        for (unsigned i = 0; i < 20; ++i) companion_total += pc_balance_damage(memory, soldier, vehicle, 4);
        require(companion_total == 4, "Weak companion attacks lost the 5 percent fraction");
        memory.store32(root + 0xb4, vehicle);
        require(pc_balance_damage(memory, soldier, vehicle, 100) == 100, "Piloted Tachikoma was weakened");
        memory.store32(root + 0xb4, player); memory.store32(vehicle + 0xd0, 1);
        require(pc_balance_damage(memory, soldier, vehicle, 100) == 100, "Enemy Tachikoma was weakened");
        memory.store32(vehicle + 0xd0, 0);
        constexpr std::uint32_t state = 0x0800e000, clock = 0x0800f000;
        memory.store32(0x08ac5d80, clock);
        const auto at = [&](float seconds) { memory.store32(clock + 0x64, std::bit_cast<std::uint32_t>(seconds)); };
        memory.store32(state, 0x08a8e1d0); memory.store32(state + 0x38, soldier);
        memory.store32(state + 0x3c, ai); memory.store32(state + 0x10, 4);
        memory.store32(ai + 0x104, player); memory.store32(soldier + 0x90, 100);
        at(1); pc_balance_reposition(memory, state);
        require(memory.load8(state + 0x18) == 0, "Movement started without an idle interval");
        at(3);
        context = {}; context.pc = 0x08923cf8; context.gpr[19] = state; context.gpr[29] = 0x08010000;
        psprecomp::recomp_unit_0071(runtime, context);
        require(memory.load8(state + 0x18) == 1 && memory.load32(state + 0x1c) == 1,
                "Generated patrol movement hook did not request its native lateral state");
        memory.store32(state + 4, 1); memory.store8(state + 0x18, 0);
        require(pc_balance_keep_strafing(memory, state), "Visible target cancelled the native lateral state");
        at(4); pc_balance_reposition(memory, state);
        require(!memory.load8(state + 0x18), "Ongoing movement was interrupted");
        memory.store32(state, 0x08a8db10); memory.store32(state + 4, 0);
        at(6); context = {}; context.pc = 0x08916ee4; context.gpr[19] = state; context.gpr[29] = 0x08010000;
        psprecomp::recomp_unit_0068(runtime, context);
        require(memory.load32(state + 0x1c) == 2, "Assault used the wrong lateral state");
        memory.store32(state + 0x1c, 7); at(9); pc_balance_reposition(memory, state);
        require(memory.load32(state + 0x1c) == 7, "Native non-idle transition was overwritten");
        memory.store32(state, 0x08a8e1d0); memory.store32(state + 4, 1);
        // Native random pause: random % 2 + 0.5 + (1 - skill * 0.1).
        // Start with random=1 and skill=1's contribution=0.9: 2.4 -> 1.68.
        memory.store32(0x08a8d72c, std::bit_cast<std::uint32_t>(0.5f));
        const auto guest_pause = [&] {
            context = {}; context.pc = 0x08910d50; context.gpr[16] = ai;
            context.gpr[2] = 1; context.fpr[20] = 0.9f; context.gpr[29] = 0x08004000;
            psprecomp::recomp_unit_0067(runtime, context);
            return std::bit_cast<float>(memory.load32(ai + 1476));
        };
        require(close(guest_pause(), 1.68f) && close(guest_pause(), 1.68f),
                "Generated attack pause hook missing or compounded");
        set_pc_balance_enabled(false);
        require(pc_balance_damage(memory, player, soldier, 20) == 32 && close(guest_pause(), 1.68f),
                "Toggle changed combat before next initialization");
        for (unsigned mode : {2u, 3u, 4u, 5u, 0xffffffffu}) {
            memory.store32(run_mode, mode);
            require(pc_balance_damage(memory, player, soldier, 20) == 20 && close(guest_pause(), 2.4f),
                    "Multiplayer combat changed");
            require(pc_balance_damage(memory, soldier, vehicle, 100) == 100 &&
                    !pc_balance_keep_strafing(memory, state), "Multiplayer companion or movement changed");
            memory.store32(state + 4, 0); memory.store8(state + 0x18, 0);
            at(20); pc_balance_reposition(memory, state);
            require(!memory.load8(state + 0x18), "Multiplayer issued a movement command");
            memory.store32(state + 4, 1);
        }
        // Also reject a multiplayer transition while a campaign player is
        // still latched, even though the debug run_mode remains zero.
        memory.store32(run_mode, 0); memory.store32(session + 0xdc8, 1);
        require(pc_balance_damage(memory, player, soldier, 20) == 20 && close(guest_pause(), 2.4f) &&
                pc_balance_damage(memory, soldier, vehicle, 100) == 100 &&
                !pc_balance_keep_strafing(memory, state), "Real multiplayer flag retained combat modifiers");
        memory.store32(state + 4, 0); memory.store8(state + 0x18, 0);
        at(25); pc_balance_reposition(memory, state);
        require(!memory.load8(state + 0x18), "Real multiplayer flag allowed a movement command");
        prepare(250, 250);
        require(!apply_pc_balance(memory, player, false), "Disabled initialization accepted");
        require(pc_balance_damage(memory, player, soldier, 20) == 20 && close(guest_pause(), 2.4f),
                "Disabled mission retained combat modifiers");
        require(pc_balance_damage(memory, soldier, vehicle, 100) == 100 &&
                !pc_balance_keep_strafing(memory, state), "Disabled mission retained new modifiers");
        error.clear(); return true;
    } catch (const std::exception &exception) { error = exception.what(); return false; }
}
}
