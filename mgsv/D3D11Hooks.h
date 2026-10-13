#pragma once
// MGSV's Direct3D 11 device (DXVK under Proton), seen from the inside: which pipeline state the game
// has bound on each context when it draws, what it wrote into its constant buffers, which shaders
// and input layouts it created. Capture.cpp decides what to keep; this file only observes.
//
// How we get in (re-backlog M4, MGS5VR scene_capture.cpp):
//  - d3d11.dll!D3D11CreateDevice is detoured from DllMain (MGSV imports it statically, so d3d11 is
//    mapped before our DllMain runs); the game stores the result at 0x142C6B870/78 (tpp-mod fact,
//    verified against the device-creation code 0x1419F4EB0) -- polled as a fallback.
//  - Hooks are MinHook detours on the DXVK functions the vtables point to (the device, the immediate
//    context and a deferred context we create once give the three vtables). NOT vtable slot swaps:
//    MGSV quits silently (exit code 0) about a second after device creation when its D3D11 vtables are
//    modified (VERIFIED live 2026-10-12); MGS5VR also detours the functions. Fox records its scene on
//    24 DEFERRED contexts (worker threads) and executes the command lists on the immediate context, so
//    draws are observed while they are recorded, on whatever thread records them.
//  - MGSV keeps its DXGI factory at 0x142C6B880 and its adapter at 0x142C6B888 (not the swap chain);
//    the swap chain comes from the factory later, so DXVK's CreateSwapChain* are detoured too, and the
//    new swap chain's Present gives the frame boundary and the one place where we may use the immediate
//    context ourselves (staging copies).
// Our own D3D calls go through the same detoured functions; an OwnCalls guard on the calling thread
// makes every hook pass straight through.

#include "D3D11Reflect.h"

