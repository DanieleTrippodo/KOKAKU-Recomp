#pragma once
#include <algorithm>
#include <cstdint>
namespace psprecomp { class GuestMemory; }
namespace kokaku {

// How the copy of a remote player is played back. DME timestamps share the
// session clock; the copy follows that clock from a distance, between the
// position samples actually received, and never beyond the last one.
//
// The distance is 80 ms, which suits samples that arrive as regularly as on a
// local network. Over the internet some arrive late. The playback then reaches
// the newest sample and has nothing to move towards: the original game clears
// its history at that point, so the player jumps to the next sample and stands
// still while the history fills again. Two things keep that from happening:
// the distance grows to what a late sample needed and shrinks back slowly, and
// a history that ran out is kept for a moment, so a late sample costs a short
// pause instead of a jump.
struct ReplicaClock {
    std::uint32_t newest{};    // timestamp of the newest sample seen
    std::uint32_t delay{};     // current distance from the session clock, ms
    std::uint32_t cursor{};    // playback time handed to the game last
    std::uint32_t updated{};   // session time of that call
    std::uint32_t settle{};    // session time counted towards the next shrink
    bool started{};
};

inline constexpr std::uint32_t replica_delay_min = 80;    // the local network value
inline constexpr std::uint32_t replica_delay_max = 250;
inline constexpr std::uint32_t replica_margin_ms = 4;     // added to what a late sample needed
inline constexpr std::uint32_t replica_shrink_ms = 300;   // the distance shrinks by 1 ms per this much time
// How long a history that ran out is kept: enough for the sample after a lost
// one, short enough that a player who simply stopped is not kept "arriving".
inline constexpr std::uint32_t replica_hold_ms = 120;
inline constexpr std::uint32_t replica_cadence_ms = 150;  // samples further apart are not a moving player's

// True when a is later than b on the wrapping session clock.
constexpr bool replica_later(std::uint32_t a, std::uint32_t b) noexcept {
    return static_cast<std::int32_t>(a - b) > 0;
}

// The playback time for this frame. `now` is the session clock, `step` what it
// advanced since the previous frame, `oldest` and `newest` the timestamps at
// both ends of the history.
constexpr std::uint32_t replica_playback_time(ReplicaClock &clock, std::uint32_t now, std::uint32_t step,
                                              std::uint32_t oldest, std::uint32_t newest) noexcept {
    // Another history (a respawn, a long silence, timestamps that went back) starts afresh.
    if (!clock.started || replica_later(clock.newest, newest) || now - clock.updated > 1'000u) {
        clock = ReplicaClock{};
        clock.started = true;
        clock.newest = newest;
        clock.delay = replica_delay_min;
        clock.cursor = oldest;
    } else {
        if (newest != clock.newest) {
            // A new sample. Up to the frame before, the playback had only the
            // previous one to move towards: this is how far behind the clock
            // it had to be to still have that one ahead of it.
            const auto apart = newest - clock.newest;
            const auto needed = now - step - clock.newest;
            if (apart <= replica_cadence_ms && static_cast<std::int32_t>(needed) > 0 && needed < 1'000u)
                clock.delay = std::max(clock.delay, std::min(needed + replica_margin_ms, replica_delay_max));
            clock.newest = newest;
        }
        clock.settle += now - clock.updated;
        while (clock.settle >= replica_shrink_ms) {
            clock.settle -= replica_shrink_ms;
            if (clock.delay > replica_delay_min) --clock.delay;
        }
    }
    clock.updated = now;
    auto target = now - clock.delay;
    if (replica_later(oldest, target)) target = oldest;
    if (replica_later(target, newest)) target = newest;
    // A distance that just grew must not pull the player back along its path.
    if (replica_later(clock.cursor, target) && !replica_later(clock.cursor, newest)) target = clock.cursor;
    clock.cursor = target;
    return target;
}

// The game asks this when no new sample came and its cursor is past the newest
// one: true lets it clear the history, as it does for a player who stopped.
constexpr bool replica_history_spent(const ReplicaClock &clock, std::uint32_t now, std::uint32_t newest) noexcept {
    if (!clock.started) return true;
    return replica_later(now - clock.delay, newest + replica_hold_ms);
}

// Hooks in the remote player controller (generated unit 0050).
void update_replica_playback_clock(psprecomp::GuestMemory &, std::uint32_t controller);
std::uint32_t replica_keep_history(psprecomp::GuestMemory &, std::uint32_t controller, std::uint32_t spent);
}
