#include "KbmMap.h"

#include "Ini.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>

namespace phantom::er::Kbm
{
	namespace
	{
		// XInput's button bits (no <xinput.h> here: this file is pure).
		constexpr std::uint16_t kDUp = 0x0001, kDDown = 0x0002, kDLeft = 0x0004, kDRight = 0x0008, kStart = 0x0010, kBack = 0x0020,
								kL3 = 0x0040, kR3 = 0x0080, kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

		// Defaults follow MGSV's own PC layout where it has one (WASD, Shift, Ctrl, Space, R, E/F, Q, V, mouse buttons) and
		// the README's pad table otherwise (number keys = the D-pad's four pockets).
		constexpr ActionInfo kInfo[kActionCount]{
			{ "move_up", "W", 0, 0 },
			{ "move_down", "S", 0, 0 },
			{ "move_left", "A", 0, 0 },
			{ "move_right", "D", 0, 0 },
			{ "run", "Shift", 0, 0 },
			{ "crouch", "Ctrl", kA, 0 },
			{ "reload", "R", kB, 0 },
			{ "dive", "Space", kX, 0 },
			{ "action", "E, F", kY, 0 },
			{ "call_menu", "Q", kLB, 0 },
			{ "first_person", "V", kRB, 0 },
			{ "aim", "RMB", 0, 1 },
			{ "fire", "LMB", 0, 2 },
			{ "primary", "1, WheelUp", kDUp, 0 },
			{ "handgun", "2, WheelDown", kDDown, 0 },
			{ "support", "3", kDRight, 0 },
			{ "items", "4", kDLeft, 0 },
			{ "view", "Tab", kBack, 0 },
			{ "menu", "Esc", kStart, 0 },
			{ "left_stick_click", "C", kL3, 0 },
			{ "right_stick_click", "MMB", kR3, 0 },
			{ "chord", "G", kStart | kBack, 0 },
		};

		struct Named
		{
			const char* name;
			int         code;
		};
		// Lower case. Several names for the common ones: players write what their keyboard says.
		constexpr Named kNames[]{
			{ "lmb", 0x01 }, { "mouse1", 0x01 }, { "rmb", 0x02 }, { "mouse2", 0x02 }, { "mmb", 0x04 }, { "mouse3", 0x04 }, { "mouse4", 0x05 },
			{ "mouse5", 0x06 }, { "backspace", 0x08 }, { "tab", 0x09 }, { "enter", 0x0D }, { "return", 0x0D }, { "shift", 0x10 },
			{ "ctrl", 0x11 }, { "control", 0x11 }, { "alt", 0x12 }, { "capslock", 0x14 }, { "esc", 0x1B }, { "escape", 0x1B },
			{ "space", 0x20 }, { "pageup", 0x21 }, { "pagedown", 0x22 }, { "end", 0x23 }, { "home", 0x24 }, { "left", 0x25 }, { "up", 0x26 },
			{ "right", 0x27 }, { "down", 0x28 }, { "insert", 0x2D }, { "delete", 0x2E }, { "lshift", 0xA0 }, { "rshift", 0xA1 },
			{ "lctrl", 0xA2 }, { "rctrl", 0xA3 }, { "lalt", 0xA4 }, { "ralt", 0xA5 }, { ";", 0xBA }, { "=", 0xBB }, { ",", 0xBC },
			{ "-", 0xBD }, { ".", 0xBE }, { "/", 0xBF }, { "`", 0xC0 }, { "[", 0xDB }, { "\\", 0xDC }, { "]", 0xDD }, { "'", 0xDE },
			{ "wheelup", kWheelUp }, { "wheeldown", kWheelDown },
		};

		std::string Lower(std::string_view a_text)
		{
			std::string s;
			for (const char c : a_text) {
				if (c != ' ' && c != '\t' && c != '_') {
					s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
				}
			}
			return s;
		}

		std::int16_t Axis(float a_v)
		{
			const float v = std::clamp(a_v, -1.0f, 1.0f);
			return static_cast<std::int16_t>(std::lround(v * 32767.0f));
		}
	}

