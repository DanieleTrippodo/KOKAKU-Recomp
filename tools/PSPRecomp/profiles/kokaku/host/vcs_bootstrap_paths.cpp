#include "vcs_bootstrap_paths.hpp"

#include "psprecomp/common.hpp"

#include <array>
#include <sstream>
#include <vector>

namespace vcs {

BootstrapPaths resolve_bootstrap_paths(
    int argc, const char *const *argv,
    const std::filesystem::path &executable_directory) {
    if (argc >= 2 && argv != nullptr && argv[1] != nullptr && *argv[1] != '\0') {
        const std::filesystem::path psp_executable = argv[1];
        return {
            psp_executable,
            argc >= 3 && argv[2] != nullptr && *argv[2] != '\0'
                ? std::filesystem::path(argv[2])
                : psp_executable.parent_path().parent_path().parent_path(),
            false,
        };
    }

    const std::vector<std::filesystem::path> root_candidates{
        executable_directory / "extracted",
        executable_directory / "PSP_DATA",
        executable_directory,
        std::filesystem::current_path() / "extracted",
        std::filesystem::current_path(),
    };

    constexpr std::array<const char *, 6> file_candidates{
        "PSP_GAME/USRDIR/main.psp.prx",
        "main.psp.prx",
        "PSP_GAME/SYSDIR/BOOT.BIN",
        "BOOT.BIN",
        "PSP_GAME/SYSDIR/EBOOT.BIN",
        "EBOOT.BIN",
    };

    for (const auto &root : root_candidates) {
        for (const char *relative : file_candidates) {
            const std::filesystem::path candidate = root / relative;
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) {
                return {candidate, root, true};
            }
        }
    }

    std::ostringstream message;
    message << "Could not locate Ghost in the Shell game data (main.psp.prx).\n"
            << "Please ensure 'extracted' or 'PSP_DATA' is present beside KokakuNative.exe.\n"
            << "Command-line launch: KokakuNative <path_to_main.psp.prx> [game_root]";
    throw psprecomp::Error(message.str());
}

} // namespace vcs
