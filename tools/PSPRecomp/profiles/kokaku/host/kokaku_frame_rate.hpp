#pragma once
#include "psprecomp/guest_memory.hpp"
#include <string>
#include <chrono>
namespace kokaku {
bool apply_frame_rate_patch(psprecomp::GuestMemory &memory, unsigned fps);
// Original scene lifecycle, including fade-in and explicit skip/end.
void set_cutscene_active(psprecomp::GuestMemory &memory, bool active);
unsigned effective_game_frame_rate();
// Called once at CFrame's timing update, before its scaled/unscaled accumulators.
void adjust_frame_delta(psprecomp::GuestMemory &memory, unsigned frame,
                        float &scaled_seconds, float &unscaled_seconds);
// CInertiaBhvr::Move (0x08898fc8, Tachikoma and other inertial actors) applies
// its friction parameter once per update, so shorter updates than the PSP's
// 30 Hz lowered both top speed and acceleration. These return the equivalent
// friction and acceleration time for the current update; unchanged at 30 Hz.
float inertia_friction(const psprecomp::GuestMemory &memory, float friction);
float inertia_acceleration_seconds(const psprecomp::GuestMemory &memory, unsigned behavior, float seconds);
void wait_frame_deadline(std::chrono::steady_clock::time_point deadline);
void pace_original_game_frame();
void pace_native_game_frame();
bool native_frame_pacing_enabled(const psprecomp::GuestMemory &memory);
bool run_frame_rate_tests(std::string &error);
}
