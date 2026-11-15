#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace kokaku {
// Type A, verified in the game's KeyAssign screen: face buttons control look.
// Fractional duty preserves small mouse motion when expressing it as PSP buttons.
struct MouseLookButtons {
    int credit_x{}, credit_y{}, sign_x{}, sign_y{};
    std::uint64_t last_frame{UINT64_MAX};
    std::uint32_t buttons{};

    std::uint32_t sample(std::uint64_t frame, int x, int y, bool enabled) noexcept {
        if (!enabled) { *this = {}; return 0u; }
        if (frame == last_frame) return buttons;
        last_frame = frame;
        const auto axis = [](int value, int &credit, int &previous_sign) {
            const int sign = value < 0 ? -1 : value > 0 ? 1 : 0;
            if (sign == 0) return 0;
            if (sign != previous_sign) credit = 0;
            previous_sign = sign;
            credit += std::min(std::abs(value), 127);
            if (credit < 127) return 0;
            credit -= 127;
            return sign;
        };
        const int horizontal = axis(x, credit_x, sign_x);
        const int vertical = axis(y, credit_y, sign_y);
        buttons = (horizontal < 0 ? 0x8000u : horizontal > 0 ? 0x2000u : 0u) |
                  (vertical < 0 ? 0x4000u : vertical > 0 ? 0x1000u : 0u);
        return buttons;
    }
};
}
