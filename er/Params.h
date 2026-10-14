#pragma once

// Elden Ring's regulation params in memory (SoloParamRepository), for the combat code: find a row by
// id, read or patch a few fields, and put every patched byte back when we're done.
//
// Layout (fromsoftware-rs cs/solo_param_repository.rs + fd4/param_repository.rs [V-src]; struct names,
// row strides and our field offsets checked offline against this regulation.bin with
// tools/re/er_params.py, see docs/COMBAT.md):
//   SoloParamRepositoryImp (+0x80 + 0x48*i): holder {u32 count; ParamResCap* caps[8]}
//   ParamResCap (CS::ParamResCap vtable): FD4ResCap (name = FD4BasicHashString at +0x08), +0x80 FD4ParamResCap*
//   FD4ParamResCap: +0x78 u64 size, +0x80 ParamFile*
//   ParamFile = the PARAM file as on disk: u16 rowCount @0xA, format @0x2D (bit2: 64-bit offsets,
//   bit7: struct name by offset @0x10), 64-bit row descriptors {u32 id, u32, u64 dataOffset, u64 nameOffset}
//   from +0x40.
// Every pointer is read with guarded reads; a table that doesn't check out (struct name, row stride)
// is never used.
//
// Patches are plain data writes into the param rows (no code). They only last while a Patch object
// lives (Restore() or the destructor puts the original bytes back) and are only made while the
// puppet is active: nothing the player can use changes while Elden Ring has the controller.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace phantom::er::params
{
	// ---- pure parsing (tests run it on the real .param files) -------------------------------------
	struct ParsedFile
	{
		std::string   structName;   // "ATK_PARAM_ST", ...
		std::uint16_t rowCount{ 0 };
		bool          is64{ false };
		std::uint32_t rowStride{ 0 };  // smallest distance between two rows' data (== the row size)
		std::vector<std::pair<std::uint32_t, std::uint64_t>> rows;  // {id, data offset from the file start}
	};

	inline constexpr std::size_t kHeaderBytes = 0x40;
	inline constexpr std::size_t kRowDescBytes = 0x18;  // 64-bit offsets (the only format ER uses)

	struct Header
	{
		std::uint16_t rowCount{ 0 };
		bool          nameByOffset{ false };
		std::uint32_t nameOffset{ 0 };   // from the file start (nameByOffset)
		std::string   inlineName;        // !nameByOffset
		[[nodiscard]] std::size_t DescBytes() const { return std::size_t(rowCount) * kRowDescBytes; }
	};

	// The first kHeaderBytes of a ParamFile; nullopt for anything but a little-endian 64-bit PARAM.
	std::optional<Header> ParseHeader(const std::uint8_t* a_header);
	// The row descriptors (kHeaderBytes on, a_header.DescBytes() long) into a_out.rows / rowStride.
	bool ParseRows(const Header& a_header, const std::uint8_t* a_descs, ParsedFile& a_out);
	// A whole file in one buffer (offline: the .param files of regulation.bin).
	std::optional<ParsedFile> Parse(const std::uint8_t* a_bytes, std::size_t a_size);

	// ---- the live tables ------------------------------------------------------------------------
	struct Spec
	{
		const wchar_t* resName;      // L"AtkParam_Pc"
		const char*    structName;   // "ATK_PARAM_ST"
		std::uint32_t  rowSize;      // 0x1C8
		int            holderIndex;  // fromsoftware-rs SoloParam INDEX (checked by name; searched if wrong)
	};

	inline constexpr Spec kAtkParamPc{ L"AtkParam_Pc", "ATK_PARAM_ST", 0x1C8, 8 };
	inline constexpr Spec kBullet{ L"Bullet", "BULLET_PARAM_ST", 0x110, 10 };
	inline constexpr Spec kSpEffect{ L"SpEffectParam", "SP_EFFECT_PARAM_ST", 0x390, 15 };
	// Read only ([Progression] weapon: a weapon's reinforce type and its levels; fromsoftware-rs SoloParam indices 0 and 4).
	inline constexpr Spec kEquipParamWeapon{ L"EquipParamWeapon", "EQUIP_PARAM_WEAPON_ST", 0x298, 0 };
	inline constexpr Spec kReinforceParamWeapon{ L"ReinforceParamWeapon", "REINFORCE_PARAM_WEAPON_ST", 0x80, 4 };
	// Read only (er/Fulton.h: the runes a fultoned enemy would have dropped; fromsoftware-rs SoloParam index 6).
	inline constexpr Spec kNpcParam{ L"NpcParam", "NPC_PARAM_ST", 0x2E0, 6 };
	inline constexpr std::uint32_t kNpcGetSoul = 0x2C;  // u32 get_soul (tools/re/er_params.py fields NPC_PARAM_ST; soldier 30003214: 114)

	class Table
	{
	public:
		explicit Table(const Spec& a_spec) :
			spec_(a_spec) {}
		// Address of row a_id's data, or nullptr (no such row / table unavailable). Binds lazily and
		// re-binds if the game swapped the file (regulation reload).
		std::uint8_t* Row(std::uint32_t a_id);
		[[nodiscard]] bool        Bound() const { return file_ != nullptr; }
		[[nodiscard]] std::string Describe() const;

	private:
		bool Bind();

		Spec                                             spec_;
		std::uint8_t*                                    file_{ nullptr };
		std::uint8_t*                                    resCap_{ nullptr };
		std::unordered_map<std::uint32_t, std::uint64_t> rows_;
		std::string                                      error_{ "not bound yet" };
		std::uint64_t                                    lastTry_{ 0 };
	};

	bool Available();  // SoloParamRepository verified and created

	// ---- field patches with undo -----------------------------------------------------------------
	class Patch
	{
	public:
		Patch() = default;
		Patch(const Patch&) = delete;
		Patch& operator=(const Patch&) = delete;
		~Patch() { Restore(); }

		// Writes a_bytes at a_row + a_offset, remembering the original bytes the first time that
		// location is touched (later writes to it keep the first backup).
		bool Write(std::uint8_t* a_row, std::uint32_t a_offset, const void* a_bytes, std::size_t a_size);
		template <class T>
		bool Set(std::uint8_t* a_row, std::uint32_t a_offset, const T& a_value)
		{
			return Write(a_row, a_offset, &a_value, sizeof(T));
		}
		void Restore();
		[[nodiscard]] bool Empty() const { return saved_.empty(); }

	private:
		struct Saved
		{
			std::uint8_t*             at;
			std::vector<std::uint8_t> bytes;
		};
		std::vector<Saved> saved_;
	};

	// Field offsets used by combat (tools/re/er_params.py fields <STRUCT>; checked by tests/test_host_combat).
	namespace atk
	{
		inline constexpr std::uint32_t kSpEffectId0 = 0x18;          // i32
		inline constexpr std::uint32_t kAtkPhysCorrection = 0x3E;    // u16 (%)
		inline constexpr std::uint32_t kAtkFireCorrection = 0x42;    // u16 (%)
		inline constexpr std::uint32_t kAtkPhys = 0x50;              // u16
		inline constexpr std::uint32_t kAtkFire = 0x54;              // u16
		inline constexpr std::uint32_t kAtkStam = 0x58;              // u16
		inline constexpr std::uint32_t kDmgLevel = 0x72;             // u8
		inline constexpr std::uint32_t kAtkSuperArmor = 0x84;        // f32 (poise damage)
		inline constexpr std::uint32_t kAtkSuperArmorCorrection = 0x1B0;  // f32 (%)
		// The object-only attack (er/ExplosionLogic.h MakeObjectOnlyAttack).
		inline constexpr std::uint32_t kKnockbackDist = 0x10;       // f32
		inline constexpr std::uint32_t kAtkMag = 0x52;              // u16
		inline constexpr std::uint32_t kAtkThun = 0x56;             // u16
		inline constexpr std::uint32_t kAtkThrowEscape = 0x60;      // u16
		inline constexpr std::uint32_t kAtkObj = 0x62;              // u16: damage to destructible map assets (AssetEnvironmentGeometryParam hp / defense)
		inline constexpr std::uint32_t kTargetBits = 0x81;          // u8: bit0 oppose_target, bit1 friendly_target, bit2 self_target
		inline constexpr std::uint32_t kAtkDark = 0x188;            // u16
	}
	namespace bullet
	{
		inline constexpr std::uint32_t kAtkId = 0x0;          // i32 AtkParam row
		inline constexpr std::uint32_t kSfxIdBullet = 0x4;    // i32 flight effect
		inline constexpr std::uint32_t kSpEffectId0 = 0x6C;   // i32 applied on hit
		// The explosion visuals (er/ExplosionLogic.h) copy whole rows and neutralise these.
		inline constexpr std::uint32_t kLife = 0x10;                  // f32 s
		inline constexpr std::uint32_t kHitRadius = 0x44;             // f32 m
		inline constexpr std::uint32_t kHitRadiusMax = 0x48;          // f32 m (grows to this over spreadTime; <= 0: no growth)
		inline constexpr std::uint32_t kSpEffectIdForShooter = 0x60;  // i32 applied to the owner
		inline constexpr std::uint32_t kHitBulletId = 0x68;           // i32 child bullet on hit / expiry
		inline constexpr int           kSpEffectSlots = 5;            // i32 x 5 from kSpEffectId0
		inline constexpr std::uint32_t kIntervalCreateBulletId = 0xAC;  // i32 child bullet spawned every interval
		inline constexpr std::uint32_t kCreateLimitGroupId = 0xC1;    // u8 BulletCreateLimitParam group (0 = none)
		inline constexpr std::uint32_t kSfxIdHit = 0x8;               // i32 effect where it hits
		inline constexpr std::uint32_t kSfxIdFlick = 0xC;             // i32 effect when it is deflected
		inline constexpr std::uint32_t kNumShoot = 0x80;              // u16 bullets per spawn
		inline constexpr std::uint32_t kShootInterval = 0x18;         // f32 s: > 0 = an emitter that keeps shooting (a channelled breath)
		inline constexpr std::uint32_t kInitVelocity = 0x28;          // f32 m/s
		inline constexpr std::uint32_t kMaxVelocity = 0x34;
		inline constexpr std::uint32_t kMinVelocity = 0x38;
		inline constexpr std::uint32_t kSfxDeleteByLifeDead = 0x9F;   // i8: 0 = its flight effect plays on after it dies, 1 = cut with it
		inline constexpr std::uint32_t kSeIdBullet1 = 0xD8;           // i32 sounds: flight, flight, hit, deflection
		inline constexpr std::uint32_t kSeIdBullet2 = 0xDC;
		inline constexpr std::uint32_t kSeIdHit = 0xE0;
		inline constexpr std::uint32_t kSeIdFlick = 0xE4;
	}
	namespace weapon
	{
		inline constexpr std::uint32_t kReinforceTypeId = 0xDA;     // i16 EQUIP_PARAM_WEAPON_ST reinforce_type_id (ReinforceParamWeapon base row)
	}
	namespace speffect
	{
		inline constexpr std::uint32_t kStateInfo = 0x156;          // u16 (436 = sleep)
		inline constexpr std::uint32_t kSleepAttackPower = 0x338;   // i32
		// The damage ladder's carriers ([Combat] damage_route = speffect; combat::DamageLadder).
		inline constexpr std::uint32_t kEffectEndurance = 0x08;     // f32 s (0 = once, as Elden Ring's hemorrhage proc)
		inline constexpr std::uint32_t kMotionInterval = 0x0C;      // f32 s
		inline constexpr std::uint32_t kChangeHpRate = 0x9C;        // f32 % of max HP (positive = damage)
		inline constexpr std::uint32_t kChangeHpPoint = 0xA0;       // i32 HP (positive = damage: 99441 = 10, hemorrhage 1490001 = 100)
		inline constexpr std::uint32_t kReplaceSpEffectId = 0x124;  // i32
		inline constexpr std::uint32_t kCycleSpEffectId = 0x128;    // i32
		inline constexpr std::uint32_t kTargetBits = 0x15F;         // u8 effect_target_self 0, friend 1, enemy 2, player 3, ai 4, live 5, ghost 6
		inline constexpr std::uint32_t kTargetBits2 = 0x16C;        // u8 effect_target_oppose_target 0, friendly_target 1, self_target 2
		inline constexpr std::uint32_t kBits259 = 0x259;            // u8 bit1 is_use_atk_param_atk_power_correct, bit4 is_disable_net_sync
		inline constexpr std::uint32_t kVfxId = 0x170;              // i32
		// The carriers' target bits: those of Elden Ring's sleep build-up rows (25000..), which our hit bullet VERIFIABLY
		// lands on enemies: everyone but the shooter (0x7E), the opposing and friendly target (0x03).
		inline constexpr std::uint8_t kLadderTargetBits = 0x7E;
		inline constexpr std::uint8_t kLadderTargetBits2 = 0x03;
	}
}
