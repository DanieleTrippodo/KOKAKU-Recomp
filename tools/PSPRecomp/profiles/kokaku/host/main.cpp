#include "psprecomp/common.hpp"
#include "ge_renderer.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"
#include "audio_output.hpp"
#include "display_window.hpp"
#include "ge_gpu_backend.hpp"
#include "kokaku_profile.hpp"
#include "kokaku_pc_balance.hpp"
#include "kokaku_controls.hpp"
#include "kokaku_native_menu.hpp"
#include "kokaku_frame_rate.hpp"
#include "kokaku_mouse.hpp"
#include "kokaku_net.hpp"
#include "kokaku_transport.hpp"
#include "kokaku_lobby.hpp"
#include "kokaku_online.hpp"
#include "native_font.hpp"
#include "native_boot_screens.hpp"
#include "vcs_config.hpp"
#include "vcs_bootstrap_paths.hpp"
// #include "vcs_project2dfx.hpp"
#include "vcs_hdr_post.hpp"
#include "vcs_runtime_log.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <filesystem>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
void show_rtx_error(const std::string &error) {
#ifdef _WIN32
    const char *rtx = std::getenv("PSPRECOMP_RTX");
    const char *window = std::getenv("PSPRECOMP_WINDOW");
    if (rtx && std::string_view(rtx) == "1" && (!window || std::string_view(window) != "0"))
        MessageBoxA(nullptr, error.c_str(), "Kokaku - NVIDIA RTX", MB_OK | MB_ICONERROR);
#endif
}

#ifdef _WIN32
LONG WINAPI vcs_unhandled_exception_filter(EXCEPTION_POINTERS *info) noexcept {
    wchar_t module_path[32768]{};
    const DWORD module_len = GetModuleFileNameW(nullptr, module_path, 32768u);
    std::filesystem::path output = module_len != 0u
        ? std::filesystem::path(module_path).parent_path() / L"KokakuNative_crash.txt"
        : std::filesystem::path(L"KokakuNative_crash.txt");

    FILE *file = nullptr;
    _wfopen_s(&file, output.c_str(), L"wb");
    if (file != nullptr) {
        const unsigned long code = info != nullptr && info->ExceptionRecord != nullptr
            ? info->ExceptionRecord->ExceptionCode : 0ul;
        const void *address = info != nullptr && info->ExceptionRecord != nullptr
            ? info->ExceptionRecord->ExceptionAddress : nullptr;
        std::fprintf(file, "KokakuNative (DirectX 12 Hardware Native Port) unhandled exception\r\n");
        const auto module_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto exception_pc = reinterpret_cast<std::uintptr_t>(address);
        const auto module_rva = exception_pc >= module_base ? exception_pc - module_base : 0u;
        std::fprintf(file, "code=0x%08lX address=%p host_thread=%lu\r\n",
                     code, address, static_cast<unsigned long>(GetCurrentThreadId()));
        std::fprintf(file, "module_base=%016llX module_rva=0x%llX\r\n",
                     static_cast<unsigned long long>(module_base),
                     static_cast<unsigned long long>(module_rva));
        if (info != nullptr && info->ExceptionRecord != nullptr &&
            info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            info->ExceptionRecord->NumberParameters >= 2u) {
            const ULONG_PTR operation = info->ExceptionRecord->ExceptionInformation[0];
            const ULONG_PTR fault_address = info->ExceptionRecord->ExceptionInformation[1];
            const char *operation_name = operation == 0u ? "read" :
                (operation == 1u ? "write" : (operation == 8u ? "execute" : "unknown"));
            std::fprintf(file, "access=%s fault_address=%016llX\r\n", operation_name,
                         static_cast<unsigned long long>(fault_address));
        }
        std::fprintf(file, "guest_dispatch_pc=0x%08X guest_thread_uid=%d guest_thread_name=%s\r\n",
                     psprecomp::runtime_dispatch_pc(), psprecomp::runtime_thread_uid(),
                     psprecomp::runtime_thread_name());
#if defined(_M_X64)
        if (info != nullptr && info->ContextRecord != nullptr) {
            const CONTEXT &c = *info->ContextRecord;
            std::fprintf(file,
                         "RIP=%016llX RSP=%016llX RBP=%016llX RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\r\n",
                         static_cast<unsigned long long>(c.Rip),
                         static_cast<unsigned long long>(c.Rsp),
                         static_cast<unsigned long long>(c.Rbp),
                         static_cast<unsigned long long>(c.Rax),
                         static_cast<unsigned long long>(c.Rbx),
                         static_cast<unsigned long long>(c.Rcx),
                         static_cast<unsigned long long>(c.Rdx));
        }
#endif
        std::fflush(file);
        std::fclose(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

std::uint64_t configured_max_dispatches() {
    // A direct double-click has no launcher script to override this. Keep the
    // old environment variable for diagnostics, but make the normal default
    // long-lived enough for an actual play session.
    constexpr std::uint64_t default_limit = 4'000'000'000ull;
    const char *text = std::getenv("PSPRECOMP_MAX_DISPATCHES");
    if (text == nullptr || *text == '\0') return default_limit;

    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (errno == ERANGE || end == text || *end != '\0' || parsed == 0u ||
        parsed > std::numeric_limits<std::uint64_t>::max()) {
        throw psprecomp::Error(std::string("Invalid PSPRECOMP_MAX_DISPATCHES value: ") + text);
    }
    return static_cast<std::uint64_t>(parsed);
}

std::filesystem::path native_executable_directory(const char *argv0) {
#ifdef _WIN32
    std::wstring buffer(512u, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0u)
            throw psprecomp::Error("GetModuleFileNameW failed while locating KokakuNative.exe");
        if (length < buffer.size() - 1u) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2u);
    }
#else
    return std::filesystem::absolute(argv0 != nullptr ? argv0 : "KokakuNative").parent_path();
#endif
}

void validate_kokaku_game_root(const std::filesystem::path &root) {
    if (std::getenv("PSPRECOMP_ALLOW_INCOMPLETE_ROOT") != nullptr) return;

    constexpr std::array<const char *, 1> required_boot_files{
        "PSP_GAME/USRDIR/data.img",
    };

    std::ostringstream missing;
    std::size_t missing_count = 0u;
    for (const char *relative : required_boot_files) {
        const std::filesystem::path candidate = root / relative;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(candidate, ec)) {
            missing << "\n  - " << relative;
            ++missing_count;
        }
    }
    if (missing_count != 0u) {
        std::cerr << "[warning] Kokaku root missing data.img at root / PSP_GAME/USRDIR/data.img. Continuing anyway.\n";
    }
}

} // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(&vcs_unhandled_exception_filter);
#endif
    try {
#if defined(_WIN32)
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-audio-output") {
            std::string error;
            if (!vcs::run_audio_output_tests(error)) { std::cerr << "Audio output tests failed: " << error << '\n'; return 1; }
            std::cout << "Audio output tests passed\n"; return 0;
        }
