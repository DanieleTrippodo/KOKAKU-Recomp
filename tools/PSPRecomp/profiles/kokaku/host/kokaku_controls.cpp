#include "kokaku_controls.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace kokaku {
bool valid_binding(int key) noexcept {
    return key >= 0 && key <= wheel_down && key != 0x70 && key != 0x71;
}
Controls::Controls() {
    for (std::size_t i = 0; i < actions.size(); ++i)
        keys[i] = {actions[i].primary, actions[i].secondary};
}
void Controls::bind(std::size_t action, std::size_t slot, int key) {
    if (action >= keys.size() || slot > 1 || !valid_binding(key)) return;
    const int previous = keys[action][slot];
    if (previous == key) return;
    if (key) for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto a = actions[action].context, b = actions[i].context;
        if (a != b && a != ControlContext::Always && b != ControlContext::Always) continue;
        for (std::size_t j = 0; j < 2; ++j)
            if ((i != action || j != slot) && keys[i][j] == key)
                keys[i][j] = previous;
    }
    keys[action][slot] = key;
}
bool Controls::load(const std::filesystem::path &path, std::string &error) {
    std::ifstream in(path);
    if (!in) { error = "Unable to read " + path.string(); return false; }
    Controls candidate;
    bool has_reload = false;
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto name = line.substr(0, eq), value = line.substr(eq + 1);
        if (name == "Reload") has_reload = true;
        if (name == "Sensitivity") {
            std::istringstream number(value); float v{};
            if (number >> v && std::isfinite(v) && v >= 0.01f && v <= 2.0f) candidate.sensitivity = v;
        } else if (name == "InvertY") {
            if (value == "0" || value == "1") candidate.invert_y = value == "1";
        } else for (std::size_t i = 0; i < actions.size(); ++i) {
            if (name != actions[i].id) continue;
            std::istringstream numbers(value); int first{}, second{}; char comma{};
            if (numbers >> first >> comma >> second && comma == ',' &&
                valid_binding(first) && valid_binding(second)) candidate.keys[i] = {first, second};
        }
    }
    if (in.bad()) { error = "Unable to read controls"; return false; }
    // Earlier PC profiles assigned R to PSP Down (lock-on). Migrate only that
    // old default; keep a custom lock binding and any explicit Reload mapping.
    if (!has_reload && candidate.keys[lock_action] == std::array<int,2>{'R',0})
        candidate.keys[lock_action] = {'F',0};
    *this = candidate; error.clear(); return true;
}
bool Controls::save(const std::filesystem::path &path, std::string &error) const {
    auto temporary = path; temporary += ".tmp";
    std::ofstream out(temporary, std::ios::trunc);
    out << "[MouseKeyboard]\nSensitivity=" << sensitivity << "\nInvertY=" << invert_y << '\n';
    for (std::size_t i = 0; i < actions.size(); ++i)
        out << actions[i].id << '=' << keys[i][0] << ',' << keys[i][1] << '\n';
    out.close();
    if (!out) { error = "Unable to save " + path.string(); return false; }
    // Use the platform's atomic replace, preserving the last good file on error.
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Unable to replace " + path.string(); return false;
    }
#else
    std::error_code ec; std::filesystem::rename(temporary, path, ec);
    if (ec) { error = ec.message(); return false; }
#endif
    error.clear(); return true;
}
}
