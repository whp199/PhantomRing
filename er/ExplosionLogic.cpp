#include "ExplosionLogic.h"

#include "Params.h"

#include "phantomring_protocol.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <numbers>

namespace phantom::er::explosion
{
	namespace
	{
		template <class T>
		T Get(const std::uint8_t* a_row, std::uint32_t a_offset)
		{
			T v{};
			std::memcpy(&v, a_row + a_offset, sizeof(T));
			return v;
		}

		template <class T>
		void Set(std::uint8_t* a_row, std::uint32_t a_offset, const T& a_value)
		{
			std::memcpy(a_row + a_offset, &a_value, sizeof(T));
		}

		// Integers separated by spaces and/or commas; nullopt on anything else.
		std::optional<std::vector<std::int64_t>> Numbers(std::string_view a_text)
		{
			std::vector<std::int64_t> out;
			std::size_t               i = 0;
			while (i < a_text.size()) {
				const char c = a_text[i];
				if (c == ' ' || c == ',' || c == '\t') {
					++i;
					continue;
				}
				std::int64_t v = 0;
				const auto [end, ec] = std::from_chars(a_text.data() + i, a_text.data() + a_text.size(), v);
				if (ec != std::errc() || end == a_text.data() + i) {
					return std::nullopt;
				}
				out.push_back(v);
				i = static_cast<std::size_t>(end - a_text.data());
			}
			return out;
		}
	}

	std::string_view Name(Kind a_kind)
	{
		switch (a_kind) {
		case Kind::kLethal:
			return "lethal";
		case Kind::kStun:
			return "stun";
		case Kind::kSleep:
			return "sleep";
		case Kind::kSmoke:
			return "smoke";
		case Kind::kFlame:
			return "flame";
		case Kind::kBreaker:
			return "breaker";
		case Kind::kShrapnel:
			return "shrapnel";
		case Kind::kMuzzle:
			return "muzzle";
		}
		return "?";
	}

	std::optional<Kind> FromAmmo(std::uint32_t a_ammo)
	{
		switch (a_ammo) {
		case proto::kAmmoLethal:
			return Kind::kLethal;
		case proto::kAmmoTranq:
			return Kind::kSleep;
		case proto::kAmmoStun:
			return Kind::kStun;
		case proto::kAmmoSmoke:
			return Kind::kSmoke;
		default:
			return std::nullopt;
		}
	}

	std::optional<Kind> FromName(std::string_view a_name)
	{
		if (a_name == "lethal" || a_name == "fire" || a_name == "grenade" || a_name == "rocket") {
			return Kind::kLethal;
		}
		if (a_name == "stun" || a_name == "flash") {
			return Kind::kStun;
		}
		if (a_name == "sleep" || a_name == "tranq" || a_name == "gas") {
			return Kind::kSleep;
		}
		if (a_name == "smoke" || a_name == "flare") {
			return Kind::kSmoke;
		}
		if (a_name == "flame" || a_name == "flamethrower") {
			return Kind::kFlame;
		}
		if (a_name == "breaker" || a_name == "break") {
			return Kind::kBreaker;
		}
		if (a_name == "shrapnel") {
			return Kind::kShrapnel;
		}
		if (a_name == "muzzle" || a_name == "muzzleflash") {
			return Kind::kMuzzle;
		}
		return std::nullopt;
	}

	std::optional<Mode> ParseMode(std::string_view a_text)
	{
		if (a_text == "auto") {
			return Mode::kAuto;
		}
		if (a_text == "bullet") {
			return Mode::kBullet;
		}
		if (a_text == "sfx") {
			return Mode::kSfx;
		}
		if (a_text == "off") {
			return Mode::kOff;
		}
		return std::nullopt;
	}

	std::string_view Name(Mode a_mode)
	{
		switch (a_mode) {
		case Mode::kAuto:
			return "auto";
		case Mode::kBullet:
			return "bullet";
		case Mode::kSfx:
			return "sfx";
		case Mode::kOff:
			return "off";
		}
		return "?";
	}

