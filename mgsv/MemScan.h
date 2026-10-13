#pragma once
// Live reverse engineering from the guest console (Cheat Engine style), for fields no static analysis
// has pinned down yet (e.g. where Snake's life really lives, re-backlog M6):
//   !scan <u16|i32|f32> <value>   every committed writable address holding that value
//   !rescan <value>               keep those that hold the new value now
//   !scanlist                     the survivors (first 32) with their current values
//   !peek <addr> <u16|i32|f32>    !poke <addr> <u16|i32|f32> <value>
// Runs on the link worker thread; reads are guarded, so stale addresses can't crash MGSV.

#include <string>
#include <string_view>

namespace phantom::guest::memscan
{
	// Handles "scan", "rescan", "scanlist", "peek", "poke" (the line without the leading '!');
	// returns false when the command isn't one of these.
	bool Command(std::string_view a_cmd, std::string& a_out);
}
