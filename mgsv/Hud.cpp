#include "Hud.h"

#include "Addresses.h"
#include "Config.h"
#include "Hook.h"
#include "Link.h"
#include "Log.h"
#include "Logic.h"
#include "Mapping.h"
#include "State.h"
#include "Verify.h"
#include "phantomring_protocol.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace phantom::guest::capture::hud
{
	namespace
	{
		// MGSV's UI job executor (MGS5VR ui_renderer.cpp hooks the same function on this exe):
		// mov rax,rsp; push rdi; sub rsp,0x680  -- re-read from the local 1.0.15.4 exe.
		constexpr addr::Site kUiExecute{ 0x1402E7770, { 0x48, 0x8b, 0xc4, 0x57, 0x48, 0x81, 0xec, 0x80, 0x06, 0x00, 0x00 } };

		using ExecuteFn = std::uintptr_t (*)(void*, void*, void*, std::uint32_t);
		ExecuteFn g_execute{ nullptr };

		thread_local int t_ui = 0;

		std::atomic<bool>          g_hooked{ false };
		std::atomic<bool>          g_enabled{ true };
		std::atomic<bool>          g_failed{ false };
		std::atomic<int>           g_every{ 1 };
		std::atomic<std::uint64_t> g_skipPs{ 0 };  // dev: `!capture hud skip <ps hash>` leaves those UI draws out (0 = none)
		std::atomic<std::uint64_t> g_skippedPs{ 0 };
		// The night-vision overlay: the one UI pixel shader whose draws covered the whole layer with the goggles on (VERIFIED live
		// 2026-10-13: layer coverage 100% with them on, 4.6% -- weapon box, item box, prompts -- with this shader left out, 2.2%
		// with them off). Its textures (g_ReflectionTexture, g_tex_mesh) say it is a filter, not a HUD element; it is also drawn
		// with the goggles off and is kept then.
		constexpr std::uint64_t    kNvgOverlayPs = 0xae282a7fe2bdbdd9;
		std::atomic<bool>          g_nightVision{ false };
		std::atomic<std::uint64_t> g_nvgLeftOut{ 0 };
		std::atomic<std::uint64_t> g_jobs{ 0 }, g_uiDraws{ 0 }, g_mirrored{ 0 }, g_skippedTarget{ 0 }, g_windowDraws{ 0 }, g_published{ 0 }, g_skippedEmpty{ 0 },
			g_notReady{ 0 };

		std::uintptr_t Execute(void* a_renderer, void* a_info, void* a_task, std::uint32_t a_worker)
		{
			++t_ui;
			g_jobs.fetch_add(1, std::memory_order_relaxed);
			const auto r = g_execute(a_renderer, a_info, a_task, a_worker);
			--t_ui;
			return r;
		}

		void Fail(const std::string& a_why)
		{
			if (!g_failed.exchange(true)) {
				Log::Error("hud: {} -- HUD layer off", a_why);
			}
		}

		bool Rgba8(std::uint32_t a_f) { return a_f == DXGI_FORMAT_R8G8B8A8_UNORM || a_f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
		bool Bgra8(std::uint32_t a_f) { return a_f == DXGI_FORMAT_B8G8R8A8_UNORM || a_f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }
		bool Srgb(std::uint32_t a_f) { return a_f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || a_f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }

		// ---- our render target (one live; replaced ones are kept a while for command lists in flight) ----
		constexpr int kStaging = 3;
		struct Target
		{
			std::uint32_t            width{ 0 }, height{ 0 };
			std::uint32_t            rtvFormat{ 0 };
			ID3D11Texture2D*         tex{ nullptr };
			ID3D11RenderTargetView*  rtv{ nullptr };
			ID3D11Texture2D*         staging[kStaging]{};
			std::uint64_t            copiedAt[kStaging]{};      // present number, 0 = free
			std::uint64_t            drawsAt[kStaging]{};       // mirrored draws in that frame's window
			std::uint64_t            frameAt[kStaging]{};
			std::uint64_t            retiredAt{ 0 };

			void Release()
			{
				for (auto*& s : staging) {
					if (s) {
						s->Release();
						s = nullptr;
					}
				}
				if (rtv) {
					rtv->Release();
					rtv = nullptr;
				}
				if (tex) {
					tex->Release();
					tex = nullptr;
				}
			}
		};
		std::atomic<Target*> g_target{ nullptr };
		std::mutex           g_targetLock;  // creation / retirement
		std::vector<Target*> g_retired;     // released some presents after retirement
		std::atomic<std::uint64_t> g_present{ 0 };

		// The main UI target's size: the largest 8-bit colour target UI draws went to (iDroid pages and
		// map tiles may render into smaller offscreen textures first; those aren't the HUD itself).
		std::atomic<std::uint64_t> g_mainSize{ 0 };  // w << 32 | h
		// UI draws since the last present that matched g_mainSize / were skipped for their size: a
		// latched size nothing draws to any more decays (logic::MainSizeDecay).
		std::atomic<std::uint32_t> g_mainMatched{ 0 }, g_sizeSkipped{ 0 };
		logic::MainSizeDecay       g_mainDecay;  // OnPresent only

		Target* Create(ID3D11Device* a_dev, std::uint32_t a_w, std::uint32_t a_h, std::uint32_t a_rtvFormat)
		{
			auto* t = new Target;
			t->width = a_w;
			t->height = a_h;
			t->rtvFormat = a_rtvFormat;
			D3D11_TEXTURE2D_DESC d{};
			d.Width = a_w;
			d.Height = a_h;
			d.MipLevels = 1;
			d.ArraySize = 1;
			d.Format = Rgba8(a_rtvFormat) ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_B8G8R8A8_TYPELESS;
			d.SampleDesc.Count = 1;
			d.Usage = D3D11_USAGE_DEFAULT;
			d.BindFlags = D3D11_BIND_RENDER_TARGET;
			bool ok = SUCCEEDED(a_dev->CreateTexture2D(&d, nullptr, &t->tex)) && t->tex;
			if (ok) {
				D3D11_RENDER_TARGET_VIEW_DESC rd{};
				rd.Format = static_cast<DXGI_FORMAT>(a_rtvFormat);
				rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				ok = SUCCEEDED(a_dev->CreateRenderTargetView(t->tex, &rd, &t->rtv)) && t->rtv;
			}
			D3D11_TEXTURE2D_DESC sd = d;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			for (int i = 0; ok && i < kStaging; ++i) {
				ok = SUCCEEDED(a_dev->CreateTexture2D(&sd, nullptr, &t->staging[i])) && t->staging[i];
			}
			if (!ok) {
				t->Release();
				delete t;
				return nullptr;
			}
			Log::Info("hud: layer target {}x{} {} created", a_w, a_h, d3d::FormatName(a_rtvFormat));
			return t;
		}

		// Recording thread: the target for a UI draw into a game target of this size/format (nullptr: none).
		Target* TargetFor(std::uint32_t a_w, std::uint32_t a_h, std::uint32_t a_format)
		{
			auto* t = g_target.load(std::memory_order_acquire);
			if (t && t->width == a_w && t->height == a_h && t->rtvFormat == a_format) {
				return t;
			}
			auto* dev = d3d::Device();
			if (!dev || g_failed.load()) {
				return nullptr;
			}
			std::scoped_lock lock(g_targetLock);
			t = g_target.load(std::memory_order_acquire);
			if (t && t->width == a_w && t->height == a_h && t->rtvFormat == a_format) {
				return t;
			}
			d3d::OwnCalls own;
			auto*         fresh = Create(dev, a_w, a_h, a_format);
			if (!fresh) {
				Fail(std::format("could not create a {}x{} {} target", a_w, a_h, d3d::FormatName(a_format)));
				return nullptr;
			}
			if (t) {
				t->retiredAt = g_present.load();
				g_retired.push_back(t);
			}
			g_target.store(fresh, std::memory_order_release);
			return fresh;
		}

		// ---- blend states: the game's colour blending, coverage accumulated in alpha ------------------
		std::shared_mutex                                        g_blendLock;
		std::unordered_map<std::uint64_t, ID3D11BlendState*>     g_blends;  // by the game's desc

		ID3D11BlendState* Derived(ID3D11BlendState* a_game)
		{
			D3D11_BLEND_DESC d{};
			if (a_game) {
				a_game->GetDesc(&d);
			} else {
				d.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
				d.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
				d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
				d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
				d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
				d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
				d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			}
			const auto key = d3d::Hash64(&d, sizeof(d));
			{
				std::shared_lock lock(g_blendLock);
				if (const auto it = g_blends.find(key); it != g_blends.end()) {
					return it->second;
				}
			}
			D3D11_BLEND_DESC ours{};
			ours.AlphaToCoverageEnable = FALSE;
			ours.IndependentBlendEnable = FALSE;
			auto r = d.RenderTarget[0];
			if (r.BlendEnable) {
				// Alpha = coverage, so the layer composites back exactly: "over" for normal blending, replace
				// for opaque-in-blend, unchanged for additive/multiplicative (light that adds, no coverage).
				switch (r.DestBlend) {
				case D3D11_BLEND_INV_SRC_ALPHA:
					r.SrcBlendAlpha = D3D11_BLEND_ONE;
					r.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
					break;
				case D3D11_BLEND_ZERO:
					r.SrcBlendAlpha = D3D11_BLEND_ONE;
					r.DestBlendAlpha = D3D11_BLEND_ZERO;
					break;
				default:
					r.SrcBlendAlpha = D3D11_BLEND_ZERO;
					r.DestBlendAlpha = D3D11_BLEND_ONE;
					break;
				}
				r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			}
			if (r.RenderTargetWriteMask & 7) {
				r.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
			}
			ours.RenderTarget[0] = r;
			ID3D11BlendState* bs = nullptr;
			{
				d3d::OwnCalls own;
				if (auto* dev = d3d::Device(); !dev || FAILED(dev->CreateBlendState(&ours, &bs))) {
					bs = nullptr;
				}
			}
			if (!bs) {
				Fail("CreateBlendState failed");
				return nullptr;
			}
			std::unique_lock lock(g_blendLock);
			auto [it, fresh] = g_blends.try_emplace(key, bs);
			if (!fresh) {
				bs->Release();
			}
			return it->second;
		}

		void Issue(ID3D11DeviceContext* a_c, const d3d::DrawArgs& a_a)
		{
			if (a_a.indexed) {
				if (a_a.instances != 1 || a_a.startInstance) {
					a_c->DrawIndexedInstanced(a_a.indexCount, a_a.instances, a_a.startIndex, a_a.baseVertex, a_a.startInstance);
				} else {
					a_c->DrawIndexed(a_a.indexCount, a_a.startIndex, a_a.baseVertex);
				}
			} else if (a_a.instances != 1 || a_a.startInstance) {
				a_c->DrawInstanced(a_a.indexCount, a_a.instances, a_a.startIndex, a_a.startInstance);
			} else {
				a_c->Draw(a_a.indexCount, a_a.startIndex);
			}
		}

		// ---- publishing -------------------------------------------------------------------------------
		std::uint32_t g_back{ 1 };
		std::uint32_t g_gen{ ~0u };
		std::uint64_t g_prevWindow{ 0 };
		bool          g_lastEmpty{ false };

		void Publish(const Target& a_t, const D3D11_MAPPED_SUBRESOURCE& a_ms, std::uint64_t a_frame)
		{
			std::unique_lock<std::mutex> lock;
			auto*                        base = link::LockRender(lock);
			if (!base) {
				return;
			}
			if (const auto gen = link::AttachGeneration(); gen != g_gen) {
				g_gen = gen;  // Link reset the HUD triple state on attach: middle 0, ours 1, the host's 2
				g_back = 1;
			}
			auto*       ctl = reinterpret_cast<proto::HudCtl*>(base + proto::kOffHudCtl);
			auto*       hdrs = reinterpret_cast<proto::HudSlotHdr*>(base + proto::kOffHudCtl + 0x40);
			auto*       px = base + proto::kOffHudSlots + std::uint64_t(g_back) * proto::kHudSlotBytes;
			const auto  w = std::min(a_t.width, proto::kMaxHudW), h = std::min(a_t.height, proto::kMaxHudH);
			const auto* src = static_cast<const std::uint8_t*>(a_ms.pData);
			const bool  swizzle = Bgra8(a_t.rtvFormat);
			for (std::uint32_t y = 0; y < h; ++y) {
				const auto* row = src + std::size_t(y) * a_ms.RowPitch;
				auto*       out = px + std::size_t(y) * w * 4;
				if (!swizzle) {
					std::memcpy(out, row, std::size_t(w) * 4);
				} else {
					for (std::uint32_t x = 0; x < w; ++x) {
						out[x * 4 + 0] = row[x * 4 + 2];
						out[x * 4 + 1] = row[x * 4 + 1];
						out[x * 4 + 2] = row[x * 4 + 0];
						out[x * 4 + 3] = row[x * 4 + 3];
					}
				}
			}
			proto::HudSlotHdr hdr{};
			hdr.width = w;
			hdr.height = h;
			hdr.flags = kHudPremultiplied | (Srgb(a_t.rtvFormat) ? kHudSrgb : 0u);
			hdr.frame = a_frame;
			hdrs[g_back] = hdr;
			const auto old = Atomic(ctl->state).exchange(g_back | kTripleDirty, std::memory_order_acq_rel);
			g_back = old & 3;
			Atomic(ctl->framesPublished).fetch_add(1, std::memory_order_relaxed);
			g_published.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void InstallEarly()
	{
		if (!Matches(kUiExecute, "UI job executor")) {
			Log::Warn("hud: UI job executor unverified -- no HUD layer");
			return;
		}
		if (!Hooks::Create("UiJobExecute", reinterpret_cast<void*>(kUiExecute.va), &Execute, &g_execute)) {
			return;
		}
		g_hooked.store(true);
		g_enabled.store(Cfg().hud);
		g_every.store(std::max(1, Cfg().hudEvery));
	}

	bool InUiJob() { return t_ui > 0; }

	void OnDraw(d3d::ContextState& a_s, const d3d::DrawArgs& a_a)
	{
		if (t_ui <= 0) {
			return;
		}
		g_uiDraws.fetch_add(1, std::memory_order_relaxed);
		if (!g_enabled.load(std::memory_order_relaxed) || g_failed.load(std::memory_order_relaxed) || !link::HostAlive()) {
			return;
		}
		const auto& rt = a_s.rt;
		if (rt.count < 1 || !a_s.rtv[0] || !(Rgba8(rt.formats[0]) || Bgra8(rt.formats[0])) || rt.width < 320 || rt.height < 180 || rt.width > proto::kMaxHudW ||
		    rt.height > proto::kMaxHudH) {
			g_skippedTarget.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const std::uint64_t size = std::uint64_t(rt.width) << 32 | rt.height;
		auto                main = g_mainSize.load(std::memory_order_relaxed);
		if (std::uint64_t(rt.width) * rt.height > (main >> 32) * (main & 0xFFFFFFFF)) {
			g_mainSize.store(size, std::memory_order_relaxed);
			main = size;
		}
		if (size != main) {
			g_skippedTarget.fetch_add(1, std::memory_order_relaxed);
			g_sizeSkipped.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		g_mainMatched.fetch_add(1, std::memory_order_relaxed);
		if (const auto skip = g_skipPs.load(std::memory_order_relaxed); skip && a_s.psInfo && a_s.psInfo->hash == skip) {
			g_skippedPs.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		if (g_nightVision.load(std::memory_order_relaxed) && a_s.psInfo && a_s.psInfo->hash == kNvgOverlayPs) {
			g_nvgLeftOut.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		auto* t = TargetFor(rt.width, rt.height, rt.formats[0]);
		if (!t) {
			return;
		}
		d3d::OwnCalls     own;
		auto*             c = a_s.ctx;
		ID3D11BlendState* bs = nullptr;
		FLOAT             factor[4]{};
		UINT              mask = 0xFFFFFFFF;
		c->OMGetBlendState(&bs, factor, &mask);
		auto* ours = Derived(bs);
		if (ours || !bs) {
			ID3D11RenderTargetView* rtv = t->rtv;
			c->OMSetRenderTargets(1, &rtv, nullptr);
			c->OMSetBlendState(ours, factor, mask);
			Issue(c, a_a);
			c->OMSetRenderTargets(rt.count, a_s.rtv, a_s.dsv);
			c->OMSetBlendState(bs, factor, mask);
			g_mirrored.fetch_add(1, std::memory_order_relaxed);
			g_windowDraws.fetch_add(1, std::memory_order_relaxed);
		}
		if (bs) {
			bs->Release();
		}
	}

	void OnPresent(ID3D11Device* a_dev, ID3D11DeviceContext* a_imm)
	{
		const auto present = g_present.fetch_add(1) + 1;
		const bool matched = g_mainMatched.exchange(0, std::memory_order_relaxed) != 0;
		if (g_mainDecay.Present(matched, g_sizeSkipped.exchange(0, std::memory_order_relaxed) != 0)) {
			const auto old = g_mainSize.exchange(0);
			Log::Info("hud: no UI draw went to the {}x{} target for {} presents -- adopting the current one", old >> 32, old & 0xFFFFFFFF,
				logic::MainSizeDecay::kPresents);
		}
		auto*      t = g_target.load(std::memory_order_acquire);
		if (!t || !a_dev || !a_imm) {
			return;
		}
		d3d::OwnCalls own;
		{
			// Targets replaced a while ago: command lists that drew into them have long executed.
			std::scoped_lock lock(g_targetLock);
			std::erase_if(g_retired, [&](Target* r) {
				if (present - r->retiredAt < 16) {
					return false;
				}
				r->Release();
				delete r;
				return true;
			});
		}
		// 1. The oldest finished copy goes out (non-blocking; a copy still in flight waits for the next Present).
		int oldest = -1;
		for (int i = 0; i < kStaging; ++i) {
			if (t->copiedAt[i] && (oldest < 0 || t->copiedAt[i] < t->copiedAt[oldest])) {
				oldest = i;
			}
		}
		if (oldest >= 0) {
			D3D11_MAPPED_SUBRESOURCE ms{};
			const HRESULT            hr = a_imm->Map(t->staging[oldest], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
			if (SUCCEEDED(hr) && ms.pData) {
				// Two empty copy windows in a row: the HUD is surely empty, and the host already has an empty one.
				const bool empty = t->drawsAt[oldest] == 0;
				if (empty && g_lastEmpty) {
					g_skippedEmpty.fetch_add(1, std::memory_order_relaxed);
				} else {
					Publish(*t, ms, t->frameAt[oldest]);
				}
				g_lastEmpty = empty;
				a_imm->Unmap(t->staging[oldest], 0);
				t->copiedAt[oldest] = 0;
			} else if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
				g_notReady.fetch_add(1, std::memory_order_relaxed);
				if (present - t->copiedAt[oldest] > 30) {
					t->copiedAt[oldest] = 0;  // lost: free the slot
				}
			} else {
				Fail(std::format("Map of the HUD staging copy failed ({:#x})", static_cast<std::uint32_t>(hr)));
				return;
			}
		}
		// 2. This frame's layer -> a free staging texture, then clear the target for the next frame.
		const auto window = g_windowDraws.exchange(0);
		if (present % static_cast<std::uint64_t>(g_every.load()) == 0) {
			for (int i = 0; i < kStaging; ++i) {
				if (!t->copiedAt[i]) {
					a_imm->CopyResource(t->staging[i], t->tex);
					t->copiedAt[i] = present;
					// Draws of this frame may have been recorded in the previous window too (deferred contexts).
					t->drawsAt[i] = window + g_prevWindow;
					t->frameAt[i] = state::Frame().frame;
					break;
				}
			}
		}
		g_prevWindow = window;
		static const FLOAT kClear[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
		a_imm->ClearRenderTargetView(t->rtv, kClear);
	}

	void SetNightVision(bool a_on) { g_nightVision.store(a_on, std::memory_order_relaxed); }

	std::string Command(std::string_view a_args)
	{
		while (!a_args.empty() && a_args.front() == ' ') {
			a_args.remove_prefix(1);
		}
		if (a_args == "on" || a_args == "off") {
			g_enabled.store(a_args == "on");
		} else if (a_args.starts_with("every")) {
			g_every.store(std::max(1, std::atoi(std::string(a_args.substr(5)).c_str())));
		} else if (a_args.starts_with("skip")) {
			g_skipPs.store(std::strtoull(std::string(a_args.substr(4)).c_str(), nullptr, 16));
			g_skippedPs.store(0);
		} else if (a_args == "reset") {
			g_failed.store(false);
			g_mainSize.store(0);
		}
		return Describe();
	}

	std::string Describe()
	{
		const auto* t = g_target.load(std::memory_order_acquire);
		const auto  main = g_mainSize.load();
		return std::format("hud: hooked {} enabled {} failed {} every {}; ui jobs {} ui draws {} mirrored {} (other targets {}); main target {}x{}; layer {}; "
		                   "published {} (empty skipped {}, copies not ready {}); night vision {} ({} overlay draws left out); dev skip ps {:#x} ({} left out)",
			g_hooked.load(), g_enabled.load(), g_failed.load(), g_every.load(), g_jobs.load(), g_uiDraws.load(), g_mirrored.load(), g_skippedTarget.load(), main >> 32,
			main & 0xFFFFFFFF, t ? std::format("{}x{} {}", t->width, t->height, d3d::FormatName(t->rtvFormat)) : std::string("none"), g_published.load(),
			g_skippedEmpty.load(), g_notReady.load(), g_nightVision.load() ? "on" : "off", g_nvgLeftOut.load(), g_skipPs.load(), g_skippedPs.load());
	}

	std::string ProbeLine(const d3d::ContextState& a_s, const d3d::DrawArgs& a_a)
	{
		ID3D11Resource* res = nullptr;
		if (a_s.rtv[0]) {
			a_s.rtv[0]->GetResource(&res);
			if (res) {
				res->Release();  // bound: the game keeps it alive
			}
		}
		std::string blend = "default";
		{
			d3d::OwnCalls     own;
			ID3D11BlendState* bs = nullptr;
			FLOAT             f[4]{};
			UINT              m = 0;
			a_s.ctx->OMGetBlendState(&bs, f, &m);
			if (bs) {
				D3D11_BLEND_DESC d{};
				bs->GetDesc(&d);
				const auto& r = d.RenderTarget[0];
				blend = std::format("en {} c {}/{}/{} a {}/{}/{} mask {:#x}", static_cast<int>(r.BlendEnable), static_cast<int>(r.SrcBlend), static_cast<int>(r.DestBlend),
					static_cast<int>(r.BlendOp), static_cast<int>(r.SrcBlendAlpha), static_cast<int>(r.DestBlendAlpha), static_cast<int>(r.BlendOpAlpha),
					static_cast<int>(r.RenderTargetWriteMask));
				bs->Release();
			}
		}
		std::string tex;
		if (a_s.psInfo) {
			for (unsigned i = 0; i < 4; ++i) {
				if (const auto* r = a_s.psInfo->TextureAt(i)) {
					tex += std::format("{}t{}:{} ", tex.empty() ? "" : "", i, r->name);
					// Which texture: a full-screen overlay (night vision noise) shows by its texture, not its shader.
					ID3D11Texture2D*     t = nullptr;
					D3D11_TEXTURE2D_DESC d{};
					if (d3d::SrvTexture(a_s.psSrv[i], &t, &d)) {
						tex += std::format("[{} {}x{} {}] ", static_cast<void*>(t), d.Width, d.Height, d3d::FormatName(d.Format));  // not ours to release
					}
				}
			}
		}
		return std::format("  UI tid {} ctx#{}{} rt {} {}x{} {} n{} dsv {} {} {} inst {} vs {:#018x} ps {:#018x} blend [{}] {}\n", ::GetCurrentThreadId(), a_s.index,
			a_s.deferred ? "d" : "i", static_cast<void*>(res), a_s.rt.width, a_s.rt.height, d3d::FormatName(a_s.rt.formats[0]), a_s.rt.count, a_s.dsv != nullptr,
			a_a.indexed ? "idx" : "plain", a_a.indexCount, a_a.instances, a_s.vsInfo ? a_s.vsInfo->hash : 0, a_s.psInfo ? a_s.psInfo->hash : 0, blend, tex);
	}
}
