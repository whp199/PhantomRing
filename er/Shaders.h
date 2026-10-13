#pragma once

// HLSL for drawing MGSV's captured meshes, compiled at runtime with D3DCompile (Proton ships Wine's
// d3dcompiler_47, which compiles through vkd3d-shader): one pixel shader, and one vertex shader per
// distinct input layout -- each layout's elements are declared with the shader type their DXGI format
// needs (float for FLOAT/UNORM/SNORM, uint/int for UINT/SINT; D3D12 requires the match), then decoded
// to the canonical position/normal/tangent/uv/weights/indices the shared skinning code uses.
//
// Root signature (built in Renderer.cpp, indices in er/RenderShared.h):
//   b0  root CBV  FrameCB  (anchor, camera, light, output encoding, stage C constants)
//   b1  root constants DrawCB (flags, texture mask, matrix count, material params, owner fade)
//   b2  root CBV  Bones    (this draw's 3x4 matrices, 3 float4 rows each, at most kMaxBones)
//   t0..t5 table  textures by ROLE: albedo, normal, SRM, extra, raw, raw  (+ static sampler s0)
//   t6  table     Elden Ring's scene depth (forward stages)
//   b3  root CBV  PrevBones (the same draw's matrices one Elden Ring frame earlier: velocity)
//   b4  root CBV  ErView   (Elden Ring's own view constants, er/RenderView.h, when FrameCB says so)
//
// Pixel shaders: PSMain (stages A/B: lit here), PSGbuffer (stage C: Elden Ring's G-buffer encoding),
// PSVelocity (stage C: Elden Ring's velocity target). Every vertex shader also outputs the unjittered
// and the previous-frame clip positions.

#include "D3D12Util.h"
#include "phantomring_protocol.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace phantom::er::Shaders
{
	inline constexpr std::uint32_t kMaxBones = 256;
	inline constexpr std::uint32_t kDrawConstants = 16;  // DWORDs of DrawCB (er/RenderShared.h)

	// DrawCB.flags bits (host-side; the DrawRecord flags are mapped onto these)
	enum : std::uint32_t
	{
		kShAlphaTest = 1u << 0,
		kShSkinned = 1u << 1,
		kShSrgbAlbedo = 1u << 2,   // albedo texture holds sRGB values the sampler does not decode
		kShNormalAG = 1u << 3,     // normal map xy in alpha/green (DXT5nm), else red/green
		kShTranslucent = 1u << 4,
		kShCamo = 1u << 5,         // MGSV camo: albedo x camo pattern (t3) where the mask (t4) is set ([Render] camo_materials)
		kShPoster = 1u << 6,       // MGSV's box poster: t4 on the second UV set wherever it isn't its plain background ([Render] poster_materials)
	};

	bool Init();  // loads d3dcompiler_47 (false: logged, renderer off)

	// The canonical name of a guest semantic ("POSITION", ...), or "" if the shaders don't use it.
	std::string_view Canonical(const char* a_semantic);

	// Any HLSL (the overlay's own shaders): null on failure, logged.
	d3d::ComPtr<ID3DBlob> Compile(std::string_view a_source, const char* a_entry, const char* a_target);

	enum class Pixel
	{
		kForward,
		kGbuffer,
		kVelocity,
		kShadow,
	};
	d3d::ComPtr<ID3DBlob> PixelShader(Pixel a_kind = Pixel::kForward);
	d3d::ComPtr<ID3DBlob> VertexShader(std::span<const proto::RenInputElement> a_elements);
	// er/RenderMarkers.h: VSMarker / PSMarker (forward stages' FrameCB, Elden Ring's scene depth test, Encode).
	d3d::ComPtr<ID3DBlob> MarkerShader(bool a_pixel);
	std::string           VertexSource(std::span<const proto::RenInputElement> a_elements);  // for logs/tests
}
