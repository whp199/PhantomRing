// PhantomRing shared-memory protocol (Elden Ring host plugin <-> MGSV guest plugin <-> Linux tools).
//
// This header is the single source of truth for the byte layout. tools/phantomring/proto.py mirrors
// it for the Linux-side launcher, fake peers and consoles; if you change anything here, change it
// there too and bump kVersion.
//
// Its conventions (this header comment, the ActorTable/ActorRecord layout, the byte rings' head/tail/data
// offsets, the overlay triple buffer) follow SkyCraft's skycraft_protocol.h (MIT, Copyright (c) 2026 chasmlol,
// https://github.com/chasmlol/SkyCraft); see THIRD-PARTY-NOTICES.md.
//
// Transport: one file both games map, normally /dev/shm/phantomring_v1 (Z:\dev\shm\phantomring_v1
// inside Proton). The two games run in different Proton prefixes (different wineservers), so named
// kernel objects (Local\..., events) do NOT reach the other side; a file mapping on the host's
// /dev/shm does, coherently. Nothing blocks on the other side: per-frame data is seqlocked
// latest-value slots, discrete things travel on SPSC rings, and liveness is heartbeats.
//
// All values are little-endian. Both games are Y-up, meters. Unless noted, positions are in the
// writer's own world space (MGSV world for guest-written data, Elden Ring "Havok" space for host-
// written data); the host does all conversions between the two worlds (see docs/DESIGN.md §4).
// Angles are radians; yaw is atan2(forward.x, forward.z) in the writer's world; quaternions are xyzw.
// QPC values are QueryPerformanceCounter ticks (under Proton: CLOCK_MONOTONIC_RAW, 100 ns, the same
// clock in every prefix); qpcFrequency says the unit.
#pragma once

#include <cstddef>
#include <cstdint>

namespace phantom::proto
{
	inline constexpr std::uint32_t kMagic = 0x47524850;  // "PHRG"
	inline constexpr std::uint32_t kVersion = 14;  // v2: GuestState buddy fields (0x90..0xA8); v3: kHEvHeal; v4: kAmmoSmoke;
	                                              // v5: kGEvThrow, kGEvPlaced, kGEvPlacedGone, kHEvDetonate;
	                                              // v6: GuestState aimThrow* (0xA8..0xC4) + kGuestThrowAim; v7: kGEvBuddyOrder;
	                                              // v8: kGuestInBox, kGuestBoxPoster, kGEvFire pad[1] = MGSV damage share;
	                                              // v9: kPlacedAgain (kGEvPlaced pad[0]: an announcement repeated);
	                                              // v10: kGuestNvg, kHEvRested, kHEvTarnished;
	                                              // v11: Header guestProtocol/hostRelease/guestRelease (0xE0..0x100);
	                                              // v12: kGEvFulton;
	                                              // v13: kGuestBinoculars, kGuestDiving, kGEvTimeSkip, kGEvCqc, kAmmoDecoy,
	                                              //      kOwnerProjectile;
	                                              // v14: kGuestJumping
	inline constexpr wchar_t       kDefaultMapping[] = L"file:Z:\\dev\\shm\\phantomring_v1";

	// ---- region offsets ---------------------------------------------------------------------
	inline constexpr std::uint64_t kOffHeader = 0x0;            // Header
	inline constexpr std::uint64_t kOffHostState = 0x100;       // HostState    (ER -> MGSV, seqlock)
	inline constexpr std::uint64_t kOffGuestState = 0x200;      // GuestState   (MGSV -> ER, seqlock)
	inline constexpr std::uint64_t kOffPad = 0x400;             // PadState     (ER -> MGSV, seqlock)
	inline constexpr std::uint64_t kOffPose = 0x1000;           // PoseBlock    (MGSV -> ER, seqlock)
	inline constexpr std::uint64_t kOffGuestEvents = 0x4000;    // EntryRing<GuestEvent, 512> (MGSV -> ER)
	inline constexpr std::uint64_t kOffHostEvents = 0xB000;     // EntryRing<HostEvent, 512>  (ER -> MGSV)
	inline constexpr std::uint64_t kOffActors = 0x12000;        // ActorTable   (ER -> MGSV, seqlock)
	inline constexpr std::uint64_t kOffGuestConsole = 0x18000;  // Console: Lua run inside MGSV (dev tool)
	inline constexpr std::uint64_t kOffHostConsole = 0x1A000;   // Console: commands run inside ER (dev tool)
	inline constexpr std::uint64_t kOffDrawCtl = 0x1C000;       // DrawCtl + 3 DrawSlotHdr (MGSV -> ER)
	inline constexpr std::uint64_t kOffHudCtl = 0x1D000;        // HudCtl + 3 HudSlotHdr  (MGSV -> ER)
	inline constexpr std::uint64_t kOffRenderRing = 0x100000;   // byte ring (MGSV -> ER): meshes, textures
	inline constexpr std::uint64_t kRenderRingBytes = 96ull << 20;
	inline constexpr std::uint64_t kOffDrawSlots = kOffRenderRing + kRenderRingBytes;  // 3 x kDrawSlotBytes
	inline constexpr std::uint64_t kDrawSlotBytes = 4ull << 20;
	inline constexpr std::uint64_t kOffHudSlots = kOffDrawSlots + 3 * kDrawSlotBytes;  // 3 x kHudSlotBytes
	inline constexpr std::uint32_t kMaxHudW = 3840;
	inline constexpr std::uint32_t kMaxHudH = 2160;
	inline constexpr std::uint64_t kHudSlotBytes = std::uint64_t(kMaxHudW) * kMaxHudH * 4;
	inline constexpr std::uint64_t kMappingBytes = kOffHudSlots + 3 * kHudSlotBytes;

