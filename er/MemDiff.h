#pragma once

// Live reverse engineering helper (dev console `snap`): finds the bytes that follow a game state you can switch
// back and forth (pad vs keyboard glyphs, a menu open or closed), so a field can be found without a debugger.
//
//   snap reset             the regions: eldenring.exe's writable sections, every FD4 singleton (0x4000 bytes) and
//                          every heap object a singleton points to (0x800 bytes each, first level, deduplicated)
//   snap take <label>      copy them all (labels are one word: "pad", "kb" ...)
//   snap diff <a> <b>      bytes equal in every <a> snapshot and in every <b> snapshot, but different between the two
//   snap clear
//
// Reads go through ReadProcessMemory (a freed object is an unreadable region, never a crash). Takes ~100 ms per
// snapshot at most; nothing runs unless asked.
//
// Also `input key <vk> | mouse <dx> <dy>`: SendInput inside Elden Ring's Wine desktop, i.e. keyboard/mouse activity
// for Elden Ring's own input device logic without touching the player's desktop.

#include <string>
#include <vector>

namespace phantom::er::MemDiff
{
	std::string Command(const std::vector<std::string>& a_words);
	std::string Input(const std::vector<std::string>& a_words);
}
