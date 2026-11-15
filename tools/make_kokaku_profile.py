import os

with open('tools/PSPRecomp/profiles/vcs/host/vcs_profile.cpp', 'r', encoding='utf-8', errors='ignore') as f:
    code = f.read()

# Replace header include
code = code.replace('#include "vcs_profile.hpp"', '#include "kokaku_profile.hpp"\n#include "bridge_units.hpp"')

# Remove VCS fast paths and 2dfx includes that aren't needed or can be empty
code = code.replace('#include "vcs_native_fast_paths.hpp"', '// #include "vcs_native_fast_paths.hpp"')
code = code.replace('#include "vcs_project2dfx.hpp"', '// #include "vcs_project2dfx.hpp"')
code = code.replace('#include "vcs_camera_input.hpp"', '// #include "vcs_camera_input.hpp"')
code = code.replace('#include "vcs_vehicle_input.hpp"', '// #include "vcs_vehicle_input.hpp"')

# In effective_controller_analog, stub vcs_camera_set_axes and vcs_set_host_drive_inputs
code = code.replace('vcs_camera_set_axes(host.camera_x, host.camera_y);', '// vcs_camera_set_axes(host.camera_x, host.camera_y);')
code = code.replace('vcs_set_host_drive_inputs(host.accelerate, host.brake);', '// vcs_set_host_drive_inputs(host.accelerate, host.brake);')

# In install_profile, remove install_native_fast_paths
code = code.replace('install_native_fast_paths(runtime);', '// install_native_fast_paths(runtime);')

# Remove VCS specific function overrides
code = code.replace('runtime.register_function(0x08B562D8u, &vcs_sprintf, "vcs_sprintf");', '')
code = code.replace('runtime.register_function(0x088B4FA8u, &vcs_path_hash, "vcs_path_hash");', '')
code = code.replace('runtime.register_function(0x08B1B36Cu, &vcs_load_codec_modules, "vcs_load_codec_modules");', '')
code = code.replace('if (std::getenv("PSPRECOMP_NO_FAST_DEFLATE") == nullptr)\n        runtime.register_function(0x08B648B0u, &vcs_raw_deflate_fast, "vcs_raw_deflate_fast");', '')

# Remove project2dfx render call
code = code.replace('project2dfx_render_frame(', '// project2dfx_render_frame(')