	// ---- header @0x0 ------------------------------------------------------------------------
	// The Linux launcher creates and sizes the file (zeroed). The host stamps magic/version and a
	// fresh sessionNonce every time Elden Ring attaches; the guest copies sessionNonce into
	// guestNonce when it (re)attaches and resets its producer sides whenever the two differ.
	// PIDs are *unix* pids: Windows pids collide across Proton prefixes. On real Windows (both games on one
	// kernel, named mapping Local\PhantomRing_v1, docs/WINDOWS.md) they are Windows pids.
	enum class Status : std::uint32_t
	{
		kAbsent = 0,
		kBooting = 1,     // process up, plugin loaded, game not ready yet
		kTitle = 2,       // guest: at MGSV's title/menus, being steered into the sandbox
		kLoading = 3,     // a load is in progress
		kReady = 4,       // in the world, controllable, publishing state
		kFailed = 5,      // gave up; see statusText
		kExiting = 6,
	};

	struct Header
	{
		std::uint32_t magic;
		std::uint32_t version;
		std::uint64_t sessionNonce;       // host: new random value per host attach
		std::uint64_t guestNonce;         // guest: == sessionNonce once attached to this session
		std::uint32_t hostUnixPid;
		std::uint32_t guestUnixPid;
		std::int64_t  qpcFrequency;       // QueryPerformanceFrequency (same clock in both prefixes)
		std::int64_t  hostHeartbeatQpc;   // QPC at the host's last frame
		std::int64_t  guestHeartbeatQpc;  // QPC at the guest's last frame
		Status        hostStatus;
		Status        guestStatus;
		char          hostStatusText[64];   // human-readable, NUL-terminated
		char          guestStatusText[96];  // shown in Elden Ring while MGSV boots
		// v11, kept at these offsets in every later version: who is talking, so that two plugins of different protocol
		// versions (which never attach) can say so in words (common/Version.h MismatchText). Zero = a plugin older than
		// v11. The guest writes its two even while it refuses to attach; the host writes hostRelease on attach.
		std::uint32_t guestProtocol;      // the guest's kVersion
		char          hostRelease[14];    // the VERSION file of each plugin, e.g. "0.2.0-alpha", NUL-padded
		char          guestRelease[14];
	};
	static_assert(sizeof(Header) <= 0x100);
	static_assert(offsetof(Header, guestProtocol) == 0xE0 && offsetof(Header, guestRelease) == 0xF2);

	inline constexpr std::int64_t kHeartbeatTimeoutMs = 3000;

	// ---- host -> guest state @0x100 (seqlock: seq odd while writing) ------------------------------
	enum HostFlags : std::uint32_t
	{
		kHostInGame = 1u << 0,      // a character is loaded and the player exists
		kHostMenuOpen = 1u << 1,    // an Elden Ring menu owns input: guest should see a neutral pad
		kHostLoading = 1u << 2,     // loading screen / area transition
		kHostPuppeting = 1u << 3,   // MGSV drives the Elden Ring player right now
		kHostPlayerDead = 1u << 4,  // the Elden Ring player is dead (death screen / respawn)
		kHostCutscene = 1u << 5,
	};

