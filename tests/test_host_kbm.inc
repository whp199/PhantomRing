// Included by tests/test_host.cpp: keyboard and mouse as Snake's pad (er/KbmMap.h): key names, the [Keyboard] bindings,
// the key-to-pad mapping, the wheel's taps, the mouse curve, the device arbiter and the chord key's tap.
#include "Ini.h"
#include "Kbm.h"
#include "KbmFilter.h"
#include "KbmMap.h"

#include <fstream>

namespace
{
	Kbm::Input KeysDown(std::initializer_list<int> a_codes, std::uint64_t a_now = 1000)
	{
		Kbm::Input in;
		in.nowMs = a_now;
		for (const int c : a_codes) {
			in.down[static_cast<std::size_t>(c)] = true;
		}
		return in;
	}

	// A stand-in for MapVirtualKey: the real DIKs of the keys the filter tests use (Esc 0x01, W 0x11, E 0x12, F 0x21, G 0x22,
	// Space 0x39, Enter 0x1C, Up 0xC8), none for mouse buttons, 0x80 + the VK for anything else.
	int TestDik(int a_vk)
	{
		switch (a_vk) {
		case 0x1B:
			return 0x01;
		case 'W':
			return 0x11;
		case 'E':
			return 0x12;
		case 'F':
			return 0x21;
		case 'G':
			return 0x22;
		case 0x20:
			return 0x39;
		case 0x0D:
			return 0x1C;
		case 0x26:
			return 0xC8;
		default:
			return a_vk <= 0x06 ? -1 : (0x80 | (a_vk & 0x7F));
		}
	}
}

