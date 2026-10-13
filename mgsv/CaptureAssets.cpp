#include "CaptureAssets.h"

#include "D3D11Hooks.h"
#include "Log.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <unordered_map>

namespace phantom::guest::capture::assets
{
	namespace
	{
		// Budget per Present: MGSV must keep its frame rate while Snake's ~60 MB of textures stream in.
		constexpr int           kMaxStartsPerPresent = 4;
		constexpr std::uint64_t kMaxBytesPerPresent = 16ull << 20;
		constexpr std::uint64_t kMaxBufferBytes = 64ull << 20;
		constexpr std::uint64_t kEvictAfterPresents = 60 * 60;  // a minute unused

		enum class St
		{
			kQueued,
			kCopying,
			kReady,
			kFailed,
		};

		struct Entry
		{
			ID3D11Resource*      src{ nullptr };
			bool                 texture{ false };
			St                   st{ St::kQueued };
			ID3D11Resource*      staging{ nullptr };
			std::uint64_t        started{ 0 };
			std::uint64_t        lastUse{ 0 };
			std::uint64_t        bytes{ 0 };
			D3D11_TEXTURE2D_DESC tdesc{};
			std::shared_ptr<const std::vector<std::uint8_t>> buf;
			std::shared_ptr<const TextureBytes>              tex;
		};

		std::mutex                                    g_lock;
		std::unordered_map<ID3D11Resource*, Entry>    g_entries;  // node-based: Entry addresses are stable until erased (Present thread only)
		std::deque<ID3D11Resource*>                   g_queue;
		std::atomic<std::uint64_t>                    g_present{ 0 };
		std::atomic<std::uint64_t>                    g_copied{ 0 }, g_copiedBytes{ 0 }, g_failed{ 0 }, g_waits{ 0 };

		Entry& Ensure(ID3D11Resource* a_res, bool a_texture)
		{
			auto [it, fresh] = g_entries.try_emplace(a_res);
			if (fresh) {
				a_res->AddRef();  // keeps the address ours while cached
				it->second.src = a_res;
				it->second.texture = a_texture;
				g_queue.push_back(a_res);
			}
			it->second.lastUse = g_present.load(std::memory_order_relaxed);
			return it->second;
		}

		bool BlockFormat(std::uint32_t a_f, std::uint32_t& a_blockBytes)
		{
			switch (a_f) {
			case DXGI_FORMAT_BC1_TYPELESS:
			case DXGI_FORMAT_BC1_UNORM:
			case DXGI_FORMAT_BC1_UNORM_SRGB:
			case DXGI_FORMAT_BC4_TYPELESS:
			case DXGI_FORMAT_BC4_UNORM:
			case DXGI_FORMAT_BC4_SNORM:
				a_blockBytes = 8;
				return true;
			case DXGI_FORMAT_BC2_TYPELESS:
			case DXGI_FORMAT_BC2_UNORM:
			case DXGI_FORMAT_BC2_UNORM_SRGB:
			case DXGI_FORMAT_BC3_TYPELESS:
			case DXGI_FORMAT_BC3_UNORM:
			case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC5_TYPELESS:
			case DXGI_FORMAT_BC5_UNORM:
			case DXGI_FORMAT_BC5_SNORM:
			case DXGI_FORMAT_BC6H_TYPELESS:
			case DXGI_FORMAT_BC6H_UF16:
			case DXGI_FORMAT_BC6H_SF16:
			case DXGI_FORMAT_BC7_TYPELESS:
			case DXGI_FORMAT_BC7_UNORM:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
				a_blockBytes = 16;
				return true;
			default:
				return false;
			}
		}

		std::uint32_t PixelBytes(std::uint32_t a_f)
		{
			switch (a_f) {
			case DXGI_FORMAT_R32G32B32A32_FLOAT:
				return 16;
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_UNORM:
				return 8;
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8X8_UNORM:
			case DXGI_FORMAT_R10G10B10A2_UNORM:
			case DXGI_FORMAT_R16G16_FLOAT:
			case DXGI_FORMAT_R16G16_UNORM:
			case DXGI_FORMAT_R32_FLOAT:
				return 4;
			case DXGI_FORMAT_R8G8_UNORM:
			case DXGI_FORMAT_R16_FLOAT:
			case DXGI_FORMAT_R16_UNORM:
				return 2;
			case DXGI_FORMAT_R8_UNORM:
			case DXGI_FORMAT_A8_UNORM:
				return 1;
			default:
				return 0;
			}
		}

		std::uint64_t TextureSize(const D3D11_TEXTURE2D_DESC& a_d)
		{
			std::uint64_t total = 0;
			for (UINT m = 0; m < a_d.MipLevels; ++m) {
				const auto w = std::max(1u, a_d.Width >> m), h = std::max(1u, a_d.Height >> m);
				total += std::uint64_t(RowBytes(a_d.Format, w)) * Rows(a_d.Format, h);
			}
			return total;
		}