	struct HostState
	{
		std::uint32_t seq;
		std::uint32_t flags;               // HostFlags
		std::uint64_t frame;
		std::int64_t  qpc;
		float         timeOfDayHours;      // Elden Ring clock, 0..24 (guest may sync MGSV's)
		float         cameraMaxDistance;   // ER-side camera collision: clamp MGSV's boom to this (m); 0 = no clamp
		float         erHpFrac;            // the puppet's HP fraction (informational)
		std::uint32_t erWorldId;           // ER map/block id the player is in
		float         groundY;             // ER ground height under the player (host space), NaN if unknown
		float         fallSpeed;           // m/s downward while the puppet falls (guest may play a fall)
		std::uint8_t  reserved[0x100 - 0x30];
	};
	static_assert(sizeof(HostState) == 0x100);

	// ---- guest -> host state @0x200 (seqlock) ---------------------------------------------------
	enum GuestFlags : std::uint32_t
	{
		kGuestReady = 1u << 0,       // in the sandbox mission, controllable; everything below is valid
		kGuestLoading = 1u << 1,
		kGuestDead = 1u << 2,
		kGuestInMenu = 1u << 3,      // iDroid / pause / any MGSV UI that owns input
		kGuestCutscene = 1u << 4,
		kGuestOnVehicle = 1u << 5,   // riding something (D-Walker, D-Horse, vehicle)
		kGuestAiming = 1u << 6,
		kGuestSprint = 1u << 7,
		kGuestStanding = 1u << 8,
		kGuestCrouching = 1u << 9,
		kGuestProne = 1u << 10,
		kGuestCqc = 1u << 11,        // a CQC action is playing
		kGuestFirstPerson = 1u << 12,
		kGuestThrowAim = 1u << 13,   // (v6) Snake aims a throwable: aimThrowPos/Vel/Kind are this frame's (the host draws the arc)
		kGuestInBox = 1u << 14,      // (v8) Snake is inside a cardboard box (the host hides him from Elden Ring's enemies while he keeps still)
		kGuestBoxPoster = 1u << 15,  // (v8) ...and it is a box with a poster/pin-up on it (MGSV: soldiers who see it are distracted)
		kGuestNvg = 1u << 16,        // (v10) Snake's night-vision goggles are on (the host recreates the effect on Elden Ring's frame)
		kGuestBinoculars = 1u << 17, // (v13) Snake looks through his binoculars (the camera is their view): the host marks the Elden Ring
		                             // characters near the view centre (MGSV-style markers with distance, through walls)
		kGuestDiving = 1u << 18,     // (v13) Snake is in MGSV's dive (X): the host may carry the puppet over short gaps/ledges
		kGuestJumping = 1u << 19,    // (v14) Snake jumps ([Jump] button, MGSV has no jump): set from the press until he lands in MGSV; the host
		                             // gives the puppet an Elden Ring-like jump through its collision on the rising edge
	};

	struct GuestState
	{
		std::uint32_t seq;
		std::uint32_t flags;          // GuestFlags
		std::uint64_t frame;
		std::int64_t  qpc;            // QPC when this state was sampled (MGSV's frame)
		std::uint16_t missionCode;
		std::uint16_t locationCode;
		std::uint32_t odoEpoch;       // bumps when the odometer restarts (guest re-attached)

		// Snake's *intended* motion: the sum of his frame-to-frame displacement in MGSV world, with
		// every guest-made warp (holding him in place, recentring) taken out. The host applies the
		// difference since the last value it consumed to the Elden Ring player (which resolves it
		// against Elden Ring's collision).
		double odo[3];
		// Where Snake (the player root) is in MGSV world right now, and his facing (MGSV yaw, rad).
		float rootPos[3];
		float rootYaw;
		// Root velocity (m/s, MGSV world), smoothed over a few frames.
		float velocity[3];

		// MGSV's player camera: position and orientation (quat xyzw) in MGSV world, vertical FOV (rad).
		float camPos[3];
		float camQuat[4];
		float camFovY;
		float camNear;

		std::uint16_t life, lifeMax;  // vars.playerLife / playerLifeMax
		std::uint16_t stamina;
		std::uint16_t stance;         // PlayerStance (0 stand, 1 squat, 2 crawl)
		std::uint16_t weaponId;       // equipped weapon (MGSV equip id), 0 = none
		std::uint16_t ammoInClip;
		std::uint16_t ammoStock;
		std::uint16_t buddyType;      // vars.buddyType
		std::uint32_t vehicleId;      // vars.playerVehicleGameObjectId (0xFFFF = none)
		std::uint32_t phase;          // vars.playerPhase (alert phase in MGSV)