#include <d3d11_1.h>
#include <dxgi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace phantom::guest::d3d
{
	inline constexpr unsigned kVsCbSlots = 14;
	inline constexpr unsigned kPsCbSlots = 14;
	inline constexpr unsigned kPsSrvSlots = 16;
	inline constexpr unsigned kVsSrvSlots = 8;
	inline constexpr unsigned kVbSlots = 4;
	inline constexpr unsigned kRtSlots = 8;

	struct InputElement
	{
		std::string   semantic;  // as MGSV named it
		std::uint32_t semanticIndex{ 0 };
		std::uint32_t format{ 0 };  // DXGI_FORMAT
		std::uint32_t slot{ 0 };
		std::uint32_t offset{ 0 };  // resolved (D3D11_APPEND_ALIGNED_ELEMENT replaced by the real offset)
		bool          perInstance{ false };
	};

	struct InputLayoutInfo
	{
		std::vector<InputElement> elements;
		std::uint64_t             hash{ 0 };  // of the element list
		bool                      skinned{ false };  // has blend indices + weights (any naming)
		std::string               Describe() const;
	};

	struct CbBind
	{
		ID3D11Buffer* buffer{ nullptr };
		std::uint32_t first{ 0 };    // in 16-byte constants (VSSetConstantBuffers1), 0 for plain binds
		std::uint32_t count{ 0 };    // 0 = the whole buffer
	};

	struct VbBind
	{
		ID3D11Buffer* buffer{ nullptr };
		std::uint32_t stride{ 0 };
		std::uint32_t offset{ 0 };
	};

	// The CPU-side contents last written into a constant buffer on one context (Map/Unmap or
	// UpdateSubresource). For Map we keep the pointer DXVK returned: it stays valid host-visible
	// memory at least until that context's command list executes, i.e. for the whole recording.
	struct CbShadow
	{
		ID3D11Buffer*            buffer{ nullptr };
		const std::uint8_t*      mapped{ nullptr };  // Map: base of the buffer's current slice
		std::uint32_t            bytes{ 0 };
		std::vector<std::uint8_t> copy;               // UpdateSubresource: our copy
		std::uint64_t            serial{ 0 };         // LRU
	};

	struct RtInfo
	{
		std::uint32_t count{ 0 };              // bound render targets
		std::uint32_t formats[kRtSlots]{};      // DXGI_FORMAT of each RTV
		std::uint32_t depthFormat{ 0 };         // of the DSV, 0 = none
		std::uint32_t width{ 0 }, height{ 0 };  // of target 0 (or the DSV)
		std::uint64_t signature{ 0 };           // formats + size: identifies a pass's target set
	};

	struct ContextState
	{
		ID3D11DeviceContext*   ctx{ nullptr };
		bool                   deferred{ false };
		std::uint32_t          index{ 0 };  // stable small id for logs

		ID3D11InputLayout*     layout{ nullptr };
		const InputLayoutInfo* layoutInfo{ nullptr };
		VbBind                 vb[kVbSlots]{};
		ID3D11Buffer*          ib{ nullptr };
		std::uint32_t          ibFormat{ 0 };
		std::uint32_t          ibOffset{ 0 };
		std::uint32_t          topology{ 0 };
		ID3D11VertexShader*    vs{ nullptr };
		const ShaderInfo*      vsInfo{ nullptr };
		ID3D11PixelShader*     ps{ nullptr };
		const ShaderInfo*      psInfo{ nullptr };
		CbBind                 vsCb[kVsCbSlots]{};
		CbBind                 psCb[kPsCbSlots]{};
		ID3D11ShaderResourceView* vsSrv[kVsSrvSlots]{};
		ID3D11ShaderResourceView* psSrv[kPsSrvSlots]{};
		ID3D11RenderTargetView*   rtv[kRtSlots]{};
		ID3D11DepthStencilView*   dsv{ nullptr };
		RtInfo                    rt{};

		std::vector<CbShadow> shadows;  // small LRU of written constant buffers
		std::uint64_t         shadowSerial{ 0 };

		void Reset();  // ClearState / FinishCommandList(FALSE)
	};

	struct DrawArgs
	{
		std::uint32_t indexCount{ 0 };  // vertex count for non-indexed draws
		std::uint32_t startIndex{ 0 };
		std::int32_t  baseVertex{ 0 };
		std::uint32_t instances{ 1 };
		bool          indexed{ true };
		std::uint32_t startInstance{ 0 };  // so a draw can be replayed exactly (Hud.cpp)
	};

	// Copies what the shader sees at constant-buffer bind `a_bind`, bytes [a_offset, a_offset + a_bytes)
	// of its window, from the context's shadow. False when we never saw the buffer written on this context.
	bool ReadConstants(const ContextState& a_state, const CbBind& a_bind, std::uint32_t a_offset, void* a_dst, std::uint32_t a_bytes);

	struct Callbacks
	{
		void (*draw)(ContextState& a_state, const DrawArgs& a_args){ nullptr };
		// Before the game's Present, on the presenting thread, which owns the immediate context.
		void (*present)(ID3D11DeviceContext* a_immediate){ nullptr };
		void (*execute)(ID3D11DeviceContext* a_immediate, ID3D11CommandList* a_list){ nullptr };
		void (*finish)(ContextState& a_state, ID3D11CommandList* a_list){ nullptr };
		void (*setTargets)(ContextState& a_state){ nullptr };
	};

	// DllMain: detour D3D11CreateDevice (created disabled; Hooks::EnableAll turns it on).
	bool InstallEarly(const Callbacks& a_callbacks);
	// Maintenance thread, ~1 Hz: adopt the device from the game's globals if the detour missed it, and
	// find the swap chain for Present.
	void Poll();

	ID3D11Device*        Device();
	ID3D11DeviceContext* Immediate();
	bool                 PresentHooked();
	std::uint64_t        Presents();

	const InputLayoutInfo* LayoutInfo(ID3D11InputLayout* a_layout);
	const ShaderInfo*      VertexShaderInfo(ID3D11VertexShader* a_vs);
	const ShaderInfo*      PixelShaderInfo(ID3D11PixelShader* a_ps);
	// The texture behind an SRV (no reference added) and its description; false for non-2D views.
	bool SrvTexture(ID3D11ShaderResourceView* a_srv, ID3D11Texture2D** a_tex, D3D11_TEXTURE2D_DESC* a_desc);

	// Counters for the log / console.
	struct Stats
	{
		std::uint64_t draws{ 0 }, drawsDeferred{ 0 }, maps{ 0 }, cbMaps{ 0 }, cbUpdates{ 0 }, executes{ 0 }, finishes{ 0 }, layouts{ 0 }, vertexShaders{ 0 },
			pixelShaders{ 0 }, contexts{ 0 };
	};
	Stats Counters();

	// While alive on a thread, every hook on that thread passes straight through (our own D3D calls).
	class OwnCalls
	{
	public:
		OwnCalls();
		~OwnCalls();
		OwnCalls(const OwnCalls&) = delete;
		OwnCalls& operator=(const OwnCalls&) = delete;
	};

	const char* FormatName(std::uint32_t a_dxgiFormat);
}
