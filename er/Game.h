#pragma once

// Typed access to Elden Ring's runtime: the player and its modules, the camera, the physics world
// (ray casts), the debug drawer, characters around the player, menus, clock. Every pointer chain is
// walked with guarded reads and checked against the verified vtables (er/Addresses.h); anything
// that doesn't check out is "not available this frame", never a crash.
//
// Offsets: docs/research/elden-ring-runtime-re.md §3 (tag in comments: [V] verified in code,
// [V-src] from fromsoftware-rs / the TGA table, [U] unverified -> checked live).

#include "Math.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace phantom::er::game
{
	// ChrIns
	inline constexpr std::uint32_t kChrHandle = 0x8;          // FieldInsHandle (u64) [V-src]
	inline constexpr std::uint32_t kChrSetEntry = 0x10;       // ChrSetEntry*: +8 load_status (2 = active) [V-src]
	inline constexpr std::uint32_t kChrCtrl = 0x58;           // [V]
	inline constexpr std::uint32_t kChrNpcParam = 0x60;       // [V-src]
	inline constexpr std::uint32_t kChrType = 0x68;           // [V-src]
	inline constexpr std::uint32_t kChrTeam = 0x6C;           // u8 [V]
	inline constexpr std::uint32_t kChrLastHitBy = 0x180;     // FieldInsHandle [V-src]
	inline constexpr std::uint32_t kChrModules = 0x190;       // [V]
	inline constexpr std::uint32_t kChrFlags1C4 = 0x1C4;      // bit5 no_gravity [V-src]
	inline constexpr std::uint32_t kChrFlags1C5 = 0x1C5;      // bit7 dead [V], bit3 enable_render [V writes, meaning V-src]
	inline constexpr std::uint32_t kChrBaseTransparency = 0x24C;  // f32 base_transparency (1 opaque .. 0 gone: body, armour AND weapons) [V live]
	inline constexpr std::uint32_t kChrTransparencyMod = 0x250;  // f32 base_transparency_modifier (-1 fade out, +1 in) [V-src]
	inline constexpr std::uint32_t kChrDebugFlags = 0x538;    // u32: bit3 no hit, bit4 no attack, bit5 no move [V-src]
	// ... bit 12 character_disabled (fromsoftware-rs ChrDebugFlags: no updates, no rendering, no physics; "set most of the time
	// on remote characters' horses"): the Fulton's way to take an enemy out of the world without a death (er/Fulton.h) [V-src]
	inline constexpr std::uint32_t kChrDebugDisabled = 0x1000;
	// modules
	inline constexpr std::uint32_t kModData = 0x0;
	inline constexpr std::uint32_t kModPhysics = 0x68;
	inline constexpr std::uint32_t kModLadder = 0x78;         // CS::CSPlayerLadderModule (RTTI checked) [V live]
	inline constexpr std::uint32_t kModRide = 0xE8;           // CS::CSChrRideModule (RTTI checked) [V live]
	inline constexpr std::uint32_t kLadderState = 0x18;       // i8, -1 = not on a ladder (fromsoftware-rs LadderState) [V live: -1 on foot]
	inline constexpr std::uint32_t kRideMounting = 0x162;     // bool is_mounting (fromsoftware-rs) [V-src]
	inline constexpr std::uint32_t kRideMounted = 0x163;      // bool is_mounted (fromsoftware-rs) [V-src; 0 on foot live]
	// CSChrPhysicsModule
	inline constexpr std::uint32_t kPhysOrientation = 0x50;   // hkQuaternion xyzw [V]
	inline constexpr std::uint32_t kPhysPosition = 0x70;      // hkVector4 [V]
	// CSChrDataModule
	inline constexpr std::uint32_t kDataHp = 0x138;           // [V]
	inline constexpr std::uint32_t kDataMaxHp = 0x13C;        // [V]
	inline constexpr std::uint32_t kDataDebug = 0x19B;        // bit0 NoDead, bit1 NoDamage [V-src CT, code tests V]
	// WorldChrMan
	inline constexpr std::uint32_t kWcmMainPlayer = 0x1E508;  // [V]
	inline constexpr std::uint32_t kWcmChrCam = 0x1ECE0;      // [V]
	inline constexpr std::uint32_t kWcmByDistance = 0x1F1D0;  // DLVector<{ChrIns*, f32 dist, u32}> {alloc, first, last, end} [V]
	// CSCamera / CSCam
	inline constexpr std::uint32_t kCameraPersCam1 = 0x08;    // [V]
	inline constexpr std::uint32_t kCamMatrix = 0x10;         // rows right, up, forward, position [V layout]
	inline constexpr std::uint32_t kCamFov = 0x50;            // [V layout, units U]
	inline constexpr std::uint32_t kCamAspect = 0x54;
	inline constexpr std::uint32_t kCamNear = 0x58;
	inline constexpr std::uint32_t kCamFar = 0x5C;
	// CSMenuManImp
	inline constexpr std::uint32_t kMenuUiStates = 0x90;      // u8[0x46] bit0 created, bit1 visible [V-src]
	inline constexpr std::uint32_t kMenuUiStateCount = 0x46;
	inline constexpr std::uint32_t kMenuBlocking = 0x798;     // u64, non-zero while a blocking menu is open [V code, E4]
	// FieldArea / GameRend (compass camera, docs/HOST.md "Compass")
	inline constexpr std::uint32_t kFieldAreaGameRend = 0x20;  // GameRend* [V live: == CSFeMan+0x10 == MoveMapStep+0xE8]
	inline constexpr std::uint32_t kGameRendCompassCam = 0x18; // CSPersCam*: FUN_140B01FC0 passes [[step+0xE8]+0x18] to 0x7774A0 [V code]
	// CSActionButtonManImp
	inline constexpr std::uint32_t kActionPresented = 0x20;   // entry on screen (null = no prompt); entry+0x08 param id [V, E6]
	// RendMan
	inline constexpr std::uint32_t kRendDebugEzDraw = 0x28;   // CSEzDraw* [V-src fsrs]

	struct Player
	{
		std::uint8_t* chr{ nullptr };
		std::uint8_t* modules{ nullptr };
		std::uint8_t* data{ nullptr };
		std::uint8_t* phys{ nullptr };
		std::uint8_t* ctrl{ nullptr };
		[[nodiscard]] explicit operator bool() const { return chr && modules && data && phys && ctrl; }
	};

	struct Character
	{
		std::uint8_t* chr{ nullptr };
		std::uint64_t handle{ 0 };
		bool          isPlayer{ false };
		bool          isEnemy{ false };
		std::uint8_t  team{ 0 };
		std::int32_t  npcParam{ 0 };
		std::int32_t  chrType{ 0 };
		Vec3          pos;
		Quat          orientation;
		std::int32_t  hp{ 0 }, maxHp{ 0 };
		bool          dead{ false };
		bool          disabled{ false };  // ChrIns+0x538 bit 12 (kChrDebugDisabled): out of the world (fultoned)
		float         distance{ 0 };
	};

	std::uint8_t* WorldChrMan();
	Player        MainPlayer();                      // validated (vtables) or empty
	bool          IsActive(const Player& a_player);  // ChrSetEntry load_status 2 or 4 (in the world)
	int           LoadStatus(const Player& a_player);  // ChrSetEntry +8, -1 unreadable
	bool          IsDead(const Player& a_player);
	Vec3          Position(const Player& a_player);
	Quat          Orientation(const Player& a_player);
	std::int32_t  Hp(const Player& a_player);
	std::int32_t  MaxHp(const Player& a_player);
	void          SetHp(const Player& a_player, std::int32_t a_hp);
	std::uint64_t LastHitBy(const Player& a_player);

	bool SetPosition(const Player& a_player, const Vec3& a_pos);  // ChrIns::SetPosition (game warp path)
	bool SetYaw(const Player& a_player, float a_yaw);             // ChrIns::SetYaw
	bool Kill(const Player& a_player);                            // ChrIns::Kill(chr, false)

	// Bits in single bytes/words of the player; return the previous state.
	bool GetBit(std::uint8_t* a_byte, std::uint8_t a_mask);
	bool SetBit(std::uint8_t* a_byte, std::uint8_t a_mask, bool a_on);

	// Physics world ray cast. a_ignore: a ChrIns to skip (the puppet). Must run on a game task.
	bool CanCastRay();
	bool CastRay(const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit, const std::uint8_t* a_ignore, std::uint32_t a_filter);

	// Snake's own ray (docs/HOST.md "Collision filters"): cast_ray's query and collector, built here, with one difference: hits
	// the game's snipe collector drops for their material class (2 or 5: material id / 100) are kept when a_keep has that bit
	// (bit c = class c). Live 2026-10-14: Stormveil's wooden walkways and scaffolds are class 5 -- the Tarnished stands on them,
	// cast_ray never sees them, and Snake fell through. Same filter, same ignored character, same closest hit otherwise.
	// false (nothing cast) when the addresses aren't verified: callers fall back to CastRay.
	// a_layers: the hknp broad-phase layers the query looks in (the query's +0x24 byte, ANDed with each layer's mask byte; cast_ray
	// uses 0xFB). Live 2026-10-14 the Stormveil wooden walkways are in the layer whose mask is 0x04: cast_ray never looks there.
	inline constexpr std::uint8_t kCastRayLayers = 0xFB;
	bool CanCastRayKeep();
	bool CastRayKeep(const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit, const std::uint8_t* a_ignore, std::uint32_t a_filter, std::uint32_t a_keep,
		std::uint8_t a_layers = kCastRayLayers);

	// One hit of CastRayAll: Havok's collision result for it.
	struct RayHit
	{
		Vec3          pos;
		Vec3          normal;
		float         fraction{ 0 };
		std::uint32_t body{ 0 };         // hknpBodyId
		std::uint16_t hkMaterial{ 0 };   // the shape's hknpMaterialId
		std::uint32_t shapeKey{ 0 };
		std::uint32_t shapeFilter{ 0 };  // the shape's collision filter info (bits 0-6 = its layer)
		std::int32_t  material{ -1 };    // Elden Ring's material id (0xC75570; -1 none): id / 100 = the class the collector drops
	};
	// Every hit along the ray that a_filter lets through, nearest first, whatever its material (diagnostics: `call rayall`).
	// Returns how many (at most a_max are written); -1 when it can't cast.
	int CastRayAll(const Vec3& a_from, const Vec3& a_delta, const std::uint8_t* a_ignore, std::uint32_t a_filter, RayHit* a_out, int a_max,
		std::uint8_t a_layers = kCastRayLayers);
	// The broad-phase layers' mask bytes (the world's hknpWideBroadPhase: +0x10 layers, +0x18 count, each layer's +0x19D0), for the log.
	std::string BroadPhaseLayers();

	// Camera: the final render camera (CSCamera.pers_cam_1); nullptr if unavailable.
	std::uint8_t* PersCam1();
	std::uint8_t* ChrCamObject();
	// The camera Elden Ring's HUD compass (and its on-screen markers) is computed from: GameRend's first CSPersCam
	// ([FieldArea+0x20]+0x18; CSFeMan+0x10 holds the same GameRend). ER refreshes it from its own follow camera
	// every frame, before DrawParamUpdate. nullptr if unavailable.
	std::uint8_t* GameRendCam();

	// Debug drawing through RendMan's CSEzDraw (UNVERIFIED that retail renders it).
	std::uint8_t* EzDraw();
	bool          DrawLine(const Vec3& a_from, const Vec3& a_to);
	void          SetDrawColor(float a_r, float a_g, float a_b, float a_a);

	// Up to a_max characters by distance (WorldChrMan's distance-sorted list), including the player. Disabled ones (fultoned:
	// kChrDebugDisabled) only with a_includeDisabled: nothing may shoot, target or mark a character out of the world.
	std::vector<Character> Characters(std::size_t a_max, bool a_includeDisabled = false);
	// A validated ChrIns (any character: vtables of the ChrIns and its data/physics modules, a controller) as a Player, for
	// SetPosition and the like; empty if it isn't one.
	Player ChrAsPlayer(std::uint8_t* a_chr);

	// CSMenuMan UI states (0x46 bytes) or empty.
	std::vector<std::uint8_t> UiStates();

	// re-backlog E4/E6 (code [V], meaning confirmed live). nullopt = manager unavailable this frame.
	std::optional<bool> ModalMenuOpen();     // CSMenuMan+0x798 != 0: a blocking menu owns input
	std::optional<bool> CutscenePlaying();   // [[CSRemo]+8]+0x14C bit0
	std::int32_t        ActionPromptParam(); // ActionButtonParam id of the prompt on screen, -1 = none/unknown

	// Feet position of any validated character (ChrIns*), e.g. a hit's attacker.
	std::optional<Vec3> ChrPosition(const std::uint8_t* a_chr);

	// What the player is on (Tarnished mode: Snake must not take over on Torrent or a ladder): each module is read only once its
	// RTTI name is the expected one; kUnknown when it isn't (logged once by the caller).
	enum class Mounted
	{
		kNone,
		kTorrent,
		kLadder,
		kUnknown
	};
	Mounted PlayerMounted(const Player& a_player);
	// The player's CSPlayerLadderModule state (+0x18, fromsoftware-rs LadderState: -1 not on a ladder, 0 StartBottom, 1 StartTop,
	// 2..5 climbing up/down, 6 EndTop, 7 EndBottom, 8/9 idle on it, ...); nullopt when the module isn't the verified one.
	std::optional<int> LadderState(const Player& a_player);

	// Elden Ring clock (hours, 0..24) or NaN.
	float TimeOfDay();

	// MSVC RTTI name ("CS::PlayerIns") of the object at a_object, or "" (module-local vtables only).
	std::string RttiName(const void* a_object);
	std::string RttiNameOfVtable(const void* a_vtable);
}
