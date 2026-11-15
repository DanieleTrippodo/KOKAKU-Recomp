#pragma once
#include "kokaku_controls.hpp"
namespace kokaku {
Controls controls_snapshot();
bool controls_commit(const Controls &value, std::string &error);
bool controls_open() noexcept;
void show_controls(void *owner);
bool controls_message(void *message);
}
