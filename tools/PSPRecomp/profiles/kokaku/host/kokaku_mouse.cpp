#include "kokaku_mouse.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace kokaku {
bool is_local_controlled_actor(const psprecomp::GuestMemory &memory, std::uint32_t actor) {
    if (!memory.contains(actor, 0x7c)) return false;
    const auto type = memory.load32(actor);
    if (type != 0x08A74358u && type != 0x08A88678u) return false;
    // CGame::GetControlledActor (0x08818C9C): the local input target changes
    // from CPlayer to CTachikoma on boarding. Both share the motion commit.
    const auto root = memory.load32(0x08AA8DDCu);
    if (!memory.contains(root, 0xb8)) return false;
    const auto controlled = memory.load32(root + 0xb4u);
    return actor == (controlled ? controlled : memory.load32(root + 0x94u));
}

std::uint32_t local_weapon_holder(const psprecomp::GuestMemory &memory, std::uint32_t actor) {
    if (!is_local_controlled_actor(memory, actor)) return 0;
    // The original virtual GetWeaponHolder accessors return these embedded
    // inventories: CPlayer + 0x6a0, CTachikoma + 0x2510 (CTckmWeaponHolder).
    const bool vehicle = memory.load32(actor) == 0x08A88678u;
    const auto holder = actor + (vehicle ? 0x2510u : 0x6a0u);
    if (!memory.contains(holder, 0x82c) ||
        memory.load32(holder) != (vehicle ? 0x08A8D158u : 0x08A96830u) ||
        memory.load32(holder+0x50cu) != actor) return 0;
    const auto count = memory.load32(holder+0x504u);
    return count && count <= 64 ? holder : 0;
}

bool apply_mouse_look(psprecomp::GuestMemory &memory, std::uint32_t player, std::uint32_t angles,
                      const vcs::MouseMotion &motion) {
    // NPCs and the unoccupied Tachikoma also call this shared motion update.
    if (!motion.enabled || !is_local_controlled_actor(memory, player) || !memory.contains(angles, 16) ||
        !std::isfinite(motion.yaw_degrees) || !std::isfinite(motion.pitch_degrees)) return false;
    const auto read = [&](std::uint32_t address) { return std::bit_cast<float>(memory.load32(address)); };
    const float pitch = read(player + 0x24), yaw = read(angles + 4);
    // Limits verified in the guest's pitch routines at 0x0880A0C4/0x0880A120:
    // their this pointer is player+4; lower=-this[20], upper=this[16].
    const float lower = -read(player + 0x18), upper = read(player + 0x14);
    if (!std::isfinite(pitch) || !std::isfinite(yaw) || !std::isfinite(lower) ||
        !std::isfinite(upper) || lower > 0 || upper < 0 || lower < -std::numbers::pi_v<float> ||
        upper > std::numbers::pi_v<float>) return false;
    constexpr double radians = std::numbers::pi / 180.0;
    // Relative displacement, not a button or a velocity: no deadzone, duty
    // cycling, acceleration, delayed tail, or multiplication by frame time.
    if (motion.yaw_degrees != 0) {
        const auto next = static_cast<float>(std::remainder(yaw - motion.yaw_degrees * radians, 2 * std::numbers::pi));
        memory.store32(angles + 4, std::bit_cast<std::uint32_t>(next));
    }
    if (motion.pitch_degrees != 0) {
        const auto next = static_cast<float>(std::clamp(pitch + motion.pitch_degrees * radians,
                                                       static_cast<double>(lower), static_cast<double>(upper)));
        memory.store32(player + 0x24, std::bit_cast<std::uint32_t>(next));
    }
    return true;
}

void scripted_mouse_motion(std::uint64_t vblank, vcs::MouseMotion &motion) {
    // PSPRECOMP_MOUSE_TEST=1 with _START/_END/_X/_Y is one window of degrees
    // per motion update. PSPRECOMP_MOUSE_TEST_SCRIPT="start:end:x:y;..." adds
    // further windows, so one run can turn, board and then aim.
    struct Window { std::uint64_t start, end; float x, y; };
    static const std::vector<Window> windows = [] {
        std::vector<Window> result;
        const auto number = [](const char *name, const char *fallback) {
            const char *value = std::getenv(name);
            return std::string(value && *value ? value : fallback);
        };
        if (std::getenv("PSPRECOMP_MOUSE_TEST"))
            result.push_back({std::strtoull(number("PSPRECOMP_MOUSE_TEST_START", "5000").c_str(), nullptr, 0),
                              std::strtoull(number("PSPRECOMP_MOUSE_TEST_END", "5010").c_str(), nullptr, 0),
                              std::strtof(number("PSPRECOMP_MOUSE_TEST_X", "0").c_str(), nullptr),
                              std::strtof(number("PSPRECOMP_MOUSE_TEST_Y", "0").c_str(), nullptr)});
        if (const char *script = std::getenv("PSPRECOMP_MOUSE_TEST_SCRIPT")) {
            const char *cursor = script;
            while (*cursor) {
                char *next{};
                Window window{};
                window.start = std::strtoull(cursor, &next, 0);
                if (next == cursor || *next != ':') break;
                cursor = next + 1; window.end = std::strtoull(cursor, &next, 0);
                if (next == cursor || *next != ':') break;
                cursor = next + 1; window.x = std::strtof(cursor, &next);
                if (next == cursor || *next != ':') break;
                cursor = next + 1; window.y = std::strtof(cursor, &next);
                if (next == cursor) break;
                result.push_back(window);
                cursor = *next == ';' ? next + 1 : next;
                if (*next && *next != ';') break;
            }
        }
        return result;
    }();
    for (const auto &window : windows)
        if (vblank >= window.start && vblank <= window.end) motion = {true, window.x, window.y};
}

