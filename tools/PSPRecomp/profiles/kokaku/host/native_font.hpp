#pragma once
#include "psprecomp/runtime.hpp"
#include <string>

namespace kokaku {
// Native Windows font substitute; this does not decode Sony PGF fonts.
void install_native_font(psprecomp::Runtime &runtime);
bool run_font_self_tests(std::string &error);
}
