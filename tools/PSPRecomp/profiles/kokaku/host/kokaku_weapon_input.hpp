#pragma once
#include "kokaku_controls.hpp"
#include <deque>

namespace kokaku {
constexpr int reload_request = 2;
// Weapon bindings are semantic actions, not PSP D-pad buttons. Keep every
// wheel detent, and trigger a keyboard/mouse-button binding only on its edge.
class WeaponCycleInput {
    std::array<bool, 3> held_{};
    std::deque<int> pending_;
public:
    template<class Pressed>
    void sample(const Controls &controls, int wheel, bool active, Pressed pressed) {
        if (!active) pending_.clear();
        for (std::size_t direction = 0; direction < held_.size(); ++direction) {
            bool down = false, up_wheel = false, down_wheel = false;
            for (int key : controls.keys[direction == 2 ? reload_action : 8 + direction]) {
                up_wheel |= key == wheel_up;
                down_wheel |= key == wheel_down;
                if (key > 0 && key < wheel_up) down |= pressed(key);
            }
            int count = int(down && !held_[direction]);
            held_[direction] = down;
            if (up_wheel && wheel > 0) count += wheel;
            if (down_wheel && wheel < 0) count -= wheel;
            while (active && count-- > 0 && pending_.size() < 32)
                pending_.push_back(direction == 2 ? reload_request : direction ? 1 : -1);
        }
    }
    int pop() {
        if (pending_.empty()) return 0;
        const int result = pending_.front();
        pending_.pop_front();
        return result;
    }
    void clear() { pending_.clear(); }
    void suspend() { pending_.clear(); held_.fill(true); }
};
}
