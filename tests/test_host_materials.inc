// Included by test_render.cpp: er/MaterialRoles (texture role groups, content traits) and how BindDraw / DrawFlags use them.
// The thresholds were measured on the real-MGSV recordings (docs/HOST.md "Material roles"); these blocks are synthetic.

#include "MaterialRoles.h"

namespace materials_test
{
	std::uint16_t Pack565(float a_r, float a_g, float a_b)
	{
		auto q = [](float v, int bits) { return static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * float((1 << bits) - 1))); };
		return static_cast<std::uint16_t>(q(a_r, 5) << 11 | q(a_g, 6) << 5 | q(a_b, 5));
	}

	// One BC1 colour block (or the colour half of a BC3 block) with these two endpoints.
	void PutColourBlock(std::vector<std::uint8_t>& a_out, std::uint16_t a_c0, std::uint16_t a_c1, bool a_bc3)
	{
		if (a_bc3) {
			for (int i = 0; i < 8; ++i) {
				a_out.push_back(i < 2 ? 0xFF : 0);  // alpha block: both endpoints 255
			}
		}
		a_out.push_back(static_cast<std::uint8_t>(a_c0 & 0xFF));
		a_out.push_back(static_cast<std::uint8_t>(a_c0 >> 8));
		a_out.push_back(static_cast<std::uint8_t>(a_c1 & 0xFF));
		a_out.push_back(static_cast<std::uint8_t>(a_c1 >> 8));
		for (int i = 0; i < 4; ++i) {
			a_out.push_back(0);
		}
	}

	// A w x h mip whose blocks are "special" (the given endpoints) for the first a_share of them, dark brown otherwise.
	void PutMip(std::vector<std::uint8_t>& a_out, std::uint32_t a_w, std::uint32_t a_h, bool a_bc3, std::uint16_t a_c0, std::uint16_t a_c1, float a_share)
	{
		const std::uint32_t blocks = ((a_w + 3) / 4) * ((a_h + 3) / 4);
		const auto          special = static_cast<std::uint32_t>(std::lround(a_share * float(blocks)));
		for (std::uint32_t k = 0; k < blocks; ++k) {
			if (k < special) {
				PutColourBlock(a_out, a_c0, a_c1, a_bc3);
			} else {
				PutColourBlock(a_out, Pack565(0.30f, 0.22f, 0.12f), Pack565(0.20f, 0.15f, 0.08f), a_bc3);
			}
		}
	}
}

