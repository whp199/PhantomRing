#pragma once
// MGSV's own audio output, measured in the game (docs/GUEST.md 18.2): what Wwise hands XAudio2 (MGSV loads xaudio2_7 under
// Wine) is the mix the player hears from MGSV, so its RMS per 0.5 s is the dBFS a recording of MGSV's stream would show.
// Recording MGSV's stream from outside (parecord --monitor-stream) yields no samples on the test machine's PipeWire, so the
// guest meters it itself: CoCreateInstance(CLSID_XAudio2) is watched, the engine's CreateSourceVoice and the voices'
// SubmitSourceBuffer get a vtable slot each (no code patched in xaudio2_7), and every buffer's samples are summed.
// [Audio] Meter = true (default false: nothing hooked). Console: `!sound meter [mark | since | <n>]`.

#include <string>

namespace phantom::guest::audio_meter
{
	// DllMain, before MGSV creates its audio engine. Logs what it did.
	void Install();
	// `!sound meter`: the last 0.5 s windows (dBFS) of the busiest voice; `mark` starts a span, `since` reports it.
	std::string Command(const std::string& a_args);
}
