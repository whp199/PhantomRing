#pragma once
// Live reverse engineering: a hardware write/access watch on one address (debug register DR0 on every thread of
// MGSV), to find the code behind a field found with !scan (e.g. the D-Walker whistle record, docs/GUEST.md 18.3).
//   !watch <addr> [w|rw] [1|2|4|8]   arm it (default: writes, 4 bytes)
//   !watch <addr> pw|prw [n]         page-guard mode (works under Wine): the page keeps its own protection plus
//                                    PAGE_GUARD, and gets it back on the next !watch or !unwatch
//   !watchlist                       the instructions that hit it (address after the access, count, return addresses)
//   !unwatch                         clear DR0 everywhere
// Dev tool only: nothing arms it unless the console asks.

#include <string>
#include <string_view>

namespace phantom::guest::watch
{
	bool Command(std::string_view a_cmd, std::string& a_out);
}