		void Fail(Entry& a_e, const std::string& a_why)
		{
			if (a_e.staging) {
				a_e.staging->Release();
				a_e.staging = nullptr;
			}
			{
				std::scoped_lock lock(g_lock);
				a_e.st = St::kFailed;
			}
			g_failed.fetch_add(1, std::memory_order_relaxed);
			Log::Warn("capture: copy of {} {} failed: {}", a_e.texture ? "texture" : "buffer", static_cast<void*>(a_e.src), a_why);
		}

		void Start(ID3D11Device* a_dev, ID3D11DeviceContext* a_imm, Entry& a_e)
		{
			if (a_e.texture) {
				auto* src = static_cast<ID3D11Texture2D*>(a_e.src);
				src->GetDesc(&a_e.tdesc);
				const auto& d = a_e.tdesc;
				if (d.ArraySize != 1 || d.SampleDesc.Count != 1 || (d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) || !RowBytes(d.Format, 4)) {
					Fail(a_e, std::format("unsupported texture {}x{} format {} array {} samples {} misc {:#x}", d.Width, d.Height, static_cast<int>(d.Format),
						d.ArraySize, d.SampleDesc.Count, d.MiscFlags));
					return;
				}
				D3D11_TEXTURE2D_DESC sd = d;
				sd.Usage = D3D11_USAGE_STAGING;
				sd.BindFlags = 0;
				sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				sd.MiscFlags = 0;
				ID3D11Texture2D* staging = nullptr;
				if (FAILED(a_dev->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
					Fail(a_e, "CreateTexture2D(staging) failed");
					return;
				}
				a_e.staging = staging;
				a_e.bytes = TextureSize(d);
			} else {
				auto*             src = static_cast<ID3D11Buffer*>(a_e.src);
				D3D11_BUFFER_DESC d{};
				src->GetDesc(&d);
				if (!d.ByteWidth || d.ByteWidth > kMaxBufferBytes) {
					Fail(a_e, std::format("buffer of {} bytes", d.ByteWidth));
					return;
				}
				D3D11_BUFFER_DESC sd{};
				sd.ByteWidth = d.ByteWidth;
				sd.Usage = D3D11_USAGE_STAGING;
				sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				ID3D11Buffer* staging = nullptr;
				if (FAILED(a_dev->CreateBuffer(&sd, nullptr, &staging)) || !staging) {
					Fail(a_e, "CreateBuffer(staging) failed");
					return;
				}
				a_e.staging = staging;
				a_e.bytes = d.ByteWidth;
			}
			a_imm->CopyResource(a_e.staging, a_e.src);
			a_e.started = g_present.load(std::memory_order_relaxed);
			std::scoped_lock lock(g_lock);
			a_e.st = St::kCopying;
		}

		// True when done (ready or failed), false when the GPU isn't there yet.
		bool Finish(ID3D11DeviceContext* a_imm, Entry& a_e)
		{
			D3D11_MAPPED_SUBRESOURCE ms{};
			HRESULT                  hr = a_imm->Map(a_e.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
			if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
				g_waits.fetch_add(1, std::memory_order_relaxed);
				// Don't let a copy wait forever (a lost flush): after a second, wait for it.
				if (g_present.load() - a_e.started < 60) {
					return false;
				}
				hr = a_imm->Map(a_e.staging, 0, D3D11_MAP_READ, 0, &ms);
			}
			if (FAILED(hr) || !ms.pData) {
				Fail(a_e, std::format("Map {:#x}", static_cast<std::uint32_t>(hr)));
				return true;
			}
			if (!a_e.texture) {
				auto v = std::make_shared<std::vector<std::uint8_t>>(static_cast<const std::uint8_t*>(ms.pData), static_cast<const std::uint8_t*>(ms.pData) + a_e.bytes);
				a_imm->Unmap(a_e.staging, 0);
				std::scoped_lock lock(g_lock);
				a_e.buf = std::move(v);
				a_e.st = St::kReady;
			} else {
				const auto& d = a_e.tdesc;
				auto        t = std::make_shared<TextureBytes>();
				t->desc = d;
				t->bytes.resize(a_e.bytes);
				std::size_t at = 0;
				for (UINT m = 0; m < d.MipLevels; ++m) {
					if (m > 0) {
						hr = a_imm->Map(a_e.staging, m, D3D11_MAP_READ, 0, &ms);
						if (FAILED(hr) || !ms.pData) {
							Fail(a_e, std::format("Map mip {} {:#x}", m, static_cast<std::uint32_t>(hr)));
							return true;
						}
					}
					const auto w = std::max(1u, d.Width >> m), h = std::max(1u, d.Height >> m);
					const auto rowBytes = RowBytes(d.Format, w), rows = Rows(d.Format, h);
					for (UINT r = 0; r < rows; ++r) {
						std::memcpy(t->bytes.data() + at, static_cast<const std::uint8_t*>(ms.pData) + std::size_t(r) * ms.RowPitch, rowBytes);
						at += rowBytes;
					}
					a_imm->Unmap(a_e.staging, m);
				}
				std::scoped_lock lock(g_lock);
				a_e.tex = std::move(t);
				a_e.st = St::kReady;
			}
			a_e.staging->Release();
			a_e.staging = nullptr;
			g_copied.fetch_add(1, std::memory_order_relaxed);
			g_copiedBytes.fetch_add(a_e.bytes, std::memory_order_relaxed);
			return true;
		}
	}

