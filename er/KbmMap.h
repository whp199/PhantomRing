#pragma once

// Keyboard and mouse onto the pad MGSV receives (roadmap 2.14 tier 1): the pure part -- key names, the [Keyboard]
// bindings, the key-to-pad mapping, the mouse-to-right-stick curve, which device is in use, and the Tarnished-mode tap of
// the chord key. No Windows here: tests/test_host_kbm.inc drives it. The glue (Elden Ring's DirectInput, the XInput
// bridge) is er/Kbm.h.
//
// Why a pad and not MGSV's own raw input: MGSV runs hidden and unfocused, so its raw input is empty (DESIGN §6), and
// everything the host already does with the pad (Start/View and the chord, the prompt button, MGSV's menu owning the pad,
// buttons held over from Elden Ring's menus) then works for the keyboard unchanged. Tier 2 (native mouse aim through
// MGSV's raw-input handler) stays open.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace phantom
{
	class Ini;
}

namespace phantom::er::Kbm
{
	// Key codes: Windows virtual-key codes 1..0xFF (mouse buttons are VKs too: 1 LMB, 2 RMB, 4 MMB, 5/6 side buttons), plus
	// the wheel's notches as two pseudo keys.
	inline constexpr int kWheelUp = 0x100;
	inline constexpr int kWheelDown = 0x101;
	inline constexpr int kKeyCodes = 0x102;

	// "W", "shift", "LMB", "WheelUp", "F5", "0x41" -> its code; -1 = unknown.
	[[nodiscard]] int         KeyCode(std::string_view a_name);
	[[nodiscard]] std::string KeyName(int a_code);
	// "E, F" -> both codes; "none" or nothing known -> empty (the action is unbound).
	[[nodiscard]] std::vector<int> ParseKeys(std::string_view a_text);

