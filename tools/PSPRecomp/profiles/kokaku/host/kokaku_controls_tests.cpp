#include "kokaku_controls.hpp"
#include "kokaku_control_icons.hpp"
#include "kokaku_weapon_input.hpp"
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace kokaku {
bool run_controls_tests(std::string &error) {
    const auto directory = std::filesystem::temp_directory_path() /
        ("kokaku-controls-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto require = [](bool ok, const char *message) { if (!ok) throw std::runtime_error(message); };
    try {
        std::filesystem::create_directories(directory);
        Controls c;
        WeaponCycleInput weapons;
        const auto released = [](int) { return false; };
        weapons.sample(c, -3, true, released);
        require(weapons.pop()==1 && weapons.pop()==1 && weapons.pop()==1 && weapons.pop()==0,
            "Fast wheel scrolling lost or repeated detents");
        weapons.sample(c, 1, true, released);
        require(weapons.pop()==-1, "Wheel up did not select the previous weapon");
        c.bind(8, 0, wheel_down);
        weapons.sample(c, -1, true, released);
        require(weapons.pop()==-1, "Weapon wheel direction ignored remapping");
        c.keys[9]={0,5};
        const auto mouse5 = [](int key) { return key==5; };
        weapons.sample(c, 0, true, mouse5);
        weapons.sample(c, 0, true, mouse5);
        require(weapons.pop()==1 && weapons.pop()==0, "Held secondary mouse binding repeated weapon changes");
        weapons.sample(c, 0, true, released);
        weapons.sample(c, 0, true, mouse5);
        require(weapons.pop()==1, "Re-pressed weapon binding was lost");
        weapons.sample(c, -1, true, released);
        weapons.sample(c, -4, false, mouse5);
        weapons.sample(c, 0, true, mouse5);
        require(weapons.pop()==0, "Menu input leaked into gameplay weapon selection");
        weapons.suspend();
        weapons.sample(c, 0, true, mouse5);
        require(weapons.pop()==0, "Focus restoration triggered a held weapon binding");
        weapons.sample(c, 0, true, released);
        weapons.sample(c, 0, true, mouse5);
        require(weapons.pop()==1, "Focus restoration left the weapon binding disabled");
        c=Controls{};
        require(c.keys[reload_action][0]=='R' && c.keys[lock_action][0]=='F',
            "Reload and lock-on must use separate default keys");
        weapons.sample(c, 0, true, [](int k) { return k=='R'; });
        weapons.sample(c, 0, true, [](int k) { return k=='R'; });
        require(weapons.pop()==reload_request && weapons.pop()==0, "Held R repeated or failed to request reload");
        weapons.sample(c, 0, true, released);
        weapons.sample(c, 0, true, [](int k) { return k=='F'; });
        require(weapons.pop()==0, "Lock-on was routed to reload");
        c.keys[reload_action]={0,5};
        weapons.sample(c, 0, true, mouse5);
        require(weapons.pop()==reload_request, "Reload ignored its secondary mouse binding");
        c=Controls{};
        require(control_prompt_key(c,0x4000)=='Z' && control_prompt_key(c,0x4000,ControlContext::Game)=='K',
            "Menu and gameplay prompts use different bindings");
        // All seven commands on the in-mission Select/Tab sheet must advertise
        // a key that emits the actual PSP button in the selected input mode.
        const auto check_command_sheet = [&](const Controls &bindings, bool fps) {
            const auto context = input_control_context(fps);
            for (auto button : {0x100u, 0x200u, 0x10u, 0x1000u, 0x8000u, 0x2000u, 0x4000u}) {
                const int key = control_prompt_key(bindings, button, context);
                if (!key) continue;
                const auto mask = control_buttons(bindings, context, [=](int k) { return k && k == key; });
                require((mask & button) != 0, "Command-sheet prompt does not trigger its advertised action");
            }
        };
        check_command_sheet(c, true); check_command_sheet(c, false);
        require(control_prompt_key(c,0x100,input_control_context(true))==0x20 &&
                control_prompt_key(c,0x200,input_control_context(true))==1 &&
                control_prompt_key(c,0x10,input_control_context(true))=='E',
                "Tab sheet resolved menu bindings while gameplay input was active");
        Controls remapped = c;
        remapped.bind(5,0,'B'); remapped.keys[6]={0,5}; remapped.bind(11,0,'U');
        check_command_sheet(remapped, true); check_command_sheet(remapped, false);
        require(control_prompt_key(remapped,0x100,ControlContext::Game)=='B' &&
                control_prompt_key(remapped,0x200,ControlContext::Game)==5 &&
                control_prompt_key(remapped,0x1000,ControlContext::Game)=='U',
                "Command sheet ignored remapped or secondary-only bindings");
        c.bind(17,0,'T');
        require(control_prompt_key(c,0x4000)=='T',"Prompt ignored a remapped confirm key");
        c.keys[17]={0,2};
        require(control_prompt_key(c,0x4000)==2,"Prompt ignored a secondary mouse binding");
        c.keys[17]={0,0};
        require(control_prompt_key(c,0x4000)==0,"Unbound prompt showed a default key");
        require(control_prompt_button(0xa077a021)==0 && control_prompt_button(0x5677c7e5)==0x4000,
            "UI panel was mistaken for a button icon");
        require(control_key_name(0x20)==L"Space" && control_key_name(0x0d)==L"Enter" && control_key_name(2)==L"Right mouse",
            "Key names must stay English on a localized OS");
        require(control_icon(1)!=control_icon(2) && control_icon(wheel_up)!=control_icon(wheel_down),
            "Mouse buttons and wheel directions must be visually distinct");
        std::vector<std::byte> icon_texture(32*32*4,std::byte{255});
        replace_control_texture(icon_texture,2);
        require(icon_texture[(4*32+12)*4+3]==std::byte{255} && icon_texture.back()==std::byte{},
            "Prompt texture lost mouse pixels or transparent padding");
        c=Controls{};
        c.bind(0, 0, 'S');
        require(c.keys[0][0] == 'S' && c.keys[1][0] == 'W', "Movement conflict did not swap");
        c.bind(6, 1, 5);
        c.bind(8, 0, wheel_down);
        require(c.keys[9][0] == wheel_up, "Wheel conflict did not swap");
        c.bind(0, 0, 0x71);
        require(c.keys[0][0] == 'S', "Reserved F2 was rebound");
        c.sensitivity = 0.0375f;
        c.invert_y = true;
        const auto path = directory / "controls.ini";
        require(c.save(path, error), "Save failed");
        Controls loaded;
        require(loaded.load(path, error), "Load failed");
        require(loaded.keys == c.keys && loaded.sensitivity == c.sensitivity && loaded.invert_y,
            "Bindings or mouse preferences did not survive reload");
        const auto legacy = directory / "legacy.ini";
        std::ofstream(legacy) << "Down=82,0\nPreviousWeapon=257,0\nNextWeapon=256,0\n";
        Controls migrated;
        require(migrated.load(legacy,error) && migrated.keys[reload_action][0]=='R' &&
            migrated.keys[lock_action][0]=='F' && migrated.keys[8][0]==wheel_down,
            "Legacy R lock binding was not migrated without losing custom weapon controls");
        std::ofstream(legacy) << "Down=71,0\nReload=84,5\n";
        require(migrated.load(legacy,error) && migrated.keys[lock_action][0]=='G' &&
            migrated.keys[reload_action]==std::array<int,2>{'T',5}, "Custom reload/lock settings were overwritten");
        std::filesystem::remove(legacy);
        c.sensitivity = 0.2f;
        require(c.save(path, error) && loaded.load(path, error) && loaded.sensitivity == 0.2f,
            "Replacing existing settings failed");
        std::ofstream(path) << "Sensitivity=nan\nInvertY=garbage\nForward=113,0\nAttack=5,6\n";
        require(loaded.load(path, error) && loaded.sensitivity == Controls{}.sensitivity &&
            loaded.keys[0][0] == 'W' && loaded.keys[6] == std::array<int,2>{5,6},
            "Malformed configuration was not handled safely");
        require(!c.save(directory / "missing" / "controls.ini", error), "Failed save was reported successful");
        std::filesystem::remove(path);
        std::filesystem::remove(directory);
        error.clear();
        return true;
    } catch (const std::exception &e) { error = e.what(); return false; }
}
}
