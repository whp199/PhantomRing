#include "Hud.h"

#include "ErBars.h"
#include "Host.h"
#include "Ini.h"
#include "InputBridge.h"
#include "Link.h"
#include "Log.h"
#include "Overlay.h"
#include "Puppet.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <format>
#include <mutex>
#include <sstream>

namespace phantom::er::Hud
{
	namespace
	{
		constexpr float  kTextFade = 0.3f;        // s: status line in/out
		constexpr float  kToastIn = 0.25f;        // s
		constexpr float  kToastOut = 0.6f;        // s, at the end of its life
		constexpr float  kReticleFade = 0.12f;    // s: the reticle follows aiming quickly
		constexpr double kFailedShown = 30.0;     // s: a failure stays up after MGSV exits
		constexpr std::size_t kMaxToasts = 3;

		float Approach(float a_value, float a_target, float a_dt, float a_seconds)
		{
			const float step = a_seconds > 0.001f ? a_dt / a_seconds : 1.0f;
			return a_value < a_target ? std::min(a_target, a_value + step) : std::max(a_target, a_value - step);
		}

		std::mutex          g_lock;
		Brain               g_brain;
		Model               g_model;
		std::atomic<std::int64_t> g_lastHudQpc{ 0 };
		std::atomic<std::uint64_t> g_layerHeldUntil{ 0 };  // HideLayerFor (GetTickCount64)
		std::atomic<std::uint64_t> g_layerHolds{ 0 };
		std::atomic<std::uint64_t> g_hudFrames{ 0 };
		int                 g_aimOverride{ -1 };  // console: -1 MGSV's flag, 0 never, 1 always
		std::vector<std::pair<std::string, Tone>> g_pending;  // console messages for the next Tick
		// (0.7.0) The controls card.
		CardTimer           g_card;
		std::string         g_cardWanted;       // why it should show from the next Tick on ("" = not asked)
		bool                g_cardSessionDone{ false };  // shown at Snake's first takeover this session (or not due: seen before)
		std::uint32_t       g_helpSeen{ 0 };    // InputBridge::HelpPresses() last Tick
		std::uint32_t       g_cardClosesSeen{ 0 };  // InputBridge::CardCloses() last Tick
		bool                g_cardMarkPending{ false };  // the first-run card: PhantomRingHost.firstrun once it was drawn
		std::uint64_t       g_cardShows{ 0 };
		std::filesystem::path FirstRunPath() { return std::filesystem::path(ModuleDir()) / L"PhantomRingHost.firstrun"; }
	}

	void CardTimer::Show(double a_now)
	{
		// Up already: stays up (no new fade in), its time starts again.
		shownAt_ = shownAt_ >= 0.0 && closeAt_ < 0.0 ? a_now - kIn : a_now;
		closeAt_ = -1.0;
		lastNow_ = a_now;
		released_ = false;
	}

	void CardTimer::Close(double a_now)
	{
		if (Open()) {
			closeAt_ = a_now;
		}
	}

	float CardTimer::Update(double a_now, bool a_buttons, float a_seconds, bool a_visible)
	{
		const double dt = lastNow_ >= 0.0 ? std::max(0.0, a_now - lastNow_) : 0.0;
		lastNow_ = a_now;
		if (shownAt_ < 0.0) {
			return 0.0f;
		}
		if (!a_visible && closeAt_ < 0.0) {
			shownAt_ += dt;     // unseen: its time stands still
			released_ = false;  // a press made meanwhile (a menu's) must be let go first
			return 0.0f;
		}
		if (!a_buttons) {
			released_ = true;
		} else if (released_ && closeAt_ < 0.0) {
			closeAt_ = a_now;  // any press closes it
		}
		if (closeAt_ < 0.0 && a_now - shownAt_ >= a_seconds) {
			closeAt_ = a_now;
		}
		const float in = std::clamp(static_cast<float>((a_now - shownAt_) / kIn), 0.0f, 1.0f);
		if (closeAt_ < 0.0) {
			return in;
		}
		const float out = 1.0f - static_cast<float>((a_now - closeAt_) / kOut);
		if (out <= 0.0f) {
			shownAt_ = closeAt_ = -1.0;
			return 0.0f;
		}
		return std::min(in, out);
	}