	const ActionInfo& Info(Action a_action) { return kInfo[static_cast<std::size_t>(a_action)]; }

	int KeyCode(std::string_view a_name)
	{
		const auto s = Lower(a_name);
		if (s.empty()) {
			return -1;
		}
		if (s.size() > 2 && s[0] == '0' && s[1] == 'x') {
			char*      end = nullptr;
			const long v = std::strtol(s.c_str(), &end, 16);
			return (*end == 0 && v > 0 && v < kKeyCodes) ? static_cast<int>(v) : -1;
		}
		if (s.size() == 1 && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9'))) {
			return std::toupper(static_cast<unsigned char>(s[0]));  // VK_A..VK_Z, VK_0..VK_9 are the ASCII upper case
		}
		if (s.size() >= 2 && s.size() <= 3 && s[0] == 'f') {
			const int n = std::atoi(s.c_str() + 1);
			if (n >= 1 && n <= 24 && std::to_string(n) == s.substr(1)) {
				return 0x70 + n - 1;  // VK_F1..
			}
		}
		for (const auto& n : kNames) {
			if (s == n.name) {
				return n.code;
			}
		}
		return -1;
	}

	std::string KeyName(int a_code)
	{
		if ((a_code >= 'A' && a_code <= 'Z') || (a_code >= '0' && a_code <= '9')) {
			return std::string(1, static_cast<char>(a_code));
		}
		if (a_code >= 0x70 && a_code < 0x70 + 24) {
			return std::format("F{}", a_code - 0x70 + 1);
		}
		static constexpr Named kPretty[]{ { "LMB", 0x01 }, { "RMB", 0x02 }, { "MMB", 0x04 }, { "Mouse4", 0x05 }, { "Mouse5", 0x06 }, { "Tab", 0x09 },
			{ "Enter", 0x0D }, { "Shift", 0x10 }, { "Ctrl", 0x11 }, { "Alt", 0x12 }, { "Esc", 0x1B }, { "Space", 0x20 }, { "WheelUp", kWheelUp },
			{ "WheelDown", kWheelDown } };
		for (const auto& n : kPretty) {
			if (n.code == a_code) {
				return n.name;
			}
		}
		for (const auto& n : kNames) {
			if (n.code == a_code) {
				return n.name;
			}
		}
		return std::format("{:#04x}", a_code);
	}

