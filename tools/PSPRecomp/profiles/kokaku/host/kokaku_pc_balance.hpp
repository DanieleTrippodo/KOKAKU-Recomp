#pragma once
#include "psprecomp/guest_memory.hpp"
#include <string>

namespace kokaku {
void set_pc_balance_enabled(bool enabled);
bool pc_balance_enabled();
// Called once at the end of CPlayer initialization, after carried HP is restored.
bool apply_pc_balance(psprecomp::GuestMemory &memory, std::uint32_t player, bool enabled);
// Combat modifiers use the selection latched at the last player initialization.
std::uint32_t pc_balance_damage(psprecomp::GuestMemory &memory, std::uint32_t target,
                               std::uint32_t source, std::uint32_t damage);
std::uint32_t pc_balance_hit_damage(psprecomp::GuestMemory &memory, std::uint32_t target,
                                   std::uint32_t hit, std::uint32_t damage, unsigned source_offset = 0);
float pc_balance_attack_pause(psprecomp::GuestMemory &memory, std::uint32_t ai, float pause);
void pc_balance_reposition(psprecomp::GuestMemory &memory, std::uint32_t battle_state);
bool pc_balance_keep_strafing(psprecomp::GuestMemory &memory, std::uint32_t battle_state);
void pc_balance_trace_shot(psprecomp::GuestMemory &memory, std::uint32_t hit);
bool run_pc_balance_tests(std::string &error);
}