	std::string ControlsCardText(const std::string& a_jump, const std::string& a_item, const std::string& a_cycle, const std::string& a_card)
	{
		return std::format(
			"Snake's controls (PhantomRing)\n"
			"{}: jump    Y: climb up / vault / ladders, Elden Ring's prompts\n"
			"View + {}: flask (Elden Ring's quick item)    View + {}: next item\n"
			"View tapped: map    View held: MGSV's iDroid\n"
			"View + Start held 1 s: Tarnished mode (and back)\n"
			"LB held: buddy orders (right stick, then click it)\n"
			"View + {}: this card again    A: close",
			a_jump, a_item, a_cycle, a_card);
	}

	void ShowControlsCard(const char* a_why)
	{
		std::scoped_lock lock(g_lock);
		g_cardWanted = a_why ? a_why : "asked";
	}

	void Brain::Message(std::string a_text, Tone a_tone, double a_now, float a_seconds)
	{
		// The same text again just restarts its clock (a flapping guest must not stack copies).
		std::erase_if(toasts_, [&](const Toast& t) { return t.text == a_text; });
		toasts_.push_back({ std::move(a_text), a_tone, a_now, std::max(0.5, static_cast<double>(a_seconds)) });
		while (toasts_.size() > kMaxToasts) {
			toasts_.erase(toasts_.begin());
		}
	}

	void Brain::Update(const Inputs& a_in, const OverlayConfig& a_cfg, float a_dt)
	{
		const float dt = std::clamp(a_dt, 0.0f, 0.25f);
		auto&       o = out_;
		o.readHud = a_in.guestAttached && a_in.guestAlive;
		o.attach = a_in.attach;

		// MGSV's layer and the reticle belong to Snake's screen only.
		const bool snakeScreen = a_cfg.enabled && a_in.puppet && a_in.guestAlive && !a_in.menuOpen && !a_in.cutscene && !a_in.loading && !a_in.dead && !a_in.dying;
		// Only a fresh layer: a guest that stopped publishing (it skips only after an empty frame) must
		// not leave its last frame frozen over Elden Ring.
		o.hudOpacity = a_in.hudHeld ? 0.0f
		                            : Approach(o.hudOpacity, snakeScreen && a_cfg.hud && a_in.hudFresh ? 1.0f : 0.0f, dt, static_cast<float>(a_cfg.hudFadeMs) / 1000.0f);
		const bool reticle = snakeScreen && a_in.aiming && (a_cfg.reticle == "on" || (a_cfg.reticle == "auto" && !(a_cfg.hud && a_in.hudFresh)));
		o.reticleOpacity = Approach(o.reticleOpacity, reticle ? 1.0f : 0.0f, dt, kReticleFade);

		// Events.
		if (first_) {
			first_ = false;
			absentSince_ = a_in.now;
			wasPuppet_ = a_in.puppet;
			wasAlive_ = a_in.guestAlive;
		}
		if (a_cfg.messages) {
			if (a_in.puppet && !wasPuppet_) {
				Message("Snake is in control", Tone::kGood, a_in.now, a_cfg.messageSeconds);
			} else if (!a_in.puppet && wasPuppet_ && !a_in.guestAlive) {
				Message("MGSV stopped -- Elden Ring has the controller", Tone::kWarn, a_in.now, a_cfg.messageSeconds);
			}
		}
		if (wasAlive_ && !a_in.guestAlive) {
			absentSince_ = a_in.now;
		}
		wasPuppet_ = a_in.puppet;
		wasAlive_ = a_in.guestAlive;
		everAlive_ |= a_in.guestAlive;

		// The status line: what MGSV is doing while Snake is not playing.
		std::string text;
		Tone        tone = Tone::kInfo;
		const bool  live = a_in.guestAlive && a_in.guestAttached;
		if (live && a_in.guestStatus == proto::Status::kFailed) {
			failedText_ = a_in.guestText.empty() ? "MGSV: failed (see PhantomRingGuest.log)" : a_in.guestText;
			failedAt_ = a_in.now;
		} else if (live) {
			failedText_.clear();  // it recovered: a later exit is not that failure
		}
		if (!a_in.versionText.empty()) {
			text = a_in.versionText;  // MGSV's plugin speaks another protocol and never attaches (common/Version.h)
			tone = Tone::kError;
		} else if (live) {
			switch (a_in.guestStatus) {
			case proto::Status::kBooting:
			case proto::Status::kTitle:
			case proto::Status::kLoading:
				text = a_in.guestText.empty() ? "MGSV: starting" : a_in.guestText;
				break;
			case proto::Status::kFailed:
				text = failedText_;
				tone = Tone::kError;
				break;
			case proto::Status::kExiting:
				text = "MGSV is closing";
				tone = Tone::kWarn;
				break;
			case proto::Status::kReady:
				if (!a_in.inGame) {
					text = "MGSV ready -- Snake takes over once your character is in the world";
					tone = Tone::kGood;
				}
				break;
			case proto::Status::kAbsent:
				break;
			}
		} else if (!failedText_.empty() && a_in.now - failedAt_ < kFailedShown) {
			text = failedText_;
			tone = Tone::kError;
		} else if (a_in.now - absentSince_ < a_cfg.absentSeconds) {
			// After a loss the message already said who has the controller.
			text = everAlive_ ? "MGSV is not running" : "MGSV is not running -- Elden Ring plays as usual";
			tone = Tone::kWarn;
		}
		const bool showStatus = a_cfg.enabled && a_cfg.status && !a_in.puppet && !a_in.cutscene && !text.empty();
		if (showStatus) {
			statusText_ = text;
			statusTone_ = tone;
		}
		statusOpacity_ = Approach(statusOpacity_, showStatus ? 1.0f : 0.0f, dt, kTextFade);

		o.lines.clear();
		if (statusOpacity_ > 0.0f && !statusText_.empty()) {
			o.lines.push_back({ statusText_, statusTone_, statusOpacity_ });
		}
		std::erase_if(toasts_, [&](const Toast& t) { return a_in.now - t.born >= t.seconds; });
		if (a_cfg.enabled && a_cfg.messages && !a_in.cutscene) {
			for (const auto& t : toasts_) {
				const double age = a_in.now - t.born;
				const float  in = std::clamp(static_cast<float>(age / kToastIn), 0.0f, 1.0f);
				const float  out = std::clamp(static_cast<float>((t.seconds - age) / kToastOut), 0.0f, 1.0f);
				o.lines.push_back({ t.text, t.tone, std::min(in, out) });
			}
		}
	}