	std::optional<Look> ParseLook(std::string_view a_text)
	{
		const auto n = Numbers(a_text);
		if (!n || n->empty() || n->size() > 2 || (*n)[0] <= 0 || (*n)[0] > 0x7FFFFFFF) {
			return std::nullopt;
		}
		Look l;
		l.templateId = static_cast<std::uint32_t>((*n)[0]);
		if (n->size() == 2) {
			if ((*n)[1] < -1 || (*n)[1] > 0x7FFFFFFF) {
				return std::nullopt;
			}
			l.sfx = static_cast<std::int32_t>((*n)[1]);
		}
		return l;
	}

	std::optional<std::vector<std::uint32_t>> ParseIds(std::string_view a_text)
	{
		const auto n = Numbers(a_text);
		if (!n || n->size() < 4 || n->size() > kKinds) {
			return std::nullopt;
		}
		std::vector<std::uint32_t> out;
		for (const auto v : *n) {
			if (v <= 0 || v > 0x7FFFFFFF || std::ranges::find(out, static_cast<std::uint32_t>(v)) != out.end()) {
				return std::nullopt;  // carriers must be real, distinct rows: two kinds sharing one would overwrite each other
			}
			out.push_back(static_cast<std::uint32_t>(v));
		}
		return out;
	}

	float NativeRadius(float a_hitRadius, float a_hitRadiusMax)
	{
		float r = std::isfinite(a_hitRadius) ? a_hitRadius : 0.0f;
		if (std::isfinite(a_hitRadiusMax) && a_hitRadiusMax > r) {
			r = a_hitRadiusMax;
		}
		return std::clamp(r, 1.0f, 6.0f);
	}

	std::vector<Vec3> Layout(float a_radius, float a_nativeRadius, int a_maxBursts, float a_phase)
	{
		std::vector<Vec3> out{ Vec3{} };
		const float       native = std::max(0.5f, std::isfinite(a_nativeRadius) ? a_nativeRadius : 1.0f);
		if (a_maxBursts < 4 || !std::isfinite(a_radius) || a_radius <= native * 1.25f) {
			return out;  // one burst covers it (or no room for a ring: it needs three)
		}
		// The ring sits inside the edge by most of a burst, so the outer bursts reach about the blast radius.
		float       ring = std::max(native * 0.8f, a_radius - native * 0.6f);
		const float spacing = native * 1.6f;
		const int   n = std::clamp(static_cast<int>(std::lround(2.0f * std::numbers::pi_v<float> * ring / spacing)), 3, a_maxBursts - 1);
		// Too few bursts for that ring: pull it in until they touch (a tight cluster reads as one big blast, a sparse
		// ring of small ones does not).
		ring = std::min(ring, static_cast<float>(n) * spacing / (2.0f * std::numbers::pi_v<float>));
		for (int i = 0; i < n; ++i) {
			const float a = a_phase + 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(n);
			out.push_back({ ring * std::cos(a), 0.0f, ring * std::sin(a) });
		}
		return out;
	}

	bool CloudGate::Allow(Kind a_kind, const Vec3& a_pos, double a_nowMs, double a_repeatMs, float a_near)
	{
		// Forget clouds MGSV stopped reporting (a few repeats without a report).
		std::erase_if(clouds_, [&](const Cloud& c) { return a_nowMs - c.lastMs > std::max(3.0 * c.repeatMs, 2000.0) || a_nowMs < c.lastMs; });
		if (a_repeatMs <= 0) {
			return true;
		}
		for (auto& c : clouds_) {
			if (c.kind == a_kind && LengthXZ(c.pos - a_pos) <= a_near && std::fabs(c.pos.y - a_pos.y) <= 4.0f) {
				if (a_nowMs - c.lastMs < a_repeatMs) {
					return false;
				}
				c.lastMs = a_nowMs;
				c.repeatMs = a_repeatMs;
				return true;
			}
		}
		if (clouds_.size() >= 64) {
			clouds_.erase(clouds_.begin());
		}
		clouds_.push_back({ a_kind, a_pos, a_nowMs, a_repeatMs });
		return true;
	}

