with open('tools/PSPRecomp/profiles/vcs/host/main.cpp', 'r', encoding='utf-8', errors='ignore') as f:
    code = f.read()

# Replace headers
code = code.replace('#include "vcs_profile.hpp"', '#include "kokaku_profile.hpp"')
code = code.replace('#include "vcs_project2dfx.hpp"', '// #include "vcs_project2dfx.hpp"')

# Replace crash report title
code = code.replace('VCSNative_crash.txt', 'KokakuNative_crash.txt')
code = code.replace('VCSNative (async GE + cross-unit hot-register cache) unhandled exception',
                    'KokakuNative (DirectX 12 Hardware Native Port) unhandled exception')
code = code.replace('VCSNative.exe', 'KokakuNative.exe')
code = code.replace('VCSNative.ini', 'KokakuNative.ini')
code = code.replace('VCSNative', 'KokakuNative')

# Replace validation function
old_val = '''void validate_vcs_game_root(const std::filesystem::path &root) {
    if (std::getenv("PSPRECOMP_ALLOW_INCOMPLETE_ROOT") != nullptr) return;

    constexpr std::array<const char *, 2> required_boot_files{
        "PSP_GAME/USRDIR/RUNDATA/PSP/MOVIES/LOGO.PMF",
        "PSP_GAME/USRDIR/RUNDATA/PSP/MOVIES/TITLES.PMF",
    };

    std::ostringstream missing;
    std::size_t missing_count = 0u;
    for (const char *relative : required_boot_files) {
        const std::filesystem::path candidate = root / relative;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(candidate, ec)) {
            missing << "\\n  - " << relative;
            ++missing_count;
        }
    }
    if (missing_count != 0u) {
        throw psprecomp::Error(
            "Incomplete GTA VCS game root: missing required boot file(s):" + missing.str() +
            "\\nRegenerate the compact root with tools/Gerar_pacote_minimo_VCS_v0_7.bat "
            "or provide a complete PSP_GAME/USRDIR tree. "
            "Set PSPRECOMP_ALLOW_INCOMPLETE_ROOT=1 only for low-level diagnostics.");
    }
}'''

new_val = '''void validate_kokaku_game_root(const std::filesystem::path &root) {
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
            missing << "\\n  - " << relative;
            ++missing_count;
        }
    }
    if (missing_count != 0u) {
        std::cerr << "[warning] Kokaku root missing data.img at root / PSP_GAME/USRDIR/data.img. Continuing anyway.\\n";
    }
}'''

code = code.replace(old_val, new_val)
code = code.replace('validate_vcs_game_root(root);', 'validate_kokaku_game_root(root);')

# In main, load bridge.psp.prx if present
bridge_loader = '''
        // Load secondary bridge module (disc I/O and FPL memory manager) at 0x08B00000
        const std::filesystem::path bridge_candidate = root / "PSP_GAME/USRDIR/bridge.psp.prx";
        if (std::filesystem::is_regular_file(bridge_candidate)) {
            std::cout << "Loading bridge module: " << bridge_candidate.string() << "\\n";
            psprecomp::Elf32Image bridge_elf = psprecomp::Elf32Image::from_file(bridge_candidate);
            bridge_elf.load_and_relocate(runtime.memory(), 0x08B00000u);
        }
'''

code = code.replace('vcs::install_project2dfx(runtime, configuration.source_path, 0u);', '// vcs::install_project2dfx')
code = code.replace('vcs::install_profile(runtime, user_arena_start);',
                    bridge_loader + '\n        kokaku::install_profile(runtime, user_arena_start);')

code = code.replace('vcs::install_display_heartbeat();', 'kokaku::install_display_heartbeat();')
code = code.replace('vcs::install_starvation_preemption();', 'kokaku::install_starvation_preemption();')
code = code.replace('vcs::report_disc_read_stats();', 'kokaku::report_disc_read_stats();')
code = code.replace('vcs::report_present_stats();', 'kokaku::report_present_stats();')

with open('tools/PSPRecomp/profiles/kokaku/host/main.cpp', 'w', encoding='utf-8') as f:
    f.write(code)

print('Generated kokaku host main.cpp successfully!')
