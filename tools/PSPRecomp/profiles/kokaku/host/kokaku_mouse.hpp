#pragma once
#include "display_window.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

namespace kokaku {
bool is_local_controlled_actor(const psprecomp::GuestMemory &, std::uint32_t actor);
std::uint32_t local_weapon_holder(const psprecomp::GuestMemory &, std::uint32_t actor);
bool update_weapon_cycle(psprecomp::Runtime &, psprecomp::AllegrexContext &, std::uint32_t player);
// Called at the character motion update, before its orientation is committed.
void update_mouse_look(psprecomp::GuestMemory &, std::uint32_t player, std::uint32_t angles);
bool apply_mouse_look(psprecomp::GuestMemory &, std::uint32_t player, std::uint32_t angles,
                      const vcs::MouseMotion &motion);
// Deterministic diagnostic mouse input (PSPRECOMP_MOUSE_TEST / _SCRIPT); never active in normal play.
void scripted_mouse_motion(std::uint64_t vblank, vcs::MouseMotion &motion);
// PSPRECOMP_ACTOR_TRACE=1: one line per motion update of the local actor.
void trace_local_actor(const psprecomp::GuestMemory &, std::uint32_t actor, std::uint32_t angles,
                       std::uint64_t vblank);
bool run_mouse_tests(std::string &error);
}