static void TestMaterialRoles()
{
	using namespace materials_test;
	namespace mr = MaterialRoles;
	constexpr std::uint32_t kBc1 = DXGI_FORMAT_BC1_UNORM, kBc3 = DXGI_FORMAT_BC3_UNORM;

	// Role groups: t0 diffuse, t1 normal, t2 SRM, t3-t5 one "extra" group (the guest numbers them by declaration order).
	CHECK(mr::GroupOf(0) == mr::kAlbedo && mr::GroupOf(1) == mr::kNormal && mr::GroupOf(2) == mr::kSrm);
	CHECK(mr::GroupOf(3) == mr::kExtra && mr::GroupOf(4) == mr::kExtra && mr::GroupOf(5) == mr::kExtra);
	CHECK(mr::Admit(0, mr::kAlbedo) && mr::Admit(mr::kAlbedo, mr::kAlbedo) && mr::Admit(mr::kExtra, mr::kExtra));
	CHECK(!mr::Admit(mr::kSrm, mr::kAlbedo) && !mr::Admit(mr::kNormal, mr::kAlbedo) && !mr::Admit(mr::kAlbedo, mr::kSrm));
	CHECK(!mr::Admit(mr::kNormal, mr::kSrm) && !mr::Admit(mr::kSrm, mr::kNormal) && !mr::Admit(mr::kAlbedo | mr::kExtra, mr::kNormal));
	// Extras share: MGSV's flat normal map is also one shader's t5 (fulton.prrec), either order, and never bar a role.
	CHECK(mr::Admit(mr::kNormal, mr::kExtra) && mr::Admit(mr::kExtra, mr::kNormal) && mr::Admit(mr::kNormal | mr::kExtra, mr::kNormal));
	CHECK(mr::Admit(mr::kAlbedo, mr::kExtra) && mr::Admit(mr::kSrm | mr::kExtra, mr::kExtra));
	CHECK(mr::Admit(mr::kExtra, mr::kAlbedo) && mr::Admit(mr::kAlbedo | mr::kExtra, mr::kAlbedo));  // symmetric: an extra never poisons a diffuse
	CHECK(mr::ModeOf("on") == mr::Mode::kOn && mr::ModeOf("true") == mr::Mode::kOn && mr::ModeOf("1") == mr::Mode::kOn && mr::ModeOf("log") == mr::Mode::kLog);
	CHECK(mr::ModeOf("off") == mr::Mode::kOff && mr::ModeOf("false") == mr::Mode::kOff && mr::ModeOf("0") == mr::Mode::kOff && mr::ModeOf("") == mr::Mode::kOff);

	// DXT5nm (BC3, x in alpha, y in green): red and blue ~0.52 on every block, green both sides of them (MGSV's normal maps).
	const auto nrmA = Pack565(0.52f, 0.20f, 0.52f), nrmB = Pack565(0.52f, 0.85f, 0.52f);
	std::vector<std::uint8_t> n;
	PutMip(n, 64, 64, true, nrmA, nrmB, 1.0f);
	auto t = mr::Analyse(kBc3, 64, 64, 1, n.data(), n.size());
	CHECK(t.analysed && Near(t.normalShare, 1.0f) && Near(t.greenLow, 1.0f) && Near(t.greenHigh, 1.0f) && mr::NormalLike(t) && !mr::PosterLike(t));
	// The weakest recorded normal map (e2983fce): green below on 18% of the blocks, above on 7%, the rest flat 0.5.
	{
		std::vector<std::uint8_t> weak;
		const auto                flat = Pack565(0.52f, 0.52f, 0.52f), down = Pack565(0.52f, 0.30f, 0.52f), up = Pack565(0.52f, 0.75f, 0.52f);
		const std::uint32_t       blocks = 16 * 16;
		for (std::uint32_t k = 0; k < blocks; ++k) {
			PutColourBlock(weak, k < 46 ? down : flat, k >= 46 && k < 64 ? up : flat, true);  // 46/256 = 0.18, 18/256 = 0.07
		}
		t = mr::Analyse(kBc3, 64, 64, 1, weak.data(), weak.size());
		CHECK(Near(t.greenLow, 0.18f, 0.01f) && Near(t.greenHigh, 0.07f, 0.01f) && mr::NormalLike(t));
	}
	// A diffuse with a few grey blocks (D-Dog's white coat base had 0.14) is no normal map.
	std::vector<std::uint8_t> a;
	PutMip(a, 64, 64, false, nrmA, nrmB, 0.14f);
	t = mr::Analyse(kBc1, 64, 64, 1, a.data(), a.size());
	CHECK(t.analysed && t.normalShare < 0.2f && !mr::NormalLike(t));
	// Diffuses with red and blue ~0.5 everywhere are no normal map either (review 2026-10-14): mid-grey (green level with them),
	// sage (green above only), muted purple (green below only). The old rule (red/blue alone, >= 60%) took all three.
	for (const auto& [c0, c1] : { std::pair{ Pack565(0.45f, 0.45f, 0.45f), Pack565(0.58f, 0.58f, 0.58f) }, std::pair{ Pack565(0.48f, 0.62f, 0.46f), Pack565(0.55f, 0.70f, 0.52f) },
			 std::pair{ Pack565(0.52f, 0.38f, 0.56f), Pack565(0.56f, 0.42f, 0.60f) } }) {
		std::vector<std::uint8_t> d;
		PutMip(d, 64, 64, false, c0, c1, 1.0f);
		t = mr::Analyse(kBc1, 64, 64, 1, d.data(), d.size());
		CHECK(t.analysed && Near(t.normalShare, 1.0f) && !mr::NormalLike(t));
	}

	// The poster: a picture on (243, 242, 243); a layer mask (black) or cardboard is not one.
	const auto bg = Pack565(0.953f, 0.949f, 0.953f);
	std::vector<std::uint8_t> p;
	PutMip(p, 64, 64, false, bg, bg, 0.6f);
	t = mr::Analyse(kBc1, 64, 64, 1, p.data(), p.size());
	CHECK(mr::PosterLike(t) && Near(t.posterShare, 0.6f, 0.01f));
	std::vector<std::uint8_t> mask;
	PutMip(mask, 64, 64, false, 0, 0, 1.0f);
	t = mr::Analyse(kBc1, 64, 64, 1, mask.data(), mask.size());
	CHECK(t.analysed && !mr::PosterLike(t) && !mr::NormalLike(t));
	// Pure white (a default texture) is a little off the background in green: not a poster either.
	std::vector<std::uint8_t> white;
	PutMip(white, 64, 64, false, Pack565(1, 1, 1), Pack565(1, 1, 1), 1.0f);
	CHECK(!mr::PosterLike(mr::Analyse(kBc1, 64, 64, 1, white.data(), white.size())));

	// It reads the first mip no bigger than 64: a 128x128 normal-coloured mip 0, a plain 64x64 mip 1 -> mip 1 decides.
	std::vector<std::uint8_t> chain;
	PutMip(chain, 128, 128, false, nrmA, nrmB, 1.0f);
	PutMip(chain, 64, 64, false, nrmA, nrmB, 0.0f);
	PutMip(chain, 32, 32, false, nrmA, nrmB, 1.0f);
	t = mr::Analyse(kBc1, 128, 128, 3, chain.data(), chain.size());
	CHECK(t.analysed && Near(t.normalShare, 0.0f));
	// ... or the last one when every mip is bigger (a 256x256 texture with a single mip).
	std::vector<std::uint8_t> big;
	PutMip(big, 256, 256, true, nrmA, nrmB, 1.0f);
	CHECK(mr::NormalLike(mr::Analyse(kBc3, 256, 256, 1, big.data(), big.size())));
	// Short data, unsupported formats: nothing known (never refused for its content).
	CHECK(!mr::Analyse(kBc1, 128, 128, 3, chain.data(), 100).analysed);
	CHECK(!mr::Analyse(DXGI_FORMAT_BC7_UNORM, 64, 64, 1, n.data(), n.size()).analysed);
	CHECK(!mr::Analyse(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, n.data(), n.size()).analysed);
	CHECK(!mr::NormalLike(mr::Traits{}) && !mr::PosterLike(mr::Traits{}));

	// RoleFits (BindDraw): a key keeps the group it was first drawn in; a normal map is never a diffuse.
	auto& rc = Cfg().render;
	const std::string keepGuard = rc.roleGuard, keepPoster = rc.posterCheck;
	CHECK(mr::ModeOf(rc.roleGuard) == mr::Mode::kLog && mr::ModeOf(rc.posterCheck) == mr::Mode::kLog);  // the defaults: purely additive
	rc.roleGuard = "on";
	RenderAssets::Texture srm;
	srm.key = 0x1111;
	CHECK(render::RoleFits(srm, 2, 0xabc) && srm.roles.load() == mr::kSrm);
	CHECK(render::RoleFits(srm, 2, 0xdef));
	CHECK(!render::RoleFits(srm, 0, 0x46f51ad4));  // the stale key in D-Dog's diffuse slot
	CHECK(srm.roles.load() == mr::kSrm);   // a refusal records nothing
	CHECK(srm.noted.load() == mr::kNotedRole);
	// Logged and counted as a texture once, however often it comes back (the binding counter keeps counting).
	auto refusedTextures = [] {
		const auto s = mr::Describe();  // "material guard (...): N textures in a wrong role (...; B bindings) ..."
		return std::stoull(s.substr(s.find("): ") + 3));
	};
	const auto texturesBefore = refusedTextures();
	CHECK(!render::RoleFits(srm, 0, 0x46f51ad4) && !render::RoleFits(srm, 1, 0x46f51ad4));
	CHECK(refusedTextures() == texturesBefore);
	// The layer noise in the extras, then as a diffuse: extras bar nothing (the guest fix covers stale keys there).
	RenderAssets::Texture noise;
	noise.key = 0x2222;
	CHECK(render::RoleFits(noise, 5, 1) && render::RoleFits(noise, 3, 2) && render::RoleFits(noise, 4, 3) && render::RoleFits(noise, 0, 4));
	// Review 2026-10-14: a diffuse that is also an extra elsewhere (a shared or content-identical texture) stays a diffuse.
	RenderAssets::Texture shared;
	shared.key = 0x4444;
	CHECK(render::RoleFits(shared, 0, 7) && render::RoleFits(shared, 3, 8) && render::RoleFits(shared, 0, 7) && render::RoleFits(shared, 5, 9) && render::RoleFits(shared, 0, 7));
	CHECK(shared.noted.load() == 0 && !render::RoleFits(shared, 2, 7));
	RenderAssets::Texture nrm;
	nrm.key = 0x3333;
	nrm.traits = mr::Analyse(kBc3, 64, 64, 1, n.data(), n.size());
	CHECK(!render::RoleFits(nrm, 0, 5) && nrm.roles.load() == 0 && render::RoleFits(nrm, 1, 5));
	// log: the same verdicts, bound anyway (nothing on screen changes), noted once.
	rc.roleGuard = "log";
	RenderAssets::Texture srm2;
	srm2.key = 0x5555;
	CHECK(render::RoleFits(srm2, 2, 1) && render::RoleFits(srm2, 0, 1) && srm2.roles.load() == mr::kSrm && srm2.noted.load() == mr::kNotedRole);
	CHECK(render::RoleFits(nrm, 0, 6));
	rc.roleGuard = "off";
	RenderAssets::Texture srm3;
	srm3.key = 0x6666;
	CHECK(render::RoleFits(srm, 0, 6) && render::RoleFits(nrm, 0, 6) && render::RoleFits(srm3, 2, 1) && render::RoleFits(srm3, 0, 1) && srm3.noted.load() == 0);
	rc.roleGuard = "on";
	CHECK(mr::Describe().find("wrong role") != std::string::npos);

	// DrawFlags: poster_materials paint the poster only where texture 4 is one (on); log and off paint it as before.
	rc.posterCheck = "on";
	proto::DrawRecord d{};
	d.material = 0xc94150c9;
	d.flags = proto::kDrawRigid;
	RenderAssets::Mesh mesh;
	RenderAssets::Texture poster, layerMask;
	poster.traits = mr::Analyse(kBc1, 64, 64, 1, p.data(), p.size());
	layerMask.traits = mr::Analyse(kBc1, 64, 64, 1, mask.data(), mask.size());
	CHECK((render::DrawFlags(d, mesh, nullptr, nullptr, &poster) & Shaders::kShPoster) != 0);
	CHECK((render::DrawFlags(d, mesh, nullptr, nullptr, &layerMask) & Shaders::kShPoster) == 0);
	CHECK((render::DrawFlags(d, mesh, nullptr, nullptr, nullptr) & Shaders::kShPoster) == 0);
	CHECK(poster.noted.load() == mr::kNotedPoster && layerMask.noted.load() == mr::kNotedPoster);  // each noted (logged) once
	rc.posterCheck = "log";  // the default: the old rule on screen (the poster is VERIFIED live), texture 4's share logged
	RenderAssets::Texture layerMask2;
	layerMask2.traits = layerMask.traits;
	CHECK((render::DrawFlags(d, mesh, nullptr, nullptr, &layerMask2) & Shaders::kShPoster) != 0 && layerMask2.noted.load() == mr::kNotedPoster);
	rc.posterCheck = "off";
	RenderAssets::Texture layerMask3;
	CHECK((render::DrawFlags(d, mesh, nullptr, nullptr, &layerMask3) & Shaders::kShPoster) != 0 && layerMask3.noted.load() == 0);  // the old rule
	d.material = 0x12345678;
	CHECK((render::DrawFlags(d, mesh, nullptr, nullptr, &poster) & Shaders::kShPoster) == 0);
	rc.roleGuard = keepGuard;
	rc.posterCheck = keepPoster;
}
