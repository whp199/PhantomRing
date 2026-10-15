#pragma once

// Which of the guest's textures may play which part of a material (docs/HOST.md "Material roles").
//
// WHY: MGSV's draws reach us as texture keys by role (t0 diffuse, t1 normal, t2 SRM, t3-t5 the shader's
// other g_tex_* in declaration order). Live, D-Dog's coat and D-Walker next to the camera came out in
// false colours (orange, green, magenta: the colours of MGSV's SRM maps and of its blue/pink layer
// noise) and the cardboard box black, while every recording of the same models decodes cleanly (roles,
// mip chains and contents consistent over 7 recordings). The guest cached "game texture pointer -> key"
// forever while its copy of the texture was released after a minute, so a texture MGSV created later at
// a freed address (a buddy deployed later, a higher-resolution texture streamed in as the camera nears)
// was sent as the OLD texture's key: some other model's SRM, normal map or noise in the diffuse slot.
// The guest is fixed (mgsv/Capture.cpp); this is the host's own guard against any key that can't be
// what its slot says: diffuse, normal map and SRM keep apart (the first one a key is drawn as wins), and
// a diffuse that is a DXT5nm normal map by content is never used as one. With role_guard = on a refusal
// leaves the slot empty (the shader's neutral default: plain instead of wrong; an alpha card then draws
// as an opaque grey quad, since the shader takes the alpha from t0). Not purely additive, so the default
// is log: nothing changes on screen, each texture that would be refused is logged once.
//
// The box poster rule ([Render] poster_materials) may also need a check: its t4 is a poster only on a box
// that carries one (a picture on a plain near-white background); on other boxes from the same shader
// it would be a layer mask, and "replace the cardboard wherever t4 isn't near-white" paints those black
// (a hypothesis: no box recording). The poster itself is VERIFIED live (2026-10-12) while the check's
// threshold is not measured on it, so poster_check defaults to log too: its line gives the real share.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace phantom::er::MaterialRoles
{
	// [Render] role_guard / poster_check: off (the old drawing), log (the old drawing, and what the check would have done is
	// logged once per texture and counted in `render`: the default, purely additive), on (the check acts).
	enum class Mode : std::uint8_t
	{
		kOff,
		kLog,
		kOn,
	};
	// "on"/"true"/"1"/"yes" -> kOn, "log" -> kLog, anything else -> kOff.
	Mode        ModeOf(std::string_view a_value);
	const char* ModeName(Mode a_mode);

	// Role groups (a bit each). Slots 3-5 share one group: the guest numbers them by declaration order,
	// so the same texture may land in different extra slots in different shaders (seen in the recordings).
	inline constexpr std::uint8_t kAlbedo = 1u << 0;
	inline constexpr std::uint8_t kNormal = 1u << 1;
	inline constexpr std::uint8_t kSrm = 1u << 2;
	inline constexpr std::uint8_t kExtra = 1u << 3;

	std::uint8_t GroupOf(unsigned a_slot);
	const char*  GroupName(std::uint8_t a_group);

	// A texture drawn so far in a_seen's groups may now be drawn in a_group: diffuse, normal map and SRM exclude each other;
	// extras neither are refused nor refuse anything (recordings: MGSV's flat normal map is also one shader's t5, extras move
	// between t3-t5, and the guest keys by content, so a generic default may be a diffuse here and an extra there). Symmetric
	// in the extras, so the order the draws come in can't decide (before 2026-10-14 one extra binding barred a diffuse already
	// drawn: flat grey from the next frame on).
	bool Admit(std::uint8_t a_seen, std::uint8_t a_group);

	// What a texture's own blocks say (BC1/BC2/BC3 colour endpoints of the largest mip no bigger than 64
	// texels, so 16x16 blocks at most: cheap enough for the asset worker).
	struct Traits
	{
		bool  analysed{ false };
		float normalShare{ 0.0f };  // blocks whose two colours have red and blue both near 0.5: DXT5nm (x in alpha, y in green)
		float greenLow{ 0.0f };     // ... of them, blocks with green well below their red/blue (y < 0)
		float greenHigh{ 0.0f };    // ... and well above (y > 0)
		float posterShare{ 0.0f };  // blocks whose two colours are both MGSV's poster background (243, 242, 243)
	};
	// a_data: mips of slice 0, mip-major, rows tightly packed (the render ring's RenTexture payload).
	Traits Analyse(std::uint32_t a_dxgiFormat, std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_mips, const std::uint8_t* a_data, std::size_t a_bytes);

	// Recordings (7, every BC1-BC3 texture): every detailed role-1 map has normalShare 1.00 with green on both sides of red/blue
	// (greenLow >= 0.18, greenHigh >= 0.05); every diffuse and SRM normalShare <= 0.14. The green test keeps a mid-grey, sage or
	// muted purple diffuse (red and blue ~0.5 too, but green level with them or off to one side only) from counting. MGSV's
	// flat normal maps (all 0.5 grey) don't count either: drawn as a diffuse they look like the plain default anyway.
	inline bool NormalLike(const Traits& a_t) { return a_t.analysed && a_t.normalShare >= 0.9f && a_t.greenLow >= 0.02f && a_t.greenHigh >= 0.02f; }
	// Recordings: no texture of any role above 0.03 (no box recorded: the real poster's share is UNMEASURED, hence
	// poster_check = log by default, whose line gives it).
	inline bool PosterLike(const Traits& a_t) { return a_t.analysed && a_t.posterShare >= 0.15f; }

	// Counters for `render`, logs once per texture (any thread). a_first: the first time for this texture (the caller's bit in
	// RenderAssets::Texture::noted), the only time it is logged and counted as a texture: a key refused again every frame
	// would otherwise use the whole log budget in a quarter of a second and hide any other.
	inline constexpr std::uint8_t kNotedRole = 1u << 0;
	inline constexpr std::uint8_t kNotedPoster = 1u << 1;
	void        NoteRefused(std::uint64_t a_key, unsigned a_slot, std::uint32_t a_material, std::uint8_t a_seen, bool a_normalInAlbedo, bool a_acted, bool a_first);
	void        NotePoster(std::uint32_t a_material, std::uint64_t a_key, const Traits& a_t, Mode a_mode, bool a_first);
	std::string Describe();
}
