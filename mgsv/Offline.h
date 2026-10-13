#pragma once
// MGSV offline: its own WinHTTP connections (the Konami gate, mgstpp-game.konamionline.com) fail as if
// there were no network, so MGSV plays offline -- the way it does without internet. PhantomRing needs
// none of MGSV's online features, and since the July 2026 patch a first login asks for a permanent
// country/region choice and agreement to Konami's updated Terms of Use, which only the player may
// give (in a normal MGSV session). Denuvo's activation uses wininet from its own import set and is
// untouched.

namespace phantom::guest::offline
{
	// Patches MGSV's WINHTTP.dll WinHttpConnect import (IAT slot 0x14208DF38) after checking it.
	bool Install();
	[[nodiscard]] bool Enabled();
	[[nodiscard]] unsigned Refused();
}
