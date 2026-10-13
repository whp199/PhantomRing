#pragma once
// D-Walker's own sounds at Snake's level ([Audio] WalkerVolume, docs/GUEST.md 18.2).
//
// MGSV's Wwise is statically linked; its SetPosition is detoured. Fox gives most one-shot sounds a game object of their
// own (each D-Walker footstep is a new AkGameObjectID), positioned once right around the moment the sound is posted. An
// object positioned at the walker (near it, clearly away from Snake, while he isn't riding it) gets
// AK::SoundEngine::SetGameObjectOutputBusVolume(id, WalkerVolume) right there, queued behind its position.

#include <cstdint>
#include <string>

namespace phantom::guest::sound
{
	// DllMain: the SetPosition detour and the volume function, behind byte checks (refused = MGSV's volume, logged).
	bool Install();

	// MGSV's Lua thread after every Tick: where the walker and Snake are now, for the detour's test.
	void Tick(bool a_ready);

	// Console: `!sound` (what is tagged, what is near the walker), `!sound volume <gain>` (live).
	std::string Command(const std::string& a_args);
}