	bool Harmful(const std::uint8_t* a_row)
	{
		namespace b = params::bullet;
		const auto atk = Get<std::int32_t>(a_row, b::kAtkId);
		if (atk != 0 && atk != -1) {
			return true;
		}
		for (int i = 0; i < b::kSpEffectSlots; ++i) {
			if (Get<std::int32_t>(a_row, b::kSpEffectId0 + 4u * static_cast<std::uint32_t>(i)) > 0) {
				return true;
			}
		}
		return Get<std::int32_t>(a_row, b::kSpEffectIdForShooter) > 0;
	}

	Chain WalkChain(const std::uint8_t* a_template, const std::function<const std::uint8_t*(std::uint32_t)>& a_row, std::size_t a_maxRows)
	{
		namespace b = params::bullet;
		Chain                      out;
		std::vector<std::uint32_t> todo;
		auto                       children = [&](const std::uint8_t* a_r) {
			for (const auto off : { b::kHitBulletId, b::kIntervalCreateBulletId }) {
				const auto id = Get<std::int32_t>(a_r, off);
				if (id > 0) {
					todo.push_back(static_cast<std::uint32_t>(id));
				}
			}
		};
		children(a_template);
		while (!todo.empty()) {
			const auto id = todo.back();
			todo.pop_back();
			if (std::ranges::find(out.ids, id) != out.ids.end()) {
				continue;  // a loop (or a shared child) is walked once
			}
			if (out.ids.size() >= a_maxRows) {
				out.complete = false;
				break;
			}
			out.ids.push_back(id);
			const auto* r = a_row(id);
			if (!r) {
				out.complete = false;
				continue;
			}
			if (Harmful(r)) {
				out.harmless = false;
			}
			children(r);
		}
		return out;
	}

	void MakeCarrier(std::uint8_t* a_row, std::int32_t a_sfx, bool a_keepChain)
	{
		namespace b = params::bullet;
		Set<std::int32_t>(a_row, b::kAtkId, 0);
		for (int i = 0; i < b::kSpEffectSlots; ++i) {
			Set<std::int32_t>(a_row, b::kSpEffectId0 + 4u * static_cast<std::uint32_t>(i), 0);
		}
		Set<std::int32_t>(a_row, b::kSpEffectIdForShooter, 0);
		Set<std::uint8_t>(a_row, b::kCreateLimitGroupId, 0);
		if (!a_keepChain) {
			Set<std::int32_t>(a_row, b::kHitBulletId, -1);
			Set<std::int32_t>(a_row, b::kIntervalCreateBulletId, -1);
		}
		if (a_sfx > 0) {
			Set<std::int32_t>(a_row, b::kSfxIdBullet, a_sfx);
		}
	}

	std::int32_t FlightSfx(const std::uint8_t* a_row)
	{
		const auto sfx = Get<std::int32_t>(a_row, params::bullet::kSfxIdBullet);
		return sfx > 0 ? sfx : -1;
	}

	void MakeBreaker(std::uint8_t* a_row, std::int32_t a_atk, float a_radius, float a_life, float a_speed)
	{
		namespace b = params::bullet;
		MakeCarrier(a_row, -1, false);
		Set<std::int32_t>(a_row, b::kAtkId, a_atk);
		Set<std::int32_t>(a_row, b::kSfxIdBullet, -1);
		Set<std::int32_t>(a_row, b::kSfxIdHit, -1);
		Set<std::int32_t>(a_row, b::kSfxIdFlick, -1);
		for (const auto off : { b::kSeIdBullet1, b::kSeIdBullet2, b::kSeIdHit, b::kSeIdFlick }) {
			Set<std::int32_t>(a_row, off, -1);
		}
		Set<float>(a_row, b::kHitRadius, std::clamp(a_radius, 0.05f, 8.0f));
		Set<float>(a_row, b::kHitRadiusMax, -1.0f);
		Set<float>(a_row, b::kLife, std::clamp(a_life, 0.02f, 1.0f));
		// A moving breaker sweeps its path (Elden Ring tests a bullet against the map and its assets along its flight).
		const float v = std::isfinite(a_speed) ? std::clamp(a_speed, 0.0f, 400.0f) : 0.0f;
		for (const auto off : { b::kInitVelocity, b::kMaxVelocity, b::kMinVelocity }) {
			Set<float>(a_row, off, v);
		}
	}