	std::shared_ptr<const std::vector<std::uint8_t>> Buffer(ID3D11Buffer* a_buffer)
	{
		if (!a_buffer) {
			return nullptr;
		}
		std::scoped_lock lock(g_lock);
		auto&            e = Ensure(a_buffer, false);
		return e.st == St::kReady ? e.buf : nullptr;
	}

	std::shared_ptr<const TextureBytes> Texture(ID3D11Texture2D* a_texture)
	{
		if (!a_texture) {
			return nullptr;
		}
		std::scoped_lock lock(g_lock);
		auto&            e = Ensure(a_texture, true);
		return e.st == St::kReady ? e.tex : nullptr;
	}

	bool Failed(ID3D11Resource* a_resource)
	{
		std::scoped_lock lock(g_lock);
		const auto       it = g_entries.find(a_resource);
		return it != g_entries.end() && it->second.st == St::kFailed;
	}

	void Pump(ID3D11Device* a_device, ID3D11DeviceContext* a_imm)
	{
		g_present.fetch_add(1, std::memory_order_relaxed);
		if (!a_device || !a_imm) {
			return;
		}
		d3d::OwnCalls       own;  // our Map/Unmap/CopyResource must not be observed as the game's
		std::vector<Entry*> copying, starting;
		{
			std::scoped_lock lock(g_lock);
			for (auto& [res, e] : g_entries) {
				if (e.st == St::kCopying) {
					copying.push_back(&e);
				}
			}
			std::uint64_t budget = 0;
			while (!g_queue.empty() && static_cast<int>(starting.size()) < kMaxStartsPerPresent && budget < kMaxBytesPerPresent) {
				const auto it = g_entries.find(g_queue.front());
				g_queue.pop_front();
				if (it == g_entries.end() || it->second.st != St::kQueued) {
					continue;
				}
				starting.push_back(&it->second);
				if (it->second.texture) {
					D3D11_TEXTURE2D_DESC d{};
					static_cast<ID3D11Texture2D*>(it->second.src)->GetDesc(&d);
					budget += TextureSize(d);
				} else {
					D3D11_BUFFER_DESC d{};
					static_cast<ID3D11Buffer*>(it->second.src)->GetDesc(&d);
					budget += d.ByteWidth;
				}
			}
		}
		for (auto* e : copying) {
			Finish(a_imm, *e);
		}
		for (auto* e : starting) {
			Start(a_device, a_imm, *e);
		}
	}

	void Evict(std::uint64_t a_present)
	{
		std::scoped_lock lock(g_lock);
		const auto       now = g_present.load(std::memory_order_relaxed);
		for (auto it = g_entries.begin(); it != g_entries.end();) {
			auto& e = it->second;
			if ((e.st == St::kReady || e.st == St::kFailed) && now - e.lastUse > kEvictAfterPresents) {
				e.src->Release();
				it = g_entries.erase(it);
			} else {
				++it;
			}
		}
		(void)a_present;
	}

	std::string Describe()
	{
		std::size_t queued = 0, copying = 0, ready = 0, failed = 0;
		{
			std::scoped_lock lock(g_lock);
			for (const auto& [res, e] : g_entries) {
				queued += e.st == St::kQueued;
				copying += e.st == St::kCopying;
				ready += e.st == St::kReady;
				failed += e.st == St::kFailed;
			}
		}
		return std::format("copies: {} ready, {} copying, {} queued, {} failed; {} done ({:.1f} MB), {} not-ready maps", ready, copying, queued, failed, g_copied.load(),
			g_copiedBytes.load() / 1048576.0, g_waits.load());
	}

	std::uint32_t RowBytes(std::uint32_t a_format, std::uint32_t a_width)
	{
		std::uint32_t block = 0;
		if (BlockFormat(a_format, block)) {
			return ((a_width + 3) / 4) * block;
		}
		return a_width * PixelBytes(a_format);
	}

	std::uint32_t Rows(std::uint32_t a_format, std::uint32_t a_height)
	{
		std::uint32_t block = 0;
		return BlockFormat(a_format, block) ? (a_height + 3) / 4 : a_height;
	}
}