void trace_local_actor(const psprecomp::GuestMemory &memory, std::uint32_t actor, std::uint32_t angles,
                       std::uint64_t vblank) {
    static const bool enabled = std::getenv("PSPRECOMP_ACTOR_TRACE") != nullptr;
    if (!enabled || !is_local_controlled_actor(memory, actor) || !memory.contains(angles, 16)) return;
    const bool vehicle = memory.load32(actor) == 0x08A88678u;
    // Virtual GetBhvr: CPlayer + 0x590, CTachikoma + 0x23f0.
    const auto bhvr = actor + (vehicle ? 0x23f0u : 0x590u);
    const auto holder = local_weapon_holder(memory, actor);
    if (!memory.contains(bhvr, 0x120)) return;
    const auto f = [&](std::uint32_t address) { return std::bit_cast<float>(memory.load32(address)); };
    std::uint32_t ammo = ~0u, slot = ~0u;
    if (holder) {
        slot = memory.load32(holder + 0x514u);
        if (slot < memory.load32(holder + 0x504u)) {
            const auto entry = holder + 4u + 20u * slot;
            const auto component = memory.load32(entry);
            if (component < 3) {
                const auto weapon = memory.load32(entry + 8u + 4u * component);
                if (memory.contains(weapon, 0x364)) ammo = memory.load32(weapon + 0x1f4u);
            }
        }
    }
    static const auto trace_origin = std::chrono::steady_clock::now();
    // The companion is CPlayer + 0x588; boarding needs it close and in view.
    float companion[3]{};
    const auto root = memory.load32(0x08AA8DDCu);
    const auto character = memory.contains(root, 0xb8) ? memory.load32(root + 0x94u) : 0u;
    const auto other = memory.contains(character, 0x58c) ? memory.load32(character + 0x588u) : 0u;
    if (memory.contains(other, 0x4c))
        for (int i = 0; i < 3; ++i) companion[i] = f(other + 0x40u + 4u * static_cast<std::uint32_t>(i));
    // CCamctrl (world + 0x88): eye at +0x110, look-at target at +0x190.
    float camera[6]{};
    const auto view = memory.contains(root, 0x9c) ? memory.load32(root + 0x98u) : 0u;
    if (memory.contains(view, 0x1a0))
        for (int i = 0; i < 3; ++i) {
            camera[i] = f(view + 0x110u + 4u * static_cast<std::uint32_t>(i));
            camera[3 + i] = f(view + 0x190u + 4u * static_cast<std::uint32_t>(i));
        }
    std::fprintf(stderr,
        "[actor-trace] vblank=%llu actor=%s pos=%.3f,%.3f,%.3f yaw=%.5f pitch=%.5f state=%u "
        "vel=%.3f,%.3f,%.3f inertia=%.3f,%.3f input=%.3f,%.3f angvel=%.4f slot=%d ammo=%d hp=%d "
        "companion=%.3f,%.3f,%.3f ms=%.2f cam=%.3f,%.3f,%.3f look=%.3f,%.3f,%.3f\n",
        static_cast<unsigned long long>(vblank), vehicle ? "tachikoma" : "player",
        f(actor + 0x40), f(actor + 0x44), f(actor + 0x48), f(angles + 4), f(actor + 0x24),
        memory.load32(bhvr + 0x6c), f(bhvr + 0x50), f(bhvr + 0x54), f(bhvr + 0x58),
        f(bhvr + 0xe0), f(bhvr + 0xe8), f(bhvr + 0xd0), f(bhvr + 0xd8), f(bhvr + 0x14),
        static_cast<int>(slot), static_cast<int>(ammo), static_cast<int>(memory.load32(actor + 0x90)),
        companion[0], companion[1], companion[2],
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - trace_origin).count(),
        camera[0], camera[1], camera[2], camera[3], camera[4], camera[5]);
}