		// The battle buddy MGSV is running (v2; D-Walker first). The host places it from these until its
		// captured meshes arrive (and snaps it to Elden Ring ground either way), and knows when Snake
		// rides it (then rootPos/odo follow the buddy, see kGuestOnVehicle).
		std::uint32_t buddyFlags;     // GuestBuddyFlags
		std::uint16_t buddyObjectId;  // MGSV GameObjectId of the deployed buddy (0xFFFF = none)
		std::uint16_t buddyAmmo;      // D-Walker main weapon rounds (vars.buddyGearAmmoCount[0])
		float         buddyPos[3];    // feet, MGSV world (valid with kBuddyPosValid)
		float         buddyYaw;       // MGSV yaw (rad)

		// (v6) While kGuestThrowAim: where and how MGSV would release the throwable Snake aims right now -- the same
		// release point and initial velocity its own arc preview starts from and the kGEvThrow of a release now would
		// carry (MGSV world, m and m/s; gravity 9.8 m/s^2 down), and the AmmoKind of its blast.
		float         aimThrowPos[3];
		float         aimThrowVel[3];
		std::uint32_t aimThrowKind;   // AmmoKind
		std::uint8_t  reserved[0x200 - 0xC4];
	};
	static_assert(offsetof(GuestState, buddyFlags) == 0x90);
	static_assert(offsetof(GuestState, aimThrowPos) == 0xA8);
	static_assert(offsetof(GuestState, reserved) == 0xC4);
	static_assert(sizeof(GuestState) == 0x200);

	enum GuestBuddyFlags : std::uint32_t
	{
		kBuddyPresent = 1u << 0,   // the configured buddy is deployed in MGSV's sandbox
		kBuddyPosValid = 1u << 1,  // buddyPos/buddyYaw are this frame's
		kBuddyRidden = 1u << 2,    // Snake is riding it (D-Walker, D-Horse)
		kBuddyLoading = 1u << 3,   // MGSV is loading/calling it
		kBuddyMissing = 1u << 4,   // configured but not available in this save (not developed / dead)
	};

	// ---- pad @0x400 (host -> guest, seqlock) ------------------------------------------------------
	// The real controller as Elden Ring's XInput sees it; the guest returns it from its own
	// XInputGetState (user index 0) so MGSV plays with the player's controller.
	enum PadFlags : std::uint32_t
	{
		kPadConnected = 1u << 0,
		kPadNeutral = 1u << 1,  // host says: give MGSV a neutral pad (ER menu open, ER owns this press)
	};

	struct PadState
	{
		std::uint32_t seq;
		std::uint32_t flags;    // PadFlags
		std::uint32_t packet;   // bumps whenever the state changes (XINPUT_STATE::dwPacketNumber)
		std::uint16_t buttons;  // XINPUT_GAMEPAD wButtons
		std::uint8_t  leftTrigger, rightTrigger;
		std::int16_t  thumbLX, thumbLY, thumbRX, thumbRY;
		std::uint16_t rumbleLow, rumbleHigh;  // guest -> host direction is in GuestEvent; unused here
		std::uint8_t  reserved[0x40 - 0x1C];
	};
	static_assert(sizeof(PadState) == 0x40);

	// ---- pose @0x1000 (guest -> host, seqlock) ---------------------------------------------------
	// MGSV's player animation skeleton, model space (as MGSV publishes it every frame), plus the root
	// transform. Used for debug skeleton drawing and anything ER-side that needs bone positions
	// (feet, hands, muzzle). The full skinned meshes travel through the draw slots.
	inline constexpr std::uint32_t kMaxPoseBones = 256;

	struct PoseBone
	{
		float rot[4];  // quat xyzw, model space
		float pos[4];  // xyz model space, w unused
	};

	struct PoseBlock
	{
		std::uint32_t seq;
		std::uint32_t boneCount;
		std::uint64_t frame;
		float         root[16];          // model -> MGSV world, row-major, translation in [12..14]
		std::int16_t  parent[kMaxPoseBones];  // parent bone index; -1 = root; -2 = hierarchy unknown (draw joints only)
		std::uint8_t  pad[0x60 - 0x50];
		PoseBone      bones[kMaxPoseBones];
	};
	static_assert(offsetof(PoseBlock, bones) == 0x260);
	static_assert(kOffPose + sizeof(PoseBlock) <= kOffGuestEvents);