#endif
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-native-menu") {
            std::string error;
            if (!kokaku::run_native_menu_tests(error)) { std::cerr << "Native menu tests failed: " << error << '\n'; return 1; }
            std::cout << "Native menu tests passed\n"; return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-palette-cache") {
            std::string error;
            if (!vcs::run_palette_cache_tests(error)) { std::cerr << "Palette cache tests failed: " << error << '\n'; return 1; }
            std::cout << "Palette cache tests passed\n"; return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-pc-balance") {
            std::string error;
            if (!kokaku::run_pc_balance_tests(error)) { std::cerr << "PC balance tests failed: " << error << '\n'; return 1; }
            std::cout << "PC balance tests passed\n"; return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-frame-rate") {
            std::string error;
            if (!kokaku::run_frame_rate_tests(error)) { std::cerr << "Frame rate tests failed: " << error << '\n'; return 1; }
            std::cout << "Frame rate tests passed\n"; return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-atrac") {
            std::string error;
            if (!kokaku::run_atrac_self_tests(error)) {
                std::cerr << "ATRAC regression tests failed: " << error << '\n'; return 1;
            }
            std::cout << "ATRAC regression tests passed\n"; return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-mouse") {
            std::string error;
            if (!kokaku::run_mouse_tests(error)) {
                std::cerr << "Mouse self-test failed: " << error << '\n'; return 1;
            }
            std::cout << "Mouse self-test passed\n"; return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-controls") {
            std::string error;
            if (!kokaku::run_controls_tests(error)) {
                std::cerr << "Controls self-test failed: " << error << '\n';
                return 1;
            }
            std::cout << "Controls self-test passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-memory") {
            std::string error;
            if (!kokaku::run_memory_self_tests(error)) {
                std::cerr << "Memory self-test failed: " << error << '\n';
                return 1;
            }
            std::cout << "Memory self-test passed (32 and 64 MiB)\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-savedata") {
            std::string error;
            if (!kokaku::run_savedata_self_tests(error)) {
                std::cerr << "Savedata regression test failed: " << error << '\n';
                return 1;
            }
            std::cout << "Savedata regression tests passed\n";
            return 0;
        }
        if (argc == 4 && std::string_view(argv[1]) == "--dump-splashes") {
            kokaku::dump_boot_screens(argv[2], argv[3]);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-splash") {
            std::string error;
            if (!kokaku::run_boot_screen_self_tests(error)) {
                std::cerr << "Splash regression test failed: " << error << '\n';
                return 1;
            }
            std::cout << "Splash regression tests passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-font") {
            std::string error;
            if (!kokaku::run_font_self_tests(error)) {
                std::cerr << "Font regression test failed: " << error << '\n';
                return 1;
            }
            std::cout << "Font regression tests passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-vblank") {
            std::string error;
            if (!kokaku::run_vblank_self_tests(error)) {
                std::cerr << "VBLANK regression test failed: " << error << '\n';
                return 1;
            }
            std::cout << "VBLANK regression tests passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-transport") {
            std::string error;
            if (!kokaku::net::run_transport_self_tests(error)) {
                std::cerr << "Transport self-test failed: " << error << '\n';
                return 1;
            }
            std::cout << "Transport regression tests passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-lobby") {
            std::string error;
            if (!kokaku::lobby::run_lobby_self_tests(error)) {
                std::cerr << "Lobby self-test failed: " << error << '\n';
                return 1;
            }
            std::cout << "Lobby regression tests passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-online") {
            std::string error;
            if (!kokaku::run_online_self_tests(error)) {
                std::cerr << "Online self-test failed: " << error << '\n';
                return 1;
            }
            std::cout << "Online regression tests passed\n";
            return 0;
        }
        // Diagnostic against a running lobby: --test-lobby-live <url> [idle seconds]
        if ((argc == 3 || argc == 4) && std::string_view(argv[1]) == "--test-lobby-live") {
            std::string error;
            if (!kokaku::lobby::run_lobby_live_test(argv[2], argc == 4 ? std::atoi(argv[3]) : 0, error)) {
                std::cerr << "Lobby live test failed: " << error << '\n';
                return 1;
            }
            std::cout << "Lobby live test passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test-net") {
            std::string error;
            if (!kokaku::run_net_self_tests(error)) {
                std::cerr << "Network self-test failed: " << error << '\n';
                return 1;
            }
            std::cout << "Network regression tests passed\n";
            return 0;
        }
        const std::filesystem::path executable_directory =
            native_executable_directory(argc > 0 ? argv[0] : nullptr);
        const vcs::BootstrapPaths paths =
            vcs::resolve_bootstrap_paths(argc, argv, executable_directory);
        const std::filesystem::path &executable = paths.psp_executable;
        const std::filesystem::path &root = paths.game_root;
        vcs::initialize_vcs_configuration(executable_directory);
        const vcs::VcsConfiguration &configuration = vcs::vcs_configuration();
        kokaku::set_pc_balance_enabled(configuration.gameplay.pc_balance);
        kokaku::init_network_config(configuration.source_path, argc, argv);
        vcs::runtime_log_initialize(configuration);
        vcs::runtime_log_line(std::string("bootstrap executable=") + executable.string());
        vcs::runtime_log_line(std::string("bootstrap root=") + root.string());
        vcs::runtime_log_line(std::string("rendering backend=") +
                              vcs::rendering_backend_name(configuration.rendering.backend));
        validate_kokaku_game_root(root);

        psprecomp::Elf32Image elf = psprecomp::Elf32Image::from_file(executable);
        // The native loader keeps all AOT modules resident. Give game heaps the
        // expanded PSP memory layout instead of exhausting their fallback pools.
        const char *ram_option = std::getenv("PSPRECOMP_RAM_MB");
        const std::string ram_text = ram_option ? ram_option : "64";
        if (ram_text != "32" && ram_text != "64")
            throw psprecomp::Error("PSPRECOMP_RAM_MB must be 32 or 64");
        const std::uint32_t ram_megabytes = ram_text == "32" ? 32u : 64u;
        psprecomp::Runtime runtime(ram_megabytes * 1024u * 1024u);
        runtime.set_game_root(root);
        const auto relocations = elf.load_and_relocate(runtime.memory(), psprecomp::kDefaultPspUserLoadBase);
        std::uint64_t image_end = 0u;
        for (std::size_t index = 0; index < elf.segments().size(); ++index) {
            const auto &segment = elf.segments()[index];
            if (segment.type != 1u) continue;  // PT_LOAD
            const std::uint64_t start = elf.segment_runtime_address(index, psprecomp::kDefaultPspUserLoadBase);
            image_end = std::max(image_end, start + segment.memory_size);
        }
        // Load secondary bridge module (disc I/O and FPL memory manager) at 0x08B00000
        const std::filesystem::path bridge_candidate = root / "PSP_GAME/USRDIR/bridge.psp.prx";
        if (std::filesystem::is_regular_file(bridge_candidate)) {
            std::cout << "Loading bridge module: " << bridge_candidate.string() << "\n";
            psprecomp::Elf32Image bridge_elf = psprecomp::Elf32Image::from_file(bridge_candidate);
            bridge_elf.load_and_relocate(runtime.memory(), 0x08B00000u);
            for (std::size_t index = 0; index < bridge_elf.segments().size(); ++index) {
                const auto &segment = bridge_elf.segments()[index];
                if (segment.type != 1u) continue;  // PT_LOAD
                const std::uint64_t start = bridge_elf.segment_runtime_address(index, 0x08B00000u);
                image_end = std::max(image_end, start + segment.memory_size);
            }
            // Patch bridge libc heap size at 0x08B134C8 to 8 (8 KB)
            // matching the partition gap left after bridge heap init.
            runtime.memory().store32(0x08B134C8u, 8u);
            std::cout << "[mem] Patched bridge __libcl_heap_size at 0x08B134C8 = 8 KB\n";
        }

        // The AOT loading PRX has a fixed address. Reserve its complete image
        // before the bridge divides the remaining user arena into FPL pools.
        const auto loading_elf = psprecomp::Elf32Image::from_file(root / "PSP_GAME/USRDIR/loading.psp.prx");
        for (std::size_t index = 0; index < loading_elf.segments().size(); ++index) {
            const auto &segment = loading_elf.segments()[index];
            if (segment.type == 1u)
                image_end = std::max(image_end, std::uint64_t(loading_elf.segment_runtime_address(index, 0x08B20000u)) + segment.memory_size);
        }
        if (image_end == 0u || image_end > 0x0A000000ull)
            throw psprecomp::Error("Invalid PSP ELF load image extent");
        const std::uint32_t user_arena_start =
            static_cast<std::uint32_t>((image_end + 0xFFu) & ~0xFFull);
        std::cout << "[mem] user_arena_start=" << psprecomp::hex32(user_arena_start) << " (after all modules)\n";
        psprecomp::register_generated_functions(runtime);
        // vcs::install_project2dfx
        // Reads [SimulateHDR] out of the same ini. The effect itself is built
        // lazily on the first frame the Vulkan backend records.
        vcs::hdr_post_configure(configuration.source_path);

        kokaku::install_profile(runtime, user_arena_start);

        std::string gpu_backend_error;
        if (!vcs::initialize_ge_gpu_backend(gpu_backend_error))
            throw psprecomp::Error(gpu_backend_error);
        const vcs::GeGpuBackendReport gpu_start = vcs::ge_gpu_backend_report();
        vcs::runtime_log_line(std::string("ge backend requested=") +
                              vcs::ge_gpu_backend_name(gpu_start.requested) +
                              " active=" + vcs::ge_gpu_backend_name(gpu_start.active));
        vcs::runtime_log_line(std::string("ge backend status=") + gpu_start.message);

        std::cout << "KokakuNative PSP bootstrap\n"
                  << "Launch mode: "
                  << (paths.discovered_from_psp_data
                          ? "PSP_DATA beside executable"
                          : "explicit command-line paths") << "\n"
                  << "Executable: " << executable.string() << "\n"
                  << "SHA-256:   " << psprecomp::sha256_file(executable) << "\n"
                  << "Entry:     " << psprecomp::hex32(elf.runtime_entry()) << "\n"
                  << "Guest RAM: " << ram_megabytes << " MiB (native heap budget)\n"
                  << "Relocs:    " << relocations.total << " (invalid " << relocations.invalid << ", unsupported " << relocations.unsupported << ")\n"
                  << "Functions: " << runtime.function_count() << "\n"
                  << "GE backend requested: " << vcs::ge_gpu_backend_name(gpu_start.requested) << "\n"
                  << "GE backend active:    " << vcs::ge_gpu_backend_name(gpu_start.active) << "\n"
                  << "GE backend status:    " << gpu_start.message << "\n"
                  << "Config:               " << configuration.source_path.string()
                  << (configuration.loaded_from_file ? " (loaded)" : " (defaults)") << "\n"
                  << "Display output:       "
                  << vcs::display_resolution_mode_name(configuration.display.resolution_mode)
                  << " / " << vcs::display_aspect_mode_name(configuration.display.aspect_mode)
                  << " / " << vcs::display_upscale_filter_name(configuration.display.upscale_filter)
                  << (configuration.display.fullscreen ? " / fullscreen" : " / windowed") << "\n";
        const vcs::InternalResolutionDimensions internal_resolution =
            vcs::resolve_internal_resolution(configuration.rendering);
        std::cout << "Rendering backend:    "
                  << vcs::rendering_backend_name(configuration.rendering.backend) << "\n"
                  << "Internal resolution:  " << internal_resolution.width << 'x'
                  << internal_resolution.height << " ("
                  << vcs::internal_resolution_mode_name(
                         configuration.rendering.internal_resolution_mode)
                  << ")\n";
        std::cout << "Multisampling:        ";
        if (configuration.rendering.msaa <= 1u)
            std::cout << "off\n";
        else
            std::cout << configuration.rendering.msaa
                      << "x requested (clamped to adapter support)\n";
        std::cout << "Antialiasing:         ";
        if (configuration.rendering.smaa && gpu_start.active == vcs::GeGpuBackendKind::DirectX12) {
            // The bundled SMAA shaders currently belong to the Vulkan postprocess
            // path.  Do not claim DX12 SMAA is active when the native GE backend
            // never executes those passes; use the real D3D12 MSAA path instead.
            std::cout << "SMAA requested but NOT active on native DX12 (use MSAA)\n";
        } else {
            std::cout << (configuration.rendering.smaa ? "SMAA 1x (Jimenez et al.)\n" : "off\n");
        }
        if (gpu_start.active == vcs::GeGpuBackendKind::DirectX12) {
            std::cout << "DX12 physical target: " << gpu_start.offscreen_width << 'x'
                      << gpu_start.offscreen_height << " | actual MSAA "
                      << gpu_start.dx12_msaa_samples << "x\n";
        }
        std::cout << "Anisotropic filter:   ";
        if (configuration.rendering.anisotropic_filtering <= 1u)
            std::cout << "off (PSP isotropic sampling)\n";
        else
            std::cout << configuration.rendering.anisotropic_filtering
                      << "x requested, mipmapped world textures only\n";
        // Reported so "did the widescreen option actually take effect" is a
        // fact on screen rather than something to infer from the picture.
        const vcs::DisplaySurfaceDimensions output_surface =
            vcs::resolve_display_surface_dimensions(configuration.display);
        const float widescreen_aspect = vcs::resolve_widescreen_aspect_ratio(
            configuration, output_surface.width, output_surface.height);
        const float widescreen_stretch = vcs::widescreen_stretch_factor(
            configuration, output_surface.width, output_surface.height);
        std::cout << "Widescreen:           ";
        if (widescreen_aspect <= 0.0f || widescreen_stretch == 1.0f) {
            std::cout << "off (PSP 16:9 projection)\n";
        } else {
            std::cout << "on, output " << output_surface.width << 'x' << output_surface.height
                      << ", aspect " << widescreen_aspect << " (frustum x"
                      << widescreen_stretch << ", DX12 HUD /" << widescreen_stretch
                      << ") -- ThirteenAG WidescreenFixesPack\n";
        }
        for (const std::string &warning : configuration.warnings)
            std::cerr << "KokakuNative.ini warning: " << warning << "\n";

        if (runtime.function_count() == 0u) {
            std::cout << "No generated VCS functions are linked yet. Run psp_recomp after exporting functions.csv from Ghidra.\n";
            return 3;
        }
        if (const auto module = elf.find_module_info(runtime.memory(), psprecomp::kDefaultPspUserLoadBase)) {
            runtime.cpu().set_gpr(28, module->gp);
        } else {
            throw psprecomp::Error("PSP module info not found after relocation");
        }
        // Sony PSP module argument format: concatenated null-terminated strings.
        // bridge.psp.prx launches main.psp.prx with:
        //   -run 0
        //   -img disc0:/PSP_GAME/USRDIR/data.img
        //   nohost
        static const char kDefaultExecutablePath[] =
            "disc0:/PSP_GAME/USRDIR/main.psp.prx\0"
            "-run\0" "0\0"
            "-img\0" "data.img\0"
            "nohost\0";
        const std::uint32_t arg_size = static_cast<std::uint32_t>(sizeof(kDefaultExecutablePath));
        const std::uint32_t arg_addr = psprecomp::GuestMemory::kPhysicalBase + runtime.memory().size() - 0x800u;
        runtime.memory().copy_in(arg_addr, {reinterpret_cast<const std::uint8_t *>(kDefaultExecutablePath), sizeof(kDefaultExecutablePath)});
        runtime.cpu().set_gpr(31, 0u);
        runtime.cpu().set_gpr(4, arg_size);
        runtime.cpu().set_gpr(5, arg_addr);
        const std::uint64_t max_dispatches = configured_max_dispatches();
        std::cout << "Dispatch cap: " << max_dispatches << "\n";
        vcs::display_window_start();
        if (!kokaku::play_boot_screens(root)) {
            vcs::audio_output_shutdown();
            vcs::display_window_shutdown();
            vcs::shutdown_ge_gpu_backend();
            return 0;
        }
        kokaku::install_display_heartbeat();
        kokaku::install_starvation_preemption();
        runtime.run(elf.runtime_entry(), max_dispatches);
        kokaku::shutdown_network();
        const bool shutdown_diag = std::getenv("PSPRECOMP_SHUTDOWN_DIAG") != nullptr;
        if (shutdown_diag) std::cerr << "[shutdown] runtime-run-returned\n";
        std::cout << "Runtime stopped: " << runtime.stop_reason() << "\n";
        kokaku::report_thread_states();
        psprecomp::report_counted_pcs();
        if (shutdown_diag) std::cerr << "[shutdown] counted-pcs-reported\n";
        runtime.report_hle_histogram();
        if (shutdown_diag) std::cerr << "[shutdown] hle-histogram-reported\n";
        kokaku::report_disc_read_stats();
        kokaku::report_present_stats();
        if (shutdown_diag) std::cerr << "[shutdown] disc-stats-reported\n";
        const vcs::GeGpuBackendReport gpu_final = vcs::ge_gpu_backend_report();
        if (shutdown_diag) std::cerr << "[shutdown] gpu-report-copied\n";
        if (gpu_final.active == vcs::GeGpuBackendKind::DirectX12) {
            std::cout << "GE DirectX12: frames=" << gpu_final.game_frames
                      << " draws=" << gpu_final.game_draw_calls
                      << " triangles=" << gpu_final.game_triangles
                      << " vertices=" << gpu_final.game_vertices
                      << " hw_draws=" << gpu_final.hw_transform_draw_calls
                      << " pipelines=" << gpu_final.unique_pipeline_keys
                      << " target=" << gpu_final.offscreen_width << 'x' << gpu_final.offscreen_height
                      << " msaa=" << gpu_final.dx12_msaa_samples
                      << " depth=" << gpu_final.dx12_depth_bits
                      << " resolves=" << gpu_final.dx12_resolves
                      << " textured_pending=" << gpu_final.game_textured_draws_without_texture
                      << " readback_bytes=" << gpu_final.game_frame_readback_bytes
                      << "\n";
        }
        if (gpu_final.active == vcs::GeGpuBackendKind::DirectX12) {
            std::cout << "GE GPU backend: draws=" << gpu_final.draw_calls
                      << " vertices=" << gpu_final.vertices
                      << " textured=" << gpu_final.textured_draw_calls
                      << " pipelines=" << gpu_final.unique_pipeline_keys
                      << " textures=" << gpu_final.unique_texture_keys
                      << " texture_images=" << gpu_final.unique_texture_image_keys
                      << " shared_texture_images=" << gpu_final.shared_texture_images
                      << " ring=" << gpu_final.draw_ring_capacity
                      << " overwrites=" << gpu_final.draw_ring_overwrites
                      << " transfer_ready=" << gpu_final.transfer_self_test_passed
                      << " graphics_ready=" << gpu_final.offscreen_self_test_passed
                      << " offscreen=" << gpu_final.offscreen_width << "x" << gpu_final.offscreen_height
                      << " offscreen_pixels=" << gpu_final.offscreen_changed_pixels
                      << " offscreen_checksum=" << gpu_final.offscreen_checksum
                      << " upload_capacity=" << gpu_final.upload_capacity_bytes
                      << " frames_in_flight=" << gpu_final.frames_in_flight_capacity
                      << " staged_draws=" << gpu_final.staged_draw_calls
                      << " staged_vertices=" << gpu_final.staged_vertices
                      << " staged_bytes=" << gpu_final.staged_bytes
                      << " upload_wraps=" << gpu_final.upload_wraps
                      << " transfer_submissions=" << gpu_final.transfer_submissions
                      << " transfer_bytes=" << gpu_final.transfer_bytes
                      << " rejected=" << gpu_final.rejected_gpu_draws
                      << " game_frames=" << gpu_final.game_frames
                      << " game_draws=" << gpu_final.game_draw_calls
                      << " game_triangles=" << gpu_final.game_triangles
                      << " game_vertices=" << gpu_final.game_vertices
                      << " hw_transform_draws=" << gpu_final.hw_transform_draw_calls
                      << " hw_transform_vertices=" << gpu_final.hw_transform_vertices
                      << " hw_transform_prim_batches=" << gpu_final.hw_transform_prim_batches
                      << " hw_transform_unique_vertices_decoded=" << gpu_final.hw_transform_unique_vertices_decoded
                      << " hw_transform_index_reuses=" << gpu_final.hw_transform_index_reuses
                      << " game_changed_pixels=" << gpu_final.game_frame_changed_pixels
                      << " game_checksum=" << gpu_final.game_frame_checksum
                      << " texture_decode_requests=" << gpu_final.texture_decode_requests
                      << " texture_cache_hits=" << gpu_final.texture_cache_hits
                      << " decoded_textures=" << gpu_final.decoded_texture_uploads
                      << " decoded_texture_bytes=" << gpu_final.decoded_texture_bytes
                      << " decoded_t4=" << gpu_final.decoded_t4_textures
                      << " decoded_t8=" << gpu_final.decoded_t8_textures
                      << " texture_images=" << gpu_final.texture_images_created
                      << " texture_image_uploads=" << gpu_final.texture_image_uploads
                      << " texture_image_upload_bytes=" << gpu_final.texture_image_upload_bytes
                      << " descriptor_layout=" << gpu_final.texture_descriptor_layout_created
                      << " descriptor_pool=" << gpu_final.texture_descriptor_pool_created
                      << " textured_shaders=" << gpu_final.textured_shader_modules_created
                      << " textured_pipeline=" << gpu_final.textured_pipeline_created
                      << " depth_image=" << gpu_final.depth_image_created
                      << " depth_memory=" << gpu_final.depth_image_memory_bound
                      << " depth_view=" << gpu_final.depth_image_view_created
                      << " depth_attachment=" << gpu_final.depth_attachment_active
                      << " depth_pipeline_variants=" << gpu_final.depth_pipeline_variants_created
                      << " depth_tested_game_draws=" << gpu_final.depth_tested_game_draw_calls
                      << " depth_writing_game_draws=" << gpu_final.depth_writing_game_draw_calls
                      << " alpha_shader=" << gpu_final.alpha_test_shader_active
                      << " alpha_tested_game_draws=" << gpu_final.alpha_tested_game_draw_calls
                      << " blend_active=" << gpu_final.standard_alpha_blend_pipeline_active
                      << " blend_variants=" << gpu_final.blend_pipeline_variants_created
                      << " standard_blended_game_draws=" << gpu_final.standard_alpha_blended_game_draw_calls
                      << " fixed_replace_blended_game_draws=" << gpu_final.fixed_replace_blended_game_draw_calls
                      << " additive_blended_game_draws=" << gpu_final.additive_blended_game_draw_calls
                      << " unsupported_blend_game_draws=" << gpu_final.unsupported_blend_game_draw_calls
                      << " texture_function_active=" << gpu_final.observed_texture_function_shader_active
                      << " complete_texture_functions=" << gpu_final.complete_texture_function_shader_active
                      << " modulate_texture_game_draws=" << gpu_final.modulate_texture_game_draw_calls
                      << " decal_texture_game_draws=" << gpu_final.decal_texture_game_draw_calls
                      << " blend_texture_game_draws=" << gpu_final.blend_texture_game_draw_calls
                      << " replace_texture_game_draws=" << gpu_final.replace_texture_game_draw_calls
                      << " add_texture_game_draws=" << gpu_final.add_texture_game_draw_calls
                      << " double_color_texture_game_draws=" << gpu_final.double_color_texture_game_draw_calls
                      << " unsupported_texture_function_game_draws=" << gpu_final.unsupported_texture_function_game_draw_calls
                      << " color_mask_active=" << gpu_final.color_write_mask_pipeline_active
                      << " color_mask_variants=" << gpu_final.color_mask_pipeline_variants_created
                      << " masked_color_game_draws=" << gpu_final.masked_color_game_draw_calls
                      << " unsupported_partial_color_masks=" << gpu_final.unsupported_partial_color_mask_game_draw_calls
                      << " base_texture_formats=" << gpu_final.base_texture_formats_active
                      << " decoded_direct16=" << gpu_final.decoded_direct16_textures
                      << " decoded_direct32=" << gpu_final.decoded_direct32_textures
                      << " decoded_indexed16=" << gpu_final.decoded_indexed16_textures
                      << " decoded_indexed32=" << gpu_final.decoded_indexed32_textures
                      << " compressed_texture_formats=" << gpu_final.compressed_texture_formats_active
                      << " decoded_dxt1=" << gpu_final.decoded_dxt1_textures
                      << " decoded_dxt3=" << gpu_final.decoded_dxt3_textures
                      << " decoded_dxt5=" << gpu_final.decoded_dxt5_textures
                      << " mipmap_state=" << gpu_final.mipmap_state_active
                      << " mipmapped_game_draws=" << gpu_final.mipmapped_game_draw_calls
                      << " mip_linear_game_draws=" << gpu_final.mip_linear_game_draw_calls
                      << " full_mip_chain=" << gpu_final.full_mip_chain_active
                      << " uploaded_mip_levels=" << gpu_final.uploaded_mip_levels
                      << " automatic_lod_game_draws=" << gpu_final.automatic_lod_game_draw_calls
                      << " fixed_lod_game_draws=" << gpu_final.fixed_lod_game_draw_calls
                      << " slope_lod_game_draws=" << gpu_final.slope_lod_game_draw_calls
                      << " selected_nonzero_mip_game_draws=" << gpu_final.selected_nonzero_mip_game_draw_calls
                      << " framebuffer_targets=" << gpu_final.framebuffer_targets_observed
                      << " dx12_framebuffer_targets=" << gpu_final.dx12_native_framebuffer_targets
                      << " dx12_gpu_feedback=" << gpu_final.dx12_gpu_feedback_draws
                      << " dx12_self_feedback=" << gpu_final.dx12_self_feedback_snapshots
                      << " presented_target=0x" << std::hex << gpu_final.presented_framebuffer_target << std::dec
                      << " gpu_window_presented=" << gpu_final.gpu_frame_presented_to_window
                      << " release_candidate_ready=" << gpu_final.release_candidate_ready
                      << " fog_shader=" << gpu_final.fog_shader_active
                      << " fogged_game_draws=" << gpu_final.fogged_game_draw_calls
                      << " texture_samplers=" << gpu_final.texture_samplers_created
                      << " descriptor_sets=" << gpu_final.texture_descriptor_sets_allocated
                      << " textured_game_draws=" << gpu_final.textured_game_draw_calls
                      << " textured_game_triangles=" << gpu_final.textured_game_triangles
                      << " textured_game_vertices=" << gpu_final.textured_game_vertices
                      << " unbound_textured_draws=" << gpu_final.game_textured_draws_without_texture
                      << " rejected_texture_decodes=" << gpu_final.rejected_texture_decodes
                      << " evicted_textures=" << gpu_final.evicted_textures
                      << " recycled_descriptor_sets=" << gpu_final.recycled_texture_descriptor_sets
                      << " last_texture=" << gpu_final.last_texture_width << 'x'
                      << gpu_final.last_texture_height << " fmt="
                      << gpu_final.last_texture_format
                      << " texture_checksum=" << gpu_final.last_texture_checksum << "\n";
        }
        if (shutdown_diag) std::cerr << "[shutdown] before-display-shutdown\n";
        vcs::audio_output_shutdown();
        vcs::display_window_shutdown();
        if (shutdown_diag) std::cerr << "[shutdown] after-display-shutdown\n";
        vcs::shutdown_ge_gpu_backend();
        if (shutdown_diag) std::cerr << "[shutdown] after-gpu-shutdown\n";
        if (runtime.stop_reason().starts_with("RTX:")) show_rtx_error(runtime.stop_reason());
        return runtime.stop_reason().empty() ? 0 : 4;
    } catch (const std::exception &e) {
        kokaku::shutdown_network();
        std::cerr << "KokakuNative error: " << e.what() << "\n";
        show_rtx_error(e.what());
        return 1;
    }
}
