#include "kokaku_frame_rate.hpp"
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace kokaku {
namespace {
struct Constant { unsigned address, original, unlocked; };
constexpr std::array constants{
    Constant{0x08a8fa2c,0x40000000,0x3f800000}, // CFrame: 2 -> 1 display intervals.
    Constant{0x08a8fa30,0x40800000,0x40000000}, // Streaming/cutscene fallback: 4 -> 2.
    Constant{0x08a8fa3c,0x3d088889,0x3c888889}  // Fixed-step branch of CFrame::GetDeltaTime.
};
struct FrameClock {
    std::chrono::steady_clock::time_point previous{};
    unsigned object{};
    float elapsed{}, scale{};
    bool active{};

    float sample(std::chrono::steady_clock::time_point now, unsigned frame,
                 float game_elapsed, float time_scale, bool enabled, float original) {
        const auto seconds = std::chrono::duration<float>(now - previous).count();
        const bool continuous = active && enabled && object == frame &&
            game_elapsed >= elapsed && scale > 0 && time_scale > 0 &&
            seconds > 0 && seconds <= .1f;
        previous = now; object = frame; elapsed = game_elapsed;
        scale = time_scale; active = enabled;
        // Discard loading/pause/debugger gaps; never repay them in gameplay.
        // Keep collision steps inside the original 30 FPS frame budget.
        return continuous ? std::min(seconds, 1.f / 30.f) : original;
    }
};
FrameClock frame_clock;
unsigned selected_fps = 30;
bool cutscene_active = false;
bool write_frame_constants(psprecomp::GuestMemory &memory, unsigned fps) {
    for (const auto &entry : constants) {
        if (!memory.contains(entry.address, 4)) return false;
        const auto word = memory.load32(entry.address);
        if (word != entry.original && word != entry.unlocked) return false;
    }
    for (const auto &entry : constants)
        memory.store32(entry.address, fps >= 60 ? entry.unlocked : entry.original);
    return true;
}
bool realtime_campaign(const psprecomp::GuestMemory &memory, unsigned frame) {
    static const bool paced = [] {
        const char *v = std::getenv("PSPRECOMP_FRAME_LIMIT");
        return !v || (*v && *v != '0');
    }();
    if (!paced || selected_fps != 60 || !memory.contains(frame, 124) ||
        memory.load32(frame) != 0x08a8fad8u || !memory.load8(frame + 96) ||
        memory.load32(0x08aa8da8u) != 0) return false;
    const auto session = memory.load32(0x08aa8a3cu);
    // Multiplayer keeps its deterministic PSP timing. The session flag is
    // the native multiplayer selector; debug run_mode is not that selector.
    return session && memory.contains(session, 0xdcc) && memory.load32(session + 0xdc8) == 0;
}
}
bool apply_frame_rate_patch(psprecomp::GuestMemory &memory, unsigned fps) {
    // The main PRX is reloaded for mission transitions. Validate the complete
    // table and apply after every relocation, before its module_start runs.
    if (!write_frame_constants(memory, fps)) return false;
    selected_fps = fps;
    cutscene_active = false;
    frame_clock = {};
    return true;
}
unsigned effective_game_frame_rate() {
    return cutscene_active ? 30u : selected_fps;
}
void set_cutscene_active(psprecomp::GuestMemory &memory, bool active) {
    if (active == cutscene_active) return;
    const auto session = memory.load32(0x08aa8a3cu);
    // Do not introduce local timing changes into multiplayer or unknown states.
    if (active && (!session || !memory.contains(session, 0xdcc) ||
                   memory.load32(session + 0xdc8) != 0)) return;
    if (!write_frame_constants(memory, active ? 30u : selected_fps)) return;
    cutscene_active = active;
    frame_clock = {};
    if (std::getenv("PSPRECOMP_FRAME_CADENCE_DIAG"))
        std::fprintf(stderr, "[cutscene-timing] active=%u fps=%u\n",
                     unsigned(active), effective_game_frame_rate());
}
void adjust_frame_delta(psprecomp::GuestMemory &memory, unsigned frame,
                        float &scaled_seconds, float &unscaled_seconds) {
    if (!memory.contains(frame, 124)) { frame_clock = {}; return; }
    const auto scale = std::bit_cast<float>(memory.load32(frame + 112));
    const auto elapsed = std::bit_cast<float>(memory.load32(frame + 100));
    const bool active = realtime_campaign(memory, frame) && std::isfinite(scale) &&
                        scale >= 0 && std::isfinite(elapsed);
    const float original = scale > 0 ? scaled_seconds / scale : unscaled_seconds;
    const float seconds = frame_clock.sample(std::chrono::steady_clock::now(), frame,
                                             elapsed, scale, active, original);
    if (active) {
        scaled_seconds = seconds * scale;
        unscaled_seconds = seconds;
    }
}
namespace {
// Fraction of an original 30 Hz update covered by the current CFrame update.
// Unscaled time, so authored slow motion keeps its original relation to the
// friction parameter. Never above one: slow frames keep the PSP behavior.
float original_update_fraction(const psprecomp::GuestMemory &memory) {
    // PSPRECOMP_ORIGINAL_INERTIA=1 keeps the per-update friction, for A/B diagnostics only.
    static const bool per_update = [] {
        const char *value = std::getenv("PSPRECOMP_ORIGINAL_INERTIA");
        return value && *value == '1';
    }();
    if (per_update) return 1.f;
    const auto frame = memory.load32(0x08ac5d80u);
    if (!memory.contains(frame, 0x78) || memory.load32(frame) != 0x08a8fad8u) return 1.f;
    const auto unscaled = std::bit_cast<float>(memory.load32(frame + 0x74));
    if (!std::isfinite(unscaled) || unscaled <= 0) return 1.f;
    const float fraction = unscaled * 30.f;
    return fraction > .999f ? 1.f : std::max(fraction, 1.f / 64.f);
}
bool usable_friction(float friction) {
    return std::isfinite(friction) && friction > 0 && friction < 1;
}
}
float inertia_friction(const psprecomp::GuestMemory &memory, float friction) {
    const float fraction = original_update_fraction(memory);
    if (fraction == 1.f || !usable_friction(friction)) return friction;
    // The same decay per second as one multiplication every 1/30 s.
    return std::pow(friction, fraction);
}
float inertia_acceleration_seconds(const psprecomp::GuestMemory &memory, unsigned behavior, float seconds) {
    const float fraction = original_update_fraction(memory);
    if (fraction == 1.f || !memory.contains(behavior, 0xf8)) return seconds;
    // The friction getter 0x0889934c on the inertial path: actor parameters
    // (+0x110) field +0x78. Its alternate +0xb0 needs the mode check
    // 0x0898cedc, which also disables inertia in 0x088992ac.
    const auto actor = memory.load32(behavior + 0xc0);
    if (!memory.contains(actor, 0x114)) return seconds;
    const auto parameters = memory.load32(actor + 0x110);
    if (!memory.contains(parameters, 0x7c)) return seconds;
    const auto friction = std::bit_cast<float>(memory.load32(parameters + 0x78));
    if (!usable_friction(friction)) return seconds;
    // v' = f*v + a*dt per 30 Hz update. Splitting it into shorter updates with
    // friction f^k needs a*dt*(1 - f^k)/(k*(1 - f)) to reach the same speeds.
    return seconds * (1.f - std::pow(friction, fraction)) / (fraction * (1.f - friction));
}
bool native_frame_pacing_enabled(const psprecomp::GuestMemory &memory) {
    static const bool requested = [] {
        const char *v = std::getenv("PSPRECOMP_NATIVE_FRAME_PACING");
        return !v || (*v && *v != '0');
    }();
    // Cutscenes retain the PSP's interleaved vblank waits. A single 33 ms
    // native present sleep would also suspend the cooperative audio producer.
    return requested && !cutscene_active && realtime_campaign(memory, memory.load32(0x08ac5d80u));
}
void wait_frame_deadline(std::chrono::steady_clock::time_point deadline) {
    constexpr auto spin_margin=std::chrono::microseconds(500);
    const auto now=std::chrono::steady_clock::now();
    if(deadline-now>spin_margin) {
#if defined(_WIN32)
        // Sleep/sleep_until can round a short frame wait up to a scheduler tick
        // on Windows. A private high-resolution timer avoids changing the
        // system timer period and leaves only a short spin at the deadline.
        struct Timer {
            HANDLE handle=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
            ~Timer() { if(handle) CloseHandle(handle); }
        };
        static thread_local Timer timer;
        LARGE_INTEGER due{};
        due.QuadPart=-std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-spin_margin-now).count()/100;
        if(timer.handle && SetWaitableTimer(timer.handle,&due,0,nullptr,nullptr,FALSE))
            WaitForSingleObject(timer.handle,INFINITE);
        else
#endif
            std::this_thread::sleep_until(deadline-spin_margin);
    }
    while(std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
}
void pace_original_game_frame() {
    using clock=std::chrono::steady_clock;
    constexpr auto period=std::chrono::nanoseconds(1'000'000'000/30);
    static clock::time_point deadline{};
    const auto now=clock::now();
    if(deadline.time_since_epoch().count()==0 || now>deadline+period)
        deadline=now;
    wait_frame_deadline(deadline);
    deadline+=period;
}
void pace_native_game_frame() {
    using clock=std::chrono::steady_clock;
    constexpr auto period=std::chrono::nanoseconds(1'000'000'000/60);
    static clock::time_point deadline{};
    const auto now=clock::now();
    if(deadline.time_since_epoch().count()==0 || now>deadline+period)
        deadline=now;
    wait_frame_deadline(deadline);
    deadline+=period;
}
bool run_frame_rate_tests(std::string &error) {
    try {
        psprecomp::GuestMemory memory;
        const auto require=[](bool value,const char *reason) { if(!value) throw std::runtime_error(reason); };
        for(int transition=0;transition<3;++transition) {
            for(const auto &entry:constants) memory.store32(entry.address,entry.original);
            require(apply_frame_rate_patch(memory,60),"Reloaded main module rejected");
            require(apply_frame_rate_patch(memory,60),"Patch is not idempotent");
            for(const auto &entry:constants) require(memory.load32(entry.address)==entry.unlocked,"Reload reset frame timing");
            require(apply_frame_rate_patch(memory,30),"Original timing could not be restored");
            for(const auto &entry:constants) require(memory.load32(entry.address)==entry.original,"30 FPS timing differs from original");
        }
        memory.store32(constants.back().address,0x12345678);
        require(!apply_frame_rate_patch(memory,60),"Unknown executable data was patched");
        require(memory.load32(constants.front().address)==constants.front().original,"Rejected patch partially changed memory");
        using Clock = std::chrono::steady_clock;
        const auto origin = Clock::time_point(std::chrono::seconds(1));
        constexpr float step = 1.f / 60.f;
        for (const int rate : {43, 50, 60}) {
            FrameClock clock;
            float elapsed = 0;
            clock.sample(origin, 1, elapsed, 1, true, step);
            for (int i = 1; i <= rate * 10; ++i) {
                const auto now = origin + std::chrono::nanoseconds(1'000'000'000LL * i / rate);
                elapsed += clock.sample(now, 1, elapsed, 1, true, step);
            }
            require(std::abs(elapsed - 10.f) < .001f, "Simulation slows with render FPS");
        }
        FrameClock clock;
        clock.sample(origin, 1, 0, 1, true, step);
        auto now = origin + std::chrono::milliseconds(20);
        require(std::abs(clock.sample(now, 1, .02f, .5f, true, step) * .5f - .01f) < 1e-6f,
                "Authored slow motion was discarded");
        now += std::chrono::seconds(1);
        require(clock.sample(now, 1, .03f, 1, true, step) == step, "Loading stall was repaid");
        now += std::chrono::milliseconds(20);
        clock.sample(now, 1, .03f, 0, true, 0);
        now += std::chrono::milliseconds(20);
        require(clock.sample(now, 1, .03f, 1, true, step) == step, "Pause time was repaid");
        now += std::chrono::milliseconds(20);
        require(clock.sample(now, 2, 0, 1, true, step) == step, "A new CFrame inherited old time");
        now += std::chrono::milliseconds(20);
        require(clock.sample(now, 2, 0, 1, false, step) == step, "Disabled timing was changed");
        now += std::chrono::milliseconds(20);
        clock.sample(now, 2, 1, 1, true, step);
        now += std::chrono::milliseconds(20);
        require(clock.sample(now, 2, 0, 1, true, step) == step, "Mission restart inherited old time");
        now += std::chrono::milliseconds(80);
        require(clock.sample(now, 2, .02f, 1, true, step) == 1.f / 30.f,
                "A stalled frame exceeds the collision step budget");
        const unsigned object = 0x08e00000, session = 0x08e01000;
        memory.store32(object, 0x08a8fad8u); memory.store8(object + 96, 1);
        memory.store32(0x08aa8a3c, session); memory.store32(0x08aa8da8, 0);
        memory.store32(session + 0xdc8, 1); selected_fps = 60;
        require(!realtime_campaign(memory, object), "Multiplayer admits wall-clock physics");
        memory.store32(0x08aa8a3c, 0);
        require(!realtime_campaign(memory, object), "Absent session admits wall-clock physics");
        for (const auto &entry : constants) memory.store32(entry.address, entry.original);
        require(apply_frame_rate_patch(memory, 60), "Scene test setup failed");
        set_cutscene_active(memory, true);
        require(effective_game_frame_rate() == 60, "Unknown session was capped");
        memory.store32(0x08aa8a3c, session);
        set_cutscene_active(memory, true);
        require(effective_game_frame_rate() == 60, "Multiplayer timing was capped");
        memory.store32(session + 0xdc8, 0);
        for (int i = 0; i < 3; ++i) {
            set_cutscene_active(memory, true); // Fade start.
            set_cutscene_active(memory, true); // Scene start is idempotent.
            require(effective_game_frame_rate() == 30, "Cutscene is not capped at 30");
            for (const auto &entry : constants)
                require(memory.load32(entry.address) == entry.original, "Cutscene retained 60 FPS constants");
            set_cutscene_active(memory, false); // End or skip.
            require(effective_game_frame_rate() == 60, "Gameplay frame rate was not restored");
        }
        set_cutscene_active(memory, true);
        require(apply_frame_rate_patch(memory, 60) && effective_game_frame_rate() == 60,
                "Module reload retained an old cutscene");
        require(apply_frame_rate_patch(memory, 30), "Original frame rate setup failed");
        set_cutscene_active(memory, true);
        set_cutscene_active(memory, false);
        require(effective_game_frame_rate() == 30, "Scene end overwrote user-selected 30 FPS");
        // CInertiaBhvr: Tachikoma parameters (acceleration 5 x 650, friction
        // 0.9 per update, top speed 650). Its speed after a given time must not
        // depend on the update rate, and 30 Hz must stay bit-identical.
        const unsigned inertia_frame = 0x08e02000, behavior = 0x08e03000, actor = 0x08e04000,
                       parameters = 0x08e05000;
        const auto put = [&](unsigned address, float value) {
            memory.store32(address, std::bit_cast<std::uint32_t>(value));
        };
        memory.store32(0x08ac5d80, inertia_frame); memory.store32(inertia_frame, 0x08a8fad8u);
        memory.store32(behavior + 0xc0, actor); memory.store32(actor + 0x110, parameters);
        put(parameters + 0x78, .9f);
        const auto speed_after = [&](float unscaled, int updates, float time_scale = 1.f) {
            put(inertia_frame + 0x74, unscaled);
            float kept = 0, speed = 0;
            for (int i = 0; i < updates; ++i) {
                speed = std::min(650.f, kept + 3250.f *
                    inertia_acceleration_seconds(memory, behavior, unscaled * time_scale));
                kept = speed * inertia_friction(memory, .9f);
            }
            return speed;
        };
        put(inertia_frame + 0x74, 2.f / 60.f);
        require(inertia_friction(memory, .9f) == .9f &&
                inertia_acceleration_seconds(memory, behavior, 2.f / 60.f) == 2.f / 60.f,
                "30 Hz inertia differs from the original");
        put(inertia_frame + 0x74, .05f); // A dropped PSP frame keeps the PSP behavior.
        require(inertia_friction(memory, .9f) == .9f &&
                inertia_acceleration_seconds(memory, behavior, .05f) == .05f,
                "Slow frames changed the original inertia");
        const float original_speed = speed_after(1.f / 30.f, 6); // 0.2 s
        require(std::abs(original_speed - 507.6f) < .1f, "Inertia reference changed");
        for (const int rate : {45, 60, 120, 240}) {
            require(std::abs(speed_after(1.f / rate, rate / 5) - original_speed) < 1.f,
                    "Acceleration depends on the update rate");
            require(speed_after(1.f / rate, rate * 2) == 650.f, "Top speed depends on the update rate");
        }
        require(std::abs(speed_after(1.f / 60.f, 12, .5f) - speed_after(1.f / 30.f, 6, .5f)) < 1.f,
                "Slow motion inertia depends on the update rate");
        put(inertia_frame + 0x74, 1.f / 60.f);
        for (const float invalid : {0.f, 1.f, 1.5f, -.5f, std::numeric_limits<float>::quiet_NaN()}) {
            const float result = inertia_friction(memory, invalid);
            require(std::bit_cast<std::uint32_t>(result) == std::bit_cast<std::uint32_t>(invalid),
                    "Unusable friction was changed");
            put(parameters + 0x78, invalid);
            require(inertia_acceleration_seconds(memory, behavior, 1.f / 60.f) == 1.f / 60.f,
                    "Unusable friction changed the acceleration");
        }
        put(parameters + 0x78, .9f);
        memory.store32(inertia_frame, 0);
        require(inertia_friction(memory, .9f) == .9f &&
                inertia_acceleration_seconds(memory, behavior, 1.f / 60.f) == 1.f / 60.f,
                "Inertia changed without a CFrame");
        memory.store32(inertia_frame, 0x08a8fad8u);
        require(inertia_acceleration_seconds(memory, 0xB0, 1.f / 60.f) == 1.f / 60.f,
                "Invalid behavior was dereferenced");
        error.clear();return true;
    } catch(const std::exception &exception) { error=exception.what();return false; }
}
}
