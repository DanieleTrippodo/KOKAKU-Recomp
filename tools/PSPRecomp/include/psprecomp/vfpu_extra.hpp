#pragma once
#include "psprecomp/allegrex_context.hpp"

namespace psprecomp {
// VCRS computes the cyclic products used by a cross product. It overrides the
// first three prefix swizzles while retaining constants, abs and negate bits.
inline void execute_vfpu_vcrs(AllegrexContext &ctx, std::uint32_t word) noexcept {
    const unsigned length = 1u + ((word >> 7u) & 1u) + (((word >> 15u) & 1u) << 1u);
    float source[4]{}, target[4]{}, result[4]{};
    ctx.vfpu_ctrl[0] = (ctx.vfpu_ctrl[0] & ~0x3Fu) | 0x09u;
    ctx.vfpu_ctrl[1] = (ctx.vfpu_ctrl[1] & ~0x3Fu) | 0x12u;
    ctx.read_vfpu_vector_with_source_prefix(source, (word >> 8u) & 127u, length, 0u);
    ctx.read_vfpu_vector_with_source_prefix(target, (word >> 16u) & 127u, length, 1u);
    for (unsigned i = 0u; i < length; ++i) result[i] = source[i] * target[i];
    ctx.write_vfpu_vector_with_destination_prefix(result, word & 127u, length);
}
}