static void TestKbm()
{
	using namespace phantom::er::Kbm;

	// Key names.
	CHECK(KeyCode("W") == 0x57 && KeyCode("w") == 0x57 && KeyCode("1") == 0x31 && KeyCode("Shift") == 0x10 && KeyCode("ctrl") == 0x11);
	CHECK(KeyCode("LMB") == 0x01 && KeyCode("RMB") == 0x02 && KeyCode("MMB") == 0x04 && KeyCode("Mouse5") == 0x06 && KeyCode("Esc") == 0x1B);
	CHECK(KeyCode("Space") == 0x20 && KeyCode("Tab") == 0x09 && KeyCode("F1") == 0x70 && KeyCode("F12") == 0x7B && KeyCode("0x41") == 0x41);
	CHECK(KeyCode("WheelUp") == kWheelUp && KeyCode("wheel_down") == kWheelDown && KeyCode(" Page Up ") == 0x21);
	CHECK(KeyCode("bogus") == -1 && KeyCode("") == -1 && KeyCode("F25") == -1 && KeyCode("F1x") == -1 && KeyCode("0x200") == -1 && KeyCode("0x") == -1);
	CHECK((ParseKeys("E, F") == std::vector<int>{ 0x45, 0x46 }));
	CHECK((ParseKeys("E,e, bogus ,LMB") == std::vector<int>{ 0x45, 0x01 }));  // duplicates and unknown names dropped
	CHECK(ParseKeys("none").empty() && ParseKeys("").empty());
	CHECK(KeyName(0x57) == "W" && KeyName(0x01) == "LMB" && KeyName(kWheelDown) == "WheelDown" && KeyName(0x74) == "F5");
	for (int c = 1; c < kKeyCodes; ++c) {
		const auto name = KeyName(c);
		CHECK(KeyCode(name) == c || (c == 0x0D) || name.starts_with("0x"));  // every name we print reads back (Enter = Return)
	}

	// Defaults: the README's table.
	const Params d;
	CHECK(!d.enabled && d.virtualPad == -1 && d.walkTilt == 0.5f && !d.runByDefault && d.captureCursor && d.keyboardGlyphs);
	CHECK((d.keys[static_cast<std::size_t>(Action::kAction)] == std::vector<int>{ 'E', 'F' }));
	CHECK((d.keys[static_cast<std::size_t>(Action::kPrimary)] == std::vector<int>{ '1', kWheelUp }));
	CHECK((d.keys[static_cast<std::size_t>(Action::kChord)] == std::vector<int>{ 'G' }) && (d.erKeys == std::vector<int>{ 0x1B }));
	CHECK(Info(Action::kCrouch).buttons == 0x1000 && Info(Action::kReload).buttons == 0x2000 && Info(Action::kDive).buttons == 0x4000 &&
	      Info(Action::kAction).buttons == 0x8000 && Info(Action::kCallMenu).buttons == 0x0100 && Info(Action::kFirstPerson).buttons == 0x0200 &&
	      Info(Action::kAim).trigger == 1 && Info(Action::kFire).trigger == 2 && Info(Action::kView).buttons == 0x0020 && Info(Action::kChord).buttons == 0x0030);

	// The left stick: walk, run, diagonals, opposite keys.
	{
		Mapper m;
		auto   o = m.Map(KeysDown({ 'W' }), d);
		CHECK(o.lx == 0 && o.ly == 16384 && o.buttons == 0 && o.lt == 0 && o.rt == 0);
		o = m.Map(KeysDown({ 'W', 0x10 }), d);
		CHECK(o.ly == 32767);
		o = m.Map(KeysDown({ 'S', 'D' }), d);
		CHECK(o.lx == 11585 && o.ly == -11585);  // 0.5 / sqrt 2: a diagonal is no further than a straight push
		o = m.Map(KeysDown({ 'A', 'D', 'W', 0x10 }), d);
		CHECK(o.lx == 0 && o.ly == 32767);
		o = m.Map(KeysDown({ 'A', 'D' }), d);
		CHECK(o.lx == 0 && o.ly == 0);
		Params run = d;
		run.runByDefault = true;
		CHECK(m.Map(KeysDown({ 'A' }), run).lx == -32767 && m.Map(KeysDown({ 'A', 0x10 }), run).lx == -16384);
	}

	// Buttons and triggers.
	{
		Mapper m;
		CHECK(m.Map(KeysDown({ 0x11 }), d).buttons == 0x1000);
		CHECK(m.Map(KeysDown({ 'F' }), d).buttons == 0x8000 && m.Map(KeysDown({ 'E' }), d).buttons == 0x8000);
		const auto o = m.Map(KeysDown({ 0x01, 0x02 }), d);
		CHECK(o.lt == 255 && o.rt == 255 && o.buttons == 0);
		CHECK(m.Map(KeysDown({ '1', '2', '3', '4' }), d).buttons == 0x000F);
		CHECK(m.Map(KeysDown({ 0x09 }), d).buttons == 0x0020);
		CHECK(m.Map(KeysDown({ 'G' }), d).buttons == 0x0030);  // the chord: Start + View together
		CHECK(m.Map(KeysDown({ 0x04, 'C' }), d).buttons == 0x00C0);
		CHECK(m.Map(KeysDown({ 0x1B }), d).buttons == 0);  // Esc is Elden Ring's own menu key...
		auto in = KeysDown({ 0x1B });
		in.guestMenu = true;
		CHECK(m.Map(in, d).buttons == 0x0010);  // ...and Start while MGSV's menu has the pad (closes it)
		Params unbound = d;
		unbound.keys[static_cast<std::size_t>(Action::kFire)].clear();
		CHECK(m.Map(KeysDown({ 0x01 }), unbound).rt == 0);
		CHECK(m.Map(KeysDown({ 'Z', 'X', 'P' }), d) == PadOut{});  // unbound keys do nothing
	}

	// The wheel: each notch one tap (wheel_tap_ms down, as long up), a few queued.
	{
		Mapper m;
		Input  in;
		auto   at = [&](std::uint64_t a_t, int a_wheel) {
            in.nowMs = a_t;
            in.wheel = a_wheel;
            return m.Map(in, d).buttons;
		};
		CHECK(at(0, 2) == 0x0001);
		CHECK(at(50, 0) == 0x0001);
		CHECK(at(100, 0) == 0);       // released
		CHECK(at(150, 0) == 0);       // the gap
		CHECK(at(200, 0) == 0x0001);  // the second notch
		CHECK(at(300, 0) == 0);
		CHECK(at(400, 0) == 0);
		CHECK(at(410, -9) == 0x0002);  // down: D-pad down (handgun)
		int taps = 1;
		bool last = true;
		for (std::uint64_t t = 420; t < 2000; t += 10) {
			const bool now = at(t, 0) == 0x0002;
			taps += now && !last;
			last = now;
		}
		CHECK(taps == 3);  // at most 3 queued
	}

	// The mouse curve.
	{
		MouseCurve c;
		CHECK(MouseStick::Deflection(0.0f, c) == 0.0f && MouseStick::Deflection(c.minSpeed - 1.0f, c) == 0.0f);
		CHECK(Near(MouseStick::Deflection(c.minSpeed, c), c.antiDeadzone, 0.02f));  // just moving: past MGSV's dead zone at once
		float last = 0.0f;
		bool  rising = true;
		for (float s = 20.0f; s < 10000.0f; s *= 1.2f) {
			const float v = MouseStick::Deflection(s, c);
			rising = rising && v >= last && v <= 1.0f;
			last = v;
		}
		CHECK(rising && last == 1.0f);
		// u = 1000 x 0.6 / 1000 = 0.6 -> 0.6 x 1.3 = 0.78 -> 0.2 + 0.8 x 0.78 = 0.824
		CHECK(Near(MouseStick::Deflection(1000.0f, c), 0.824f, 1e-3f));
		MouseCurve linear = c;
		linear.acceleration = 0.0f;
		CHECK(Near(MouseStick::Deflection(1000.0f, linear), 0.68f, 1e-3f));
		// Acceleration: doubling a slow speed gains more than it does without.
		CHECK(MouseStick::Deflection(500.0f, c) - MouseStick::Deflection(250.0f, c) > MouseStick::Deflection(500.0f, linear) - MouseStick::Deflection(250.0f, linear));

		MouseStick st;
		float      x = 0.0f, y = 0.0f;
		for (int i = 0; i < 30; ++i) {
			st.Update(10.0f, 0.0f, 10.0f, c, x, y);  // 1000 counts/s to the right
		}
		CHECK(Near(st.Speed(), 1000.0f, 5.0f) && Near(x, 0.824f, 0.01f) && Near(y, 0.0f));
		for (int i = 0; i < 30; ++i) {
			st.Update(0.0f, 10.0f, 10.0f, c, x, y);  // then downward: the stick pulled back
		}
		CHECK(Near(x, 0.0f, 0.01f) && y < -0.8f);
		for (int i = 0; i < 30; ++i) {
			st.Update(0.0f, 0.0f, 10.0f, c, x, y);  // a still mouse centres the stick within a few frames
		}
		CHECK(x == 0.0f && y == 0.0f);
		st.Update(0.0f, 0.0f, 10.0f, c, x, y);
		CHECK(st.Speed() == 0.0f);
		MouseCurve inv = c;
		inv.invertY = true;
		inv.smoothingMs = 0.0f;  // no smoothing: the first frame tells
		st.Update(0.0f, 10.0f, 10.0f, inv, x, y);
		CHECK(y > 0.8f);
		st.Update(0.0f, 0.1f, 10.0f, inv, x, y);  // 10 counts/s: noise
		CHECK(x == 0.0f && y == 0.0f);
		// A frame twice as long with twice the counts is the same speed.
		MouseStick a, b;
		float      ax, ay, bx, by;
		a.Update(8.0f, 0.0f, 16.0f, inv, ax, ay);
		b.Update(16.0f, 0.0f, 32.0f, inv, bx, by);
		CHECK(Near(ax, bx));
		// A long stall is clamped: one count over 5 s isn't 0.2 counts/s, and a flick after one isn't a crawl.
		a.Update(50.0f, 0.0f, 5000.0f, inv, ax, ay);
		CHECK(Near(a.Speed(), 500.0f, 1.0f));
	}

	// The device in use: the pad on a tie and at the start.
	{
		Arbiter arb;
		CHECK(arb.Current() == Device::kPad);
		CHECK(arb.Update(false, false) == Device::kPad);
		CHECK(arb.Update(false, true) == Device::kKeyboard);
		CHECK(arb.Update(false, false) == Device::kKeyboard);
		CHECK(arb.Update(true, true) == Device::kPad);
		CHECK(arb.Update(false, false) == Device::kPad);
		CHECK(!PadActive(0, 0, 0, 0, 0, 0, 0) && !PadActive(0, 30, 0, 5000, -5000, 8000, 0));  // a resting pad (and a little drift)
		CHECK(PadActive(0x1000, 0, 0, 0, 0, 0, 0) && PadActive(0, 0, 31, 0, 0, 0, 0) && PadActive(0, 0, 0, 7000, 7000, 0, 0) &&
		      PadActive(0, 0, 0, 0, 0, 0, -9000));
	}

	// The chord key in Tarnished mode: a tap reaches Elden Ring at its release, a hold (the chord) never does.
	{
		ChordTap t;
		CHECK(!t.Update(true, true, 1000, 1000) && !t.Update(true, true, 1200, 1000) && t.Update(false, true, 1300, 1000));
		CHECK(!t.Update(true, true, 2000, 1000) && !t.Update(false, true, 3000, 1000));  // held chord_ms: the chord
		CHECK(!t.Update(true, false, 4000, 1000) && !t.Update(false, true, 4100, 1000));  // went down while Snake played
		CHECK(!t.Update(true, true, 5000, 1000) && !t.Update(false, false, 5100, 1000));  // a menu came up meanwhile
		CHECK(!t.Update(true, true, 6000, 0) && !t.Update(false, true, 6100, 0));         // no chord: the key isn't ours
	}

	// [Keyboard] from an ini.
	{
		{
			std::ofstream f("test_kbm.ini");
			f << "[Keyboard]\nenabled = true\nfire = Space, LMB\naim = none\nwalk_tilt = 3\nvirtual_pad = on\nmouse_sensitivity = 1.25\n"
			     "mouse_invert_y = true\ner_keys = Esc, F1\nchord = bogus\ner_interaction_keys = none\nchord_tap = key\n";
		}
		phantom::Ini ini;
		CHECK(ini.Load(L"test_kbm.ini"));
		Params p;
		p.Load(ini);
		CHECK(p.enabled && p.virtualPad == 1 && p.walkTilt == 1.0f && p.mouse.sensitivity == 1.25f && p.mouse.invertY);
		// The Tarnished-mode tap: the pad's View by default (live pass B: Elden Ring's map was on M, not the chord key G).
		CHECK(d.chordTap == Params::ChordTapTo::kView && p.chordTap == Params::ChordTapTo::kKey);
		CHECK((p.keys[static_cast<std::size_t>(Action::kFire)] == std::vector<int>{ 0x20, 0x01 }) && p.keys[static_cast<std::size_t>(Action::kAim)].empty());
		CHECK(p.keys[static_cast<std::size_t>(Action::kChord)].empty() && (p.erKeys == std::vector<int>{ 0x1B, 0x70 }) && p.erInteractionKeys.empty());
		CHECK(p.keys[static_cast<std::size_t>(Action::kUp)] == d.keys[static_cast<std::size_t>(Action::kUp)]);  // missing keys keep the defaults
		Mapper m;
		CHECK(m.Map(KeysDown({ 0x20 }), p).rt == 255 && m.Map(KeysDown({ 0x20 }), p).buttons == 0x4000);  // one key, two actions: both
	}

	// What Elden Ring's own DirectInput keyboard and mouse see (KbmFilter.h).
	{
		constexpr int kEsc = 0x01, kW = 0x11, kE = 0x12, kF = 0x21, kG = 0x22, kSpace = 0x39, kEnter = 0x1C, kUp = 0xC8;
		CHECK((d.erInteractionKeys == std::vector<int>{ 'E', 'Q', 0x0D, 0x26, 0x28, 0x25, 0x27 }));
		Params p = d;
		p.erKeys.push_back(0x01);  // LMB: no DIK, ignored
		const auto t = DikTables::Build(p, &TestDik);
		CHECK(t.er[kEsc] && !t.er[kE] && t.interaction[kEsc] && t.interaction[kE] && t.interaction[kEnter] && t.interaction[kUp]);
		CHECK(!t.interaction[kW] && !t.interaction[kSpace] && !t.interaction[kF] && !t.interaction[kG] && t.chord[kG] && t.tap == kG);
		CHECK(std::count(t.er.begin(), t.er.end(), 1) == 1 && std::count(t.chord.begin(), t.chord.end(), 1) == 1);

		const Mode none{}, chord{ Scope::kChordOnly }, buttons{ Scope::kButtonsOnly }, snake{ Scope::kFull }, guest{ Scope::kFull, true };
		CHECK(ErKeeps(t, kW, none) && ErKeeps(t, kG, none) && ErKeeps(t, kW, chord) && !ErKeeps(t, kG, chord));
		CHECK(ErKeeps(t, kEsc, snake) && !ErKeeps(t, kW, snake) && !ErKeeps(t, kE, snake) && !ErKeeps(t, kEsc, guest));
		// An interaction's buttons-only phase (review 2026-10-14: Elden Ring had the whole keyboard and mouse then, so WASD,
		// Space and a click walked, rolled and swung the hidden Tarnished): its interaction keys only, no mouse.
		CHECK(ErKeeps(t, kE, buttons) && ErKeeps(t, kEsc, buttons) && ErKeeps(t, kEnter, buttons) && ErKeeps(t, kUp, buttons));
		CHECK(!ErKeeps(t, kW, buttons) && !ErKeeps(t, kSpace, buttons) && !ErKeeps(t, kF, buttons) && !ErKeeps(t, kG, buttons));
		CHECK(ErGetsMouse(Scope::kNone) && ErGetsMouse(Scope::kChordOnly) && !ErGetsMouse(Scope::kButtonsOnly) && !ErGetsMouse(Scope::kFull));
		CHECK(ErKeeps(t, -1, snake) && ErKeeps(t, 0x100, snake));

		// Snapshots: a key hidden while held stays hidden until let go.
		SnapshotFilter f;
		std::array<std::uint8_t, 256> k{};
		const auto read = [&](Mode a_m, std::initializer_list<int> a_down, bool a_tap = false) {
			k.fill(0);
			for (const int dik : a_down) {
				k[static_cast<std::size_t>(dik)] = 0x80;
			}
			f.Keyboard(k.data(), t, a_m, a_tap);
			return k;
		};
		// Esc that closes MGSV's menu (review 2026-10-14): hidden while the menu is open, still hidden once it has closed under
		// the held key (Elden Ring would see a fresh press and open its own menu), a new press passes.
		CHECK(read(guest, { kEsc })[kEsc] == 0 && read(snake, { kEsc })[kEsc] == 0 && read(snake, {})[kEsc] == 0 && read(snake, { kEsc })[kEsc] == 0x80);
		// E that started an interaction: hidden in kFull, still hidden in its buttons-only phase; the next press passes.
		CHECK(read(snake, { kE })[kE] == 0 && read(buttons, { kE })[kE] == 0 && read(buttons, {})[kE] == 0 && read(buttons, { kE })[kE] == 0x80);
		// ...and W, Space stay hidden through the whole phase.
		CHECK(read(buttons, { kW, kSpace })[kW] == 0 && k[kSpace] == 0);
		// G held for the chord, a menu comes up (Tarnished mode -> none): not a fresh G (its map) mid-hold.
		CHECK(read(chord, { kG })[kG] == 0 && read(none, { kG })[kG] == 0 && read(none, {})[kG] == 0 && read(none, { kG })[kG] == 0x80);
		// A key Elden Ring had, hidden as Snake takes over (a release), then given back still held: no fresh press either.
		CHECK(read(none, { kW })[kW] == 0x80 && read(snake, { kW })[kW] == 0 && read(none, { kW })[kW] == 0 && read(none, { kW })[kW] == 0);
		CHECK(read(none, {})[kW] == 0 && read(none, { kW })[kW] == 0x80);
		// The chord key's tap reads as down while it is due, held or not, hidden or not.
		CHECK(read(chord, {}, true)[kG] == 0x80 && read(chord, { kG }, true)[kG] == 0x80 && read(chord, { kG })[kG] == 0);
		CHECK(read(none, {})[kG] == 0);

		// The mouse's snapshot: nothing in kFull or kButtonsOnly; a button held through the change back stays hidden.
		std::array<std::uint8_t, 8> b{};
		b[0] = 0x80;
		CHECK(!f.Mouse(b.data(), b.size(), Scope::kFull) && b[0] == 0);
		b[0] = 0x80;
		CHECK(!f.Mouse(b.data(), b.size(), Scope::kButtonsOnly) && b[0] == 0);
		b[0] = 0x80;
		CHECK(f.Mouse(b.data(), b.size(), Scope::kNone) && b[0] == 0);  // RMB/LMB still held from Snake's aim: no guard/swing
		b[0] = 0;
		CHECK(f.Mouse(b.data(), b.size(), Scope::kNone) && b[0] == 0);
		b[0] = 0x80;
		b[1] = 0x80;
		CHECK(f.Mouse(b.data(), b.size(), Scope::kChordOnly) && b[0] == 0x80 && b[1] == 0x80);
		CHECK(!f.Mouse(b.data(), b.size(), Scope::kFull) && b[0] == 0 && b[1] == 0);  // hidden = released

		// Buffered events (review 2026-10-14: dropping every hidden event lost the release of a key Elden Ring had seen go
		// down, so W stayed down in it and the Tarnished walked on alone).
		EventFilter e;
		const auto keeps = [&](Mode a_m) { return [&t, a_m](std::size_t a_dik) { return ErKeeps(t, static_cast<int>(a_dik), a_m); }; };
		// W down in Tarnished mode, the chord gives Snake control, W let go: Elden Ring gets that release.
		CHECK(e.Pass(kW, true, ErKeeps(t, kW, chord)) && e.SeesDown(kW) && e.Pass(kW, false, ErKeeps(t, kW, snake)) && !e.SeesDown(kW));
		// W down in Elden Ring's menu, Snake takes over with it still held: a made-up release at once, the real one dropped.
		CHECK(e.Pass(kW, true, ErKeeps(t, kW, none)) && e.NextRelease(keeps(none)) == -1);
		CHECK(e.NextRelease(keeps(snake)) == kW && e.NextRelease(keeps(snake)) == -1 && !e.Pass(kW, false, ErKeeps(t, kW, snake)));
		// Hidden presses never pass; their release passes only for a key Elden Ring keeps (a stray up is harmless).
		CHECK(!e.Pass(kSpace, true, ErKeeps(t, kSpace, snake)) && !e.Pass(kSpace, false, ErKeeps(t, kSpace, snake)) && !e.SeesDown(kSpace));
		CHECK(!e.Pass(kEsc, true, ErKeeps(t, kEsc, guest)) && e.Pass(kEsc, false, ErKeeps(t, kEsc, snake)) && !e.SeesDown(kEsc));
		// Esc held over MGSV's menu closing: no event can make it a press (only its release comes).
		CHECK(e.NextRelease(keeps(snake)) == -1);
		// E pressed by Snake, the interaction's buttons-only phase, E let go: no press reaches Elden Ring (only a stray release).
		CHECK(!e.Pass(kE, true, ErKeeps(t, kE, snake)) && e.Pass(kE, false, ErKeeps(t, kE, buttons)));
		// A peek works on a copy: the original still owes the release.
		CHECK(e.Pass(kW, true, true));
		{
			EventFilter peek = e;
			CHECK(peek.NextRelease(keeps(snake)) == kW && peek.NextRelease(keeps(snake)) == -1);
		}
		CHECK(e.SeesDown(kW) && e.NextRelease(keeps(snake)) == kW);
		// Mouse buttons: a held RMB (Tarnished guarding) released for Elden Ring as Snake takes the mouse.
		EventFilter m;
		CHECK(m.Pass(1, true, ErGetsMouse(Scope::kChordOnly)) && m.NextRelease([](std::size_t) { return true; }) == -1);
		CHECK(m.NextRelease([](std::size_t) { return false; }) == 1 && !m.Pass(1, false, false) && !m.Pass(0, true, false));
		CHECK(m.Pass(300, true, false));  // out of range: left alone
	}

	// The glue under wine: the DirectInput probe devices are made and their functions hooked (Elden Ring's own devices are
	// what it filters, so nothing more can be seen here); [Keyboard] off keeps every key Elden Ring's.
	{
		CHECK(Install() && Installed());
		CHECK(KeyReachesEr('G') && KeyReachesEr(0x1B) && GlyphDevice(0) == 0 && GlyphDevice(-1) == -1 && !KeyboardInUse());
		Tick();  // off: nothing to do, nothing captured
		CHECK(Describe().starts_with("keyboard: off (hooked)"));
	}
}
