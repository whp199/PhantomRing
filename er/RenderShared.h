#pragma once

// What the renderer's recorders share (er/Renderer.cpp: stages A/B, er/Gbuffer.cpp: stage C): the
// frame constant block (mirrors Shaders.cpp FrameCB), the per-draw root constants, the per-submission
// upload cursor and descriptor allocation, and the per-draw binding (textures, bones, constants).

#include "D3D12Util.h"
#include "RenderAssets.h"
#include "RenderScene.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace phantom::er::render
{
	// Must match Shaders.cpp FrameCB (float4 rows).
	struct FrameConstants
	{
		float anchor[12];
		float camRight[4], camUp[4], camFwd[4], camPos[4];
		float lightDir[4], lightColor[4], ambientUp[4], ambientDown[4];
		float output[4];
		float depth[4], depth2[4];
		// stage C
		float clip[4];      // x, y: clip z = x * viewZ + y (Elden Ring's reversed Z), zw: NDC jitter
		float prevAnchor[12];
		float prevCamRight[4], prevCamUp[4], prevCamFwd[4], prevCamPos[4];
		float gbuf[4];      // x light-set index / 255, y RT1.w (vertex AO), z RT1.z (translucency), w albedo scale
		float mat[4];       // x gloss scale, y gloss bias, z metal scale, w alpha-test reference
		float fade[4];      // xy unused, z/w velocity scale (NDC delta -> ER units)
		float misc[4];      // x velocity bias, y velocity bias (v), z frame index, w 1 = Elden Ring's view constants (b4)
	};
	static_assert(sizeof(FrameConstants) == 26 * 16);

	inline constexpr std::uint32_t kDrawConstants = 16;  // DWORDs of DrawCB

	struct UploadCursor
	{
		std::uint64_t begin, end, at;
		// 256-aligned bump allocation in this submission's part of the upload ring; ~0 when full.
		std::uint64_t Take(std::uint64_t a_bytes)
		{
			const auto off = d3d::Align(at, 256);
			if (off + a_bytes > end) {
				return ~0ull;
			}
			at = off + a_bytes;
			return off;
		}
	};

	// One recording of ours: the list, the shared root signature/heaps and this submission's slices.
	struct Recording
	{
		ID3D12Device*                       device{ nullptr };
		ID3D12GraphicsCommandList*          list{ nullptr };
		ID3D12RootSignature*                root{ nullptr };
		ID3D12DescriptorHeap*               heap{ nullptr };  // shader-visible CBV/SRV/UAV
		UINT                                srvInc{ 0 };
		UINT                                descBase{ 0 }, descUsed{ 0 }, descLimit{ 0 };
		D3D12_CPU_DESCRIPTOR_HANDLE         nullSrv{};
		std::uint8_t*                       uploadPtr{ nullptr };
		D3D12_GPU_VIRTUAL_ADDRESS           uploadVa{ 0 };
		UploadCursor*                       up{ nullptr };
		std::vector<std::shared_ptr<void>>* keep{ nullptr };
		// What BindDraw already uploaded for a snapshot's draws in this submission (stage C binds the same
		// draws once per shadow view and once for the G-buffer).
		struct Bound
		{
			std::shared_ptr<RenderAssets::Mesh> mesh;
			D3D12_GPU_VIRTUAL_ADDRESS           bones{ 0 }, prev{ 0 };
			UINT                                texSlot{ 0 };
			std::uint32_t                       flags{ 0 }, texMask{ 0 }, mats{ 0 };
		};
		const void*        cacheFor{ nullptr };
		std::vector<Bound> cache;

		D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT a_slot) const
		{
			auto h = heap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += std::size_t(descBase + a_slot) * srvInc;
			return h;
		}
		D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT a_slot) const
		{
			auto h = heap->GetGPUDescriptorHandleForHeapStart();
			h.ptr += std::uint64_t(descBase + a_slot) * srvInc;
			return h;
		}
		// Copies a_bytes to the upload ring; the GPU address, or 0 when this submission's slice is full.
		D3D12_GPU_VIRTUAL_ADDRESS Upload(const void* a_data, std::uint64_t a_bytes);
	};

	// Root signature parameter indices (Renderer.cpp builds it).
	enum RootParam : UINT
	{
		kRootFrame = 0,      // b0 CBV FrameCB
		kRootDraw = 1,       // b1 constants DrawCB
		kRootBones = 2,      // b2 CBV bones
		kRootTextures = 3,   // t0..t5 table
		kRootDepth = 4,      // t6 table (Elden Ring's scene depth, forward path)
		kRootPrevBones = 5,  // b3 CBV previous frame's bones (velocity)
		kRootErView = 6,     // b4 CBV Elden Ring's view constants (er/RenderView.h)
	};

	// One DrawRecord, resolved and bound (textures table, bones, previous bones, root constants) on
	// a_rec.list; the caller sets the pipeline (it needs the mesh's layout) and issues DrawMesh.
	struct BoundDraw
	{
		std::shared_ptr<RenderAssets::Mesh> mesh;
		std::uint32_t                       flags{ 0 };  // Shaders::kSh*
	};
	enum class BindResult
	{
		kOk,
		kMissingMesh,
		kBad,
		kFull,  // upload ring or descriptor slice full: stop this submission's draws
	};
	// Near-camera fade of one draw: a_owner 1 solid .. 0 gone (its owner's nearest part), and the per-pixel
	// distance range (gone at a_range[0] metres from the camera, solid from a_range[1]).
	struct Fade
	{
		float owner{ 1 };
		float range[2]{ 0, 0 };
	};
	BindResult BindDraw(Recording& a_rec, const RenderSnapshot& a_s, std::size_t a_index, const Fade& a_fade, BoundDraw& a_out);
	void       DrawMesh(ID3D12GraphicsCommandList* a_list, const proto::DrawRecord& a_d, const RenderAssets::Mesh& a_mesh);

	// The near-camera fade of each draw: MGSV dither-fades whole objects near its camera; Snake (owner
	// player) only per pixel ([Render] near_fade), everything else (D-Walker...) also as a whole by its
	// nearest part ([Render] near_fade_other).
	std::vector<Fade> Fades(const RenderSnapshot& a_s);
	// The frame constants of a snapshot for a a_width x a_height target. a_gbuffer: stage C (Elden Ring's
	// finite reversed-Z depth mapping, G-buffer and velocity constants), else the forward stages'
	// (infinite far, the stage A light).
	void FillFrame(FrameConstants& a_c, const RenderSnapshot& a_s, UINT a_width, UINT a_height, bool a_gbuffer);
	// The vertex shader for a mesh's input layout (compiled once per layout); null if it failed (logged).
	ID3DBlob* VertexShaderFor(const RenderAssets::Mesh& a_mesh);
	// The rasterizer setup every stage shares: [Render] cull/front_ccw/flip_x, the mirrored anchor.
	D3D12_CULL_MODE CullMode(std::uint32_t a_drawFlags);
	// The rasterizer's FrontCounterClockwise for a mesh: [Render] front_ccw is for meshes wound outward
	// (the fake guest's); MGSV's are wound inward (er/RenderMesh.h) and get the opposite.
	bool            FrontCcw(bool a_mirrored, int a_winding);
	// The pipeline description every stage starts from: our root signature, the mesh's input layout
	// (a_layout keeps the element array alive), VS + PS, culling/winding, reversed-Z depth test, 1 sample.
	void PsoBase(D3D12_GRAPHICS_PIPELINE_STATE_DESC& a_pd, std::vector<D3D12_INPUT_ELEMENT_DESC>& a_layout, const RenderAssets::Mesh& a_mesh, ID3D12RootSignature* a_root,
		ID3DBlob* a_vs, ID3DBlob* a_ps, D3D12_CULL_MODE a_cull, bool a_ccw);
	// a_from -> a_to now (into a_pre) and back afterwards (into a_post); nothing when equal.
	void Move(std::vector<D3D12_RESOURCE_BARRIER>& a_pre, std::vector<D3D12_RESOURCE_BARRIER>& a_post, ID3D12Resource* a_res, D3D12_RESOURCE_STATES a_from,
		D3D12_RESOURCE_STATES a_to, UINT a_sub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	// Shaders::kSh* flags for a draw.
	std::uint32_t DrawFlags(const proto::DrawRecord& a_d, const RenderAssets::Mesh& a_mesh, const RenderAssets::Texture* a_albedo, const RenderAssets::Texture* a_normal);
}