	void MakeSingleShot(std::uint8_t* a_row, float a_speed)
	{
		namespace b = params::bullet;
		Set<std::uint16_t>(a_row, b::kNumShoot, 1);
		Set<float>(a_row, b::kShootInterval, 0.0f);
		if (a_speed > 0.0f && std::isfinite(a_speed)) {
			for (const auto off : { b::kInitVelocity, b::kMaxVelocity, b::kMinVelocity }) {
				Set<float>(a_row, off, a_speed);
			}
		}
	}

	void MakeFlash(std::uint8_t* a_row, float a_life)
	{
		namespace b = params::bullet;
		MakeSingleShot(a_row, 0.0f);
		for (const auto off : { b::kInitVelocity, b::kMaxVelocity, b::kMinVelocity }) {
			Set<float>(a_row, off, 0.0f);
		}
		Set<float>(a_row, b::kLife, std::isfinite(a_life) ? std::clamp(a_life, 0.02f, 0.5f) : 0.06f);
		Set<std::int8_t>(a_row, b::kSfxDeleteByLifeDead, 1);
		Set<std::int32_t>(a_row, b::kHitBulletId, -1);  // nothing after it (MakeCarrier already cuts a chain it isn't told to keep)
		Set<std::int32_t>(a_row, b::kIntervalCreateBulletId, -1);
	}

	bool OnlyAttack(const std::uint8_t* a_row, std::int32_t a_atk)
	{
		namespace b = params::bullet;
		if (Get<std::int32_t>(a_row, b::kAtkId) != a_atk) {
			return false;
		}
		for (int i = 0; i < b::kSpEffectSlots; ++i) {
			if (Get<std::int32_t>(a_row, b::kSpEffectId0 + 4u * static_cast<std::uint32_t>(i)) > 0) {
				return false;
			}
		}
		return Get<std::int32_t>(a_row, b::kSpEffectIdForShooter) <= 0;
	}

	void MakeObjectOnlyAttack(std::uint8_t* a_atk, std::uint16_t a_objAttack, bool a_hitsCharacters)
	{
		namespace a = params::atk;
		for (const auto off : { a::kAtkPhys, a::kAtkMag, a::kAtkFire, a::kAtkThun, a::kAtkStam, a::kAtkDark, a::kAtkThrowEscape }) {
			Set<std::uint16_t>(a_atk, off, 0);
		}
		for (int i = 0; i < 5; ++i) {
			Set<std::int32_t>(a_atk, a::kSpEffectId0 + 4u * static_cast<std::uint32_t>(i), -1);
		}
		Set<float>(a_atk, a::kAtkSuperArmor, 0.0f);
		Set<float>(a_atk, a::kKnockbackDist, 0.0f);
		Set<std::uint8_t>(a_atk, a::kDmgLevel, 0);
		Set<std::uint16_t>(a_atk, a::kAtkObj, a_objAttack);
		auto bits = Get<std::uint8_t>(a_atk, a::kTargetBits);
		bits = a_hitsCharacters ? static_cast<std::uint8_t>(bits | 0x01) : static_cast<std::uint8_t>(bits & ~0x07);
		Set<std::uint8_t>(a_atk, a::kTargetBits, bits);
	}

	bool ObjectOnly(const std::uint8_t* a_atk)
	{
		namespace a = params::atk;
		for (const auto off : { a::kAtkPhys, a::kAtkMag, a::kAtkFire, a::kAtkThun, a::kAtkStam, a::kAtkDark }) {
			if (Get<std::uint16_t>(a_atk, off) != 0) {
				return false;
			}
		}
		for (int i = 0; i < 5; ++i) {
			if (Get<std::int32_t>(a_atk, a::kSpEffectId0 + 4u * static_cast<std::uint32_t>(i)) > 0) {
				return false;
			}
		}
		return Get<float>(a_atk, a::kAtkSuperArmor) == 0.0f && Get<std::uint8_t>(a_atk, a::kDmgLevel) == 0;
	}
}
