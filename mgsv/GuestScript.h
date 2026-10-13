#pragma once
// The Lua half of the guest. The embedded script defines the global table PhantomRing (Tick, Buddy,
// LoadMission, Setup, ...). Guest.cpp loads it into MGSV's Lua state the first time the pump runs,
// again whenever the global disappears (a new Lua state), and from phantomring.lua next to the DLL
// when that file exists (reloaded when its timestamp changes).

#include <string_view>

namespace phantom::guest::script
{
	std::string_view Embedded();
}
