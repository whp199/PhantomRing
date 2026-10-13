#include "D3D11Hooks.h"
#include "FramePace.h"

#include "Hook.h"
#include "Log.h"
#include "WindowHidden.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace phantom::guest::d3d
{
	namespace
	{
		// ---- vtable indices (mingw-w64 d3d11_1.h / dxgi1_2.h, checked with offsetof on the C vtables) ----
		enum DeviceSlot : std::size_t
		{
			kDevCreateInputLayout = 11,
			kDevCreateVertexShader = 12,
			kDevCreatePixelShader = 15,
			kDevCreateDeferredContext = 27,
		};
		enum ContextSlot : std::size_t
		{
			kCtxVSSetConstantBuffers = 7,
			kCtxPSSetShaderResources = 8,
			kCtxPSSetShader = 9,
			kCtxVSSetShader = 11,
			kCtxDrawIndexed = 12,
			kCtxDraw = 13,
			kCtxMap = 14,
			kCtxUnmap = 15,
			kCtxPSSetConstantBuffers = 16,
			kCtxIASetInputLayout = 17,
			kCtxIASetVertexBuffers = 18,
			kCtxIASetIndexBuffer = 19,
			kCtxDrawIndexedInstanced = 20,
			kCtxDrawInstanced = 21,
			kCtxIASetPrimitiveTopology = 24,
			kCtxVSSetShaderResources = 25,
			kCtxOMSetRenderTargets = 33,
			kCtxOMSetRenderTargetsAndUAVs = 34,
			kCtxUpdateSubresource = 48,
			kCtxExecuteCommandList = 58,
			kCtxClearState = 110,
			kCtxFinishCommandList = 114,
			kCtxUpdateSubresource1 = 116,
			kCtxVSSetConstantBuffers1 = 119,
			kCtxPSSetConstantBuffers1 = 123,
		};
		enum SwapSlot : std::size_t
		{
			kSwapPresent = 8,
			kSwapSetFullscreenState = 10,
			kSwapResizeTarget = 14,
			kSwapPresent1 = 22,
		};

		constexpr std::uintptr_t kGameDevice = 0x142C6B870;     // ID3D11Device*        (FUN_1419f4eb0)

		thread_local int t_own = 0;

		Callbacks                         g_cb;
		std::atomic<ID3D11Device*>        g_device{ nullptr };
		std::atomic<ID3D11DeviceContext*> g_immediate{ nullptr };
		std::atomic<bool>                 g_presentHooked{ false };
		std::atomic<std::uint64_t>        g_presents{ 0 };
		std::mutex                        g_attachLock;

		struct AtomicStats
		{
			std::atomic<std::uint64_t> draws{ 0 }, drawsDeferred{ 0 }, maps{ 0 }, cbMaps{ 0 }, cbUpdates{ 0 }, executes{ 0 }, finishes{ 0 }, layouts{ 0 },
				vertexShaders{ 0 }, pixelShaders{ 0 }, contexts{ 0 };
		} g_stats;

		// ---- registries (created on loading threads, read on render threads) -----------------------
		std::shared_mutex                                                       g_regLock;
		std::unordered_map<ID3D11InputLayout*, std::unique_ptr<InputLayoutInfo>> g_layouts;
		std::unordered_map<void*, std::unique_ptr<ShaderInfo>>                   g_shaders;  // VS and PS (distinct objects)

		// ---- per-context state: a fixed table, inserted under a lock, read lock-free ------------------
		constexpr std::size_t kMaxContexts = 128;
		struct ContextSlotEntry
		{
			std::atomic<ID3D11DeviceContext*> key{ nullptr };
			ContextState*                     state{ nullptr };
		};
		std::array<ContextSlotEntry, kMaxContexts> g_contexts;
		std::mutex                                 g_contextLock;
		std::atomic<bool>                          g_contextsFull{ false };

		ContextState* StateOf(ID3D11DeviceContext* a_ctx)
		{
			for (auto& e : g_contexts) {
				const auto k = e.key.load(std::memory_order_acquire);
				if (k == a_ctx) {
					return e.state;
				}
				if (!k) {
					break;
				}
			}
			std::scoped_lock lock(g_contextLock);
			for (auto& e : g_contexts) {
				const auto k = e.key.load(std::memory_order_acquire);
				if (k == a_ctx) {
					return e.state;
				}
				if (!k) {
					auto* st = new ContextState;
					st->ctx = a_ctx;
					st->deferred = a_ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
					st->index = static_cast<std::uint32_t>(&e - g_contexts.data());
					st->shadows.reserve(32);
					e.state = st;
					e.key.store(a_ctx, std::memory_order_release);
					g_stats.contexts.fetch_add(1, std::memory_order_relaxed);
					Log::Info("d3d11: tracking {} context #{} {}", st->deferred ? "deferred" : "immediate", st->index, static_cast<void*>(a_ctx));
					return st;
				}
			}
			if (!g_contextsFull.exchange(true)) {
				Log::Warn("d3d11: more than {} contexts -- the rest are not tracked", kMaxContexts);
			}
			return nullptr;
		}

		// ---- originals, per vtable (0 immediate, 1 deferred) ------------------------------------------
		using FnVSSetCB = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
		using FnSetSRV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
		using FnPSSetShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
		using FnVSSetShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
		using FnDrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
		using FnDraw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
		using FnMap = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
		using FnUnmap = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
		using FnIASetInputLayout = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11InputLayout*);
		using FnIASetVB = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
		using FnIASetIB = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, DXGI_FORMAT, UINT);
		using FnDrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
		using FnDrawInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
		using FnIASetTopology = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, D3D11_PRIMITIVE_TOPOLOGY);
		using FnOMSetRT = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
		using FnOMSetRTUAV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT,
			ID3D11UnorderedAccessView* const*, const UINT*);
		using FnUpdateSubresource = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
		using FnExecute = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
		using FnClearState = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
		using FnFinish = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
		using FnUpdateSubresource1 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT, UINT);
		using FnSetCB1 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);

		struct ContextOriginals
		{
			FnVSSetCB              vsSetCb{}, psSetCb{};
			FnSetSRV               psSetSrv{}, vsSetSrv{};
			FnPSSetShader          psSetShader{};
			FnVSSetShader          vsSetShader{};
			FnDrawIndexed          drawIndexed{};
			FnDraw                 draw{};
			FnMap                  map{};
			FnUnmap                unmap{};
			FnIASetInputLayout     setLayout{};
			FnIASetVB              setVb{};
			FnIASetIB              setIb{};
			FnDrawIndexedInstanced drawIndexedInstanced{};
			FnDrawInstanced        drawInstanced{};
			FnIASetTopology        setTopology{};
			FnOMSetRT              setRt{};
			FnOMSetRTUAV           setRtUav{};
			FnUpdateSubresource    update{};
			FnExecute              execute{};
			FnClearState           clearState{};
			FnFinish               finish{};
			FnUpdateSubresource1   update1{};
			FnSetCB1               vsSetCb1{}, psSetCb1{};
		};
		ContextOriginals g_orig[2];

		using FnCreateInputLayout = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
		using FnCreateVS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
		using FnCreatePS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
		using FnCreateDeferred = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
		FnCreateInputLayout g_createLayout{};
		FnCreateVS          g_createVs{};
		FnCreatePS          g_createPs{};
		FnCreateDeferred    g_createDeferred{};

		using FnPresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
		using FnPresent1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, const void*);
		FnPresent  g_present{};
		FnPresent1 g_present1{};
		using FnSetFullscreenState = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
		using FnResizeTarget = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);
		FnSetFullscreenState g_setFullscreenState{};
		FnResizeTarget       g_resizeTarget{};

		using FnCreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
		using FnCreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, HWND, const void*, const void*, IDXGIOutput*, IDXGISwapChain**);
		FnCreateSwapChain        g_createSwapChain{};
		FnCreateSwapChainForHwnd g_createSwapChainForHwnd{};

		using FnCreateDevice = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*,
			ID3D11DeviceContext**);
		FnCreateDevice g_createDevice{};

		// ---- helpers --------------------------------------------------------------------------------
		// The first call of each hook is logged once: a live run shows how far the game got through us.
		std::atomic<std::uint64_t> g_seen{ 0 };
		void Seen(int a_bit, const char* a_name, int a_kind)
		{
			const std::uint64_t bit = 1ull << (a_bit * 2 + a_kind);
			if (!(g_seen.load(std::memory_order_relaxed) & bit) && !(g_seen.fetch_or(bit) & bit)) {
				Log::Info("d3d11: first {} on {} context (thread {})", a_name, a_kind ? "deferred" : "immediate", ::GetCurrentThreadId());
			}
		}

		bool IsSkinSemantic(std::string_view a_name, bool a_indices)
		{
			std::string s(a_name);
			std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			return a_indices ? (s.find("INDICES") != std::string::npos || s.find("BONE") != std::string::npos || s == "BLENDINDEX")
			                 : (s.find("WEIGHT") != std::string::npos);
		}

		std::uint32_t FormatBytes(std::uint32_t a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R32G32B32A32_FLOAT:
			case DXGI_FORMAT_R32G32B32A32_UINT:
			case DXGI_FORMAT_R32G32B32A32_SINT:
				return 16;
			case DXGI_FORMAT_R32G32B32_FLOAT:
			case DXGI_FORMAT_R32G32B32_UINT:
			case DXGI_FORMAT_R32G32B32_SINT:
				return 12;
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_UNORM:
			case DXGI_FORMAT_R16G16B16A16_UINT:
			case DXGI_FORMAT_R16G16B16A16_SNORM:
			case DXGI_FORMAT_R16G16B16A16_SINT:
			case DXGI_FORMAT_R32G32_FLOAT:
			case DXGI_FORMAT_R32G32_UINT:
			case DXGI_FORMAT_R32G32_SINT:
				return 8;
			case DXGI_FORMAT_R16G16_FLOAT:
			case DXGI_FORMAT_R16G16_UNORM:
			case DXGI_FORMAT_R16G16_UINT:
			case DXGI_FORMAT_R16G16_SNORM:
			case DXGI_FORMAT_R16G16_SINT:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UINT:
			case DXGI_FORMAT_R8G8B8A8_SNORM:
			case DXGI_FORMAT_R8G8B8A8_SINT:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_R10G10B10A2_UNORM:
			case DXGI_FORMAT_R10G10B10A2_UINT:
			case DXGI_FORMAT_R11G11B10_FLOAT:
			case DXGI_FORMAT_R32_FLOAT:
			case DXGI_FORMAT_R32_UINT:
			case DXGI_FORMAT_R32_SINT:
				return 4;
			case DXGI_FORMAT_R16_FLOAT:
			case DXGI_FORMAT_R16_UNORM:
			case DXGI_FORMAT_R16_UINT:
			case DXGI_FORMAT_R8G8_UNORM:
			case DXGI_FORMAT_R8G8_UINT:
				return 2;
			case DXGI_FORMAT_R8_UNORM:
			case DXGI_FORMAT_R8_UINT:
				return 1;
			default:
				return 0;
			}
		}

		void ComputeRt(ContextState& a_s)
		{
			RtInfo r{};
			std::uint64_t sig = 0;
			for (unsigned i = 0; i < kRtSlots; ++i) {
				if (!a_s.rtv[i]) {
					continue;
				}
				r.count = i + 1;
				D3D11_RENDER_TARGET_VIEW_DESC d{};
				a_s.rtv[i]->GetDesc(&d);
				r.formats[i] = d.Format;
				if (!r.width) {
					ID3D11Resource* res = nullptr;
					a_s.rtv[i]->GetResource(&res);
					if (res) {
						D3D11_RESOURCE_DIMENSION dim{};
						res->GetType(&dim);
						if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
							D3D11_TEXTURE2D_DESC td{};
							static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
							r.width = td.Width;
							r.height = td.Height;
						}
						res->Release();
					}
				}
			}
			if (a_s.dsv) {
				D3D11_DEPTH_STENCIL_VIEW_DESC d{};
				a_s.dsv->GetDesc(&d);
				r.depthFormat = d.Format;
				if (!r.width) {
					ID3D11Resource* res = nullptr;
					a_s.dsv->GetResource(&res);
					if (res) {
						D3D11_RESOURCE_DIMENSION dim{};
						res->GetType(&dim);
						if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
							D3D11_TEXTURE2D_DESC td{};
							static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
							r.width = td.Width;
							r.height = td.Height;
						}
						res->Release();
					}
				}
			}
			for (unsigned i = 0; i < r.count; ++i) {
				sig = sig * 0x100000001b3ull ^ (r.formats[i] + 1);
			}
			sig = sig * 0x100000001b3ull ^ r.depthFormat;
			sig = sig * 0x100000001b3ull ^ (static_cast<std::uint64_t>(r.width) << 32 | r.height);
			r.signature = sig;
			a_s.rt = r;
		}

		CbShadow* ShadowFor(ContextState& a_s, ID3D11Buffer* a_buffer, bool a_create)
		{
			CbShadow* oldest = nullptr;
			for (auto& sh : a_s.shadows) {
				if (sh.buffer == a_buffer) {
					sh.serial = ++a_s.shadowSerial;
					return &sh;
				}
				if (!oldest || sh.serial < oldest->serial) {
					oldest = &sh;
				}
			}
			if (!a_create) {
				return nullptr;
			}
			if (a_s.shadows.size() < 32) {
				oldest = &a_s.shadows.emplace_back();
			}
			oldest->buffer = a_buffer;
			oldest->mapped = nullptr;
			oldest->bytes = 0;
			oldest->copy.clear();
			oldest->serial = ++a_s.shadowSerial;
			return oldest;
		}

		bool IsCbOrSrvBuffer(ID3D11Resource* a_res, D3D11_BUFFER_DESC& a_desc)
		{
			D3D11_RESOURCE_DIMENSION dim{};
			a_res->GetType(&dim);
			if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) {
				return false;
			}
			static_cast<ID3D11Buffer*>(a_res)->GetDesc(&a_desc);
			return (a_desc.BindFlags & (D3D11_BIND_CONSTANT_BUFFER | D3D11_BIND_SHADER_RESOURCE)) != 0;
		}

		// ---- context detours --------------------------------------------------------------------------
		template <int K>
		struct Ctx
		{
			static ContextOriginals& O() { return g_orig[K]; }

			static void STDMETHODCALLTYPE VSSetConstantBuffers(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* bufs)
			{
				Seen(0, "VSSetConstantBuffers", K);
				if (!t_own) {
					if (auto* s = StateOf(c)) {
						for (UINT i = 0; i < n && start + i < kVsCbSlots; ++i) {
							s->vsCb[start + i] = { bufs ? bufs[i] : nullptr, 0, 0 };
						}
					}
				}
				O().vsSetCb(c, start, n, bufs);
			}
			static void STDMETHODCALLTYPE VSSetConstantBuffers1(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* bufs, const UINT* first, const UINT* num)
			{
				Seen(1, "VSSetConstantBuffers1", K);
				if (!t_own) {
					if (auto* s = StateOf(c)) {
						for (UINT i = 0; i < n && start + i < kVsCbSlots; ++i) {
							s->vsCb[start + i] = { bufs ? bufs[i] : nullptr, first ? first[i] : 0, num ? num[i] : 0 };
						}
					}
				}
				O().vsSetCb1(c, start, n, bufs, first, num);
			}
			static void STDMETHODCALLTYPE PSSetConstantBuffers(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* bufs)
			{
				if (!t_own) {
					if (auto* s = StateOf(c)) {
						for (UINT i = 0; i < n && start + i < kPsCbSlots; ++i) {
							s->psCb[start + i] = { bufs ? bufs[i] : nullptr, 0, 0 };
						}
					}
				}
				O().psSetCb(c, start, n, bufs);
			}
			static void STDMETHODCALLTYPE PSSetConstantBuffers1(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* bufs, const UINT* first, const UINT* num)
			{
				if (!t_own) {
					if (auto* s = StateOf(c)) {
						for (UINT i = 0; i < n && start + i < kPsCbSlots; ++i) {
							s->psCb[start + i] = { bufs ? bufs[i] : nullptr, first ? first[i] : 0, num ? num[i] : 0 };
						}
					}
				}
				O().psSetCb1(c, start, n, bufs, first, num);
			}
			static void STDMETHODCALLTYPE PSSetShaderResources(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* views)
			{
				Seen(2, "PSSetShaderResources", K);
				if (!t_own) {
					if (auto* s = StateOf(c)) {
						for (UINT i = 0; i < n && start + i < kPsSrvSlots; ++i) {
							s->psSrv[start + i] = views ? views[i] : nullptr;
						}
					}
				}
				O().psSetSrv(c, start, n, views);
			}
			static void STDMETHODCALLTYPE VSSetShaderResources(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* views)
			{
				if (!t_own) {
					if (auto* s = StateOf(c)) {
						for (UINT i = 0; i < n && start + i < kVsSrvSlots; ++i) {
							s->vsSrv[start + i] = views ? views[i] : nullptr;
						}
					}
				}
				O().vsSetSrv(c, start, n, views);
			}
			static void STDMETHODCALLTYPE PSSetShader(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT nci)
			{
				Seen(16, "PSSetShader", K);
				if (!t_own) {
					if (auto* s = StateOf(c)) {
						s->ps = ps;
						s->psInfo = PixelShaderInfo(ps);
					}
				}
				O().psSetShader(c, ps, ci, nci);
			}
			static void STDMETHODCALLTYPE VSSetShader(ID3D11DeviceContext* c, ID3D11VertexShader* vs, ID3D11ClassInstance* const* ci, UINT nci)
			{
				Seen(3, "VSSetShader", K);
				if (!t_own) {
					if (auto* s = StateOf(c)) {
						s->vs = vs;
						s->vsInfo = VertexShaderInfo(vs);
					}
				}
				O().vsSetShader(c, vs, ci, nci);
			}
			static void STDMETHODCALLTYPE IASetInputLayout(ID3D11DeviceContext* c, ID3D11InputLayout* l)
			{
				Seen(11, "IASetInputLayout", K);
				if (!t_own) {
					if (auto* s = StateOf(c)) {
						s->layout = l;
						s->layoutInfo = LayoutInfo(l);
					}
				}
				O().setLayout(c, l);
			}
			static void STDMETHODCALLTYPE IASetVertexBuffers(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* bufs, const UINT* strides, const UINT* offsets)
			{
				Seen(4, "IASetVertexBuffers", K);
				if (!t_own) {
					if (auto* s = StateOf(c)) {
						for (UINT i = 0; i < n && start + i < kVbSlots; ++i) {
							s->vb[start + i] = { bufs ? bufs[i] : nullptr, strides ? strides[i] : 0, offsets ? offsets[i] : 0 };
						}
					}
				}
				O().setVb(c, start, n, bufs, strides, offsets);
			}
			static void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11DeviceContext* c, ID3D11Buffer* b, DXGI_FORMAT f, UINT off)
			{
				Seen(15, "IASetIndexBuffer", K);
				if (!t_own) {
					if (auto* s = StateOf(c)) {
						s->ib = b;
						s->ibFormat = f;
						s->ibOffset = off;
					}
				}
				O().setIb(c, b, f, off);
			}
			static void STDMETHODCALLTYPE IASetPrimitiveTopology(ID3D11DeviceContext* c, D3D11_PRIMITIVE_TOPOLOGY t)
			{
				if (!t_own) {
					if (auto* s = StateOf(c)) {
						s->topology = t;
					}
				}
				O().setTopology(c, t);
			}
			static void SetTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
			{
				auto* s = StateOf(c);
				if (!s) {
					return;
				}
				for (unsigned i = 0; i < kRtSlots; ++i) {
					s->rtv[i] = (rtvs && i < n) ? rtvs[i] : nullptr;
				}
				s->dsv = dsv;
				ComputeRt(*s);
				if (g_cb.setTargets) {
					g_cb.setTargets(*s);
				}
			}
			static void STDMETHODCALLTYPE OMSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
			{
				Seen(5, "OMSetRenderTargets", K);
				if (!t_own) {
					SetTargets(c, n, rtvs, dsv);
				}
				O().setRt(c, n, rtvs, dsv);
			}
			static void STDMETHODCALLTYPE OMSetRenderTargetsAndUAVs(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
				UINT uavStart, UINT nUav, ID3D11UnorderedAccessView* const* uavs, const UINT* counts)
			{
				if (!t_own && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
					SetTargets(c, n, rtvs, dsv);
				}
				O().setRtUav(c, n, rtvs, dsv, uavStart, nUav, uavs, counts);
			}
			static HRESULT STDMETHODCALLTYPE Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* out)
			{
				Seen(6, "Map", K);
				const HRESULT hr = O().map(c, r, sub, type, flags, out);
				if (!t_own && SUCCEEDED(hr) && out && out->pData && r && type != D3D11_MAP_READ) {
					g_stats.maps.fetch_add(1, std::memory_order_relaxed);
					D3D11_BUFFER_DESC d{};
					if (IsCbOrSrvBuffer(r, d)) {
						if (auto* s = StateOf(c)) {
							if (auto* sh = ShadowFor(*s, static_cast<ID3D11Buffer*>(r), true)) {
								sh->mapped = static_cast<const std::uint8_t*>(out->pData);
								sh->bytes = d.ByteWidth;
								sh->copy.clear();
								g_stats.cbMaps.fetch_add(1, std::memory_order_relaxed);
							}
						}
					}
				}
				return hr;
			}
			static void STDMETHODCALLTYPE Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) { Seen(17, "Unmap", K); O().unmap(c, r, sub); }
			static void Update(ID3D11DeviceContext* c, ID3D11Resource* r, const D3D11_BOX* box, const void* data)
			{
				D3D11_BUFFER_DESC d{};
				if (!r || !data || !IsCbOrSrvBuffer(r, d) || d.ByteWidth > 0x10000) {
					return;
				}
				auto* s = StateOf(c);
				if (!s) {
					return;
				}
				auto* sh = ShadowFor(*s, static_cast<ID3D11Buffer*>(r), true);
				if (!sh) {
					return;
				}
				const std::uint32_t lo = box ? box->left : 0;
				const std::uint32_t hi = box ? std::min<std::uint32_t>(box->right, d.ByteWidth) : d.ByteWidth;
				if (hi <= lo) {
					return;
				}
				if (sh->copy.size() != d.ByteWidth) {
					sh->copy.assign(d.ByteWidth, 0);
				}
				std::memcpy(sh->copy.data() + lo, data, hi - lo);
				sh->mapped = nullptr;
				sh->bytes = d.ByteWidth;
				g_stats.cbUpdates.fetch_add(1, std::memory_order_relaxed);
			}
			static void STDMETHODCALLTYPE UpdateSubresource(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp)
			{
				Seen(7, "UpdateSubresource", K);
				if (!t_own) {
					Update(c, r, box, data);
				}
				O().update(c, r, sub, box, data, rp, dp);
			}
			static void STDMETHODCALLTYPE UpdateSubresource1(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp,
				UINT flags)
			{
				if (!t_own) {
					Update(c, r, box, data);
				}
				O().update1(c, r, sub, box, data, rp, dp, flags);
			}
			static void OnDraw(ID3D11DeviceContext* c, const DrawArgs& a)
			{
				g_stats.draws.fetch_add(1, std::memory_order_relaxed);
				if (K == 1) {
					g_stats.drawsDeferred.fetch_add(1, std::memory_order_relaxed);
				}
				if (!g_cb.draw) {
					return;
				}
				if (auto* s = StateOf(c)) {
					try {
						g_cb.draw(*s, a);
					} catch (...) {
					}
				}
			}
			static void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext* c, UINT count, UINT start, INT base)
			{
				Seen(8, "DrawIndexed", K);
				if (!t_own) {
					OnDraw(c, { count, start, base, 1, true });
				}
				O().drawIndexed(c, count, start, base);
			}
			static void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start, INT base, UINT startInst)
			{
				Seen(12, "DrawIndexedInstanced", K);
				if (!t_own) {
					OnDraw(c, { count, start, base, inst, true, startInst });
				}
				O().drawIndexedInstanced(c, count, inst, start, base, startInst);
			}
			static void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* c, UINT count, UINT start)
			{
				Seen(13, "Draw", K);
				if (!t_own) {
					OnDraw(c, { count, start, 0, 1, false });
				}
				O().draw(c, count, start);
			}
			static void STDMETHODCALLTYPE DrawInstanced(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start, UINT startInst)
			{
				if (!t_own) {
					OnDraw(c, { count, start, 0, inst, false, startInst });
				}
				O().drawInstanced(c, count, inst, start, startInst);
			}
			static void STDMETHODCALLTYPE ExecuteCommandList(ID3D11DeviceContext* c, ID3D11CommandList* l, BOOL restore)
			{
				Seen(9, "ExecuteCommandList", K);
				O().execute(c, l, restore);
				if (!t_own) {
					g_stats.executes.fetch_add(1, std::memory_order_relaxed);
					if (auto* s = StateOf(c); s && !restore) {
						s->Reset();
					}
					if (g_cb.execute) {
						g_cb.execute(c, l);
					}
				}
			}
			static void STDMETHODCALLTYPE ClearState(ID3D11DeviceContext* c)
			{
				Seen(14, "ClearState", K);
				if (!t_own) {
					if (auto* s = StateOf(c)) {
						s->Reset();
					}
				}
				O().clearState(c);
			}
			static HRESULT STDMETHODCALLTYPE FinishCommandList(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** out)
			{
				Seen(10, "FinishCommandList", K);
				const HRESULT hr = O().finish(c, restore, out);
				if (!t_own) {
					g_stats.finishes.fetch_add(1, std::memory_order_relaxed);
					if (auto* s = StateOf(c)) {
						if (g_cb.finish && SUCCEEDED(hr) && out) {
							g_cb.finish(*s, *out);
						}
						if (!restore) {
							s->Reset();
						}
					}
				}
				return hr;
			}
		};

		// Inline detours (MinHook) on the functions the vtables point to, as MGS5VR does on this very game:
		// MGSV quits silently (exit code 0, ~1 s after device creation) when the D3D11 vtables themselves
		// are modified, so the vtables are only read. A function shared by two vtables is detoured once;
		// its detour handles both kinds of context (they only differ in the counters).
		std::mutex                              g_inlineLock;
		std::unordered_map<void*, void*>        g_trampolines;  // target -> trampoline

		template <class F>
		void Swap(void** a_vt, std::size_t a_index, F a_detour, F& a_original, const char* a_name)
		{
			void* target = a_vt[a_index];
			std::scoped_lock lock(g_inlineLock);
			if (const auto it = g_trampolines.find(target); it != g_trampolines.end()) {
				a_original = reinterpret_cast<F>(it->second);
				return;
			}
			void* tramp = nullptr;
			if (!Hooks::Create(std::format("d3d11 {}", a_name), target, reinterpret_cast<void*>(a_detour), &tramp) || !tramp) {
				Log::Error("d3d11: could not detour {} (slot {})", a_name, a_index);
				return;
			}
			g_trampolines[target] = tramp;
			a_original = reinterpret_cast<F>(tramp);
		}

		template <int K>
		void HookContextVtable(void** vt)
		{
			auto& o = g_orig[K];
			using C = Ctx<K>;
			Swap(vt, kCtxVSSetConstantBuffers, &C::VSSetConstantBuffers, o.vsSetCb, "VSSetConstantBuffers");
			Swap(vt, kCtxPSSetShaderResources, &C::PSSetShaderResources, o.psSetSrv, "PSSetShaderResources");
			Swap(vt, kCtxPSSetShader, &C::PSSetShader, o.psSetShader, "PSSetShader");
			Swap(vt, kCtxVSSetShader, &C::VSSetShader, o.vsSetShader, "VSSetShader");
			Swap(vt, kCtxDrawIndexed, &C::DrawIndexed, o.drawIndexed, "DrawIndexed");
			Swap(vt, kCtxDraw, &C::Draw, o.draw, "Draw");
			Swap(vt, kCtxMap, &C::Map, o.map, "Map");
			Swap(vt, kCtxUnmap, &C::Unmap, o.unmap, "Unmap");
			Swap(vt, kCtxPSSetConstantBuffers, &C::PSSetConstantBuffers, o.psSetCb, "PSSetConstantBuffers");
			Swap(vt, kCtxIASetInputLayout, &C::IASetInputLayout, o.setLayout, "IASetInputLayout");
			Swap(vt, kCtxIASetVertexBuffers, &C::IASetVertexBuffers, o.setVb, "IASetVertexBuffers");
			Swap(vt, kCtxIASetIndexBuffer, &C::IASetIndexBuffer, o.setIb, "IASetIndexBuffer");
			Swap(vt, kCtxDrawIndexedInstanced, &C::DrawIndexedInstanced, o.drawIndexedInstanced, "DrawIndexedInstanced");
			Swap(vt, kCtxDrawInstanced, &C::DrawInstanced, o.drawInstanced, "DrawInstanced");
			Swap(vt, kCtxIASetPrimitiveTopology, &C::IASetPrimitiveTopology, o.setTopology, "IASetPrimitiveTopology");
			Swap(vt, kCtxVSSetShaderResources, &C::VSSetShaderResources, o.vsSetSrv, "VSSetShaderResources");
			Swap(vt, kCtxOMSetRenderTargets, &C::OMSetRenderTargets, o.setRt, "OMSetRenderTargets");
			Swap(vt, kCtxOMSetRenderTargetsAndUAVs, &C::OMSetRenderTargetsAndUAVs, o.setRtUav, "OMSetRenderTargetsAndUnorderedAccessViews");
			Swap(vt, kCtxUpdateSubresource, &C::UpdateSubresource, o.update, "UpdateSubresource");
			Swap(vt, kCtxExecuteCommandList, &C::ExecuteCommandList, o.execute, "ExecuteCommandList");
			Swap(vt, kCtxClearState, &C::ClearState, o.clearState, "ClearState");
			Swap(vt, kCtxFinishCommandList, &C::FinishCommandList, o.finish, "FinishCommandList");
			Swap(vt, kCtxUpdateSubresource1, &C::UpdateSubresource1, o.update1, "UpdateSubresource1");
			Swap(vt, kCtxVSSetConstantBuffers1, &C::VSSetConstantBuffers1, o.vsSetCb1, "VSSetConstantBuffers1");
			Swap(vt, kCtxPSSetConstantBuffers1, &C::PSSetConstantBuffers1, o.psSetCb1, "PSSetConstantBuffers1");
		}

		// ---- device detours -------------------------------------------------------------------------
		HRESULT STDMETHODCALLTYPE CreateInputLayout(ID3D11Device* d, const D3D11_INPUT_ELEMENT_DESC* descs, UINT n, const void* code, SIZE_T bytes,
			ID3D11InputLayout** out)
		{
			const HRESULT hr = g_createLayout(d, descs, n, code, bytes, out);
			Seen(21, "CreateInputLayout", 0);
			if (t_own || FAILED(hr) || !out || !*out || !descs) {
				return hr;
			}
			try {
				auto          info = std::make_unique<InputLayoutInfo>();
				std::uint32_t next[16]{};
				bool          idx = false, wgt = false;
				for (UINT i = 0; i < n && i < 32; ++i) {
					InputElement e;
					e.semantic = descs[i].SemanticName ? descs[i].SemanticName : "";
					e.semanticIndex = descs[i].SemanticIndex;
					e.format = descs[i].Format;
					e.slot = descs[i].InputSlot;
					const auto slot = std::min<std::uint32_t>(e.slot, 15);
					e.offset = descs[i].AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT ? next[slot] : descs[i].AlignedByteOffset;
					next[slot] = e.offset + FormatBytes(e.format);
					e.perInstance = descs[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA;
					idx |= IsSkinSemantic(e.semantic, true);
					wgt |= IsSkinSemantic(e.semantic, false);
					info->elements.push_back(std::move(e));
				}
				info->skinned = idx && wgt;
				std::string all;
				for (const auto& e : info->elements) {
					all += std::format("{}{}:{}:{}:{}:{};", e.semantic, e.semanticIndex, e.format, e.slot, e.offset, e.perInstance);
				}
				info->hash = Hash64(all.data(), all.size());
				std::unique_lock lock(g_regLock);
				g_layouts[*out] = std::move(info);
				g_stats.layouts.fetch_add(1, std::memory_order_relaxed);
			} catch (...) {
			}
			return hr;
		}

		void RegisterShader(void* a_shader, const void* a_code, SIZE_T a_bytes)
		{
			try {
				auto info = std::make_unique<ShaderInfo>(Reflect(a_code, a_bytes));
				std::unique_lock lock(g_regLock);
				g_shaders[a_shader] = std::move(info);
			} catch (...) {
			}
		}

		HRESULT STDMETHODCALLTYPE CreateVertexShader(ID3D11Device* d, const void* code, SIZE_T bytes, ID3D11ClassLinkage* link, ID3D11VertexShader** out)
		{
			const HRESULT hr = g_createVs(d, code, bytes, link, out);
			Seen(20, "CreateVertexShader", 0);
			if (!t_own && SUCCEEDED(hr) && out && *out) {
				RegisterShader(*out, code, bytes);
				g_stats.vertexShaders.fetch_add(1, std::memory_order_relaxed);
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE CreatePixelShader(ID3D11Device* d, const void* code, SIZE_T bytes, ID3D11ClassLinkage* link, ID3D11PixelShader** out)
		{
			const HRESULT hr = g_createPs(d, code, bytes, link, out);
			Seen(22, "CreatePixelShader", 0);
			if (!t_own && SUCCEEDED(hr) && out && *out) {
				RegisterShader(*out, code, bytes);
				g_stats.pixelShaders.fetch_add(1, std::memory_order_relaxed);
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE CreateDeferredContext(ID3D11Device* d, UINT flags, ID3D11DeviceContext** out)
		{
			const HRESULT hr = g_createDeferred(d, flags, out);
			if (!t_own && SUCCEEDED(hr) && out && *out) {
				if (auto* s = StateOf(*out)) {
					s->Reset();  // a new object may reuse a released one's address
				}
			}
			return hr;
		}

		// ---- swap chain -------------------------------------------------------------------------------
		void OnPresent()
		{
			g_presents.fetch_add(1, std::memory_order_relaxed);
			auto* imm = g_immediate.load(std::memory_order_acquire);
			if (g_cb.present && imm && !t_own) {
				try {
					g_cb.present(imm);
				} catch (...) {
				}
			}
		}

		std::int64_t PresentQpc()
		{
			LARGE_INTEGER t;
			::QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		// Our Present work and the real call are timed for `!pace` (mgsv/FramePace.h).
		HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* sc, UINT sync, UINT flags)
		{
			if (flags & DXGI_PRESENT_TEST) {
				return g_present(sc, sync, flags);
			}
			const auto t0 = PresentQpc();
			OnPresent();
			const auto    t1 = PresentQpc();
			sync = framepace::BeforePresent(sync);
			const auto    t2 = PresentQpc();
			const HRESULT hr = g_present(sc, sync, flags);
			framepace::OnPresent(t2, PresentQpc(), t1 - t0, sync);
			return hr;
		}

		HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain* sc, UINT sync, UINT flags, const void* params)
		{
			if (flags & DXGI_PRESENT_TEST) {
				return g_present1(sc, sync, flags, params);
			}
			const auto t0 = PresentQpc();
			OnPresent();
			const auto    t1 = PresentQpc();
			sync = framepace::BeforePresent(sync);
			const auto    t2 = PresentQpc();
			const HRESULT hr = g_present1(sc, sync, flags, params);
			framepace::OnPresent(t2, PresentQpc(), t1 - t0, sync);
			return hr;
		}

		// Hidden MGSV on Windows must never take the monitor from Elden Ring. MGSV creates its swap chain
		// windowed and, when its display mode is Fullscreen, calls SetFullscreenState(TRUE, output) right
		// after (FUN_140245f80 -> FUN_1419f5cd0) and again on mode changes (FUN_14024ad80, followed by a
		// ResizeTarget), so the swap chain itself is held windowed.
		HRESULT STDMETHODCALLTYPE SetFullscreenState(IDXGISwapChain* sc, BOOL fullscreen, IDXGIOutput* output)
		{
			if (fullscreen && !t_own && hidden::ForceWindowed()) {
				if (static std::atomic<bool> logged{ false }; !logged.exchange(true)) {
					Log::Info("d3d11: hidden MGSV asked for exclusive fullscreen -- kept windowed");
				}
				return g_setFullscreenState(sc, FALSE, nullptr);
			}
			return g_setFullscreenState(sc, fullscreen, output);
		}

		HRESULT STDMETHODCALLTYPE ResizeTarget(IDXGISwapChain* sc, const DXGI_MODE_DESC* mode)
		{
			if (mode && !t_own && hidden::ForceWindowed()) {
				// Windowed, only the size matters; no refresh rate / scanline order that could pick a mode.
				DXGI_MODE_DESC m{};
				m.Width = mode->Width;
				m.Height = mode->Height;
				m.Format = mode->Format;
				return g_resizeTarget(sc, &m);
			}
			return g_resizeTarget(sc, mode);
		}

		bool InModule(const void* a_p, const wchar_t* a_module)
		{
			const HMODULE m = ::GetModuleHandleW(a_module);
			if (!m) {
				return false;
			}
			const auto img = ModuleImage::Of(m);
			return img.Contains(a_p);
		}

		bool LooksLikeD3dObject(const void* a_obj)
		{
			const auto vt = SafeRead<void*>(a_obj);
			return vt && *vt && (InModule(*vt, L"d3d11.dll") || InModule(*vt, L"dxgi.dll"));
		}

		// The game's swap chain: MGSV keeps its DXGI factory at 0x142C6B880 (the adapter's parent; the
		// adapter is 0x142C6B888) and creates the swap chain from it later, when its window exists. DXVK's
		// factory functions are shared by every factory, so a factory of our own tells us where they are.
		void HookSwapChain(IDXGISwapChain* a_swap, const char* a_how)
		{
			if (!a_swap || g_presentHooked.load()) {
				return;
			}
			OwnCalls own;
			auto**   vt = *reinterpret_cast<void***>(a_swap);
			Swap(vt, kSwapPresent, &Present, g_present, "Present");
			IDXGISwapChain1* swap1 = nullptr;
			if (SUCCEEDED(a_swap->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swap1))) && swap1) {
				auto** vt1 = *reinterpret_cast<void***>(swap1);
				Swap(vt1, kSwapPresent1, &Present1, g_present1, "Present1");
				swap1->Release();
			}
			if (hidden::ForceWindowed()) {
				Swap(vt, kSwapSetFullscreenState, &SetFullscreenState, g_setFullscreenState, "SetFullscreenState");
				Swap(vt, kSwapResizeTarget, &ResizeTarget, g_resizeTarget, "ResizeTarget");
			}
			Hooks::EnableAll();
			DXGI_SWAP_CHAIN_DESC desc{};
			a_swap->GetDesc(&desc);
			g_presentHooked.store(true);
			Log::Info("d3d11: swap chain {} hooked ({}: {}x{} format {} buffers {})", static_cast<void*>(a_swap), a_how, desc.BufferDesc.Width, desc.BufferDesc.Height,
				FormatName(desc.BufferDesc.Format), desc.BufferCount);
		}

		HRESULT STDMETHODCALLTYPE CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
		{
			if (desc && !desc->Windowed && !t_own && hidden::ForceWindowed()) {
				// Hidden MGSV on Windows: never exclusive fullscreen (mgsv/WindowHidden.h). MGSV itself always
				// creates windowed; what protects it is the SetFullscreenState detour.
				desc->Windowed = TRUE;
			}
			const HRESULT hr = g_createSwapChain(f, dev, desc, out);
			if (!t_own && SUCCEEDED(hr) && out && *out) {
				HookSwapChain(*out, "CreateSwapChain");
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IDXGIFactory* f, IUnknown* dev, HWND w, const void* desc, const void* fs, IDXGIOutput* o, IDXGISwapChain** out)
		{
			const HRESULT hr = g_createSwapChainForHwnd(f, dev, w, desc, fs, o, out);
			if (!t_own && SUCCEEDED(hr) && out && *out) {
				HookSwapChain(*out, "CreateSwapChainForHwnd");
			}
			return hr;
		}

		void HookFactory()
		{
			const HMODULE dxgi = ::GetModuleHandleW(L"dxgi.dll");
			using FnCreateFactory = HRESULT(WINAPI*)(REFIID, void**);
			auto* create = dxgi ? reinterpret_cast<FnCreateFactory>(reinterpret_cast<void*>(::GetProcAddress(dxgi, "CreateDXGIFactory1"))) : nullptr;
			IDXGIFactory* factory = nullptr;
			if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
				Log::Error("d3d11: no DXGI factory of our own -- Present not hooked (no frame boundary)");
				return;
			}
			auto** vt = *reinterpret_cast<void***>(factory);
			Swap(vt, 10, &CreateSwapChain, g_createSwapChain, "IDXGIFactory::CreateSwapChain");
			Swap(vt, 15, &CreateSwapChainForHwnd, g_createSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
			factory->Release();
		}

		// ---- attach -----------------------------------------------------------------------------------
		void Attach(ID3D11Device* a_device, const char* a_how)
		{
			std::scoped_lock lock(g_attachLock);
			if (g_device.load() || !a_device) {
				return;
			}
			OwnCalls own;
			ID3D11DeviceContext* imm = nullptr;
			a_device->GetImmediateContext(&imm);
			if (!imm) {
				Log::Error("d3d11: device {} has no immediate context", static_cast<void*>(a_device));
				return;
			}
			ID3D11DeviceContext* def = nullptr;
			if (FAILED(a_device->CreateDeferredContext(0, &def)) || !def) {
				Log::Error("d3d11: CreateDeferredContext failed -- deferred draws not observed");
			}
			auto** devVt = *reinterpret_cast<void***>(a_device);
			auto** immVt = *reinterpret_cast<void***>(imm);
			auto** defVt = def ? *reinterpret_cast<void***>(def) : nullptr;
			Swap(devVt, kDevCreateInputLayout, &CreateInputLayout, g_createLayout, "CreateInputLayout");
			Swap(devVt, kDevCreateVertexShader, &CreateVertexShader, g_createVs, "CreateVertexShader");
			Swap(devVt, kDevCreatePixelShader, &CreatePixelShader, g_createPs, "CreatePixelShader");
			Swap(devVt, kDevCreateDeferredContext, &CreateDeferredContext, g_createDeferred, "CreateDeferredContext");
			HookContextVtable<0>(immVt);
			if (defVt && defVt != immVt) {
				HookContextVtable<1>(defVt);
			} else if (defVt) {
				g_orig[1] = g_orig[0];
			}
			if (def) {
				def->Release();
			}
			HookFactory();
			Hooks::EnableAll();
			g_immediate.store(imm, std::memory_order_release);  // the game holds its own reference
			imm->Release();
			g_device.store(a_device, std::memory_order_release);
			Log::Info("d3d11: device {} adopted ({}): immediate {} vtables dev {} imm {} def {}", static_cast<void*>(a_device), a_how, static_cast<void*>(imm),
				static_cast<void*>(devVt), static_cast<void*>(immVt), static_cast<void*>(defVt));
		}

		HRESULT WINAPI CreateDeviceDetour(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* fl, UINT nfl, UINT sdk, ID3D11Device** dev,
			D3D_FEATURE_LEVEL* outFl, ID3D11DeviceContext** ctx)
		{
			const HRESULT hr = g_createDevice(a, t, sw, flags, fl, nfl, sdk, dev, outFl, ctx);
			if (SUCCEEDED(hr) && dev && *dev && !t_own) {
				Log::Info("d3d11: D3D11CreateDevice -> {} (flags {:#x}, driver {})", static_cast<void*>(*dev), flags, static_cast<int>(t));
				try {
					Attach(*dev, "D3D11CreateDevice");
				} catch (...) {
				}
			}
			return hr;
		}
	}

	void ContextState::Reset()
	{
		layout = nullptr;
		layoutInfo = nullptr;
		std::fill(std::begin(vb), std::end(vb), VbBind{});
		ib = nullptr;
		ibFormat = ibOffset = topology = 0;
		vs = nullptr;
		vsInfo = nullptr;
		ps = nullptr;
		psInfo = nullptr;
		std::fill(std::begin(vsCb), std::end(vsCb), CbBind{});
		std::fill(std::begin(psCb), std::end(psCb), CbBind{});
		std::fill(std::begin(vsSrv), std::end(vsSrv), nullptr);
		std::fill(std::begin(psSrv), std::end(psSrv), nullptr);
		std::fill(std::begin(rtv), std::end(rtv), nullptr);
		dsv = nullptr;
		rt = {};
	}

	bool ReadConstants(const ContextState& a_state, const CbBind& a_bind, std::uint32_t a_offset, void* a_dst, std::uint32_t a_bytes)
	{
		if (!a_bind.buffer) {
			return false;
		}
		for (const auto& sh : a_state.shadows) {
			if (sh.buffer != a_bind.buffer) {
				continue;
			}
			const std::uint64_t start = std::uint64_t(a_bind.first) * 16 + a_offset;
			const std::uint64_t window = a_bind.count ? std::uint64_t(a_bind.count) * 16 : sh.bytes;
			if (a_offset + std::uint64_t(a_bytes) > window || start + a_bytes > sh.bytes) {
				return false;
			}
			if (sh.mapped) {
				std::memcpy(a_dst, sh.mapped + start, a_bytes);
				return true;
			}
			if (sh.copy.size() >= start + a_bytes) {
				std::memcpy(a_dst, sh.copy.data() + start, a_bytes);
				return true;
			}
			return false;
		}
		return false;
	}

	std::string InputLayoutInfo::Describe() const
	{
		std::string out;
		for (const auto& e : elements) {
			out += std::format("{}{}{}:{}@s{}+{}{} ", out.empty() ? "" : "", e.semantic, e.semanticIndex, FormatName(e.format), e.slot, e.offset, e.perInstance ? "(inst)" : "");
		}
		return out;
	}

	bool InstallEarly(const Callbacks& a_callbacks)
	{
		g_cb = a_callbacks;
		const HMODULE d3d11 = ::GetModuleHandleW(L"d3d11.dll");
		if (!d3d11) {
			Log::Warn("d3d11: d3d11.dll not loaded yet -- device adopted from the game's globals later");
			return false;
		}
		auto* target = reinterpret_cast<void*>(::GetProcAddress(d3d11, "D3D11CreateDevice"));
		if (!target) {
			Log::Error("d3d11: no D3D11CreateDevice export");
			return false;
		}
		return Hooks::Create("D3D11CreateDevice", target, &CreateDeviceDetour, &g_createDevice);
	}

	void Poll()
	{
		if (!g_device.load(std::memory_order_acquire)) {
			const auto dev = SafeRead<ID3D11Device*>(reinterpret_cast<const void*>(kGameDevice));
			if (dev && *dev && LooksLikeD3dObject(*dev)) {
				Attach(*dev, "game global 0x142C6B870");
			}
			return;
		}
	}

	ID3D11Device*        Device() { return g_device.load(std::memory_order_acquire); }
	ID3D11DeviceContext* Immediate() { return g_immediate.load(std::memory_order_acquire); }
	bool                 PresentHooked() { return g_presentHooked.load() && g_present; }
	std::uint64_t        Presents() { return g_presents.load(std::memory_order_relaxed); }

	const InputLayoutInfo* LayoutInfo(ID3D11InputLayout* a_layout)
	{
		if (!a_layout) {
			return nullptr;
		}
		std::shared_lock lock(g_regLock);
		const auto       it = g_layouts.find(a_layout);
		return it == g_layouts.end() ? nullptr : it->second.get();
	}

	const ShaderInfo* VertexShaderInfo(ID3D11VertexShader* a_vs)
	{
		if (!a_vs) {
			return nullptr;
		}
		std::shared_lock lock(g_regLock);
		const auto       it = g_shaders.find(a_vs);
		return it == g_shaders.end() ? nullptr : it->second.get();
	}

	const ShaderInfo* PixelShaderInfo(ID3D11PixelShader* a_ps)
	{
		if (!a_ps) {
			return nullptr;
		}
		std::shared_lock lock(g_regLock);
		const auto       it = g_shaders.find(a_ps);
		return it == g_shaders.end() ? nullptr : it->second.get();
	}

	bool SrvTexture(ID3D11ShaderResourceView* a_srv, ID3D11Texture2D** a_tex, D3D11_TEXTURE2D_DESC* a_desc)
	{
		if (!a_srv) {
			return false;
		}
		ID3D11Resource* res = nullptr;
		a_srv->GetResource(&res);
		if (!res) {
			return false;
		}
		D3D11_RESOURCE_DIMENSION dim{};
		res->GetType(&dim);
		const bool ok = dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D;
		if (ok) {
			auto* tex = static_cast<ID3D11Texture2D*>(res);
			if (a_desc) {
				tex->GetDesc(a_desc);
			}
			if (a_tex) {
				*a_tex = tex;
			}
		}
		res->Release();  // the view (and the game) keep it alive
		return ok;
	}

	Stats Counters()
	{
		Stats s;
		s.draws = g_stats.draws.load();
		s.drawsDeferred = g_stats.drawsDeferred.load();
		s.maps = g_stats.maps.load();
		s.cbMaps = g_stats.cbMaps.load();
		s.cbUpdates = g_stats.cbUpdates.load();
		s.executes = g_stats.executes.load();
		s.finishes = g_stats.finishes.load();
		s.layouts = g_stats.layouts.load();
		s.vertexShaders = g_stats.vertexShaders.load();
		s.pixelShaders = g_stats.pixelShaders.load();
		s.contexts = g_stats.contexts.load();
		return s;
	}

	OwnCalls::OwnCalls() { ++t_own; }
	OwnCalls::~OwnCalls() { --t_own; }

	const char* FormatName(std::uint32_t a_f)
	{
		switch (a_f) {
		case 0: return "UNKNOWN";
		case 2: return "RGBA32F";
		case 3: return "RGBA32U";
		case 6: return "RGB32F";
		case 10: return "RGBA16F";
		case 11: return "RGBA16UN";
		case 12: return "RGBA16U";
		case 13: return "RGBA16SN";
		case 14: return "RGBA16I";
		case 16: return "RG32F";
		case 20: return "D32S8X24";
		case 24: return "RGB10A2UN";
		case 26: return "R11G11B10F";
		case 28: return "RGBA8UN";
		case 29: return "RGBA8UN_SRGB";
		case 30: return "RGBA8U";
		case 31: return "RGBA8SN";
		case 32: return "RGBA8I";
		case 34: return "RG16F";
		case 35: return "RG16UN";
		case 36: return "RG16U";
		case 37: return "RG16SN";
		case 39: return "R32TYPELESS";
		case 40: return "D32F";
		case 41: return "R32F";
		case 42: return "R32U";
		case 44: return "R24G8TYPELESS";
		case 45: return "D24S8";
		case 49: return "RG8UN";
		case 54: return "R16F";
		case 56: return "R16UN";
		case 57: return "R16U";
		case 61: return "R8UN";
		case 71: return "BC1";
		case 72: return "BC1_SRGB";
		case 74: return "BC2";
		case 75: return "BC2_SRGB";
		case 77: return "BC3";
		case 78: return "BC3_SRGB";
		case 80: return "BC4";
		case 81: return "BC4SN";
		case 83: return "BC5";
		case 84: return "BC5SN";
		case 87: return "BGRA8UN";
		case 91: return "BGRA8UN_SRGB";
		case 95: return "BC6H_UF16";
		case 96: return "BC6H_SF16";
		case 98: return "BC7";
		case 99: return "BC7_SRGB";
		default: return "fmt?";
		}
	}
}
