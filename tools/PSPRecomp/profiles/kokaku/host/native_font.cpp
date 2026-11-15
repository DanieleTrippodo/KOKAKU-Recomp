#include "native_font.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace kokaku {
namespace {
constexpr std::uint32_t invalid = 0x80460003u;
constexpr std::uint32_t library_handle = 0x0F000001u;
constexpr std::uint32_t font_handle = 0x0F000002u;

struct Glyph {
    int width{}, height{}, left{}, top{}, advance{};
    std::vector<std::uint8_t> alpha;
};

struct NativeFont {
    std::unordered_map<std::uint16_t, Glyph> glyphs;
    bool library_open{}, font_open{};
#ifdef _WIN32
    HDC dc{CreateCompatibleDC(nullptr)};
    HFONT font{CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Tahoma")};
    HGDIOBJ previous{dc && font ? SelectObject(dc, font) : nullptr};
    ~NativeFont() {
        if (dc && previous) SelectObject(dc, previous);
        if (font) DeleteObject(font);
        if (dc) DeleteDC(dc);
    }
#endif
    const Glyph &get(std::uint16_t code) {
        if (auto found = glyphs.find(code); found != glyphs.end()) return found->second;
        Glyph glyph{};
#ifdef _WIN32
        if (!dc || !font) throw std::runtime_error("Unable to create native font");
        if (code >= 32u) {
            GLYPHMETRICS metrics{};
            MAT2 identity{};
            identity.eM11.value = identity.eM22.value = 1;
            const DWORD size = GetGlyphOutlineW(dc, code, GGO_GRAY8_BITMAP, &metrics, 0u, nullptr, &identity);
            if (size != GDI_ERROR) {
                glyph.width = metrics.gmBlackBoxX;
                glyph.height = metrics.gmBlackBoxY;
                glyph.left = metrics.gmptGlyphOrigin.x;
                glyph.top = metrics.gmptGlyphOrigin.y;
                glyph.advance = metrics.gmCellIncX;
                // GDI may report a nominal black box for a spacing character
                // while returning no bitmap. Retain its advance, not that box.
                if (size == 0u) glyph.width = glyph.height = 0;
                std::vector<std::uint8_t> bytes(size);
                if (size != 0u && GetGlyphOutlineW(dc, code, GGO_GRAY8_BITMAP, &metrics,
                        size, bytes.data(), &identity) == GDI_ERROR)
                    throw std::runtime_error("Unable to rasterize native font glyph");
                const std::size_t pitch = (glyph.width + 3u) & ~3u;
                glyph.alpha.resize(static_cast<std::size_t>(glyph.width) * glyph.height);
                for (int y = 0; y < glyph.height; ++y)
                    for (int x = 0; x < glyph.width; ++x)
                        glyph.alpha[y * glyph.width + x] = static_cast<std::uint8_t>(
                            std::min(255u, bytes[y * pitch + x] * 255u / 64u));
            }
        }
#endif
        return glyphs.emplace(code, std::move(glyph)).first->second;
    }
};

void style(psprecomp::GuestMemory &memory, std::uint32_t destination) {
    memory.zero(destination, 168u);
    for (unsigned i = 0u; i < 4u; ++i)
        memory.store32(destination + i * 4u, std::bit_cast<std::uint32_t>(i < 2u ? 12.0f : 96.0f));
    memory.store16(destination + 20u, 1u);  // sans serif
    memory.store16(destination + 22u, 1u);  // regular
    memory.store16(destination + 26u, 2u);  // Latin
    constexpr std::array<std::uint8_t, 7> name{'T', 'a', 'h', 'o', 'm', 'a', 0};
    memory.copy_in(destination + 32u, name);
}

void char_info(psprecomp::GuestMemory &memory, std::uint32_t destination, const Glyph &glyph) {
    memory.zero(destination, 60u);
    const std::array<int, 14> values{glyph.width, glyph.height, glyph.left, glyph.top,
        glyph.width * 64, glyph.height * 64, glyph.top * 64, (glyph.top - glyph.height) * 64,
        glyph.left * 64, glyph.top * 64, 0, 0, glyph.advance * 64, 16 * 64};
    for (unsigned i = 0; i < values.size(); ++i)
        memory.store32(destination + i * 4u, static_cast<std::uint32_t>(values[i]));
}

std::uint32_t draw_glyph(psprecomp::GuestMemory &memory, std::uint32_t image, const Glyph &glyph,
                         int clip_x, int clip_y, int clip_width, int clip_height) {
    if (!memory.contains(image, 24u)) return invalid;
    const auto format = memory.load32(image);
    const int origin_x = static_cast<std::int32_t>(memory.load32(image + 4u)) >> 6;
    const int origin_y = static_cast<std::int32_t>(memory.load32(image + 8u)) >> 6;
    const int width = memory.load16(image + 12u), height = memory.load16(image + 14u);
    const unsigned pitch = memory.load16(image + 16u);
    const auto buffer = memory.load32(image + 20u);
    if (format > 4u) return invalid;
    if (format != 0u && format != 2u) return 0u;  // unsupported PSP output formats draw nothing
    if (!memory.contains(buffer, static_cast<std::size_t>(pitch) * height)) return invalid;
    const int row_pixels = format == 0u ? pitch * 2u : pitch;
    for (int y = 0; y < glyph.height; ++y) {
        const int py = origin_y + y;
        if (py < 0 || py >= height || py < clip_y ||
                static_cast<std::int64_t>(py) >= static_cast<std::int64_t>(clip_y) + clip_height) continue;
        for (int x = 0; x < glyph.width; ++x) {
            const int px = origin_x + x;
            if (px < 0 || px >= width || px >= row_pixels || px < clip_x ||
                    static_cast<std::int64_t>(px) >= static_cast<std::int64_t>(clip_x) + clip_width) continue;
            const unsigned alpha = glyph.alpha[y * glyph.width + x];
            const auto address = buffer + py * pitch + (format == 0u ? px / 2 : px);
            const unsigned old = memory.load8(address);
            if (format == 0u) {
                const unsigned shift = (px & 1) * 4u;
                const unsigned nibble = std::min(15u, ((old >> shift) & 15u) + (alpha * 15u + 127u) / 255u);
                memory.store8(address, (old & ~(15u << shift)) | (nibble << shift));
            } else {
                memory.store8(address, std::min(255u, old + alpha));
            }
        }
    }
    return 0u;
}
}

void install_native_font(psprecomp::Runtime &runtime) {
    const auto font = std::make_shared<NativeFont>();
    const auto error_out = [](psprecomp::Runtime &rt, std::uint32_t address, std::uint32_t value) {
        if (address != 0u && rt.memory().contains(address, 4u)) rt.memory().store32(address, value);
    };
    runtime.register_hle("sceLibFont", 0x67F17ED7u, [font, error_out](auto &rt, auto &ctx) {
        font->library_open = true;
        error_out(rt, ctx.gpr[5], 0u);
        ctx.set_gpr(2, library_handle);
    });
    runtime.register_hle("sceLibFont", 0x574B6FBCu, [font](auto &, auto &ctx) {
        font->library_open = font->font_open = false;
        font->glyphs.clear();
        ctx.set_gpr(2, 0u);
    });
    runtime.register_hle("sceLibFont", 0x27F6E642u, [font, error_out](auto &rt, auto &ctx) {
        const bool valid = font->library_open && ctx.gpr[4] == library_handle;
        error_out(rt, ctx.gpr[5], valid ? 0u : invalid);
        ctx.set_gpr(2, valid ? 1u : 0u);
    });
    runtime.register_hle("sceLibFont", 0xBC75D85Bu, [font](auto &rt, auto &ctx) {
        if (!font->library_open || ctx.gpr[4] != library_handle || ctx.gpr[6] == 0u ||
                !rt.memory().contains(ctx.gpr[5], 168u)) { ctx.set_gpr(2, invalid); return; }
        style(rt.memory(), ctx.gpr[5]);
        ctx.set_gpr(2, 0u);
    });
    runtime.register_hle("sceLibFont", 0xA834319Du, [font, error_out](auto &rt, auto &ctx) {
        const bool valid = font->library_open && ctx.gpr[4] == library_handle && ctx.gpr[5] == 0u;
        font->font_open = valid;
        error_out(rt, ctx.gpr[7], valid ? 0u : invalid);
        ctx.set_gpr(2, valid ? font_handle : 0u);
    });
    runtime.register_hle("sceLibFont", 0x3AEA8CB6u, [font](auto &, auto &ctx) {
        font->font_open = false;
        ctx.set_gpr(2, 0u);
    });
    runtime.register_hle("sceLibFont", 0x0DA7535Eu, [font](auto &rt, auto &ctx) {
        const auto out = ctx.gpr[5];
        if (!font->font_open || ctx.gpr[4] != font_handle || !rt.memory().contains(out, 264u)) {
            ctx.set_gpr(2, invalid); return;
        }
        rt.memory().zero(out, 264u);
        const std::array<int, 10> metrics{20, 20, 16, -4, 0, 16, 0, 16, 20, 20};
        for (unsigned i = 0; i < metrics.size(); ++i) {
            rt.memory().store32(out + 4u * i, metrics[i] * 64);
            rt.memory().store32(out + 40u + 4u * i, std::bit_cast<std::uint32_t>(static_cast<float>(metrics[i])));
        }
        rt.memory().store16(out + 80u, 20u);
        rt.memory().store16(out + 82u, 20u);
        rt.memory().store32(out + 84u, 256u);
        style(rt.memory(), out + 92u);
        rt.memory().store8(out + 260u, 4u);
        ctx.set_gpr(2, 0u);
    });
    runtime.register_hle("sceLibFont", 0xDCC80C2Fu, [font](auto &rt, auto &ctx) {
        if (!font->font_open || ctx.gpr[4] != font_handle || !rt.memory().contains(ctx.gpr[6], 60u)) {
            ctx.set_gpr(2, invalid); return;
        }
        char_info(rt.memory(), ctx.gpr[6], font->get(ctx.gpr[5] & 0xFFFFu));
        ctx.set_gpr(2, 0u);
    });
    runtime.register_hle("sceLibFont", 0xCA1E6945u, [font](auto &rt, auto &ctx) {
        if (!font->font_open || ctx.gpr[4] != font_handle) { ctx.set_gpr(2, invalid); return; }
        // Allegrex's extended argument registers carry args 4..7 in a3/t0/t1/t2.
        ctx.set_gpr(2, draw_glyph(rt.memory(), ctx.gpr[6], font->get(ctx.gpr[5] & 0xFFFFu),
            static_cast<std::int32_t>(ctx.gpr[7]), static_cast<std::int32_t>(ctx.gpr[8]),
            static_cast<std::int32_t>(ctx.gpr[9]), static_cast<std::int32_t>(ctx.gpr[10])));
    });
}

bool run_font_self_tests(std::string &error) {
    try {
        NativeFont font;
        for (unsigned code = 0u; code < 256u; ++code) (void)font.get(code);
        (void)font.get(0x2026u);
        (void)font.get(0x221Eu);
        const auto &glyph = font.get('A');
        if (glyph.width == 0 || glyph.advance == 0 ||
                std::none_of(glyph.alpha.begin(), glyph.alpha.end(), [](auto alpha) { return alpha != 0; }))
            throw std::runtime_error("Native font produced an empty A glyph");
        psprecomp::GuestMemory memory;
        constexpr auto image = 0x08801000u, buffer = 0x08802000u;
        memory.store32(image, 0u);
        memory.store32(image + 4u, 64u);  // odd pixel: preserve its neighbour's nibble
        memory.store16(image + 12u, 16u);
        memory.store16(image + 14u, 2u);
        memory.store16(image + 16u, 8u);
        memory.store32(image + 20u, buffer);
        memory.store8(buffer, 3u);
        const Glyph solid{2, 1, 0, 0, 2, {255u, 255u}};
        if (draw_glyph(memory, image, solid, 1, 0, 1, 1) != 0u ||
                memory.load8(buffer) != 0xF3u || memory.load8(buffer + 1u) != 0u)
            throw std::runtime_error("Font clipping or 4-bit neighbour preservation failed");
        memory.store32(image, 2u);
        memory.store8(buffer + 1u, 200u);
        if (draw_glyph(memory, image, solid, 1, 0, 1, 1) != 0u || memory.load8(buffer + 1u) != 255u)
            throw std::runtime_error("Font 8-bit coverage did not saturate");
        memory.store32(image + 20u, 0x0FFFFFF0u);
        if (draw_glyph(memory, image, solid, 0, 0, 16, 2) != invalid)
            throw std::runtime_error("Font accepted an invalid guest image buffer");
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}
}
