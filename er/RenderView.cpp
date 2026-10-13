#include "RenderView.h"

#include "Log.h"
#include "PacingProbe.h"
#include "Shaders.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>

namespace phantom::er::RenderView
{
	namespace
	{
		using d3d::ComPtr;

		constexpr std::uint64_t kSlotBytes = kFloat4s * 16;
		constexpr std::uint64_t kCheckEvery = 30;   // copies between two readbacks of the main view
		constexpr float         kTolerance = 0.05f;  // m between Elden Ring's view origin and pers_cam_1

		constexpr char kCopy[] = R"(
cbuffer Src : register(b0)
{
	float4 gSrc[64];
};
cbuffer Dst : register(b1)
{
	uint gOff;
};
RWByteAddressBuffer gOut : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint i : SV_DispatchThreadID)
{
	gOut.Store4((gOff + i) * 16, asuint(gSrc[i]));
}
)";

		std::mutex                  g_lock;
		ComPtr<ID3D12Device>        g_device;
		ComPtr<ID3D12RootSignature> g_root;
		ComPtr<ID3D12PipelineState> g_pso;
		ComPtr<ID3D12Resource>      g_buffer, g_readback;
		bool                        g_ok{ false };

		std::uint64_t g_copies{ 0 }, g_checks{ 0 }, g_matches{ 0 }, g_mismatches{ 0 };
		bool          g_trusted{ false };
		bool          g_pendingRecorded{ false };  // a readback recorded, its submission fence not known yet
		std::uint64_t g_pendingFence{ 0 };
		Vec3          g_pendingCam{};
		std::uint64_t g_pendingSnap{ 0 };  // the render snapshot (ER frame) whose camera the pending check compares with
		Vec3          g_lastOrigin{};
		float         g_lastError{ -1 };
		std::string   g_lastRows;
	}

	bool Init(ID3D12Device* a_device)
	{
		std::scoped_lock lock(g_lock);
		g_device = a_device;
		HMODULE d3d12 = ::GetModuleHandleW(L"d3d12.dll");
		auto    serialize = d3d12 ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(reinterpret_cast<void*>(::GetProcAddress(d3d12, "D3D12SerializeRootSignature"))) : nullptr;
		if (!serialize) {
			Log::Warn("render: view copy: no D3D12SerializeRootSignature");
			return false;
		}
		D3D12_DESCRIPTOR_RANGE range{};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
		range.NumDescriptors = 1;
		D3D12_ROOT_PARAMETER params[3]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[0].DescriptorTable.NumDescriptorRanges = 1;
		params[0].DescriptorTable.pDescriptorRanges = &range;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[2].Constants.ShaderRegister = 1;
		params[2].Constants.Num32BitValues = 1;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_ROOT_SIGNATURE_DESC desc{};
		desc.NumParameters = 3;
		desc.pParameters = params;
		ComPtr<ID3DBlob> blob, error;
		if (FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(), error.GetAddressOf())) ||
			FAILED(a_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(g_root.GetAddressOf())))) {
			Log::Warn("render: view copy root signature failed");
			return false;
		}
		auto cs = Shaders::Compile(kCopy, "CSMain", "cs_5_0");
		if (!cs) {
			return false;
		}
		D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
		pd.pRootSignature = g_root.Get();
		pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
		if (FAILED(a_device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(g_pso.GetAddressOf())))) {
			Log::Warn("render: view copy pipeline failed");
			return false;
		}
		auto bd = d3d::BufferDesc(kSlots * kSlotBytes);
		bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		const auto def = d3d::HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		const auto rb = d3d::HeapProps(D3D12_HEAP_TYPE_READBACK);
		const auto rd = d3d::BufferDesc(kSlotBytes);
		if (FAILED(a_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
				reinterpret_cast<void**>(g_buffer.GetAddressOf()))) ||
			FAILED(a_device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
				reinterpret_cast<void**>(g_readback.GetAddressOf())))) {
			Log::Warn("render: view copy buffers failed");
			return false;
		}
		g_ok = true;
		return true;
	}

	bool RecordCopies(ID3D12GraphicsCommandList* a_list, ID3D12DescriptorHeap* a_erHeap, const D3D12_GPU_DESCRIPTOR_HANDLE* a_cbs, UINT a_count, const Vec3& a_camPos,
		std::uint64_t a_snapFrame)
	{
		std::scoped_lock lock(g_lock);
		if (!g_ok || !a_erHeap || a_count == 0) {
			return false;
		}
		a_count = std::min(a_count, kSlots);
		auto buffer = g_buffer.Get();
		auto toUav = d3d::Transition(buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		a_list->ResourceBarrier(1, &toUav);
		a_list->SetDescriptorHeaps(1, &a_erHeap);
		a_list->SetComputeRootSignature(g_root.Get());
		a_list->SetPipelineState(g_pso.Get());
		a_list->SetComputeRootUnorderedAccessView(1, buffer->GetGPUVirtualAddress());
		for (UINT i = 0; i < a_count; ++i) {
			a_list->SetComputeRootDescriptorTable(0, a_cbs[i]);
			a_list->SetComputeRoot32BitConstant(2, i * kFloat4s, 0);
			a_list->Dispatch(1, 1, 1);
		}
		// Every copy while the pacing probe records: which snapshot each frame really pairs with (er/PacingProbe.h).
		const bool check = !g_pendingRecorded && g_pendingFence == 0 && (g_copies % kCheckEvery == 0 || (!g_trusted && g_copies % 4 == 0) || PacingProbe::Active());
		++g_copies;
		D3D12_RESOURCE_BARRIER b[1];
		if (check) {
			b[0] = d3d::Transition(buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
			a_list->ResourceBarrier(1, b);
			a_list->CopyBufferRegion(g_readback.Get(), 0, buffer, 0, kSlotBytes);
			b[0] = d3d::Transition(buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			g_pendingRecorded = true;
			g_pendingCam = a_camPos;
			g_pendingSnap = a_snapFrame;
		} else {
			b[0] = d3d::Transition(buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		}
		a_list->ResourceBarrier(1, b);
		return true;
	}

	D3D12_GPU_VIRTUAL_ADDRESS Slot(UINT a_index)
	{
		return g_buffer ? g_buffer->GetGPUVirtualAddress() + std::uint64_t(std::min(a_index, kSlots - 1)) * kSlotBytes : 0;
	}

	void SetSubmitFence(std::uint64_t a_fence)
	{
		std::scoped_lock lock(g_lock);
		if (g_pendingRecorded) {
			g_pendingRecorded = false;
			g_pendingFence = a_fence;
		}
	}

	void OnFence(std::uint64_t a_completed)
	{
		std::scoped_lock lock(g_lock);
		if (!g_pendingFence || a_completed < g_pendingFence) {
			return;
		}
		g_pendingFence = 0;
		const float*      v = nullptr;
		const D3D12_RANGE range{ 0, kSlotBytes };
		if (FAILED(g_readback->Map(0, &range, reinterpret_cast<void**>(const_cast<float**>(&v)))) || !v) {
			return;
		}
		++g_checks;
		const Vec3 origin{ v[12], v[13], v[14] };
		bool       sane = Finite(origin);
		for (int r = 24 * 4; r < 33 * 4; ++r) {
			sane = sane && std::isfinite(v[r]);
		}
		const float err = Length(origin - g_pendingCam);
		g_lastOrigin = origin;
		g_lastError = err;
		PacingProbe::OnRender(2, g_pendingSnap, err);
		g_lastRows = std::format("[24] {:.4f} {:.4f} {:.4f} {:.4f} [25] {:.4f} {:.4f} {:.4f} {:.4f} [27] {:.4f} {:.4f} {:.4f} {:.4f} [28] {:.3f} {:.3f} {:.3f}", v[96], v[97],
			v[98], v[99], v[100], v[101], v[102], v[103], v[108], v[109], v[110], v[111], v[112], v[113], v[114]);
		const D3D12_RANGE none{ 0, 0 };
		g_readback->Unmap(0, &none);
		const bool ok = sane && err < kTolerance;
		if (ok) {
			++g_matches;
		} else {
			++g_mismatches;
		}
		if (ok != g_trusted) {
			if (ok) {
				Log::Info("render: Elden Ring's view constants found (origin ({:.3f}, {:.3f}, {:.3f}) = the camera to {:.4f} m): Snake uses its matrices and casts shadows; {}",
					origin.x, origin.y, origin.z, err, g_lastRows);
			} else {
				Log::Warn("render: Elden Ring's view constants don't match the camera (origin ({:.3f}, {:.3f}, {:.3f}) vs ({:.3f}, {:.3f}, {:.3f}), {}); stage C uses its own "
						  "camera and draws no shadows",
					origin.x, origin.y, origin.z, g_pendingCam.x, g_pendingCam.y, g_pendingCam.z, sane ? "finite" : "not finite");
			}
			g_trusted = ok;
		}
	}

	bool MainTrusted()
	{
		std::scoped_lock lock(g_lock);
		return g_trusted;
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		return std::format("view constants: {}, {} copies, {} checks ({} matched, {} not), last origin ({:.3f}, {:.3f}, {:.3f}) error {:.4f} m; {}", g_ok ? (g_trusted ? "trusted" : "untrusted") : "off",
			g_copies, g_checks, g_matches, g_mismatches, g_lastOrigin.x, g_lastOrigin.y, g_lastOrigin.z, g_lastError, g_lastRows);
	}
}