	// What each binding does on the pad. Order = the ini's order.
	enum class Action : std::uint8_t
	{
		kUp,           // left stick
		kDown,
		kLeft,
		kRight,
		kRun,          // full tilt (walk_tilt otherwise; run_by_default swaps them)
		kCrouch,       // A: tap crouch / stand, hold prone (MGSV decides tap vs hold)
		kReload,       // B
		kDive,         // X
		kAction,       // Y (MGSV's action, or Elden Ring's while it shows a prompt: the bridge's prompt button)
		kCallMenu,     // LB
		kFirstPerson,  // RB
		kAim,          // LT
		kFire,         // RT
		kPrimary,      // D-pad up
		kHandgun,      // D-pad down
		kSupport,      // D-pad right
		kItems,        // D-pad left
		kView,         // View: a tap is Elden Ring's map, held MGSV's iDroid (er/Chord.h)
		kMenu,         // Start, only while MGSV's own menu has the pad (closes it); otherwise the key is Elden Ring's own (Esc)
		kLeftClick,    // L3
		kRightClick,   // R3 (D-Walker's orders: click the stick in)
		kChord,        // Start + View held together: the Tarnished-mode chord
		kCount,
	};
	inline constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::kCount);

	struct ActionInfo
	{
		const char*   ini;       // [Keyboard] key
		const char*   defaults;  // its default binding
		std::uint16_t buttons;   // XInput button bits
		std::uint8_t  trigger;   // 0 none, 1 LT, 2 RT
	};
	[[nodiscard]] const ActionInfo& Info(Action a_action);

	// The mouse as a right stick: its speed (counts/s, smoothed) through a curve. Why speed and not distance: a stick is a
	// rate (MGSV turns the camera while it is held), so a still mouse must give a centred stick.
	struct MouseCurve
	{
		float sensitivity{ 0.6f };   // stick deflection per 1000 counts/s, before the acceleration
		float acceleration{ 0.5f };  // u -> u * (1 + acceleration * u): slow moves stay fine, fast ones turn hard
		float antiDeadzone{ 0.2f };  // where the output starts (past MGSV's own stick dead zone, so slow moves still turn)
		float minSpeed{ 15.0f };     // counts/s below this = still (sensor noise must not turn the camera)
		float smoothingMs{ 25.0f };  // time constant of the speed's smoothing (Elden Ring reads the mouse once a frame)
		bool  invertY{ false };

		bool operator==(const MouseCurve&) const = default;
	};

	class MouseStick
	{
	public:
		// a_dx/a_dy: counts since the last update, a_dtMs the time it took. Out: stick -1..1, +y = up (pushed forward).
		void Update(float a_dx, float a_dy, float a_dtMs, const MouseCurve& a_curve, float& a_x, float& a_y);
		void Reset() { vx_ = vy_ = 0.0f; }
		[[nodiscard]] float Speed() const;  // counts/s, smoothed

		// The curve alone: speed (counts/s) -> deflection 0..1.
		[[nodiscard]] static float Deflection(float a_speed, const MouseCurve& a_curve);

	private:
		float vx_{ 0.0f }, vy_{ 0.0f };
	};

	// [Keyboard] in PhantomRingHost.ini.
	struct Params
	{
		bool                                         enabled{ false };  // off: nothing changes, Elden Ring keeps its keyboard
		std::array<std::vector<int>, kActionCount>   keys;              // per Action; Defaults() fills them
		float                                        walkTilt{ 0.5f };  // WASD without the run key
		bool                                         runByDefault{ false };
		MouseCurve                                   mouse;
		int                                          wheelTapMs{ 100 };  // one wheel notch = a press this long (and a gap as long)
		std::vector<int>                             erKeys{ 0x1B };     // keys Elden Ring still gets while Snake plays (Esc = its menu)
		// ...and also during an Elden Ring interaction's buttons-only phase (KbmFilter Scope::kButtonsOnly): E (its action),
		// Q, Enter and the arrows for a pickup or a dialog. Not F/Space/WASD/R: Elden Ring's jump, roll, walk and item would
		// move the hidden Tarnished (UNVERIFIED which of its keys a dialog needs).
		std::vector<int>                             erInteractionKeys{ 'E', 'Q', 0x0D, 0x26, 0x28, 0x25, 0x27 };
		bool                                         captureCursor{ true };
		bool                                         keyboardGlyphs{ true };  // Elden Ring's keyboard glyphs ("E: Rest") while the keyboard is in use
		int                                          virtualPad{ -1 };   // -1 auto (when no controller answers), 0 never, 1 always
		// Tarnished mode: what a tap of the chord key (let go before chord_ms) gives Elden Ring. kView: the pad's View tap --
		// its map, whatever its own key bindings are (live pass B 2026-10-15: that save had Elden Ring's map on M, and the G
		// passed on did nothing); kKey: the chord key itself (its map only where Elden Ring's map is on that key); kOff: nothing.
		enum class ChordTapTo : std::uint8_t
		{
			kOff,
			kView,
			kKey,
		};
		ChordTapTo                                   chordTap{ ChordTapTo::kView };

		Params();
		bool operator==(const Params&) const = default;
		void Load(const Ini& a_ini);
	};

	struct PadOut
	{
		std::uint16_t buttons{ 0 };
		std::uint8_t  lt{ 0 }, rt{ 0 };
		std::int16_t  lx{ 0 }, ly{ 0 }, rx{ 0 }, ry{ 0 };

		bool operator==(const PadOut&) const = default;
	};

	struct Input
	{
		std::uint64_t                 nowMs{ 0 };
		std::array<bool, kKeyCodes>   down{};       // by key code (the wheel's pseudo keys are ignored: use wheel)
		int                           wheel{ 0 };   // notches since the last call, + = up
		float                         mouseX{ 0.0f }, mouseY{ 0.0f };  // MouseStick's output
		bool                          guestMenu{ false };  // MGSV's own menu has the pad: kMenu is Start
	};

	// Keys (and the wheel, and the mouse stick) -> one pad state. Holds the wheel's queued taps.
	class Mapper
	{
	public:
		PadOut Map(const Input& a_in, const Params& a_params);
		[[nodiscard]] bool Held(const Input& a_in, const Params& a_params, Action a_action) const;

	private:
		int           wheelUp_{ 0 }, wheelDown_{ 0 };  // notches still to tap
		int           wheelDir_{ 0 };                  // the tap now running: +1 up, -1 down, 0 none
		bool          wheelPressed_{ false };          // ...in its down half (else its gap)
		std::uint64_t wheelUntil_{ 0 };
	};

	// Which device the pad MGSV gets comes from. The last one used wins; the real pad on a tie (it keeps priority), and
	// the pad is where it starts, so controller players never notice the keyboard is there.
	enum class Device : std::uint8_t
	{
		kPad,
		kKeyboard,
	};
	class Arbiter
	{
	public:
		Device Update(bool a_padActivity, bool a_kbmActivity)
		{
			if (a_padActivity) {
				device_ = Device::kPad;
			} else if (a_kbmActivity) {
				device_ = Device::kKeyboard;
			}
			return device_;
		}
		[[nodiscard]] Device Current() const { return device_; }

	private:
		Device device_{ Device::kPad };
	};

	// A real pad's state counts as "in use" past XInput's own dead zones (a resting pad never takes the device back).
	[[nodiscard]] bool PadActive(std::uint16_t a_buttons, std::uint8_t a_lt, std::uint8_t a_rt, std::int16_t a_lx, std::int16_t a_ly, std::int16_t a_rx,
		std::int16_t a_ry);

	// The chord key in Tarnished mode: held, it is the chord (Start + View on the pad: er/Chord.h) and Elden Ring never sees
	// it; let go before chord_ms, Elden Ring gets a tap at the release ([Keyboard] chord_tap: the pad's View = its map, by default).
	class ChordTap
	{
	public:
		// true once, at a release that should reach Elden Ring as a tap.
		bool Update(bool a_down, bool a_tarnished, std::uint64_t a_nowMs, int a_chordMs);

	private:
		bool          down_{ false };
		bool          armed_{ false };
		std::uint64_t since_{ 0 };
	};
}