	// ---- event rings ----------------------------------------------------------------------------
	// Layout as phantom::EntryRing<T, N>: u64 head @0, u64 tail @0x40, T data[N] @0x80.
	inline constexpr std::uint32_t kEventRingEntries = 512;

	enum GuestEventType : std::uint32_t
	{
		kGEvFire = 1,          // a shot left Snake's weapon: a = weaponId | (ammoKind << 16), v0 = muzzle (MGSV world),
		                       // v1 = direction (unit, MGSV world), b = pellets (shotguns) or 1,
		                       // pad[0] = MGSV bulletId (TppEquip.BL_*) | 0x80000000 if explosive, 0 = unknown
		                       // (ammo-counter fallback); the host's damage class comes from it (docs/COMBAT.md);
		                       // (v8) pad[1] = MGSV's lethal damage of one body hit as a share of a standard MGSV soldier's life,
		                       // in 1/10000 (10000 = one hit kills; 0 = unknown: the host falls back to its per-class table)
		kGEvMelee = 2,         // a CQC strike/kick/throw landed point: a = MeleeKind, v0 = center (MGSV world), v1 = facing, b = range (mm),
		                       // pad[0] = MGSV's raw CQC attack id (logged; for mapping throws/chokes live)
		kGEvDied = 3,          // MGSV's life reached zero (the guest keeps Snake alive; the host kills the ER player)
		kGEvRumble = 4,        // a = low motor, b = high motor (0..65535)
		kGEvBuddyFire = 5,     // like kGEvFire, fired by the buddy (D-Walker gatling, etc.); D-Walker's flamethrower: pad[0] = 0
		                       // (no bullet), pad[1] = the stream's reach (mm) -> fire damage along that reach (docs/COMBAT.md)
		kGEvExplosion = 6,     // Snake's thrown/placed explosive (grenade, gas, molotov, C4, claymore, mine): v0 = center (MGSV world),
		                       // b = radius (mm), a = AmmoKind (lethal; tranq = sleep gas; stun = stun grenade/net mine; smoke = no
		                       // damage), pad[0] = MGSV attack id | blast class << 16 | owner << 24 (informational). Gun-fired
		                       // explosive rounds never come this way: they are kGEvFire with the explosive bit (docs/COMBAT.md)
		kGEvLog = 7,           // guest wants the host to show a corner message: a = MessageId
		// v5: thrown explosives and placed ones. MGSV simulates them over its flat airport, which isn't Elden Ring's
		// ground: the host flies its own copy through Elden Ring's collision (arc preview, the grenade in flight, the
		// landing) and puts the blast where ITS copy landed.
		kGEvThrow = 8,         // Snake released a thrown explosive: v0 = release point, v1 = initial velocity (m/s), both MGSV world;
		                       // a = AmmoKind of its blast, b = throw id (non-zero, unique per session; the matching kGEvExplosion
		                       // carries it in pad[1]), pad[0] = MGSV equip id | fuse ms << 16 (0 = explodes on impact)
		kGEvPlaced = 9,        // Snake placed an explosive (claymore, AP/sleep/stun mine, C4): v0 = position (MGSV world),
		                       // v1 = facing (unit, MGSV world; claymores), a = AmmoKind, b = placed id (non-zero, unique per
		                       // session), pad[0] = MGSV equip id (low 16 bits) | kPlacedAgain, pad[1] = trigger radius (mm; 0 =
		                       // remote only: C4)
		kGEvPlacedGone = 10,   // that placed explosive is gone (detonated, picked up, despawned): b = placed id
		// v7: the player gave the buddy an order through MGSV's own call menu (hold LB, right stick, click it). MGSV answers in
		// its own way (a voice line, its HUD's log); the host shows it plainly too, so a player can tell an order was given.
		kGEvBuddyOrder = 11,   // a = MGSV buddy command (20 whistle D-Walker, 21 Normal, 24 Stealth, 25 Search, 26 Intercept Mode,
		                       // others: other buddies; docs/GUEST.md 18.3), b = MGSV buddy type (BuddyType: 4 = D-Walker)
		kGEvFulton = 12,       // (v12) Snake fultoned the MGSV proxy soldier that stood in for an Elden Ring character (ActorProxy):
		                       // pad[0] | pad[1] << 32 = that character's ActorRecord handle, v0 = the proxy's position (MGSV world),
		                       // a = 1 the balloon went up (MGSV's own fulton succeeded), 0 = it failed/was cancelled; the host
		                       // removes that enemy from Elden Ring's world (no death drop) and shows the balloon
		kGEvTimeSkip = 13,     // (v13) MGSV's clock was fast-forwarded (Phantom Cigar): a = MGSV game minutes advanced; the host advances
		                       // Elden Ring's clock by the same game time (WorldAreaTime)
		kGEvCqc = 14,          // (v13) a CQC grab/hold/throw/choke/interrogate/release began: a = CqcAction, v0 = Snake (MGSV world),
		                       // v1 = his facing (unit), b = range (mm) of the grab; pad[0] = MGSV's raw CQC action id; pad[1] = the
		                       // target: a proxy's ER handle low 32 bits if the grab is on a proxy (ActorProxy), else 0
	};

