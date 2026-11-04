// er/NoiseLogic: which of Snake's shots make noise in Elden Ring and how far it carries; the suppressor wearing out.
#include "NoiseLogic.h"

static void TestNoise()
{
	using namespace phantom::er::noise;
	// MGSV's bullets by class (tools/re/data/tppequip_bullet_ids_1.0.15.4.txt).
	CHECK(ClassOfShot(10, false, false) == WeaponClass::kPistol);   // BL_HgTranq: the WU S.Pistol
	CHECK(ClassOfShot(5, false, false) == WeaponClass::kPistol);    // BL_Hg9x19mm
	CHECK(ClassOfShot(15, false, false) == WeaponClass::kRifle);    // BL_Rf556x45mm: the SVG-76
	CHECK(ClassOfShot(40, false, false) == WeaponClass::kRifle);    // BL_RfTranq
	CHECK(ClassOfShot(42, false, false) == WeaponClass::kRifle);    // BL_Rf762mmMachinegun
	CHECK(ClassOfShot(0, false, false) == WeaponClass::kRifle);     // unknown (the ammo-counter fallback)
	CHECK(ClassOfShot(37, false, false) == WeaponClass::kSniper);   // BL_Rf762mmSniperMag
	CHECK(ClassOfShot(18, false, false) == WeaponClass::kShotgun);  // BL_12GA00Back
	CHECK(ClassOfShot(27, false, false) == WeaponClass::kLauncher); // BL_40mmGrenade
	CHECK(ClassOfShot(46, false, false) == WeaponClass::kLauncher); // BL_ms02: the FB MR R-LAUNCHER's missile
	CHECK(ClassOfShot(76, false, false) == WeaponClass::kLauncher); // a tank's autocannon
	CHECK(ClassOfShot(5, true, false) == WeaponClass::kLauncher);   // anything explosive
	CHECK(ClassOfShot(0, false, true) == WeaponClass::kFlame);      // D-Walker's flamethrower
	CHECK(Name(WeaponClass::kShotgun) == "shotgun");

	// pistol < rifle < shotgun / launcher; a suppressor makes the shot as quiet as [Noise] suppressed, never louder.
	const Radii r;
	CHECK(RadiusOf(WeaponClass::kPistol, false, r) < RadiusOf(WeaponClass::kRifle, false, r));
	CHECK(RadiusOf(WeaponClass::kRifle, false, r) < RadiusOf(WeaponClass::kShotgun, false, r));
	CHECK(RadiusOf(WeaponClass::kRifle, false, r) < RadiusOf(WeaponClass::kLauncher, false, r));
	CHECK(RadiusOf(WeaponClass::kPistol, true, r) == 0.0f && RadiusOf(WeaponClass::kRifle, true, r) == 0.0f);
	Radii loud = r;
	loud.suppressed = 3.0f;
	CHECK(RadiusOf(WeaponClass::kPistol, true, loud) == 3.0f);
	loud.pistol = 2.0f;
	CHECK(RadiusOf(WeaponClass::kPistol, true, loud) == 2.0f);  // never louder than unsuppressed
	loud.rifle = -5.0f;
	CHECK(RadiusOf(WeaponClass::kRifle, false, loud) == 0.0f);

	// The request's scale: the row's radius x scale = the wanted radius (2010: 12 m).
	CHECK(Near(ScaleFor(20.0f, 12.0f) * 12.0f, 20.0f));
	CHECK(Near(ScaleFor(55.0f, 12.0f) * 12.0f, 55.0f));
	CHECK(ScaleFor(0.0f, 12.0f) == 0.0f && ScaleFor(20.0f, 0.0f) == 0.0f && ScaleFor(NAN, 12.0f) == 0.0f);
	CHECK(ScaleFor(1e6f, 12.0f) == 100.0f);

	// The suppressor wears out: the flag drops right after a suppressed shot and stays off (confirmMs), the same weapon in hand.
	{
		SuppressorWatch w;
		CHECK(!w.Update(true, 0x24, false, 1000));
		w.OnShot(true, 1500);
		CHECK(!w.Update(true, 0x24, false, 1510));
		CHECK(!w.Update(false, 0x24, false, 1530));  // dropped: not yet (it must stay off 300 ms)
		CHECK(!w.Update(false, 0x24, false, 1700));
		CHECK(w.Update(false, 0x24, false, 1840));   // worn out
		CHECK(!w.Update(false, 0x24, false, 1900));  // once
		CHECK(!w.Update(true, 0x24, false, 5000));   // a new one (a rest, an ammo pickup)
		CHECK(!w.Update(false, 0x24, false, 5100));  // dropped with no shot near it: not wear (a menu, MGSV's own reason)
		CHECK(!w.Update(false, 0x24, false, 5600));
	}
	{
		SuppressorWatch w;
		CHECK(!w.Update(true, 0x24, false, 1000));
		w.OnShot(true, 1500);
		CHECK(!w.Update(false, 0x7, false, 1520));  // a weapon switch right after the shot: the rifle has none, nothing wore out
		CHECK(!w.Update(false, 0x7, false, 2000));
	}
	// Review 0.7.0: a switch whose weapon id (vars) changes before the flag (the WeaponState), or after it, is no wear either.
	{
		SuppressorWatch w;
		CHECK(!w.Update(true, 0x24, false, 1000));
		w.OnShot(true, 1500);
		CHECK(!w.Update(true, 0x7, false, 1510));    // the id first, the flag still on
		CHECK(!w.Update(false, 0x7, false, 1530));
		CHECK(!w.Update(false, 0x7, false, 2000));
	}
	{
		SuppressorWatch w;
		CHECK(!w.Update(true, 0x24, false, 1000));
		w.OnShot(true, 1500);
		CHECK(!w.Update(false, 0x24, false, 1510));  // the flag first, the same id: pending ...
		CHECK(!w.Update(false, 0x7, false, 1560));   // ... the id follows within 300 ms: a switch
		CHECK(!w.Update(false, 0x7, false, 2000) && !w.Update(false, 0x24, false, 2100));
	}
	{
		SuppressorWatch w;  // MGSV's menu, a climb, CQC ... around the drop: no wear
		CHECK(!w.Update(true, 0x24, false, 1000));
		w.OnShot(true, 1500);
		CHECK(!w.Update(false, 0x24, true, 1520));
		CHECK(!w.Update(false, 0x24, false, 1600) && !w.Update(false, 0x24, false, 2000));
		CHECK(!w.Update(true, 0x24, false, 3000));
		w.OnShot(true, 3500);
		CHECK(!w.Update(false, 0x24, false, 3520));  // pending ...
		CHECK(!w.Update(false, 0x24, true, 3600));   // ... a menu opens: not wear
		CHECK(!w.Update(false, 0x24, false, 4000));
	}
	{
		SuppressorWatch w;  // the state's drop comes in before the shot's event
		CHECK(!w.Update(true, 0x24, false, 1000));
		CHECK(!w.Update(false, 0x24, false, 1500));
		w.OnShot(true, 1510);
		CHECK(!w.Update(false, 0x24, false, 1600));
		CHECK(w.Update(false, 0x24, false, 1820));
	}
	{
		SuppressorWatch w;
		CHECK(!w.Update(true, 0x24, false, 1000));
		w.OnShot(false, 1500);               // an unsuppressed shot (the flag still on: another weapon's, a race) is no wear
		CHECK(!w.Update(false, 0x24, false, 1520) && !w.Update(false, 0x24, false, 2000));
	}
	{
		SuppressorWatch w;
		CHECK(!w.Update(true, 0x24, false, 1000));
		w.OnShot(true, 1500);
		CHECK(!w.Update(false, 0x24, false, 4000) && !w.Update(false, 0x24, false, 4500));  // too long after the shot
	}

	// A quiet hit (a suppressed dart) never shares the patched row with a loud one: the row's AI sounds differ.
	using phantom::er::combat::HitKey;
	CHECK(HitKey(0, 0, false, 0, 1, 120, true) != HitKey(0, 0, false, 0, 1, 120, false));
	CHECK(HitKey(0, 0, false, 0, 1, 120) == HitKey(0, 0, false, 0, 1, 120, false));
	CHECK(HitKey(0, 0, false, 0, 11, 1023, true) != phantom::er::combat::kUniqueKey);
}