bool run_mouse_tests(std::string &error) {
    try {
        psprecomp::GuestMemory memory;
        constexpr std::uint32_t player=0x08001000, angles=0x08002000, root=0x08003000;
        const auto put = [&](std::uint32_t a, float f) { memory.store32(a, std::bit_cast<std::uint32_t>(f)); };
        const auto get = [&](std::uint32_t a) { return std::bit_cast<float>(memory.load32(a)); };
        const auto require = [](bool ok, const char *msg) { if (!ok) throw std::runtime_error(msg); };
        memory.store32(player, 0x08A74358);
        memory.store32(0x08AA8DDC, root);
        memory.store32(root+0x94, player);
        memory.store32(root+0xb4, player);
        put(player+0x14, 1.134464f); put(player+0x18, 1.134464f);
        for (int i=0; i<1000; ++i)
            require(apply_mouse_look(memory,player,angles,{true,0.001f,-0.001f}), "Small motion rejected");
        constexpr float degree=std::numbers::pi_v<float>/180.f;
        require(std::abs(get(angles+4)+degree)<0.000001f && std::abs(get(player+0x24)+degree)<0.000001f,
                "Sub-pixel motion was lost or scaled by time");
        const auto yaw=memory.load32(angles+4), pitch=memory.load32(player+0x24);
        for (int i=0; i<1000; ++i) apply_mouse_look(memory,player,angles,{true,0,0});
        apply_mouse_look(memory,player,angles,{false,100,100});
        require(memory.load32(angles+4)==yaw && memory.load32(player+0x24)==pitch, "Idle or disabled mouse drifted");
        apply_mouse_look(memory,player,angles,{true,720,10000});
        require(std::abs(get(angles+4)+degree)<0.000001f && get(player+0x24)==1.134464f, "Yaw wrap or pitch limit failed");
        apply_mouse_look(memory,player,angles,{true,0,-10000});
        require(get(player+0x24)==-1.134464f, "Upper look limit failed");
        constexpr std::uint32_t vehicle=0x08004000, vehicle_angles=0x08005000;
        memory.store32(vehicle, 0x08A88678);
        put(vehicle+0x14, 1.134464f); put(vehicle+0x18, 1.134464f);
        const auto character_holder = player+0x6a0u, vehicle_holder = vehicle+0x2510u;
        memory.store32(character_holder, 0x08A96830u);
        memory.store32(character_holder+0x50cu, player);
        memory.store32(character_holder+0x504u, 5);
        memory.store32(vehicle_holder, 0x08A8D158u);
        memory.store32(vehicle_holder+0x50cu, vehicle);
        memory.store32(vehicle_holder+0x504u, 5);
        require(local_weapon_holder(memory,player)==character_holder && !local_weapon_holder(memory,vehicle),
            "On-foot input selected the wrong inventory");
        require(!apply_mouse_look(memory,vehicle,vehicle_angles,{true,10,10}), "Mouse steered the unoccupied Tachikoma");
        memory.store32(root+0xb4, vehicle);
        require(local_weapon_holder(memory,vehicle)==vehicle_holder && !local_weapon_holder(memory,player),
            "Mounted input did not switch to the Tachikoma inventory");
        memory.store32(vehicle_holder+0x50cu, player);
        require(!local_weapon_holder(memory,vehicle), "An inventory with the wrong owner was accepted");
        memory.store32(vehicle_holder+0x50cu, vehicle);
        require(!apply_mouse_look(memory,player,angles,{true,10,10}), "Inactive character consumed mounted mouse motion");
        require(apply_mouse_look(memory,vehicle,vehicle_angles,{true,10,10}) &&
            std::abs(get(vehicle_angles+4)+10*degree)<0.000001f &&
            std::abs(get(vehicle+0x24)-10*degree)<0.000001f, "Mounted mouse yaw/pitch failed");
        memory.store32(root+0xb4, player);
        require(local_weapon_holder(memory,player)==character_holder && !local_weapon_holder(memory,vehicle),
            "Weapon input did not return to the character after exit");
        require(apply_mouse_look(memory,player,angles,{true,1,1}) &&
            !apply_mouse_look(memory,vehicle,vehicle_angles,{true,1,1}), "Mouse target did not return to character after exit");
        memory.store32(player,0x08A74000);
        require(!apply_mouse_look(memory,player,angles,{true,10,10}), "Mouse changed an NPC");
        require(!apply_mouse_look(memory,0xB0,angles,{true,10,10}), "Invalid player was dereferenced");
        error.clear(); return true;
    } catch(const std::exception &e) { error=e.what(); return false; }
}
}