	enum CqcAction : std::uint32_t  // (v13) kGEvCqc a
	{
		kCqcGrab = 0,
		kCqcHold = 1,
		kCqcThrow = 2,
		kCqcChoke = 3,
		kCqcInterrogate = 4,
		kCqcRelease = 5,
	};

	// (v9) kGEvPlaced pad[0] bit: this one was announced before (to this host or an earlier one) and is announced again (the
	// host's puppet restarted, a new host session). v0 is then MGSV's point, NOT where it lies in Elden Ring: a host that knows
	// the id keeps its own place; one that doesn't can't know where it is (it was placed through an anchor of before).
	inline constexpr std::uint32_t kPlacedAgain = 1u << 16;

	enum AmmoKind : std::uint32_t
	{
		kAmmoLethal = 0,
		kAmmoTranq = 1,   // sleep build-up in Elden Ring
		kAmmoStun = 2,    // stagger / poise damage
		kAmmoSmoke = 3,   // (v4) kGEvExplosion only: smoke / flare, no damage
		kAmmoDecoy = 4,   // (v13) kGEvPlaced only: a decoy (enemies who see it stop and look at it; no damage)
	};

	enum MeleeKind : std::uint32_t
	{
		kMeleePunch = 0,
		kMeleeKick = 1,
		kMeleeThrow = 2,     // CQC throw: big poise damage
		kMeleeChoke = 3,
	};

	struct GuestEvent
	{
		std::uint32_t type;
		std::uint32_t a;
		std::uint32_t b;
		std::uint32_t frame;  // low 32 bits of GuestState::frame when it happened
		float         v0[3];
		float         v1[3];
		std::uint32_t pad[2];
	};
	static_assert(sizeof(GuestEvent) == 48);

	enum HostEventType : std::uint32_t
	{
		kHEvHurt = 1,       // an Elden Ring hit landed on the puppet: a = ER damage, b = HurtKind,
		                    // v0 = direction the hit came from (unit, MGSV world)
		kHEvRespawn = 2,    // the ER player respawned (grace/death): refill Snake's life, clear status
		kHEvWarped = 3,     // the ER player was moved by Elden Ring itself (grace travel, elevator, door)
		kHEvStatus = 4,     // an ER status build-up proc'd on the puppet: a = StatusKind
		kHEvHeal = 5,       // (v3) Elden Ring healed the puppet (Crimson Tears, a grace rest, regeneration): a = amount in
		                    // kHEvHurt units (Snake's life / LifeScale: the same share of his life), b = ER HP healed
		kHEvDetonate = 6,   // (v5) an Elden Ring enemy came within a placed explosive's trigger radius: b = placed id; the guest
		                    // sets it off in MGSV (its blast then comes back as kGEvExplosion with pad[1] = placed id)
		kHEvRested = 7,     // (v10) the ER player rested at a grace or respawned (grace, Stake of Marika, death): Elden Ring refills
		                    // flasks then, so the guest refills Snake's ammo (and life) the same way; a = 0 rest, 1 respawn
		kHEvTarnished = 8,  // (v10) "Tarnished mode": a = 1 the player took the Tarnished (Snake parks: neutral pad, no follow
		                    // whistles), a = 0 Snake is back
	};

	enum HurtKind : std::uint32_t
	{
		kHurtMelee = 0,
		kHurtProjectile = 1,
		kHurtMagic = 2,
		kHurtFall = 3,
		kHurtOther = 4,
	};

	struct HostEvent
	{
		std::uint32_t type;
		std::uint32_t a;
		std::uint32_t b;
		std::uint32_t frame;
		float         v0[3];
		float         v1[3];
		std::uint32_t pad[2];
	};
	static_assert(sizeof(HostEvent) == 48);

	inline constexpr std::uint64_t kEventRingBytes = 0x80 + sizeof(GuestEvent) * kEventRingEntries;
	static_assert(kOffGuestEvents + kEventRingBytes <= kOffHostEvents);
	static_assert(kOffHostEvents + kEventRingBytes <= kOffActors);