	std::vector<int> ParseKeys(std::string_view a_text)
	{
		std::vector<int> out;
		std::size_t      at = 0;
		while (at <= a_text.size()) {
			auto       comma = a_text.find(',', at);
			const auto item = a_text.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at);
			if (const int code = KeyCode(item); code > 0 && std::ranges::find(out, code) == out.end()) {
				out.push_back(code);
			}
			if (comma == std::string_view::npos) {
				break;
			}
			at = comma + 1;
		}
		return out;
	}

	Params::Params()
	{
		for (std::size_t i = 0; i < kActionCount; ++i) {
			keys[i] = ParseKeys(kInfo[i].defaults);
		}
	}

	void Params::Load(const Ini& a_ini)
	{
		constexpr const char* kSection = "Keyboard";
		enabled = a_ini.GetBool(kSection, "enabled", enabled);
		for (std::size_t i = 0; i < kActionCount; ++i) {
			const auto text = a_ini.Get(kSection, kInfo[i].ini);
			if (!text.empty()) {
				keys[i] = ParseKeys(text);  // "none" parses to nothing: the action is unbound
			}
		}
		walkTilt = std::clamp(static_cast<float>(a_ini.GetFloat(kSection, "walk_tilt", walkTilt)), 0.1f, 1.0f);
		runByDefault = a_ini.GetBool(kSection, "run_by_default", runByDefault);
		mouse.sensitivity = static_cast<float>(a_ini.GetFloat(kSection, "mouse_sensitivity", mouse.sensitivity));
		mouse.acceleration = static_cast<float>(a_ini.GetFloat(kSection, "mouse_acceleration", mouse.acceleration));
		mouse.antiDeadzone = std::clamp(static_cast<float>(a_ini.GetFloat(kSection, "mouse_anti_deadzone", mouse.antiDeadzone)), 0.0f, 0.9f);
		mouse.minSpeed = static_cast<float>(a_ini.GetFloat(kSection, "mouse_min_speed", mouse.minSpeed));
		mouse.smoothingMs = static_cast<float>(a_ini.GetFloat(kSection, "mouse_smoothing_ms", mouse.smoothingMs));
		mouse.invertY = a_ini.GetBool(kSection, "mouse_invert_y", mouse.invertY);
		wheelTapMs = std::clamp(static_cast<int>(a_ini.GetInt(kSection, "wheel_tap_ms", wheelTapMs)), 20, 1000);
		if (const auto text = a_ini.Get(kSection, "er_keys"); !text.empty()) {
			erKeys = ParseKeys(text);
		}
		if (const auto text = a_ini.Get(kSection, "er_interaction_keys"); !text.empty()) {
			erInteractionKeys = ParseKeys(text);
		}
		captureCursor = a_ini.GetBool(kSection, "capture_cursor", captureCursor);
		keyboardGlyphs = a_ini.GetBool(kSection, "keyboard_glyphs", keyboardGlyphs);
		if (const auto text = Lower(a_ini.Get(kSection, "virtual_pad")); !text.empty()) {
			virtualPad = text == "auto" ? -1 : (text == "on" || text == "true" || text == "1" ? 1 : 0);
		}
		if (const auto text = Lower(a_ini.Get(kSection, "chord_tap")); !text.empty()) {
			chordTap = text == "key" ? ChordTapTo::kKey : (text == "off" || text == "none" || text == "false" ? ChordTapTo::kOff : ChordTapTo::kView);
		}
	}

	float MouseStick::Speed() const { return std::sqrt(vx_ * vx_ + vy_ * vy_); }

	float MouseStick::Deflection(float a_speed, const MouseCurve& a_curve)
	{
		if (!(a_speed >= a_curve.minSpeed) || a_speed <= 0.0f) {
			return 0.0f;
		}
		const float u = a_speed * std::max(a_curve.sensitivity, 0.0f) / 1000.0f;
		const float accel = u * (1.0f + std::max(a_curve.acceleration, 0.0f) * u);
		return std::min(1.0f, a_curve.antiDeadzone + (1.0f - a_curve.antiDeadzone) * std::min(accel, 1.0f));
	}

	void MouseStick::Update(float a_dx, float a_dy, float a_dtMs, const MouseCurve& a_curve, float& a_x, float& a_y)
	{
		// A long gap (a load, a stall) must not turn a pending delta into a tiny speed or a held one into a huge one.
		const float dt = std::clamp(a_dtMs, 1.0f, 100.0f);
		const float ix = a_dx * 1000.0f / dt;
		const float iy = a_dy * 1000.0f / dt;
		const float alpha = a_curve.smoothingMs > 0.0f ? 1.0f - std::exp(-dt / a_curve.smoothingMs) : 1.0f;
		vx_ += (ix - vx_) * alpha;
		vy_ += (iy - vy_) * alpha;
		const float speed = Speed();
		const float m = Deflection(speed, a_curve);
		if (m <= 0.0f) {
			a_x = a_y = 0.0f;
			if (speed < 1.0f) {
				vx_ = vy_ = 0.0f;  // settled: no denormal tail
			}
			return;
		}
		// Mouse y grows downward, a stick's upward.
		a_x = vx_ / speed * m;
		a_y = (a_curve.invertY ? vy_ : -vy_) / speed * m;
	}

	bool Mapper::Held(const Input& a_in, const Params& a_params, Action a_action) const
	{
		for (const int code : a_params.keys[static_cast<std::size_t>(a_action)]) {
			if (code == kWheelUp || code == kWheelDown) {
				if (wheelPressed_ && wheelDir_ == (code == kWheelUp ? 1 : -1)) {
					return true;
				}
			} else if (code > 0 && code < kKeyCodes && a_in.down[static_cast<std::size_t>(code)]) {
				return true;
			}
		}
		return false;
	}

	PadOut Mapper::Map(const Input& a_in, const Params& a_params)
	{
		// The wheel: each notch one tap (down wheel_tap_ms, then up as long, so MGSV sees separate presses), at most a few queued.
		if (a_in.wheel > 0) {
			wheelUp_ = std::min(wheelUp_ + a_in.wheel, 3);
		} else if (a_in.wheel < 0) {
			wheelDown_ = std::min(wheelDown_ - a_in.wheel, 3);
		}
		if (wheelDir_ != 0 && a_in.nowMs >= wheelUntil_) {
			if (wheelPressed_) {
				wheelPressed_ = false;
				wheelUntil_ = a_in.nowMs + static_cast<std::uint64_t>(a_params.wheelTapMs);
			} else {
				wheelDir_ = 0;
			}
		}
		if (wheelDir_ == 0 && (wheelUp_ > 0 || wheelDown_ > 0)) {
			wheelDir_ = wheelUp_ > 0 ? 1 : -1;
			(wheelDir_ > 0 ? wheelUp_ : wheelDown_) -= 1;
			wheelPressed_ = true;
			wheelUntil_ = a_in.nowMs + static_cast<std::uint64_t>(a_params.wheelTapMs);
		}

		PadOut out;
		for (std::size_t i = 0; i < kActionCount; ++i) {
			const auto  action = static_cast<Action>(i);
			const auto& info = kInfo[i];
			if ((info.buttons == 0 && info.trigger == 0) || !Held(a_in, a_params, action)) {
				continue;
			}
			if (action == Action::kMenu && !a_in.guestMenu) {
				continue;  // Esc is Elden Ring's own menu key; only MGSV's open menu takes it as Start (to close it)
			}
			out.buttons |= info.buttons;
			if (info.trigger == 1) {
				out.lt = 255;
			} else if (info.trigger == 2) {
				out.rt = 255;
			}
		}

		const float x = (Held(a_in, a_params, Action::kRight) ? 1.0f : 0.0f) - (Held(a_in, a_params, Action::kLeft) ? 1.0f : 0.0f);
		const float y = (Held(a_in, a_params, Action::kUp) ? 1.0f : 0.0f) - (Held(a_in, a_params, Action::kDown) ? 1.0f : 0.0f);
		if (x != 0.0f || y != 0.0f) {
			const bool  run = Held(a_in, a_params, Action::kRun) != a_params.runByDefault;
			const float tilt = run ? 1.0f : a_params.walkTilt;
			const float n = tilt / std::sqrt(x * x + y * y);  // a diagonal is as far as a straight push, not 1.41x
			out.lx = Axis(x * n);
			out.ly = Axis(y * n);
		}
		out.rx = Axis(a_in.mouseX);
		out.ry = Axis(a_in.mouseY);
		return out;
	}

	bool PadActive(std::uint16_t a_buttons, std::uint8_t a_lt, std::uint8_t a_rt, std::int16_t a_lx, std::int16_t a_ly, std::int16_t a_rx, std::int16_t a_ry)
	{
		constexpr int kLeftDz = 7849, kRightDz = 8689, kTrigger = 30;  // XINPUT_GAMEPAD_*_THUMB_DEADZONE, _TRIGGER_THRESHOLD
		const auto    out = [](int a_x, int a_y, int a_dz) { return a_x * a_x + a_y * a_y > a_dz * a_dz; };
		return a_buttons != 0 || a_lt > kTrigger || a_rt > kTrigger || out(a_lx, a_ly, kLeftDz) || out(a_rx, a_ry, kRightDz);
	}

	bool ChordTap::Update(bool a_down, bool a_tarnished, std::uint64_t a_nowMs, int a_chordMs)
	{
		bool tap = false;
		if (a_down && !down_) {
			armed_ = a_tarnished && a_chordMs > 0;
			since_ = a_nowMs;
		} else if (!a_down && down_) {
			// Held chord_ms the chord fired (or was about to): no tap then; a mode that changed meanwhile (a menu came up)
			// takes it too.
			tap = armed_ && a_tarnished && a_nowMs - since_ < static_cast<std::uint64_t>(a_chordMs);
			armed_ = false;
		}
		down_ = a_down;
		return tap;
	}
}
