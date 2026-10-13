#pragma once
// While PhantomRing runs, MGSV must never write the player's saves (the sandbox mission, warps,
// disabled soldiers and buddy changes would all end up in them). Two layers:
//
//  1. Steam Cloud: MGSV writes TPP_GAME_DATA & co. through ISteamRemoteStorage (interface
//     STEAMREMOTESTORAGE_INTERFACE_VERSION012; the exe's own call sites use slot 0 FileWrite, 1 FileRead,
//     3 FileDelete, 10 FileExists, 12 GetFileSize). We swap the write/delete/stream slots on that
//     interface's vtable and keep the "written" files in memory, answering the matching reads from
//     there, so the game sees consistent saves for the session and nothing reaches Steam.
//  2. Files: CreateFileW / DeleteFileW / MoveFileWithProgressW (process-wide, so the CRT's fopen and
//     the in-process Steam client are covered too) redirect any write to a path under \userdata\ or
//     named like a save (TPP_*, MGO_*, PERSONAL_DATA) into <dll dir>\PhantomRingSaveSink\.
//
// The launcher also backs the save folders up before every run (belt and braces).

namespace phantom::guest::saves
{
	bool InstallFileGuards();  // DllMain-safe: creates the kernel32 hooks
	// Worker thread: swaps the ISteamRemoteStorage slots once Steam is up. kIncomplete is returned once,
	// on the call whose swap missed a slot: MGSV's real save path (FileWrite) may then be live.
	enum class SteamPoll
	{
		kWaiting,     // Steam not up yet, or the swap already ran on an earlier call
		kGuarded,     // every slot swapped (this call)
		kIncomplete,  // some slot could not be swapped (this call)
	};
	SteamPoll PollSteam();
	[[nodiscard]] bool SteamGuarded();
}