	// ---- actors @0x12000 (host -> guest, seqlock) -------------------------------------------------
	// Elden Ring characters near the player, in MGSV world (the host maps them through the same
	// anchor it uses for Snake), so the guest can mark them, aim-assist, or place proxies for them.
	inline constexpr std::uint32_t kMaxActors = 256;

	enum ActorFlags : std::uint32_t
	{
		kActorHostile = 1u << 0,
		kActorDead = 1u << 1,
		kActorBoss = 1u << 2,
		kActorInCombat = 1u << 3,
		kActorAsleep = 1u << 4,
	};

	struct ActorRecord
	{
		std::uint64_t handle;    // Elden Ring FieldInsHandle (stable while it exists)
		std::uint32_t flags;     // ActorFlags
		float         pos[3];    // feet, MGSV world
		float         yaw;       // MGSV yaw (rad)
		float         radius;    // m
		float         height;    // m
		float         hpFrac;
		char          name[24];  // UTF-8, NUL-terminated (truncated)
	};
	static_assert(sizeof(ActorRecord) == 64);

	struct ActorTable
	{
		std::uint32_t seq;
		std::uint32_t count;
		std::uint8_t  pad[0x40 - 8];
		ActorRecord   actors[kMaxActors];
	};
	static_assert(kOffActors + sizeof(ActorTable) <= kOffGuestConsole);

	// ---- dev consoles @0x18000 / 0x1A000 ----------------------------------------------------------
	// A request/response mailbox so the Linux tools can run things inside a live game:
	//   guest console: a Lua chunk run on MGSV's Lua thread (return values are tostring'd)
	//   host console:  a small command language inside Elden Ring (peek/poke/call; see er/Console.cpp)
	// Writer of a request: fill `request`, then store reqSeq+1 (release). The game sees reqSeq !=
	// respSeq, runs it, fills `response`, then stores respSeq = reqSeq (release).
	inline constexpr std::uint32_t kConsoleTextBytes = 0xFF0;

	struct Console
	{
		std::uint32_t reqSeq;
		std::uint32_t respSeq;
		std::uint32_t status;  // 0 ok, 1 error (response holds the message)
		std::uint32_t pad;
		char          request[kConsoleTextBytes];
		char          response[0x1000];
	};
	static_assert(sizeof(Console) == 0x2000);

	// ---- draw slots (guest -> host, triple buffer) -------------------------------------------------
	// Every frame the guest publishes what MGSV drew for Snake (and his buddy, and anything else it
	// is told to mirror): one DrawList per frame in the next back slot, then swaps it in (same
	// triple-buffer scheme as SkyCraft's overlay: state bits 0-1 middle index, bit 2 = unread).
	struct DrawCtl
	{
		std::uint32_t state;
		std::uint32_t pad;
		std::uint64_t framesPublished;
	};

	struct DrawSlotHdr
	{
		std::uint64_t frame;
		std::uint32_t bytes;  // used bytes of the slot
		std::uint32_t pad;
	};

	// One captured draw: a mesh (sent once on the render ring) drawn with a set of bone matrices
	// (this frame's, in the slot) and textures (sent once).
	enum DrawFlags : std::uint32_t
	{
		kDrawAlphaTest = 1u << 0,
		kDrawTwoSided = 1u << 1,
		kDrawTranslucent = 1u << 2,
		kDrawRigid = 1u << 3,  // no skinning: matrix[0] is the object -> world transform
	};

	enum DrawOwner : std::uint32_t
	{
		kOwnerPlayer = 0,
		kOwnerBuddy = 1,
		kOwnerVehicle = 2,
		kOwnerOther = 3,
		kOwnerProjectile = 4,  // (v13) a thrown grenade / fired rocket in flight (its own MGSV mesh, drawn at the host's flown copy)
	};

	inline constexpr std::uint32_t kDrawTextures = 6;

	struct DrawRecord
	{
		std::uint64_t mesh;            // RenMesh key
		std::uint64_t textures[kDrawTextures];  // RenTexture keys by PS slot t0..t5 (0 = none)
		std::uint32_t indexCount;
		std::uint32_t startIndex;
		std::int32_t  baseVertex;
		std::uint32_t firstMatrix;     // into the list's matrices
		std::uint32_t matrixCount;
		std::uint32_t flags;           // DrawFlags
		std::uint32_t owner;           // DrawOwner
		std::uint32_t material;        // guest-side material/shader class id (for picking a host shader)
		float         params[8];       // material constants the guest extracted (tint, roughness...)
		std::uint32_t pad[2];
	};
	static_assert(sizeof(DrawRecord) == 0x80);

