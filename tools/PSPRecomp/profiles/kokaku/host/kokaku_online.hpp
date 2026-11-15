#pragma once

// Online multiplayer: rooms instead of "whoever is on the same LAN".
//
// A player who creates a room in the original Multi Play menu is announced on
// the local network and, when a lobby is configured, on the internet list.
// A player who chooses Join gets a room list in place of the original blind
// search; picking a room connects the transport to its members, after which
// the game's own search finds exactly that room.

#include "psprecomp/runtime.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace kokaku {

// Hooks called from patched ULUS10020 code (tools/patch_kokaku_online.py).
// The search dialog polls CKoDmeP2P::Join every frame; returning true keeps
// it waiting while the room list is open.
bool online_join_hold(psprecomp::GuestMemory &memory, std::uint32_t ui);
void online_join_result(psprecomp::GuestMemory &memory, std::uint32_t result);
// The search page returns the page to show next, or -1 to stay.
void online_join_page(psprecomp::GuestMemory &memory, std::uint32_t page);
void online_create_result(psprecomp::GuestMemory &memory, std::uint32_t result);

// Called once per display vblank from the guest thread.
void online_frame(psprecomp::GuestMemory &memory, std::uint64_t vblank);

// Guest ad hoc lifecycle, from the network HLE.
void online_adhoc_connected(const std::string &group);
void online_adhoc_disconnected();
void online_shutdown();

struct OnlineRoomRow {
    std::string name, map, mode, players, tag;
    bool joinable{};
};

struct OnlineView {
    bool browser{};              // the room list covers the search dialog
    std::vector<OnlineRoomRow> rows;
    int cursor{};
    std::string headline;        // list source and totals
    std::string details;         // full rules of the highlighted room
    std::string message;         // progress or failure of the last action
    std::string footer;          // own connection quality
    bool busy{};                 // a join is in progress: no navigation
    std::string banner;          // one line while inside a room
};
// False when nothing has to be drawn this frame.
bool online_view(OnlineView &view);

// Readable form of the seven room rule values the game stores.
std::string online_rules_text(const std::vector<int> &rules);

bool run_online_self_tests(std::string &error);

} // namespace kokaku
