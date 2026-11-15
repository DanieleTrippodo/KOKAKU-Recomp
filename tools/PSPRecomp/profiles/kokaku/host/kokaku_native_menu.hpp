#pragma once
#include "psprecomp/runtime.hpp"
#include <filesystem>
#include <string>
namespace kokaku {
void install_native_menu(psprecomp::Runtime &runtime);
bool native_menu_key(int key);
bool native_menu_active();
bool native_option_screen();
bool native_menu_navigation_active();
void native_menu_navigation_drawn(bool title);
void native_menu_frame(std::uint64_t frame, std::uint32_t buffer, std::uint32_t stride, std::uint32_t format);
void native_savedata_open(const std::filesystem::path &root, const std::string &game, const std::string &save, bool writing);
// -2 pending, -1 cancelled, 0..4 selected slot. Existing save name is slot zero.
int native_savedata_update(std::uint32_t buttons);
std::string native_savedata_name(int slot);
bool run_native_menu_tests(std::string &error);
}
