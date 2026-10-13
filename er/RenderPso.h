#pragma once

// Elden Ring's pipeline states, as it creates them: which shaders (by vkd3d-proton's shader hash, the
// name VKD3D_SHADER_DUMP_PATH gives the dumped DXIL/SPIR-V), which render-target/depth formats, and the
// depth-stencil state. The frame probe uses it to name the shaders of each pass (re-backlog E5: what
// writes ER's G-buffer, what lights it), and stage C uses it to recognise the G-buffer's pipelines.
//
// Hooked from the dummy device of er/D3D12Hooks at DLL init, before Elden Ring creates its device (the
// vtables are vkd3d-proton's, shared by every device): ID3D12Device::CreateGraphicsPipelineState,
// ::CreateComputePipelineState, ID3D12Device2::CreatePipelineState (stream) and
// ID3D12PipelineLibrary1::LoadGraphicsPipeline/LoadComputePipeline/LoadPipeline. Each costs one FNV pass
// over the bytecode and a map insert; nothing changes what Elden Ring creates.

#include "D3D12Util.h"

#include <cstdint>
#include <string>

namespace phantom::er::RenderPso
{
	struct Info
	{
		std::uint64_t vs{ 0 }, ps{ 0 }, cs{ 0 };   // vkd3d shader hashes (0 = none)
		std::uint32_t psBytes{ 0 };
		std::uint8_t  numRt{ 0 };
		DXGI_FORMAT   rtv[8]{};
		DXGI_FORMAT   dsv{ DXGI_FORMAT_UNKNOWN };
		bool          depth{ false }, depthWrite{ false }, stencil{ false };
		std::uint8_t  depthFunc{ 0 };
		std::uint8_t  stencilRead{ 0 }, stencilWrite{ 0 };
		std::uint8_t  stencilPass{ 0 }, stencilFunc{ 0 };   // front face D3D12_STENCIL_OP / COMPARISON_FUNC
		std::uint8_t  cull{ 0 };
		bool          depthClip{ true };
		std::int32_t  depthBias{ 0 };
		float         slopeBias{ 0 }, biasClamp{ 0 };
		std::uint8_t  writeMask[8]{};                       // per render target
		bool          blend0{ false };
		void*         root{ nullptr };                      // ID3D12RootSignature* (not owned)
	};

	// From D3D12Hooks::Install with its dummy device (and a pipeline library of it). False: logged.
	bool Install(ID3D12Device* a_dummy);
	// What Elden Ring created this pipeline with; false if we never saw it.
	bool Find(const void* a_pso, Info& a_out);
	// What a pipeline-state stream describes (exposed for the offline tests).
	Info ParseStream(const D3D12_PIPELINE_STATE_STREAM_DESC& a_desc);
	// The depth/raster state Elden Ring's shadow-caster pipelines use most (depth-only, D16), so ours match
	// its depth function and bias. False: none seen.
	bool ShadowState(Info& a_out);
	// vkd3d-proton's shader hash (FNV-1 over the bytecode): the dump file name.
	std::uint64_t ShaderHash(const void* a_code, std::size_t a_size);
	// One line per render-target signature with its pipeline count (`render psos`).
	std::string Describe(bool a_full = false);
	// "5 RT 24/28/29/49/10 ds 20" style text for logs.
	std::string Formats(const Info& a_info);
}
