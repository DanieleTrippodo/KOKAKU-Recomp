#include "native_boot_screens.hpp"
#include "display_window.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace kokaku {
namespace {
using Bytes = std::vector<std::byte>;
constexpr std::array<const char *, 4> names{"Production I.G", "BANDAI", "Sony Computer Entertainment", "G-Artists"};

std::uint32_t word(std::span<const std::byte> data, std::size_t offset, unsigned count = 4) {
    if (offset > data.size() || count > data.size() - offset)
        throw std::runtime_error("Truncated splash GIM");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= std::to_integer<std::uint32_t>(data[offset + i]) << (8 * i);
    return value;
}

Bytes decode_logo(std::span<const std::byte> data) {
    // logo.psp.prx contains four 480x272 indexed8, swizzled pictures and
    // linear RGBA8888 palettes. Offsets below are the GIM block fields.
    if (word(data, 0x30, 2) != 4 || word(data, 0x44, 2) != 5 ||
            word(data, 0x46, 2) != 1 || word(data, 0x48, 2) != 480 ||
            word(data, 0x4A, 2) != 272 || word(data, 0x4C, 2) != 8)
        throw std::runtime_error("Unsupported splash GIM layout");
    const std::size_t palette_block = 0x30ull + word(data, 0x34);
    if (word(data, palette_block, 2) != 5 || word(data, palette_block + 0x14, 2) != 3 ||
            word(data, palette_block + 0x18, 2) != 256)
        throw std::runtime_error("Invalid splash GIM palette");
    const std::size_t palette = palette_block + 0x10 + word(data, palette_block + 0x40);
    const std::size_t pixels = 0x40ull + word(data, 0x70);
    if (palette > data.size() || 1024 > data.size() - palette ||
            pixels > data.size() || 480 * 272 > data.size() - pixels)
        throw std::runtime_error("Truncated splash GIM pixels");
    Bytes rgba(480 * 272 * 4);
    for (std::size_t y = 0; y < 272; ++y) {
        for (std::size_t x = 0; x < 480; ++x) {
            const auto tiled = (y / 8) * 480 * 8 + (x / 16) * 128 + (y % 8) * 16 + x % 16;
            const auto index = std::to_integer<unsigned>(data[pixels + tiled]);
            std::copy_n(data.begin() + palette + index * 4, 4, rgba.begin() + (y * 480 + x) * 4);
        }
    }
    return rgba;
}

std::vector<Bytes> read_logos(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0 || input.tellg() > 4 * 1024 * 1024)
        throw std::runtime_error("Unable to read logo.psp.prx");
    Bytes file(static_cast<std::size_t>(input.tellg()));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(file.data()), file.size());
    if (!input) throw std::runtime_error("Incomplete logo.psp.prx read");
    std::vector<Bytes> logos;
    constexpr std::string_view magic = "MIG.00.1PSP";
    for (std::size_t at = 0; at + 0x80 <= file.size(); ++at) {
        if (!std::equal(magic.begin(), magic.end(), reinterpret_cast<const char *>(file.data() + at))) continue;
        const auto gim = std::span(file).subspan(at);
        if (word(gim, 0x48, 2) != 480 || word(gim, 0x4A, 2) != 272) continue;
        const std::size_t length = 0x10ull + word(gim, 0x14);
        if (length > gim.size()) throw std::runtime_error("Truncated embedded splash");
        logos.push_back(decode_logo(gim.first(length)));
        at += length - 1;
    }
    if (logos.size() != names.size()) throw std::runtime_error("Expected four original company splashes");
    return logos;
}
}

bool play_boot_screens(const std::filesystem::path &root) {
    if (!vcs::display_window_enabled() || std::getenv("PSPRECOMP_SKIP_SPLASH") != nullptr) return true;
    try {
        const auto logos = read_logos(root / "PSP_GAME/USRDIR/logo.psp.prx");
        Bytes frame(480 * 272 * 4);
        vcs::display_window_set_aspect_lock(true);
        // Native timing: 0.3 s fade in, 1.6 s hold, 0.3 s fade out.
        // The original pictures are preserved; PRX animation code is not run.
        for (std::size_t logo = 0; logo < logos.size(); ++logo) {
            std::cout << "[startup] splash " << names[logo] << '\n';
            vcs::display_window_set_status(names[logo]);
            const auto started = std::chrono::steady_clock::now();
            for (unsigned tick = 0; tick < 132; ++tick) {
                if (vcs::display_window_close_requested()) return false;
                // Start or Cross skips the sequence, with no synthetic guest input.
                if (vcs::display_window_buttons() & (0x8u | 0x4000u)) {
                    vcs::display_window_set_aspect_lock(false);
                    return true;
                }
                const unsigned opacity = tick < 18 ? tick * 255 / 18 :
                    (tick >= 114 ? (131 - tick) * 255 / 18 : 255);
                for (std::size_t p = 0; p < frame.size(); p += 4) {
                    for (unsigned c = 0; c < 3; ++c)
                        frame[p + c] = std::byte(std::to_integer<unsigned>(logos[logo][p + c]) * opacity / 255);
                    frame[p + 3] = std::byte{255};
                }
                vcs::display_window_present_rgba(frame, 480, 272);
                std::this_thread::sleep_until(started + std::chrono::microseconds((tick + 1) * 1000000ull / 60));
            }
        }
        vcs::display_window_set_status("Caricamento...");
    } catch (const std::exception &error) {
        std::cerr << "[startup] " << error.what() << "; continuing to title\n";
    }
    vcs::display_window_set_aspect_lock(false);
    return !vcs::display_window_close_requested();
}

void dump_boot_screens(const std::filesystem::path &path, const std::filesystem::path &output) {
    const auto logos = read_logos(path);
    std::filesystem::create_directories(output);
    for (std::size_t i = 0; i < logos.size(); ++i) {
        std::ofstream file(output / ("splash-" + std::to_string(i) + ".ppm"), std::ios::binary);
        file << "P6\n480 272\n255\n";
        for (std::size_t p = 0; p < logos[i].size(); p += 4)
            file.write(reinterpret_cast<const char *>(logos[i].data() + p), 3);
        if (!file) throw std::runtime_error("Unable to write splash diagnostic");
        std::cout << "[startup] decoded " << names[i] << " 480x272\n";
    }
}

bool run_boot_screen_self_tests(std::string &error) {
    try {
        Bytes data(0x20300);
        const auto put = [&](std::size_t at, unsigned value) {
            for (unsigned i = 0; i < 4; ++i) data[at + i] = std::byte((value >> (8 * i)) & 255);
        };
        put(0x30, 4); put(0x34, 0x1FE50); put(0x44, 0x00010005);
        put(0x48, (272u << 16) | 480); put(0x4C, 8); put(0x70, 0x40);
        put(0x1FE80, 5); put(0x1FE94, 3); put(0x1FE98, 256); put(0x1FEC0, 0x40);
        put(0x1FED4, 0xFF332211); // Palette entry 1.
        data[0x80 + 128] = std::byte{1}; // x=16,y=0 after unswizzling.
        const auto rgba = decode_logo(data);
        if (word(rgba, 16 * 4) != 0xFF332211 || word(rgba, 0) != 0)
            throw std::runtime_error("Splash tile/palette decoding failed");
        data.resize(0x100);
        bool rejected = false;
        try { (void)decode_logo(data); } catch (const std::runtime_error &) { rejected = true; }
        if (!rejected) throw std::runtime_error("Truncated splash was accepted");
        error.clear(); return true;
    } catch (const std::exception &exception) { error = exception.what(); return false; }
}
}