# Add bridge and extra modules registration inside install_profile
bridge_registration = '''
    // Register Kokaku bridge module (recompiled BOOT.BIN)
    extern void register_bridge_functions(psprecomp::Runtime &runtime);
    register_bridge_functions(runtime);

    static const std::pair<std::uint32_t, std::uint32_t> kBridgeExports[] = {
        {0xB076DA49u, 0x08B00C5Cu}, {0x8645E28Du, 0x08B00D34u}, {0xC4F93C94u, 0x08B00FD0u},
        {0x064D3302u, 0x08B01004u}, {0x5A951117u, 0x08B01038u}, {0x3DF63212u, 0x08B01060u},
        {0x5061AA01u, 0x08B0108Cu}, {0x4AC981CBu, 0x08B010A0u}, {0x5DE21B0Du, 0x08B0111Cu},
        {0x7FF51A62u, 0x08B01148u}, {0xC588AF58u, 0x08B0115Cu}, {0x2A1BEFEEu, 0x08B0116Cu},
        {0xEF26E373u, 0x08B01194u}, {0x5678724Cu, 0x08B011A8u}, {0x807CA033u, 0x08B011DCu},
        {0x59289BC2u, 0x08B01210u}, {0x39B94416u, 0x08B01224u}, {0xE8891813u, 0x08B03AB4u},
        {0xCB33C0EDu, 0x08B03AC0u}, {0x9C7627B3u, 0x08B03AECu}, {0x0CFC491Cu, 0x08B03B18u},
        {0x8D472102u, 0x08B03B3Cu}, {0xA44E5B66u, 0x08B03B60u}, {0x8C60B28Fu, 0x08B03B8Cu},
        {0x0DDC9CEFu, 0x08B03BB8u}, {0x962469A1u, 0x08B03BDCu}, {0xA75FB018u, 0x08B03C00u},
        {0xF107BB21u, 0x08B03C20u}, {0xC7042BCFu, 0x08B03C44u}, {0xD0D30F85u, 0x08B03C68u},
        {0x6DA99434u, 0x08B03C8Cu}, {0x828B7E6Eu, 0x08B03CACu}, {0xFD0FE611u, 0x08B03CCCu},
        {0x8EC7C007u, 0x08B03CF0u}, {0x52FB7A52u, 0x08B06B8Cu}, {0xD8A37D98u, 0x08B06CC0u},
        {0x357733ECu, 0x08B06ACCu}, {0x3A379628u, 0x08B06B3Cu}, {0x3263871Fu, 0x08B06D50u},
        {0xD1A939DFu, 0x08B06D60u}, {0x547560B2u, 0x08B06E08u}, {0x1D02948Eu, 0x08B06F44u},
        {0x521BB84Du, 0x08B06FDCu}, {0x2F5A5454u, 0x08B070DCu}, {0x1FCEACA5u, 0x08B0728Cu},
        {0x68A3E28Du, 0x08B0747Cu}, {0xE4EF9D4Cu, 0x08B06B7Cu}, {0x481C6EFDu, 0x08B0766Cu},
        {0x4BCCD798u, 0x08B07678u}, {0x3E4C351Eu, 0x08B07698u}, {0xDB7CFFD8u, 0x08B076B8u},
        {0xB5C5E3C0u, 0x08B076D8u}, {0x57F4CD3Bu, 0x08B076FCu},
    };
    for (const auto &[nid, func_addr] : kBridgeExports) {
        runtime.register_hle("bridge", nid, [func_addr](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.pc = func_addr;
        });
    }

    // ModuleMgr: sceKernelLoadModule
    runtime.register_hle("ModuleMgrForUser", 0x977DE386u,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::string path = rt.memory().read_c_string(ctx.gpr[4]);
            const std::int32_t uid = next_module_uid++;
            std::cerr << "[module] sceKernelLoadModule path=\\"" << path << "\\" -> uid=" << uid << "\\n";
            loaded_modules.emplace(uid, true);
            ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
        });

    // sceLibFont
    static std::uint32_t next_font_handle = 1u;
    runtime.register_hle("sceLibFont", 0x67F17ED7u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, next_font_handle++); });
    runtime.register_hle("sceLibFont", 0x574B6FBCu, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceLibFont", 0x27F6E642u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceLibFont", 0xBC75D85Bu, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceLibFont", 0xA834319Du, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, next_font_handle++); });
    runtime.register_hle("sceLibFont", 0x3AEA8CB6u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceLibFont", 0x0DA7535Eu, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceLibFont", 0xDCC80C2Fu, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    runtime.register_hle("sceLibFont", 0xCA1E6945u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });

    // sceSuspendForUser (power tick)
    runtime.register_hle("sceSuspendForUser", 0x090CCB3Fu, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });

    // Networking stubs (sceNet / adhoc / apctl)
    auto net_stub_ok = [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); };
    runtime.register_hle("sceNet", 0x39AF39A6u, net_stub_ok);
    runtime.register_hle("sceNet", 0x281928A9u, net_stub_ok);
    runtime.register_hle("sceNet", 0x89360950u, net_stub_ok);
    runtime.register_hle("sceNet", 0x0BF0A3AEu, net_stub_ok);
    runtime.register_hle("sceNetAdhoc", 0xE1D621D7u, net_stub_ok);
    runtime.register_hle("sceNetAdhoc", 0xA62C6F57u, net_stub_ok);
    runtime.register_hle("sceNetAdhoc", 0x6F92741Bu, net_stub_ok);
    runtime.register_hle("sceNetAdhoc", 0xABED3790u, net_stub_ok);
    runtime.register_hle("sceNetAdhoc", 0xDFE53E03u, net_stub_ok);
    runtime.register_hle("sceNetAdhoc", 0x7F27BB5Eu, net_stub_ok);
    runtime.register_hle("sceNetAdhoc", 0xC7C1FC57u, net_stub_ok);
    runtime.register_hle("sceNetAdhocctl", 0xE26F226Eu, net_stub_ok);
    runtime.register_hle("sceNetAdhocctl", 0x9D689E13u, net_stub_ok);
    runtime.register_hle("sceNetAdhocctl", 0x0AD043EDu, net_stub_ok);
    runtime.register_hle("sceNetAdhocctl", 0x34401D65u, net_stub_ok);
    runtime.register_hle("sceNetAdhocctl", 0x20B317A0u, net_stub_ok);
    runtime.register_hle("sceNetAdhocctl", 0x6402490Bu, net_stub_ok);
    runtime.register_hle("sceNetApDialogDummy", 0xF213BE65u, net_stub_ok);
    runtime.register_hle("sceNetApctl", 0xB3EDD0ECu, net_stub_ok);
    runtime.register_hle("sceNetApctl", 0x2BEFDF23u, net_stub_ok);
    runtime.register_hle("sceNetApctl", 0x5963991Bu, net_stub_ok);
    runtime.register_hle("sceNetApctl", 0x24FE91A1u, net_stub_ok);
    runtime.register_hle("sceNetApctl", 0x5DEAC81Bu, net_stub_ok);
    runtime.register_hle("sceNetInet", 0xA9ED66B9u, net_stub_ok);
    runtime.register_hle("sceNetResolver", 0x6138194Au, net_stub_ok);
    runtime.register_hle("sceWlanDrv", 0xD7763699u, net_stub_ok);
'''

# Insert bridge registration right after thread_table initialization in install_profile
marker = 'thread_table.threads.emplace(0, std::move(module_thread));'
code = code.replace(marker, marker + '\n' + bridge_registration)

# Add namespace alias at end of file: namespace kokaku { using namespace vcs; }
namespace_alias = '''
namespace kokaku {
void install_profile(psprecomp::Runtime &runtime, std::uint32_t user_arena_start) {
    vcs::install_profile(runtime, user_arena_start);
}
void install_display_heartbeat() { vcs::install_display_heartbeat(); }
void install_starvation_preemption() { vcs::install_starvation_preemption(); }
void report_disc_read_stats() { vcs::report_disc_read_stats(); }
void report_present_stats() { vcs::report_present_stats(); }
bool run_profile_self_tests(std::string &error) { return vcs::run_profile_self_tests(error); }
}
'''
code += '\n' + namespace_alias

with open('tools/PSPRecomp/profiles/kokaku/host/kokaku_profile.cpp', 'w', encoding='utf-8') as f:
    f.write(code)

print('Generated kokaku_profile.cpp successfully!')
