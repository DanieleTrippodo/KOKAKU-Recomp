#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace kokaku {
constexpr int wheel_up = 256, wheel_down = 257;
constexpr std::size_t reload_action = 10, lock_action = 27;
enum class ControlContext { Always, Game, Menu };
struct ControlAction {
    const char *id;
    const wchar_t *label;
    ControlContext context;
    std::uint32_t button;
    int primary, secondary;
};
inline constexpr std::array actions{
    ControlAction{"Forward", L"Move forward", ControlContext::Always, 0, 'W', 0},
    ControlAction{"Back", L"Move backward", ControlContext::Always, 0, 'S', 0},
    ControlAction{"Left", L"Strafe left", ControlContext::Always, 0, 'A', 0},
    ControlAction{"Right", L"Strafe right", ControlContext::Always, 0, 'D', 0},
    ControlAction{"Walk", L"Walk", ControlContext::Always, 0, 0x10, 0},
    ControlAction{"Jump", L"Jump", ControlContext::Game, 0x100, 0x20, 'Q'},
    ControlAction{"Attack", L"Attack / fire", ControlContext::Game, 0x200, 1, 0},
    ControlAction{"Action", L"Interact", ControlContext::Game, 0x10, 'E', 2},
    ControlAction{"PreviousWeapon", L"Previous weapon", ControlContext::Game, 0x80, wheel_up, 0},
    ControlAction{"NextWeapon", L"Next weapon", ControlContext::Game, 0x20, wheel_down, 0},
    ControlAction{"Reload", L"Reload", ControlContext::Game, 0, 'R', 0},
    ControlAction{"LookUp", L"Look up", ControlContext::Game, 0x1000, 'I', 0},
    ControlAction{"LookDown", L"Look down", ControlContext::Game, 0x4000, 'K', 0},
    ControlAction{"LookLeft", L"Look left", ControlContext::Game, 0x8000, 'J', 0},
    ControlAction{"LookRight", L"Look right", ControlContext::Game, 0x2000, 'L', 0},
    ControlAction{"Pause", L"Pause / Start", ControlContext::Always, 8, 0x1b, 0x0d},
    ControlAction{"Select", L"Select", ControlContext::Always, 1, 9, 0},
    ControlAction{"Confirm", L"Menu: confirm", ControlContext::Menu, 0x4000, 'Z', 0x20},
    ControlAction{"Cancel", L"Menu: back", ControlContext::Menu, 0x2000, 'X', 2},
    ControlAction{"Square", L"Menu: square action", ControlContext::Menu, 0x8000, 'C', 0},
    ControlAction{"Triangle", L"Menu: triangle action", ControlContext::Menu, 0x1000, 'V', 0},
    ControlAction{"MenuUp", L"Menu: up", ControlContext::Menu, 0x10, 0x26, 0},
    ControlAction{"MenuDown", L"Menu: down", ControlContext::Menu, 0x40, 0x28, 0},
    ControlAction{"MenuLeft", L"Menu: left", ControlContext::Menu, 0x80, 0x25, 0},
    ControlAction{"MenuRight", L"Menu: right", ControlContext::Menu, 0x20, 0x27, 0},
    ControlAction{"MenuL", L"Menu: left shoulder", ControlContext::Menu, 0x100, 'Q', 0},
    ControlAction{"MenuR", L"Menu: right shoulder", ControlContext::Menu, 0x200, 'E', 1},
    ControlAction{"Down", L"Lock-on", ControlContext::Game, 0x40, 'F', 0}
};
struct Controls {
    std::array<std::array<int, 2>, actions.size()> keys{};
    float sensitivity{0.12f}; // Degrees per raw mouse count.
    bool invert_y{};
    Controls();
    // A duplicate in the same context is swapped, so both actions remain usable.
    void bind(std::size_t action, std::size_t slot, int key);
    bool load(const std::filesystem::path &path, std::string &error);
    bool save(const std::filesystem::path &path, std::string &error) const;
};
inline ControlContext input_control_context(bool fps_mouse) {
    return fps_mouse ? ControlContext::Game : ControlContext::Menu;
}
// Shared by live input, prompt validation and deterministic keyboard diagnostics.
template<class Pressed>
std::uint32_t control_buttons(const Controls &controls, ControlContext context, Pressed pressed) {
    std::uint32_t buttons{};
    for (std::size_t i = 0; i < actions.size(); ++i) {
        if (i == 8 || i == 9) continue; // Native weapon actions do not emit PSP left/right.
        const auto &action = actions[i];
        if (action.context != ControlContext::Always && action.context != context) continue;
        if (pressed(controls.keys[i][0]) || pressed(controls.keys[i][1])) buttons |= action.button;
    }
    return buttons;
}
bool valid_binding(int key) noexcept;
bool run_controls_tests(std::string &error);
}
