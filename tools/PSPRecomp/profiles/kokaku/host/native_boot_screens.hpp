#pragma once
#include <filesystem>
#include <string>

namespace kokaku {
// Host reconstruction using the original indexed GIM pictures in logo.psp.prx.
// Returns false when the player closes the window during the sequence.
bool play_boot_screens(const std::filesystem::path &game_root);
void dump_boot_screens(const std::filesystem::path &logo_prx, const std::filesystem::path &output);
bool run_boot_screen_self_tests(std::string &error);
}