	struct DrawList
	{
		std::uint64_t frame;
		std::uint32_t drawCount;
		std::uint32_t matrixCount;
		// The anchor these matrices are relative to: MGSV world position + yaw of Snake's root this
		// frame (the host maps anchor -> Elden Ring player position/yaw).
		float         anchorPos[3];
		float         anchorYaw;
		std::uint8_t  pad[0x40 - 0x20];
		// DrawRecord draws[drawCount]; then float matrices[matrixCount][12] (3x4 row-major, MGSV world
		// minus anchorPos, i.e. translation is relative to the anchor).
	};
	static_assert(sizeof(DrawList) == 0x40);

	// ---- render ring (guest -> host) ------------------------------------------------------------
	// Byte ring like SkyCraft's: u64 head @0, u64 tail @0x40, data @0x80; every message starts 8-byte
	// aligned with {u32 type, u32 payloadBytes}; kRenPad means "skip to the start of the ring".
	inline constexpr std::uint64_t kRenRingHeadOff = 0x00;
	inline constexpr std::uint64_t kRenRingTailOff = 0x40;
	inline constexpr std::uint64_t kRenRingDataOff = 0x80;
	inline constexpr std::uint64_t kRenRingDataBytes = kRenderRingBytes - kRenRingDataOff;

	enum RenType : std::uint32_t
	{
		kRenPad = 0,
		kRenClearAll = 1,  // guest (re)attached: drop every mesh/texture
		kRenMesh = 2,      // RenMesh + RenInputElement[elementCount] + vertex bytes + index bytes
		kRenTexture = 3,   // RenTexture + subresource bytes (mip-major, tightly packed rows)
		kRenForget = 4,    // RenForget: a mesh or texture MGSV released
	};

	struct RenMsgHeader
	{
		std::uint32_t type;
		std::uint32_t payloadBytes;
	};

	// D3D11_INPUT_ELEMENT_DESC as captured from MGSV (semantic names shortened to 16 chars).
	struct RenInputElement
	{
		char          semantic[16];
		std::uint32_t semanticIndex;
		std::uint32_t format;        // DXGI_FORMAT
		std::uint32_t inputSlot;
		std::uint32_t alignedByteOffset;
	};
	static_assert(sizeof(RenInputElement) == 32);

	struct RenMesh
	{
		std::uint64_t key;
		std::uint32_t elementCount;
		std::uint32_t vertexStride;   // slot 0 stride (bytes)
		std::uint32_t vertexBytes;
		std::uint32_t indexFormat;    // DXGI_FORMAT_R16_UINT or R32_UINT
		std::uint32_t indexBytes;
		std::uint32_t topology;       // D3D_PRIMITIVE_TOPOLOGY
		std::uint32_t streams;        // number of vertex streams packed back to back (slot order)
		std::uint32_t streamStride[4];
		std::uint32_t streamBytes[4];
		std::uint32_t pad[3];
	};
	static_assert(sizeof(RenMesh) == 0x50);

	struct RenTexture
	{
		std::uint64_t key;
		std::uint32_t format;     // DXGI_FORMAT
		std::uint32_t width, height;
		std::uint32_t mipLevels;
		std::uint32_t arraySize;
		std::uint32_t dataBytes;
	};
	static_assert(sizeof(RenTexture) == 0x20);

	struct RenForget
	{
		std::uint64_t key;
	};

	// ---- HUD overlay (guest -> host, triple buffer; SkyCraft's GUI layer) -------------------------
	struct HudCtl
	{
		std::uint32_t state;
		std::uint32_t pad;
		std::uint64_t framesPublished;
	};

	struct HudSlotHdr
	{
		std::uint32_t width;
		std::uint32_t height;
		std::uint32_t flags;  // bit0: premultiplied alpha
		std::uint32_t pad;
		std::uint64_t frame;
		std::uint8_t  reserved[0x40 - 0x18];
	};
	static_assert(sizeof(HudSlotHdr) == 0x40);
	static_assert(kOffDrawCtl + sizeof(DrawCtl) + 3 * sizeof(DrawSlotHdr) <= kOffHudCtl);
	static_assert(kOffHudCtl + 0x40 + 3 * sizeof(HudSlotHdr) <= kOffRenderRing);
}
