#include "RenderPso.h"

#include "Hook.h"
#include "Log.h"

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <map>
#include <tuple>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace phantom::er::RenderPso
{
	namespace
	{
		using d3d::ComPtr;

		// vtable indices (mingw-w64 d3d12.h, offsetof on the C vtables)
		constexpr std::size_t kDevCreateGraphics = 10;
		constexpr std::size_t kDevCreateCompute = 11;
		constexpr std::size_t kDevCreatePipelineLibrary = 44;
		constexpr std::size_t kDevCreatePipelineState = 47;
		constexpr std::size_t kLibLoadGraphics = 9;
		constexpr std::size_t kLibLoadCompute = 10;
		constexpr std::size_t kLibLoadPipeline = 13;

		using CreateGraphicsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
		using CreateComputeFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
		using CreateStreamFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
		using LoadGraphicsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary*, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
		using LoadComputeFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary*, LPCWSTR, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
		using LoadStreamFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary1*, LPCWSTR, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);

		CreateGraphicsFn g_createGraphics{ nullptr };
		CreateComputeFn  g_createCompute{ nullptr };
		CreateStreamFn   g_createStream{ nullptr };
		LoadGraphicsFn   g_loadGraphics{ nullptr };
		LoadComputeFn    g_loadCompute{ nullptr };
		LoadStreamFn     g_loadStream{ nullptr };

		std::atomic<bool>                        g_installed{ false };
		std::shared_mutex                        g_lock;
		std::unordered_map<const void*, Info>    g_psos;
		std::atomic<std::uint64_t>               g_created{ 0 }, g_viaStream{ 0 }, g_viaLibrary{ 0 }, g_compute{ 0 };

		std::uint64_t Hash(const D3D12_SHADER_BYTECODE& a_b)
		{
			return a_b.pShaderBytecode && a_b.BytecodeLength ? ShaderHash(a_b.pShaderBytecode, a_b.BytecodeLength) : 0;
		}

		void FromDepthStencil(Info& a_i, BOOL a_depth, D3D12_DEPTH_WRITE_MASK a_write, D3D12_COMPARISON_FUNC a_func, BOOL a_stencil, UINT8 a_read, UINT8 a_wmask,
			const D3D12_DEPTH_STENCILOP_DESC& a_front)
		{
			a_i.depth = a_depth;
			a_i.depthWrite = a_write == D3D12_DEPTH_WRITE_MASK_ALL;
			a_i.depthFunc = static_cast<std::uint8_t>(a_func);
			a_i.stencil = a_stencil;
			a_i.stencilRead = a_read;
			a_i.stencilWrite = a_wmask;
			a_i.stencilPass = static_cast<std::uint8_t>(a_front.StencilPassOp);
			a_i.stencilFunc = static_cast<std::uint8_t>(a_front.StencilFunc);
		}

		void FromBlend(Info& a_i, const D3D12_BLEND_DESC& a_b)
		{
			for (int r = 0; r < 8; ++r) {
				const auto& rt = a_b.RenderTarget[a_b.IndependentBlendEnable ? r : 0];
				a_i.writeMask[r] = rt.RenderTargetWriteMask;
			}
			a_i.blend0 = a_b.RenderTarget[0].BlendEnable;
		}

		void Remember(void* a_pso, const Info& a_info)
		{
			std::unique_lock lock(g_lock);
			g_psos[a_pso] = a_info;
		}

		Info FromGraphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& a_d)
		{
			Info i;
			i.vs = Hash(a_d.VS);
			i.ps = Hash(a_d.PS);
			i.psBytes = static_cast<std::uint32_t>(a_d.PS.BytecodeLength);
			i.numRt = static_cast<std::uint8_t>(std::min<UINT>(a_d.NumRenderTargets, 8));
			std::copy(a_d.RTVFormats, a_d.RTVFormats + 8, i.rtv);
			i.dsv = a_d.DSVFormat;
			const auto& ds = a_d.DepthStencilState;
			FromDepthStencil(i, ds.DepthEnable, ds.DepthWriteMask, ds.DepthFunc, ds.StencilEnable, ds.StencilReadMask, ds.StencilWriteMask, ds.FrontFace);
			FromBlend(i, a_d.BlendState);
			i.cull = static_cast<std::uint8_t>(a_d.RasterizerState.CullMode);
			i.depthClip = a_d.RasterizerState.DepthClipEnable;
			i.depthBias = a_d.RasterizerState.DepthBias;
			i.slopeBias = a_d.RasterizerState.SlopeScaledDepthBias;
			i.biasClamp = a_d.RasterizerState.DepthBiasClamp;
			i.root = a_d.pRootSignature;
			return i;
		}

		// D3D12_PIPELINE_STATE_STREAM_DESC: subobjects {type, inner} each aligned to a pointer (d3dx12's
		// CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT). Unknown types end the walk (what we have is kept).
		Info FromStream(const D3D12_PIPELINE_STATE_STREAM_DESC& a_d)
		{
			Info        i;
			const auto* p = static_cast<const std::uint8_t*>(a_d.pPipelineStateSubobjectStream);
			const auto* end = p + a_d.SizeInBytes;
			auto        inner = [](const std::uint8_t* a_at, std::size_t a_align) { return a_at + d3d::Align(sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE), a_align); };
			auto        next = [](const std::uint8_t* a_in, std::size_t a_size) { return a_in + a_size; };
			while (p && p + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= end) {
				const auto     type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(p);
				const uint8_t* in = nullptr;
				std::size_t    size = 0;
				switch (type) {
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
					in = inner(p, 8);
					size = sizeof(void*);
					i.root = *reinterpret_cast<void* const*>(in);
					break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
					in = inner(p, 8);
					size = sizeof(D3D12_SHADER_BYTECODE);
					const auto& b = *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(in);
					if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) {
						i.vs = Hash(b);
					} else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) {
						i.ps = Hash(b);
						i.psBytes = static_cast<std::uint32_t>(b.BytecodeLength);
					} else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) {
						i.cs = Hash(b);
					}
					break;
				}
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
					in = inner(p, 8);
					size = sizeof(D3D12_STREAM_OUTPUT_DESC);
					break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
					in = inner(p, 4);
					size = sizeof(D3D12_BLEND_DESC);
					FromBlend(i, *reinterpret_cast<const D3D12_BLEND_DESC*>(in));
					break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
					in = inner(p, 4);
					size = 4;
					break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
					in = inner(p, 4);
					size = 4;
					i.dsv = *reinterpret_cast<const DXGI_FORMAT*>(in);
					break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
					in = inner(p, 4);
					size = sizeof(D3D12_RASTERIZER_DESC);
					{
						const auto& r = *reinterpret_cast<const D3D12_RASTERIZER_DESC*>(in);
						i.cull = static_cast<std::uint8_t>(r.CullMode);
						i.depthClip = r.DepthClipEnable;
						i.depthBias = r.DepthBias;
						i.slopeBias = r.SlopeScaledDepthBias;
						i.biasClamp = r.DepthBiasClamp;
					}
					break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: {
					in = inner(p, 4);
					size = sizeof(D3D12_DEPTH_STENCIL_DESC);
					const auto& ds = *reinterpret_cast<const D3D12_DEPTH_STENCIL_DESC*>(in);
					FromDepthStencil(i, ds.DepthEnable, ds.DepthWriteMask, ds.DepthFunc, ds.StencilEnable, ds.StencilReadMask, ds.StencilWriteMask, ds.FrontFace);
					break;
				}
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: {
					in = inner(p, 4);
					size = sizeof(D3D12_DEPTH_STENCIL_DESC1);
					const auto& ds = *reinterpret_cast<const D3D12_DEPTH_STENCIL_DESC1*>(in);
					FromDepthStencil(i, ds.DepthEnable, ds.DepthWriteMask, ds.DepthFunc, ds.StencilEnable, ds.StencilReadMask, ds.StencilWriteMask, ds.FrontFace);
					break;
				}
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
					in = inner(p, 8);
					size = sizeof(D3D12_INPUT_LAYOUT_DESC);
					break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
					in = inner(p, 4);
					size = sizeof(D3D12_RT_FORMAT_ARRAY);
					const auto& f = *reinterpret_cast<const D3D12_RT_FORMAT_ARRAY*>(in);
					i.numRt = static_cast<std::uint8_t>(std::min<UINT>(f.NumRenderTargets, 8));
					std::copy(f.RTFormats, f.RTFormats + 8, i.rtv);
					break;
				}
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
					in = inner(p, 4);
					size = sizeof(DXGI_SAMPLE_DESC);
					break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
					in = inner(p, 8);
					size = sizeof(D3D12_CACHED_PIPELINE_STATE);
					break;
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
					in = inner(p, 8);
					size = sizeof(D3D12_VIEW_INSTANCING_DESC);
					break;
				default:
					return i;
				}
				p = reinterpret_cast<const std::uint8_t*>(d3d::Align(reinterpret_cast<std::uintptr_t>(next(in, size)), sizeof(void*)));
			}
			return i;
		}

		HRESULT STDMETHODCALLTYPE CreateGraphicsDetour(ID3D12Device* a_self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* a_desc, REFIID a_riid, void** a_out)
		{
			const HRESULT hr = g_createGraphics(a_self, a_desc, a_riid, a_out);
			if (SUCCEEDED(hr) && a_desc && a_out && *a_out) {
				Remember(*a_out, FromGraphics(*a_desc));
				++g_created;
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE CreateComputeDetour(ID3D12Device* a_self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* a_desc, REFIID a_riid, void** a_out)
		{
			const HRESULT hr = g_createCompute(a_self, a_desc, a_riid, a_out);
			if (SUCCEEDED(hr) && a_desc && a_out && *a_out) {
				Info i;
				i.cs = Hash(a_desc->CS);
				i.root = a_desc->pRootSignature;
				Remember(*a_out, i);
				++g_compute;
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE CreateStreamDetour(ID3D12Device2* a_self, const D3D12_PIPELINE_STATE_STREAM_DESC* a_desc, REFIID a_riid, void** a_out)
		{
			const HRESULT hr = g_createStream(a_self, a_desc, a_riid, a_out);
			if (SUCCEEDED(hr) && a_desc && a_out && *a_out) {
				Remember(*a_out, FromStream(*a_desc));
				++g_viaStream;
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE LoadGraphicsDetour(ID3D12PipelineLibrary* a_self, LPCWSTR a_name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* a_desc, REFIID a_riid, void** a_out)
		{
			const HRESULT hr = g_loadGraphics(a_self, a_name, a_desc, a_riid, a_out);
			if (SUCCEEDED(hr) && a_desc && a_out && *a_out) {
				Remember(*a_out, FromGraphics(*a_desc));
				++g_viaLibrary;
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE LoadComputeDetour(ID3D12PipelineLibrary* a_self, LPCWSTR a_name, const D3D12_COMPUTE_PIPELINE_STATE_DESC* a_desc, REFIID a_riid, void** a_out)
		{
			const HRESULT hr = g_loadCompute(a_self, a_name, a_desc, a_riid, a_out);
			if (SUCCEEDED(hr) && a_desc && a_out && *a_out) {
				Info i;
				i.cs = Hash(a_desc->CS);
				i.root = a_desc->pRootSignature;
				Remember(*a_out, i);
				++g_viaLibrary;
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE LoadStreamDetour(ID3D12PipelineLibrary1* a_self, LPCWSTR a_name, const D3D12_PIPELINE_STATE_STREAM_DESC* a_desc, REFIID a_riid, void** a_out)
		{
			const HRESULT hr = g_loadStream(a_self, a_name, a_desc, a_riid, a_out);
			if (SUCCEEDED(hr) && a_desc && a_out && *a_out) {
				Remember(*a_out, FromStream(*a_desc));
				++g_viaLibrary;
			}
			return hr;
		}

		void* Slot(void* a_object, std::size_t a_index) { return (*reinterpret_cast<void***>(a_object))[a_index]; }
	}

	Info ParseStream(const D3D12_PIPELINE_STATE_STREAM_DESC& a_desc) { return FromStream(a_desc); }

	std::uint64_t ShaderHash(const void* a_code, std::size_t a_size)
	{
		// vkd3d-proton's vkd3d_shader_hash: FNV-1 (multiply, then xor) over every byte.
		std::uint64_t h = 14695981039346656037ull;
		const auto*   b = static_cast<const std::uint8_t*>(a_code);
		for (std::size_t i = 0; i < a_size; ++i) {
			h = (h * 1099511628211ull) ^ b[i];
		}
		return h;
	}

	bool Install(ID3D12Device* a_dummy)
	{
		if (g_installed.load()) {
			return true;
		}
		struct H
		{
			const char* name;
			void*       target;
			void*       detour;
			void**      original;
		};
		std::vector<H> hooks = {
			{ "ID3D12Device::CreateGraphicsPipelineState", Slot(a_dummy, kDevCreateGraphics), reinterpret_cast<void*>(&CreateGraphicsDetour), reinterpret_cast<void**>(&g_createGraphics) },
			{ "ID3D12Device::CreateComputePipelineState", Slot(a_dummy, kDevCreateCompute), reinterpret_cast<void*>(&CreateComputeDetour), reinterpret_cast<void**>(&g_createCompute) },
		};
		ComPtr<ID3D12Device2> dev2;
		if (SUCCEEDED(a_dummy->QueryInterface(__uuidof(ID3D12Device2), reinterpret_cast<void**>(dev2.GetAddressOf())))) {
			hooks.push_back({ "ID3D12Device2::CreatePipelineState", Slot(dev2.Get(), kDevCreatePipelineState), reinterpret_cast<void*>(&CreateStreamDetour),
				reinterpret_cast<void**>(&g_createStream) });
		}
		ComPtr<ID3D12Device1>         dev1;
		ComPtr<ID3D12PipelineLibrary> lib;
		if (SUCCEEDED(a_dummy->QueryInterface(__uuidof(ID3D12Device1), reinterpret_cast<void**>(dev1.GetAddressOf()))) &&
			SUCCEEDED(dev1->CreatePipelineLibrary(nullptr, 0, __uuidof(ID3D12PipelineLibrary), reinterpret_cast<void**>(lib.GetAddressOf())))) {
			hooks.push_back({ "ID3D12PipelineLibrary::LoadGraphicsPipeline", Slot(lib.Get(), kLibLoadGraphics), reinterpret_cast<void*>(&LoadGraphicsDetour),
				reinterpret_cast<void**>(&g_loadGraphics) });
			hooks.push_back({ "ID3D12PipelineLibrary::LoadComputePipeline", Slot(lib.Get(), kLibLoadCompute), reinterpret_cast<void*>(&LoadComputeDetour),
				reinterpret_cast<void**>(&g_loadCompute) });
			ComPtr<ID3D12PipelineLibrary1> lib1;
			if (SUCCEEDED(lib.As(&lib1))) {
				hooks.push_back({ "ID3D12PipelineLibrary1::LoadPipeline", Slot(lib1.Get(), kLibLoadPipeline), reinterpret_cast<void*>(&LoadStreamDetour),
					reinterpret_cast<void**>(&g_loadStream) });
			}
		} else {
			Log::Warn("render: no pipeline library on the dummy device: pipelines Elden Ring loads from one stay unnamed");
		}
		std::vector<void*> made;
		bool               ok = true;
		for (const auto& h : hooks) {
			if (!Hooks::Create(h.name, h.target, h.detour, h.original)) {
				ok = h.original != reinterpret_cast<void**>(&g_createGraphics) && h.original != reinterpret_cast<void**>(&g_createCompute);
				if (!ok) {
					break;
				}
				continue;
			}
			made.push_back(h.target);
		}
		for (void* t : made) {
			if (ok && MH_EnableHook(t) != MH_OK) {
				ok = false;
			}
		}
		if (!ok) {
			for (void* t : made) {
				MH_DisableHook(t);
				MH_RemoveHook(t);
			}
			Log::Error("render: pipeline watch hooks failed: the probe can't name shaders and stage C can't check Elden Ring's G-buffer pipelines");
			return false;
		}
		g_installed = true;
		Log::Info("render: pipeline watch live ({} hooks)", made.size());
		return true;
	}

	bool Find(const void* a_pso, Info& a_out)
	{
		std::shared_lock lock(g_lock);
		const auto       it = g_psos.find(a_pso);
		if (it == g_psos.end()) {
			return false;
		}
		a_out = it->second;
		return true;
	}

	bool ShadowState(Info& a_out)
	{
		std::shared_lock lock(g_lock);
		std::map<std::tuple<int, int, float, float, bool>, std::pair<std::uint32_t, const Info*>> votes;
		for (const auto& [pso, i] : g_psos) {
			if (!i.cs && i.numRt == 0 && i.dsv == DXGI_FORMAT_D16_UNORM && i.depth && i.depthWrite) {
				auto& v = votes[{ i.depthFunc, i.depthBias, i.slopeBias, i.biasClamp, i.depthClip }];
				++v.first;
				v.second = &i;
			}
		}
		const Info* best = nullptr;
		std::uint32_t most = 0;
		for (const auto& [k, v] : votes) {
			if (v.first > most) {
				most = v.first;
				best = v.second;
			}
		}
		if (!best) {
			return false;
		}
		a_out = *best;
		return true;
	}

	std::string Formats(const Info& a_i)
	{
		if (a_i.cs) {
			return "compute";
		}
		std::string s = std::format("{} RT", a_i.numRt);
		for (int r = 0; r < a_i.numRt; ++r) {
			s += std::format("{}{}", r ? "/" : " ", static_cast<int>(a_i.rtv[r]));
		}
		s += std::format(" ds {}", static_cast<int>(a_i.dsv));
		if (a_i.depth) {
			s += std::format(" z{}{}", static_cast<int>(a_i.depthFunc), a_i.depthWrite ? "w" : "");
		}
		if (a_i.depthBias || a_i.slopeBias != 0.0f || !a_i.depthClip) {
			s += std::format(" bias {} slope {} clamp {}{}", a_i.depthBias, a_i.slopeBias, a_i.biasClamp, a_i.depthClip ? "" : " noclip");
		}
		if (a_i.stencil) {
			s += std::format(" st r{:02x} w{:02x} f{} p{}", a_i.stencilRead, a_i.stencilWrite, static_cast<int>(a_i.stencilFunc), static_cast<int>(a_i.stencilPass));
		}
		return s;
	}

	std::string Describe(bool a_full)
	{
		std::shared_lock                     lock(g_lock);
		std::map<std::string, std::uint32_t> sigs;
		for (const auto& [pso, i] : g_psos) {
			if (!i.cs) {
				std::string k = std::format("{} RT", i.numRt);
				for (int r = 0; r < i.numRt; ++r) {
					k += std::format("{}{}", r ? "/" : " ", static_cast<int>(i.rtv[r]));
				}
				k += std::format(" ds {}", static_cast<int>(i.dsv));
				++sigs[k];
			}
		}
		std::string s = std::format("pipelines: hooks {}, {} known ({} graphics created, {} compute, {} via stream, {} from a library), {} target signatures",
			g_installed.load() ? "live" : "off", g_psos.size(), g_created.load(), g_compute.load(), g_viaStream.load(), g_viaLibrary.load(), sigs.size());
		if (a_full) {
			for (const auto& [k, n] : sigs) {
				s += std::format("\n  {:5} x {}", n, k);
			}
		}
		return s;
	}
}
