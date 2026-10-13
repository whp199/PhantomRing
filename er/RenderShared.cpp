#include "RenderShared.h"

#include "Config.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numbers>
#include <unordered_map>

namespace phantom::er::render
{
	D3D12_GPU_VIRTUAL_ADDRESS Recording::Upload(const void* a_data, std::uint64_t a_bytes)
	{
		const auto off = up->Take(a_bytes);
		if (off == ~0ull) {
			return 0;
		}
		std::memcpy(uploadPtr + off, a_data, a_bytes);
		return uploadVa + off;
	}

	ID3DBlob* VertexShaderFor(const RenderAssets::Mesh& a_mesh)
	{
		static std::mutex                                                   s_lock;
		static std::unordered_map<std::uint64_t, d3d::ComPtr<ID3DBlob>>   s_vs;
		std::scoped_lock                                                    lock(s_lock);
		auto [it, fresh] = s_vs.try_emplace(a_mesh.layoutHash);
		if (fresh) {
			it->second = Shaders::VertexShader(a_mesh.elements);
		}
		return it->second.Get();
	}

	D3D12_CULL_MODE CullMode(std::uint32_t a_flags)
	{
		const auto& c = Cfg().render.cull;
		if ((a_flags & proto::kDrawTwoSided) || c == "none") {
			return D3D12_CULL_MODE_NONE;
		}
		return c == "front" ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
	}

	bool FrontCcw(bool a_mirrored, int a_winding)
	{
		// The anchor mirror, a flipped projection and an inward-wound mesh each reverse the screen winding.
		const auto& rc = Cfg().render;
		return ((rc.frontCcw != a_mirrored) != rc.flipX) != (a_winding < 0);
	}

