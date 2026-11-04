#pragma once

// The pure half of er/Noise.h (tests/test_host_noise.inc): which of Snake's shots make noise and how far it carries, and
// when the suppressor on his weapon has just worn out.

#include <cstdint>
#include <string_view>

namespace phantom::er::noise
{
	// How loud a shot is, by what fired it (MGSV's TppEquip.BL_* bullet id: Hg* handguns, Rf* rifles, Rf*Sniper*, 12GA*
	// shotguns, 40mm / 20mm / ms* launchers and missiles, the vehicles' guns).
	enum class WeaponClass : std::uint8_t
	{
		kPistol,
		kRifle,
		kSniper,
		kShotgun,
		kLauncher,
		kFlame,  // D-Walker's flamethrower (kGEvBuddyFire with no bullet)
	};
	WeaponClass      ClassOfShot(std::uint32_t a_bulletId, bool a_explosive, bool a_flame);
	std::string_view Name(WeaponClass a_class);

	// [Noise] radii (m) of the AI sound a shot makes at the muzzle; 0 = none.
	struct Radii
	{
		float pistol{ 20.0f };
		float rifle{ 35.0f };
		float sniper{ 40.0f };
		float shotgun{ 45.0f };
		float launcher{ 55.0f };
		float flame{ 10.0f };
		float suppressed{ 0.0f };  // a shot through a working suppressor (protocol v16 kFireSuppressed): MGSV's own is near silent

		bool operator==(const Radii&) const = default;
	};
	float RadiusOf(WeaponClass a_class, bool a_suppressed, const Radii& a_radii);

	// Elden Ring's AI sound radius is its AiSoundParam row's radius x the request's scale (0x140348960): the scale that makes
	// a row of a_rowRadius carry a_wanted metres (0 when either is not positive).
	float ScaleFor(float a_wanted, float a_rowRadius);

	// "Suppressor worn out": MGSV's suppressor wears with every suppressed shot and comes off when it is spent. The guest's
	// kGuestSuppressorOn (v16) then drops right after a suppressed shot, with the same weapon still in hand (a weapon switch or
	// putting the weapon away drops it too, and is no wear). Review 0.7.0: the weapon is the GuestState's weaponId at the shot
	// (not whatever was last in hand while the flag was on: a switch could match in either order), the drop must hold for
	// confirmMs with that weapon still in hand, and a drop next to MGSV's menu, a climb, a vault, CQC, the box, a vehicle, a death,
	// a load or a cutscene (a_blocked, in this state or the one before) is never wear.
	class SuppressorWatch
	{
	public:
		void OnShot(bool a_suppressed, std::uint64_t a_nowMs);
		// Each guest state: returns true once, when the suppressor just wore out.
		bool Update(bool a_suppressorOn, std::uint32_t a_weapon, bool a_blocked, std::uint64_t a_nowMs);
		void Reset() { *this = {}; }

		std::uint64_t windowMs{ 1000 };  // the flag must drop this soon after the last suppressed shot (live: 20 ms)
		std::uint64_t confirmMs{ 300 };  // ...and stay off this long, the same weapon in hand

	private:
		bool          wasOn_{ false };
		bool          prevBlocked_{ false };
		std::uint32_t weapon_{ 0 };      // GuestState weaponId in the last state
		std::uint32_t shotWeapon_{ 0 };  // ...at the last suppressed shot
		std::uint64_t lastSuppressedMs_{ 0 };
		std::uint64_t pendingSince_{ 0 };  // the flag dropped then: wear, if it holds confirmMs
		std::uint64_t dropAt_{ 0 };        // the flag's last drop (its shot's event may come after it)
		std::uint32_t dropWeapon_{ 0 };
	};
}
