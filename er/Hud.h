#pragma once

// What PhantomRing shows over Elden Ring's frame (DESIGN.md "HUD"; SkyCraft's GUI layer and corner
// messages), decided once per Elden Ring frame on the task thread and drawn at Present by er/Overlay:
//
//  - MGSV's own HUD layer (the guest's HudCtl triple buffer): only while Snake plays and no Elden Ring
//    menu, cutscene, load or death owns the screen; it fades out (hud_fade_ms) when that ends, so a guest
//    that goes away leaves its last frame fading rather than frozen.
//  - a fallback reticle at the screen centre while MGSV says Snake aims and no HUD layer arrives
//    ([Overlay] reticle = auto) -- the real guest does not publish its HUD yet.
//  - corner text: MGSV's boot progress (Header.guestStatusText) and failures while Snake is not playing,
//    "MGSV is not running" for a while, and short event messages ("Snake is in control").
//
// The decisions are a pure function of Inputs (Brain), so tests/test_host_overlay.inc checks them
// offline; Tick() samples the game and the link for it.

#include "Config.h"
#include "phantomring_protocol.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace phantom::er
{
	struct Frame;

	namespace Hud
	{
		enum class Tone : std::uint8_t
		{
			kInfo,   // MGSV amber: progress
			kGood,   // green: Snake in control, MGSV ready
			kWarn,   // yellow: MGSV absent/stopped
			kError,  // red: MGSV failed
		};

		struct Line
		{
			std::string text;
			Tone        tone{ Tone::kInfo };
			float       opacity{ 0 };
		};

		struct Model
		{
			float             hudOpacity{ 0 };      // MGSV HUD layer
			float             reticleOpacity{ 0 };  // fallback reticle
			bool              readHud{ false };     // the guest is attached: take its HUD frames
			std::uint32_t     attach{ 0 };          // link attach count: the HUD reader restarts on a change
			bool              guestMenu{ false };   // MGSV's own menu (iDroid, pause) is up: its layer is one picture, drawn whole
			std::vector<Line> lines;                // status first, then messages (oldest first)
		};

		// Everything the decisions depend on, sampled once per Elden Ring frame.
		struct Inputs
		{
			double        now{ 0 };  // seconds, monotonic
			bool          puppet{ false };
			bool          guestAttached{ false };
			bool          guestAlive{ false };
			proto::Status guestStatus{ proto::Status::kAbsent };
			std::string   guestText;
			std::string   versionText;  // Link::VersionMismatch(): shown instead of MGSV's status while it is set
			std::uint32_t attach{ 0 };
			bool          inGame{ false };
			bool          menuOpen{ false };
			bool          cutscene{ false };
			bool          loading{ false };
			bool          dead{ false };
			bool          dying{ false };     // Snake/the player died and has not respawned yet
			bool          aiming{ false };    // GuestState kGuestAiming
			bool          hudFresh{ false };  // a guest HUD frame arrived in the last 1.5 s
		};

		class Brain
		{
		public:
			void         Update(const Inputs& a_in, const OverlayConfig& a_cfg, float a_dt);
			void         Message(std::string a_text, Tone a_tone, double a_now, float a_seconds);
			const Model& Out() const { return out_; }

		private:
			struct Toast
			{
				std::string text;
				Tone        tone;
				double      born, seconds;
			};

			Model              out_;
			std::vector<Toast> toasts_;
			bool               first_{ true };
			bool               wasPuppet_{ false };
			bool               everAlive_{ false };
			bool               wasAlive_{ false };
			double             absentSince_{ 0 };
			std::string        failedText_;
			double             failedAt_{ -1e9 };
			std::string        statusText_;
			Tone               statusTone_{ Tone::kInfo };
			float              statusOpacity_{ 0 };
		};

		void        Tick(const Frame& a_frame);  // FrameBegin, after Puppet::Decide
		void        Notify(std::string a_text, Tone a_tone);  // any module: a corner message from the next Tick on
		Model       Snapshot();                  // any thread (Present)
		void        NoteHudFrame();              // Present thread: a guest HUD frame arrived
		std::string Command(std::string_view a_args);  // dev console "hud ..."
		std::string Describe();
	}
}