	void FillFrame(FrameConstants& a_c, const RenderSnapshot& a_s, UINT a_width, UINT a_height, bool a_gbuffer)
	{
		const auto& rc = Cfg().render;
		auto        set = [](float* a_d, const Vec3& a_v, float a_w) {
            a_d[0] = a_v.x;
            a_d[1] = a_v.y;
            a_d[2] = a_v.z;
            a_d[3] = a_w;
		};
		auto camera = [&](const RenderCamera& a_cam, float* a_r, float* a_u, float* a_f, float* a_p) {
			const float aspect = a_height ? static_cast<float>(a_width) / static_cast<float>(a_height) : (a_cam.aspect > 0 ? a_cam.aspect : 16.0f / 9.0f);
			const float sy = 1.0f / std::tan(a_cam.fovY * 0.5f);
			const float sx = sy / aspect * (rc.flipX ? -1.0f : 1.0f);
			const float nearZ = a_cam.nearZ > 0.005f && a_cam.nearZ < 5.0f ? a_cam.nearZ : 0.1f;
			set(a_r, Normalize(a_cam.right), sx);
			set(a_u, Normalize(a_cam.up), sy);
			set(a_f, Normalize(a_cam.fwd), nearZ);
			set(a_p, a_cam.pos, 1);
		};
		std::memcpy(a_c.anchor, a_s.anchor, sizeof(a_c.anchor));
		camera(a_s.camera, a_c.camRight, a_c.camUp, a_c.camFwd, a_c.camPos);
		if (a_s.havePrev) {
			std::memcpy(a_c.prevAnchor, a_s.prevAnchor, sizeof(a_c.prevAnchor));
			camera(a_s.prevCamera, a_c.prevCamRight, a_c.prevCamUp, a_c.prevCamFwd, a_c.prevCamPos);
		} else {
			std::memcpy(a_c.prevAnchor, a_s.anchor, sizeof(a_c.prevAnchor));
			camera(a_s.camera, a_c.prevCamRight, a_c.prevCamUp, a_c.prevCamFwd, a_c.prevCamPos);
		}
		const float nearZ = a_c.camFwd[3];
		const float farZ = a_s.camera.farZ > nearZ * 2.0f ? a_s.camera.farZ : 10000.0f;
		if (a_gbuffer && rc.gbufferDepth == "finite") {
			// Reversed Z with a far plane: depth = n (f - z) / (z (f - n)) = A + B / z.
			a_c.clip[0] = -nearZ / (farZ - nearZ);
			a_c.clip[1] = nearZ * farZ / (farZ - nearZ);
		} else {
			a_c.clip[0] = 0;
			a_c.clip[1] = nearZ;
		}

		// Stage A/B light (stage C uses Elden Ring's own).
		Vec3  lightDir = Normalize({ rc.lightDir[0], rc.lightDir[1], rc.lightDir[2] });
		Vec3  lightColor{ rc.lightColor[0], rc.lightColor[1], rc.lightColor[2] };
		float intensity = rc.lightIntensity;
		Vec3  ambUp{ rc.ambientUp[0], rc.ambientUp[1], rc.ambientUp[2] };
		Vec3  ambDown{ rc.ambientDown[0], rc.ambientDown[1], rc.ambientDown[2] };
		if (rc.dayNight && a_s.hour >= 0.0f && a_s.hour <= 24.0f) {
			// Sun from 6 h (one horizon) over the top to 18 h; the moon the other half of the day.
			constexpr float kPi = std::numbers::pi_v<float>;
			const float     sunArc = (a_s.hour - 6.0f) / 12.0f * kPi;
			const float     day = std::sin(sunArc);  // > 0 by day
			const float     arc = day >= 0 ? sunArc : sunArc - kPi;
			const Vec3      tilt = Normalize({ lightDir.x, 0, lightDir.z });
			const float     elev = std::max(0.15f, std::abs(std::sin(arc)));
			lightDir = Normalize(Vec3{ std::cos(arc) * 0.8f, 0, 0 } + tilt * 0.6f + Vec3{ 0, elev * 1.5f, 0 });
			const float dayK = std::clamp(day * 2.5f, 0.0f, 1.0f);
			const Vec3  moon{ 0.55f, 0.65f, 1.0f };
			lightColor = lightColor * dayK + moon * (1.0f - dayK);
			intensity = rc.lightIntensity * (0.18f + 0.82f * dayK);
			const float amb = 0.3f + 0.7f * dayK;
			ambUp = (ambUp * dayK + Vec3{ 0.16f, 0.2f, 0.32f } * (1.0f - dayK)) * amb;
			ambDown = (ambDown * dayK + Vec3{ 0.05f, 0.05f, 0.07f } * (1.0f - dayK)) * amb;
		}
		set(a_c.lightDir, lightDir, intensity);
		set(a_c.lightColor, lightColor, rc.exposure);
		set(a_c.ambientUp, ambUp, 0);
		set(a_c.ambientDown, ambDown, 0);
		a_c.output[0] = 0;
		a_c.output[1] = rc.hdrPaperWhite;
		a_c.output[2] = static_cast<float>(rc.debugView);
		a_c.output[3] = a_s.dissolve;

		a_c.gbuf[0] = static_cast<float>(std::clamp(rc.gbufferLightSet >= 0 ? rc.gbufferLightSet : 3, 0, 255)) / 255.0f;  // stage C overrides auto
		a_c.gbuf[1] = rc.gbufferAo;
		a_c.gbuf[2] = rc.gbufferTranslucency;
		a_c.gbuf[3] = rc.albedoScale;
		a_c.mat[0] = rc.glossScale;
		a_c.mat[1] = rc.glossBias;
		a_c.mat[2] = rc.metalScale;
		a_c.mat[3] = rc.alphaRef;
		a_c.fade[2] = rc.velocityScale[0];
		a_c.fade[3] = rc.velocityScale[1];
		a_c.misc[0] = rc.velocityBias[0];
		a_c.misc[1] = rc.velocityBias[1];
		a_c.misc[2] = static_cast<float>(a_s.erFrame & 0xFFFF);
	}

	void PsoBase(D3D12_GRAPHICS_PIPELINE_STATE_DESC& a_pd, std::vector<D3D12_INPUT_ELEMENT_DESC>& a_layout, const RenderAssets::Mesh& a_mesh, ID3D12RootSignature* a_root,
		ID3DBlob* a_vs, ID3DBlob* a_ps, D3D12_CULL_MODE a_cull, bool a_ccw)
	{
		a_layout.clear();
		for (const auto& e : a_mesh.elements) {
			D3D12_INPUT_ELEMENT_DESC d{};
			d.SemanticName = e.semantic;
			d.SemanticIndex = e.semanticIndex;
			d.Format = static_cast<DXGI_FORMAT>(e.format);
			d.InputSlot = e.inputSlot;
			d.AlignedByteOffset = e.alignedByteOffset;
			d.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
			a_layout.push_back(d);
		}
		a_pd = {};
		a_pd.pRootSignature = a_root;
		a_pd.VS = { a_vs->GetBufferPointer(), a_vs->GetBufferSize() };
		if (a_ps) {
			a_pd.PS = { a_ps->GetBufferPointer(), a_ps->GetBufferSize() };
		}
		a_pd.SampleMask = UINT_MAX;
		a_pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		a_pd.RasterizerState.CullMode = a_cull;
		a_pd.RasterizerState.FrontCounterClockwise = a_ccw;
		a_pd.RasterizerState.DepthClipEnable = TRUE;
		a_pd.DepthStencilState.DepthEnable = TRUE;
		a_pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reversed Z
		a_pd.InputLayout = { a_layout.data(), static_cast<UINT>(a_layout.size()) };
		a_pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		a_pd.SampleDesc.Count = 1;
	}