	void Tick(const Frame& a_f)
	{
		auto&  link = Link::Get();
		Inputs in;
		in.now = Link::QpcToMs(Link::Qpc()) / 1000.0;
		in.puppet = Puppet::Active() || Puppet::ClimbVisual();  // Snake on a ladder (Elden Ring climbs): still Snake's screen
		in.guestAlive = a_f.guestAlive;
		in.guestAttached = link.AttachCount() > 0 && a_f.guestAlive;
		in.guestStatus = link.GuestStatus();
		in.guestText = link.GuestStatusText();
		in.versionText = link.VersionMismatch();
		in.attach = link.AttachCount();
		in.inGame = a_f.inGame;
		in.menuOpen = a_f.menuOpen;
		in.cutscene = a_f.cutscene;
		in.loading = a_f.loading;
		in.dead = a_f.dead;
		in.dying = Puppet::Dying();  // any death (Snake's, or the Tarnished's while Snake waits)
		in.aiming = g_aimOverride >= 0 ? g_aimOverride == 1 : (a_f.guestReady && (a_f.guest.flags & proto::kGuestAiming) != 0);
		const auto last = g_lastHudQpc.load(std::memory_order_relaxed);
		in.hudFresh = last != 0 && Link::QpcToMs(Link::Qpc() - last) < 1500.0;
		in.hudHeld = ::GetTickCount64() < g_layerHeldUntil.load(std::memory_order_relaxed);

		std::scoped_lock lock(g_lock);
		for (auto& [text, tone] : g_pending) {
			g_brain.Message(std::move(text), tone, in.now, Cfg().overlay.messageSeconds);
		}
		g_pending.clear();
		const bool wasShown = g_model.hudOpacity > 0;
		g_brain.Update(in, Cfg().overlay, a_f.dt);
		g_model = g_brain.Out();
		// (0.7.0) The controls card: at Snake's first takeover (once per install, or every session), on View + card_button, on `hud card`.
		// Only where it can be drawn (review 0.7.0): a takeover while Elden Ring's grace menu, a cutscene or MGSV's menu is up waits
		// for Snake's screen, and the first-run marker is written once the card was drawn, never for a card nobody saw.
		const auto& oc = Cfg().overlay;
		const bool  cardScreen = Puppet::Active() && !a_f.menuOpen && !a_f.cutscene && !a_f.loading && !a_f.dead && !Puppet::Dying() &&
		                        !(a_f.guestReady && (a_f.guest.flags & proto::kGuestInMenu));
		if (Puppet::Active() && cardScreen && !g_cardSessionDone && oc.enabled && oc.controlsCard != "off" && a_f.guestReady) {
			g_cardSessionDone = true;
			std::error_code ec;
			if (oc.controlsCard == "always" || !std::filesystem::exists(FirstRunPath(), ec)) {
				g_cardWanted = "Snake's first takeover";
				g_cardMarkPending = oc.controlsCard != "always";
			}
		}
		const auto help = InputBridge::HelpPresses();
		if (help != g_helpSeen) {
			g_helpSeen = help;
			if (Puppet::Active() && oc.controlsCard != "off") {
				g_cardWanted = std::format("View + {}", oc.cardButtonName);
			}
		}
		if (!g_cardWanted.empty()) {
			g_card.Show(in.now);
			++g_cardShows;
			Log::Info("overlay: the controls card ({}; it fades after {:.0f} s or at a press)", g_cardWanted, oc.cardSeconds);
			g_cardWanted.clear();
		}
		const auto closes = InputBridge::CardCloses();
		if (closes != g_cardClosesSeen) {
			g_cardClosesSeen = closes;
			g_card.Close(in.now);
			Log::Info("overlay: the controls card closed with A (that A kept from MGSV until let go)");
		}
		const bool  anyButton = InputBridge::LastButtons() != 0;
		const float card = g_card.Update(in.now, anyButton, oc.cardSeconds, cardScreen);
		g_model.cardOpacity = cardScreen ? card : 0.0f;
		InputBridge::SetCardUp(g_model.cardOpacity > 0.0f && g_card.Open());
		if (g_model.cardOpacity > 0.0f) {
			g_model.card = ControlsCardText(Cfg().jumpButtonName, Cfg().flaskButtonName, Cfg().flaskCycleName, oc.cardButtonName);
			if (g_cardMarkPending) {
				g_cardMarkPending = false;
				std::ofstream(FirstRunPath()) << "PhantomRing: the controls card was shown here once ([Overlay] controls_card = first). "
				                                 "Delete this file to see it again at the next takeover.\n";
				Log::Info("overlay: the first controls card is on screen -- {} written", FirstRunPath().filename().string());
			}
		}
		// The iDroid fills MGSV's screen: moving its bottom-right part up with the weapon box cut it (VERIFIED live).
		g_model.guestMenu = a_f.guestReady && (a_f.guest.flags & proto::kGuestInMenu) != 0;
		if (wasShown != (g_model.hudOpacity > 0)) {
			Log::Debug("overlay: MGSV HUD layer {}", g_model.hudOpacity > 0 ? "shown" : "hidden");
		}
	}

