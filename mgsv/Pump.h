#pragma once
// Everything that runs on MGSV's Lua thread (via the lua_pcall depth-1 hook): loading our script,
// the dev console, the per-update PhantomRing.Tick and the boot automation that steers MGSV from its
// title screen into the sandbox mission with the buddy deployed.

#include <string>

namespace phantom::guest::pump
{
	void Run(void* a_L);
	std::string Describe();  // '!status'
}