	void Move(std::vector<D3D12_RESOURCE_BARRIER>& a_pre, std::vector<D3D12_RESOURCE_BARRIER>& a_post, ID3D12Resource* a_res, D3D12_RESOURCE_STATES a_from,
		D3D12_RESOURCE_STATES a_to, UINT a_sub)
	{
		if (a_from == a_to) {
			return;
		}
		a_pre.push_back(d3d::Transition(a_res, a_from, a_to, a_sub));
		a_post.push_back(d3d::Transition(a_res, a_to, a_from, a_sub));
	}

	std::uint32_t DrawFlags(const proto::DrawRecord& a_d, const RenderAssets::Mesh& a_mesh, const RenderAssets::Texture* a_albedo, const RenderAssets::Texture* a_normal)
	{
		const auto&   rc = Cfg().render;
		std::uint32_t f = 0;
		if (a_d.flags & proto::kDrawAlphaTest) {
			f |= Shaders::kShAlphaTest;
		}
		// MGSV's hair cards, lashes and decals come with an alpha-carrying diffuse (BC3) but no alpha-test
		// flag from the guest; its opaque materials use BC1. Drawn opaque, the cards cover the head.
		if (a_albedo && rc.alphaTest == "auto") {
			const auto f2 = d3d::Typed(a_albedo->format);
			if (f2 == DXGI_FORMAT_BC2_UNORM || f2 == DXGI_FORMAT_BC3_UNORM || f2 == DXGI_FORMAT_BC2_UNORM_SRGB || f2 == DXGI_FORMAT_BC3_UNORM_SRGB ||
				f2 == DXGI_FORMAT_BC7_UNORM || f2 == DXGI_FORMAT_BC7_UNORM_SRGB) {
				f |= Shaders::kShAlphaTest;
			}
		}
		if (std::find(rc.camoMaterials.begin(), rc.camoMaterials.end(), a_d.material) != rc.camoMaterials.end()) {
			f |= Shaders::kShCamo;
		}
		if (std::find(rc.posterMaterials.begin(), rc.posterMaterials.end(), a_d.material) != rc.posterMaterials.end()) {
			f |= Shaders::kShPoster;
		}
		if (a_d.flags & proto::kDrawTranslucent) {
			f |= Shaders::kShTranslucent;
		}
		if (!(a_d.flags & proto::kDrawRigid) && a_mesh.hasBlendIndices) {
			f |= Shaders::kShSkinned;
		}
		if (a_albedo && rc.albedoSrgb && !d3d::Info(a_albedo->format).srgb) {
			f |= Shaders::kShSrgbAlbedo;
		}
		if (a_normal) {
			const bool bc3 = a_normal->format == DXGI_FORMAT_BC3_UNORM;
			if (rc.normalXy == "ag" || (rc.normalXy == "auto" && bc3)) {
				f |= Shaders::kShNormalAG;
			}
		}
		return f;
	}

	namespace
	{
		void SetDraw(Recording& a_rec, const proto::DrawRecord& a_d, const Recording::Bound& a_b, const Fade& a_fade)
		{
			std::uint32_t rc[kDrawConstants]{};
			rc[0] = a_b.flags;
			rc[1] = a_b.texMask;
			rc[2] = a_b.mats;
			rc[3] = a_d.owner;
			std::memcpy(rc + 4, a_d.params, sizeof(float) * 8);
			std::memcpy(rc + 12, &a_fade.owner, 4);
			std::memcpy(rc + 13, a_fade.range, 8);
			auto* list = a_rec.list;
			list->SetGraphicsRoot32BitConstants(kRootDraw, kDrawConstants, rc, 0);
			list->SetGraphicsRootConstantBufferView(kRootBones, a_b.bones);
			list->SetGraphicsRootConstantBufferView(kRootPrevBones, a_b.prev);
			list->SetGraphicsRootDescriptorTable(kRootTextures, a_rec.Gpu(a_b.texSlot));
		}
	}

