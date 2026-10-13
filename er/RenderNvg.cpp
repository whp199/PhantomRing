#include "RenderNvg.h"

#include "Config.h"
#include "Log.h"
#include "Shaders.h"

#include <cstring>
#include <format>
#include <unordered_map>

namespace phantom::er::RenderNvg
{
	namespace
	{
		using d3d::ComPtr;

		// b1 = the renderer's 16 root constants (DrawCB's slot); t6 = the forward stages' scene-depth table slot.
		constexpr char kSource[] = R"(
Texture2D tScene : register(t6);
cbuffer NvgCB : register(b1)
{
	uint   nFrame;
	uint   nEncoded;     // 1: the texels hold sRGB-encoded values (decoded here, encoded back)
	float  nStrength;    // 0..1: the goggles fading in/out
	float  nScanlines;   // darkening of every other line pair, 0 = off
	float4 nP0;          // x gain, y floor, z grain, w vignette
	float4 nTint;        // rgb: linear colour of full brightness; w: the curve (luminance ^ w before the gain)
	float4 nSize;        // width, height, 1/width, 1/height
};
float4 VSNvg(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float3 Decode(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 Encode(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float Noise(uint2 p, uint f)
{
	uint h = p.x * 1973u + p.y * 9277u + f * 26699u;
	h = (h ^ (h >> 13)) * 0x5bd1e995u;
	h = h ^ (h >> 15);
	return float(h & 0xFFFFu) / 65535.0 - 0.5;
}
float4 PSNvg(float4 pos : SV_Position) : SV_Target
{
	float3 src = tScene.Load(int3(int2(pos.xy), 0)).rgb;
	float3 lin = nEncoded ? Decode(saturate(src)) : src;
	float  L = dot(lin, float3(0.2126, 0.7152, 0.0722));
	L = pow(max(L, 0.0), nTint.w > 0.0 ? nTint.w : 1.0);  // < 1 lifts the dark more than the light (a tube's auto-gain)
	float  v = 1.0 - exp(-L * nP0.x);              // the tube amplifies the dark, highlights roll off softly
	v = nP0.y + (1.0 - nP0.y) * v;
	v += Noise(uint2(pos.xy), nFrame) * nP0.z * (0.5 + 0.5 * (1.0 - v));  // grain, strongest in the dark
	float2 uv = pos.xy * nSize.zw;
	float2 d = (uv - 0.5) * float2(nSize.x * nSize.w, 1.0);
	float  r = length(d) / length(float2(0.5 * nSize.x * nSize.w, 0.5));  // 0 centre .. 1 corner
	v *= 1.0 - nP0.w * smoothstep(0.45, 1.0, r);
	if (nScanlines > 0.0) {
		v *= 1.0 - nScanlines * step(0.5, frac(pos.y * nSize.w * 540.0));
	}
	float3 nvg = nTint.rgb * saturate(v);
	float3 o = lerp(lin, nvg, saturate(nStrength));
	return float4(nEncoded ? Encode(saturate(o)) : o, 1.0);
}
)";

		bool                                                  g_failed{ false };
		ComPtr<ID3DBlob>                                      g_vs, g_ps;
		ComPtr<ID3D12Resource>                                g_copy;
		D3D12_RESOURCE_DESC                                   g_copyDesc{};
		std::unordered_map<int, ComPtr<ID3D12PipelineState>>  g_psos;
		std::uint64_t                                         g_frames{ 0 };

		void Fail(std::string_view a_why)
		{
			if (!g_failed) {
				g_failed = true;
				Log::Error("render: night vision off for this session: {} (Elden Ring's frame is left alone)", a_why);
			}
		}

		ID3D12PipelineState* Pso(ID3D12Device* a_device, ID3D12RootSignature* a_root, DXGI_FORMAT a_format)
		{
			if (auto it = g_psos.find(static_cast<int>(a_format)); it != g_psos.end()) {
				return it->second.Get();
			}
			if (!g_vs) {
				g_vs = Shaders::Compile(kSource, "VSNvg", "vs_5_0");
				g_ps = Shaders::Compile(kSource, "PSNvg", "ps_5_0");
				if (!g_vs || !g_ps) {
					Fail("its shaders did not compile");
					return nullptr;
				}
			}
			D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
			pd.pRootSignature = a_root;
			pd.VS = { g_vs->GetBufferPointer(), g_vs->GetBufferSize() };
			pd.PS = { g_ps->GetBufferPointer(), g_ps->GetBufferSize() };
			pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			pd.RasterizerState.DepthClipEnable = FALSE;
			pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
			pd.DepthStencilState.DepthEnable = FALSE;
			pd.SampleMask = UINT_MAX;
			pd.NumRenderTargets = 1;
			pd.RTVFormats[0] = a_format;
			pd.SampleDesc.Count = 1;
			ComPtr<ID3D12PipelineState> pso;
			if (FAILED(a_device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(pso.GetAddressOf())))) {
				Fail(std::format("its pipeline for {} failed", d3d::Name(a_format)));
				return nullptr;
			}
			Log::Info("render: night-vision pipeline for {}", d3d::Name(a_format));
			return (g_psos[static_cast<int>(a_format)] = pso).Get();
		}

		// Our copy of Elden Ring's scene colour (same size, format and mips; no render-target flag), kept in COPY_DEST between uses.
		bool EnsureCopy(ID3D12Device* a_device, ID3D12Resource* a_target)
		{
			auto desc = a_target->GetDesc();
			if (g_copy && desc.Width == g_copyDesc.Width && desc.Height == g_copyDesc.Height && desc.Format == g_copyDesc.Format &&
				desc.MipLevels == g_copyDesc.MipLevels) {
				return true;
			}
			if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1) {
				Fail(std::format("Elden Ring's scene colour is not a plain 2D texture (dimension {}, {} samples, {} layers)", static_cast<int>(desc.Dimension),
					desc.SampleDesc.Count, desc.DepthOrArraySize));
				return false;
			}
			desc.Flags = D3D12_RESOURCE_FLAG_NONE;
			desc.Alignment = 0;
			const auto             props = d3d::HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			ComPtr<ID3D12Resource> copy;
			if (FAILED(a_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
					reinterpret_cast<void**>(copy.GetAddressOf())))) {
				Fail(std::format("its {}x{} {} copy texture could not be made", desc.Width, desc.Height, d3d::Name(desc.Format)));
				return false;
			}
			g_copy = copy;
			g_copyDesc = desc;
			Log::Info("render: night vision: copy of Elden Ring's scene colour {}x{} {} ({} mips)", desc.Width, desc.Height, d3d::Name(desc.Format), desc.MipLevels);
			return true;
		}
	}

	bool Record(render::Recording& a_rec, const Target& a_t, float a_strength, std::uint64_t a_frame)
	{
		if (g_failed || !a_t.resource || a_strength <= 0.0f || !a_t.width || !a_t.height) {
			return false;
		}
		auto* device = a_rec.device;
		if (!EnsureCopy(device, a_t.resource)) {
			return false;
		}
		auto* pso = Pso(device, a_rec.root, a_t.format);
		if (!pso || a_rec.descUsed + 1 >= a_rec.descLimit) {
			return false;
		}
		const UINT                      slot = a_rec.descUsed++;
		D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = a_t.format;  // the RTV's view of it: an sRGB view decodes on read as its RTV encodes on write
		sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sd.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(g_copy.Get(), &sd, a_rec.Cpu(slot));
		const render::FrameConstants fc{};  // b0 is not read; bound so every root parameter is valid
		const auto                   fcVa = a_rec.Upload(&fc, sizeof(fc));
		if (!fcVa) {
			return false;
		}

		const auto&   rc = Cfg().render;
		std::uint32_t k[render::kDrawConstants]{};
		auto          put = [&](int a_i, float a_v) { std::memcpy(k + a_i, &a_v, 4); };
		k[0] = static_cast<std::uint32_t>(a_frame);
		k[1] = a_t.srgbView ? 0u : 1u;
		put(2, a_strength);
		put(3, rc.nvgScanlines);
		put(4, rc.nvgGain);
		put(5, rc.nvgFloor);
		put(6, rc.nvgGrain);
		put(7, rc.nvgVignette);
		put(8, rc.nvgTint[0]);
		put(9, rc.nvgTint[1]);
		put(10, rc.nvgTint[2]);
		put(11, rc.nvgCurve);
		put(12, static_cast<float>(a_t.width));
		put(13, static_cast<float>(a_t.height));
		put(14, 1.0f / static_cast<float>(a_t.width));
		put(15, 1.0f / static_cast<float>(a_t.height));

		auto*                  list = a_rec.list;
		D3D12_RESOURCE_BARRIER b[2]{};
		UINT                   n = 0;
		if (a_t.state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
			b[n++] = d3d::Transition(a_t.resource, a_t.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		}
		if (n) {
			list->ResourceBarrier(n, b);
		}
		list->CopyResource(g_copy.Get(), a_t.resource);
		b[0] = d3d::Transition(a_t.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		b[1] = d3d::Transition(g_copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		list->ResourceBarrier(2, b);

		list->OMSetRenderTargets(1, &a_t.rtv, FALSE, nullptr);
		const D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(a_t.width), static_cast<float>(a_t.height), 0, 1 };
		const D3D12_RECT     sr{ 0, 0, static_cast<LONG>(a_t.width), static_cast<LONG>(a_t.height) };
		list->RSSetViewports(1, &vp);
		list->RSSetScissorRects(1, &sr);
		list->SetGraphicsRootSignature(a_rec.root);
		ID3D12DescriptorHeap* heaps[] = { a_rec.heap };
		list->SetDescriptorHeaps(1, heaps);
		list->SetGraphicsRootConstantBufferView(render::kRootFrame, fcVa);
		list->SetGraphicsRootConstantBufferView(render::kRootErView, fcVa);
		list->SetGraphicsRootDescriptorTable(render::kRootDepth, a_rec.Gpu(slot));
		list->SetGraphicsRoot32BitConstants(render::kRootDraw, render::kDrawConstants, k, 0);
		list->SetPipelineState(pso);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->DrawInstanced(3, 1, 0, 0);

		n = 0;
		if (a_t.state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
			b[n++] = d3d::Transition(a_t.resource, D3D12_RESOURCE_STATE_RENDER_TARGET, a_t.state);
		}
		b[n++] = d3d::Transition(g_copy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
		list->ResourceBarrier(n, b);
		if (++g_frames == 1) {
			Log::Info("render: night vision drawn over Elden Ring's scene before its HUD ({}x{} {}, strength {:.2f})", a_t.width, a_t.height, d3d::Name(a_t.format),
				a_strength);
		}
		return true;
	}

	void Reset()
	{
		g_copy.Reset();
		g_copyDesc = {};
	}

	bool Failed() { return g_failed; }

	bool ShadersCompile() { return Shaders::Compile(kSource, "VSNvg", "vs_5_0") && Shaders::Compile(kSource, "PSNvg", "ps_5_0"); }

	std::string Describe()
	{
		return std::format("night vision: {} ({} frames drawn{})", g_failed ? "OFF (failed, see the log)" : (Cfg().render.nvg ? "on" : "off ([Render] nvg)"), g_frames,
			g_copy ? std::format(", copy {}x{}", g_copyDesc.Width, g_copyDesc.Height) : std::string());
	}
}