	void Notify(std::string a_text, Tone a_tone)
	{
		std::scoped_lock lock(g_lock);
		g_pending.emplace_back(std::move(a_text), a_tone);
	}

	Model Snapshot()
	{
		std::scoped_lock lock(g_lock);
		return g_model;
	}

	void NoteHudFrame()
	{
		g_lastHudQpc.store(Link::Qpc(), std::memory_order_relaxed);
		++g_hudFrames;
	}

	void HideLayerFor(int a_ms)
	{
		if (a_ms <= 0) {
			return;
		}
		g_layerHeldUntil.store(::GetTickCount64() + static_cast<std::uint64_t>(a_ms), std::memory_order_relaxed);
		++g_layerHolds;
	}

	std::string Describe()
	{
		const Model m = Snapshot();
		std::string s = std::format("overlay: HUD layer {:.2f} reticle {:.2f} (aim {}) read {} attach {} | guest HUD frames {} (last {:.0f} ms ago) | cut at "
		                            "a climb's start {} times ({} ms, [Overlay] climb_hide_ms)\n",
			m.hudOpacity, m.reticleOpacity, g_aimOverride < 0 ? "MGSV" : (g_aimOverride ? "forced on" : "forced off"), m.readHud, m.attach, g_hudFrames.load(),
			g_lastHudQpc.load() ? Link::QpcToMs(Link::Qpc() - g_lastHudQpc.load()) : -1.0, g_layerHolds.load(), Cfg().overlay.climbHideMs);
		for (const auto& l : m.lines) {
			s += std::format("  [{:.2f}] {}\n", l.opacity, l.text);
		}
		return s + Overlay::Describe() + "\n" + ErBars::Describe();
	}

	namespace
	{
		// "0, -0.05" or "0 -0.05": the [Overlay] hud_* lists, live (missing numbers keep their value).
		void Numbers(const std::string& a_text, float* a_out, int a_n)
		{
			std::string t = a_text;
			std::replace(t.begin(), t.end(), ',', ' ');
			std::istringstream in(t);
			for (int i = 0; i < a_n; ++i) {
				float v;
				if (in >> v) {
					a_out[i] = v;
				}
			}
		}
	}