	BindResult BindDraw(Recording& a_rec, const RenderSnapshot& a_s, std::size_t a_index, const Fade& a_fade, BoundDraw& a_out)
	{
		const auto& d = a_s.draws[a_index];
		if (a_rec.cacheFor != &a_s) {
			a_rec.cacheFor = &a_s;
			a_rec.cache.assign(a_s.draws.size(), {});
		}
		if (auto& c = a_rec.cache[a_index]; c.mesh) {
			SetDraw(a_rec, d, c, a_fade);
			a_out.mesh = c.mesh;
			a_out.flags = c.flags;
			return BindResult::kOk;
		}
		auto mesh = RenderAssets::FindMesh(d.mesh);
		if (!mesh) {
			return BindResult::kMissingMesh;
		}
		if (d.indexCount == 0 || std::uint64_t(d.startIndex) + d.indexCount > mesh->indexCount) {
			return BindResult::kBad;
		}
		if (a_rec.descUsed + proto::kDrawTextures > a_rec.descLimit) {
			return BindResult::kFull;
		}
		std::shared_ptr<RenderAssets::Texture> tex[proto::kDrawTextures];
		std::uint32_t                          texMask = 0;
		const UINT                             slot = a_rec.descUsed;
		for (UINT r = 0; r < proto::kDrawTextures; ++r) {
			tex[r] = d.textures[r] ? RenderAssets::FindTexture(d.textures[r]) : nullptr;
			a_rec.device->CopyDescriptorsSimple(1, a_rec.Cpu(slot + r), tex[r] ? RenderAssets::Srv(*tex[r]) : a_rec.nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			if (tex[r]) {
				texMask |= 1u << r;
				a_rec.keep->push_back(tex[r]);
			}
		}
		a_rec.descUsed += proto::kDrawTextures;

		const std::uint32_t mats = (d.flags & proto::kDrawRigid) ? 1u : std::min(d.matrixCount, Shaders::kMaxBones);
		const std::size_t   first = std::size_t(d.firstMatrix) * 12;
		const auto          bones = a_rec.Upload(a_s.matrices.data() + first, std::uint64_t(mats) * 48);
		const auto*         prevSrc = a_s.prevMatrices.size() == a_s.matrices.size() ? a_s.prevMatrices.data() : a_s.matrices.data();
		const auto          prev = bones ? a_rec.Upload(prevSrc + first, std::uint64_t(mats) * 48) : 0;
		if (!bones || !prev) {
			return BindResult::kFull;
		}

		auto& c = a_rec.cache[a_index];
		c.mesh = mesh;
		c.bones = bones;
		c.prev = prev;
		c.texSlot = slot;
		c.flags = DrawFlags(d, *mesh, tex[0].get(), tex[1].get());
		c.texMask = texMask;
		c.mats = mats;
		SetDraw(a_rec, d, c, a_fade);
		a_rec.keep->push_back(mesh);
		a_out.mesh = std::move(mesh);
		a_out.flags = c.flags;
		return BindResult::kOk;
	}

	void DrawMesh(ID3D12GraphicsCommandList* a_list, const proto::DrawRecord& a_d, const RenderAssets::Mesh& a_mesh)
	{
		D3D12_VERTEX_BUFFER_VIEW vbv[4]{};
		const auto               base = a_mesh.buffer->GetGPUVirtualAddress();
		for (std::uint32_t s = 0; s < a_mesh.streams; ++s) {
			vbv[s].BufferLocation = base + a_mesh.streamOffset[s];
			vbv[s].SizeInBytes = a_mesh.streamBytes[s];
			vbv[s].StrideInBytes = a_mesh.streamStride[s];
		}
		a_list->IASetVertexBuffers(0, a_mesh.streams, vbv);
		const D3D12_INDEX_BUFFER_VIEW ibv{ base + a_mesh.indexOffset, a_mesh.indexBytes, a_mesh.indexFormat };
		a_list->IASetIndexBuffer(&ibv);
		a_list->IASetPrimitiveTopology(a_mesh.topology);
		a_list->DrawIndexedInstanced(a_d.indexCount, 1, a_d.startIndex, a_d.baseVertex, 0);
	}

	std::vector<Fade> Fades(const RenderSnapshot& a_s)
	{
		const auto&       rc = Cfg().render;
		std::vector<Fade> out(a_s.draws.size());
		// Each owner's nearest part: the translations of its matrices (bone joints, rigid part origins),
		// mapped by the anchor into Elden Ring's world.
		float      nearest[4] = { 1e9f, 1e9f, 1e9f, 1e9f };
		bool       occluding[4] = {};
		const auto& a = a_s.anchor;
		const Vec3  cam = a_s.camera.pos;
		// The line MGSV keeps clear: camera -> Snake's chest (the anchor is at his feet).
		const Vec3  chest{ a[3], a[7] + 1.0f, a[11] };
		const float toSnake = Length(chest - cam);
		const Vec3  dir = toSnake > 1e-3f ? (chest - cam) * (1.0f / toSnake) : Vec3{ 0, 0, 1 };
		auto test = [&](std::uint32_t a_owner, const Vec3& a_p) {
			const Vec3 w{ a[0] * a_p.x + a[1] * a_p.y + a[2] * a_p.z + a[3], a[4] * a_p.x + a[5] * a_p.y + a[6] * a_p.z + a[7],
				a[8] * a_p.x + a[9] * a_p.y + a[10] * a_p.z + a[11] };
			nearest[a_owner] = std::min(nearest[a_owner], Length(w - cam));
			const float along = Dot(w - cam, dir);
			if (along > 0.2f && along < toSnake - 0.3f && Length((w - cam) - dir * along) < rc.occluderRadius) {
				occluding[a_owner] = true;
			}
		};
		for (const auto& d : a_s.draws) {
			const std::uint32_t owner = std::min<std::uint32_t>(d.owner, 3);
			if (owner == proto::kOwnerPlayer) {
				continue;
			}
			// Where the body is: the mesh's bind-space centre carried by each of the draw's matrices (the translations
			// alone are its feet / part origins, ~1 m below the camera line: D-Walker between the camera and Snake
			// was never caught by them, VERIFIED live).
			const auto mesh = RenderAssets::FindMesh(d.mesh);
			const bool centre = mesh && mesh->hasBounds;
			const Vec3 c = centre ? Vec3{ (mesh->boundsMin[0] + mesh->boundsMax[0]) * 0.5f, (mesh->boundsMin[1] + mesh->boundsMax[1]) * 0.5f,
										  (mesh->boundsMin[2] + mesh->boundsMax[2]) * 0.5f }
			                      : Vec3{};
			for (std::uint32_t m = 0; m < d.matrixCount && std::size_t(d.firstMatrix + m) * 12 + 12 <= a_s.matrices.size(); ++m) {
				const float* t = a_s.matrices.data() + std::size_t(d.firstMatrix + m) * 12;
				test(owner, { t[3], t[7], t[11] });
				if (centre) {
					test(owner, { t[0] * c.x + t[1] * c.y + t[2] * c.z + t[3], t[4] * c.x + t[5] * c.y + t[6] * c.z + t[7], t[8] * c.x + t[9] * c.y + t[10] * c.z + t[11] });
				}
			}
		}
		for (std::size_t i = 0; i < a_s.draws.size(); ++i) {
			const std::uint32_t owner = std::min<std::uint32_t>(a_s.draws[i].owner, 3);
			auto&               f = out[i];
			if (owner == proto::kOwnerPlayer) {
				// First person (MGSV's iron sights / scope): his arms and gun right in front of the camera are what the player aims
				// with -- never faded.
				f.range[0] = a_s.firstPerson ? 0.0f : rc.nearFade[0];
				f.range[1] = a_s.firstPerson ? 0.0f : rc.nearFade[1];
			} else {
				f.range[0] = rc.nearFadeOther[0];
				f.range[1] = rc.nearFadeOther[1];
				const float span = rc.nearFadeOther[1] - rc.nearFadeOther[0];
				f.owner = span > 0.0f ? std::clamp((nearest[owner] - rc.nearFadeOther[0]) / span, 0.0f, 1.0f) : 1.0f;
				if (occluding[owner] && !a_s.riding) {
					f.owner = std::min(f.owner, std::clamp(rc.occluderFade, 0.0f, 1.0f));
				}
			}
		}
		return out;
	}
}
