#include "Hud.h"

#include "Host.h"
#include "Link.h"
#include "Log.h"
#include "Overlay.h"
#include "Puppet.h"

#include <algorithm>
#include <atomic>
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
		std::atomic<std::uint64_t> g_hudFrames{ 0 };
		int                 g_aimOverride{ -1 };  // console: -1 MGSV's flag, 0 never, 1 always
		std::vector<std::pair<std::string, Tone>> g_pending;  // console messages for the next Tick
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
		o.hudOpacity = Approach(o.hudOpacity, snakeScreen && a_cfg.hud && a_in.hudFresh ? 1.0f : 0.0f, dt, static_cast<float>(a_cfg.hudFadeMs) / 1000.0f);
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
		in.puppet = Puppet::Active();
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
		in.dying = Puppet::DeathSeconds() >= 0.0f;
		in.aiming = g_aimOverride >= 0 ? g_aimOverride == 1 : (a_f.guestReady && (a_f.guest.flags & proto::kGuestAiming) != 0);
		const auto last = g_lastHudQpc.load(std::memory_order_relaxed);
		in.hudFresh = last != 0 && Link::QpcToMs(Link::Qpc() - last) < 1500.0;

		std::scoped_lock lock(g_lock);
		for (auto& [text, tone] : g_pending) {
			g_brain.Message(std::move(text), tone, in.now, Cfg().overlay.messageSeconds);
		}
		g_pending.clear();
		const bool wasShown = g_model.hudOpacity > 0;
		g_brain.Update(in, Cfg().overlay, a_f.dt);
		g_model = g_brain.Out();
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

	std::string Describe()
	{
		const Model m = Snapshot();
		std::string s = std::format("overlay: HUD layer {:.2f} reticle {:.2f} (aim {}) read {} attach {} | guest HUD frames {} (last {:.0f} ms ago)\n", m.hudOpacity,
			m.reticleOpacity, g_aimOverride < 0 ? "MGSV" : (g_aimOverride ? "forced on" : "forced off"), m.readHud, m.attach, g_hudFrames.load(),
			g_lastHudQpc.load() ? Link::QpcToMs(Link::Qpc() - g_lastHudQpc.load()) : -1.0);
		for (const auto& l : m.lines) {
			s += std::format("  [{:.2f}] {}\n", l.opacity, l.text);
		}
		return s + Overlay::Describe();
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