	std::string Command(std::string_view a_args)
	{
		std::istringstream in{ std::string(a_args) };
		std::string        verb, key;
		in >> verb >> key;
		std::string value;
		std::getline(in, value);
		value.erase(0, value.find_first_not_of(' '));
		auto& o = Cfg().overlay;
		if (verb.empty() || verb == "status") {
			return Describe();
		}
		if (verb == "shot") {
			// hud shot: the next frame as PhantomRingHost.shot.bmp next to the DLL (half size), even with the window hidden
			Overlay::RequestShot();
			return "shot requested (`hud shot status` / the log say when it is written); last: " + Overlay::ShotStatus();
		}
		if (verb == "msg") {
			// hud msg [info|good|warn|error] <text>
			Tone        tone = Tone::kInfo;
			std::string text = key + (value.empty() ? "" : " " + value);
			if (key == "good" || key == "warn" || key == "error" || key == "info") {
				tone = key == "good" ? Tone::kGood : key == "warn" ? Tone::kWarn : key == "error" ? Tone::kError : Tone::kInfo;
				text = value;
			}
			if (text.empty()) {
				return "hud msg [info|good|warn|error] <text>";
			}
			std::scoped_lock lock(g_lock);
			g_pending.emplace_back(text, tone);
			return "message queued: " + text;
		}
		if (verb == "bars") {
			return ErBars::SetMode(key.empty() ? "status" : key);
		}
		if (verb == "card") {
			ShowControlsCard("the console");
			return std::format("controls card shown ({} so far; [Overlay] controls_card = {}, {}: {})", g_cardShows + 1, o.controlsCard,
				FirstRunPath().filename().string(), std::filesystem::exists(FirstRunPath()) ? "present (seen)" : "absent");
		}
		if (verb == "aim") {
			g_aimOverride = key == "on" ? 1 : key == "off" ? 0 : -1;
			return std::format("aiming: {}", g_aimOverride < 0 ? "from MGSV (kGuestAiming)" : (g_aimOverride ? "forced on" : "forced off"));
		}
		if (verb != "set" || key.empty()) {
			return "usage: hud [status] | hud msg [info|good|warn|error] <text> | hud aim on|off|auto | hud set <key> <value>\n"
				   "keys: enabled hud hud_fade_ms reticle (auto|on|off) reticle_scale status messages message_seconds absent_seconds text_scale corner "
				   "hud_scale hud_offset hud_weapon_region hud_weapon_bar_region hud_weapon_offset hud_notify_region hud_notify_offset (lists: \"0, -0.05\")";
		}
		auto b = [&] { return value == "1" || value == "true" || value == "on" || value == "yes"; };
		auto f = [&] { return static_cast<float>(std::atof(value.c_str())); };
		if (key == "enabled") o.enabled = b();
		else if (key == "hud") o.hud = b();
		else if (key == "hud_fade_ms") o.hudFadeMs = std::atoi(value.c_str());
		else if (key == "reticle" && (value == "auto" || value == "on" || value == "off")) o.reticle = value;
		else if (key == "reticle_scale") o.reticleScale = f();
		else if (key == "status") o.status = b();
		else if (key == "messages") o.messages = b();
		else if (key == "message_seconds") o.messageSeconds = f();
		else if (key == "absent_seconds") o.absentSeconds = f();
		else if (key == "text_scale") o.textScale = f();
		else if (key == "corner" && (value == "top_right" || value == "top_left" || value == "bottom_right" || value == "bottom_left")) o.corner = value;
		else if (key == "hud_scale") o.hudScale = f();
		else if (key == "hud_offset") Numbers(value, o.hudOffset, 2);
		else if (key == "hud_weapon_region") Numbers(value, o.hudWeaponRegion, 4);
		else if (key == "hud_weapon_bar_region") Numbers(value, o.hudWeaponBarRegion, 4);
		else if (key == "hud_weapon_offset") Numbers(value, o.hudWeaponOffset, 2);
		else if (key == "hud_notify_region") Numbers(value, o.hudNotifyRegion, 4);
		else if (key == "hud_notify_offset") Numbers(value, o.hudNotifyOffset, 2);
		else return std::format("unknown key or value: {} {} (hud set)", key, value);
		Log::Info("overlay: console set {} = {}", key, value);
		return std::format("{} = {}", key, value);
	}
}
