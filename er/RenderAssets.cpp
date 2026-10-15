#include "RenderAssets.h"

#include "Link.h"
#include "Log.h"
#include "RenderMesh.h"
#include "Shaders.h"

#include <windows.h>

#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace phantom::er::RenderAssets
{
	namespace
	{
		using d3d::ComPtr;

		constexpr std::uint32_t kSrvSlots = 8192;
		constexpr std::uint64_t kMaxPendingBytes = 768ull << 20;  // staging memory the worker may run ahead
		constexpr std::uint32_t kMaxElements = 32;

		struct Pending
		{
			std::shared_ptr<Mesh>                           mesh;
			std::shared_ptr<Texture>                        texture;
			ComPtr<ID3D12Resource>                          staging;
			std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
			std::uint64_t                                   bytes{ 0 };
		};

		ComPtr<ID3D12Device>                                   g_device;
		std::thread                                            g_thread;
		std::atomic<bool>                                      g_stop{ false };
		std::mutex                                             g_lock;
		std::unordered_map<std::uint64_t, std::shared_ptr<Mesh>>    g_meshes;
		std::unordered_map<std::uint64_t, std::shared_ptr<Texture>> g_textures;
		std::deque<Pending>                                    g_pending;
		std::atomic<std::uint64_t>                             g_pendingBytes{ 0 };

		std::mutex                   g_srvLock;
		ComPtr<ID3D12DescriptorHeap> g_srvHeap;
		UINT                         g_srvInc{ 0 };
		std::vector<std::int32_t>    g_freeSlots;

		std::atomic<std::uint64_t> g_meshCount{ 0 }, g_textureCount{ 0 }, g_forgets{ 0 }, g_clears{ 0 }, g_failures{ 0 }, g_bytesIn{ 0 }, g_uploaded{ 0 };
		std::atomic<std::uint32_t> g_winding[3]{};  // meshes wound inward / unknown / outward

		std::uint64_t Fnv(const void* a_data, std::size_t a_size, std::uint64_t a_h = 0xcbf29ce484222325ull)
		{
			const auto* p = static_cast<const std::uint8_t*>(a_data);
			for (std::size_t i = 0; i < a_size; ++i) {
				a_h = (a_h ^ p[i]) * 0x100000001b3ull;
			}
			return a_h;
		}

		std::int32_t AllocSlot()
		{
			std::scoped_lock lock(g_srvLock);
			if (g_freeSlots.empty()) {
				return -1;
			}
			const auto s = g_freeSlots.back();
			g_freeSlots.pop_back();
			return s;
		}

		ComPtr<ID3D12Resource> Committed(D3D12_HEAP_TYPE a_heap, const D3D12_RESOURCE_DESC& a_desc, D3D12_RESOURCE_STATES a_state)
		{
			ComPtr<ID3D12Resource> r;
			const auto             props = d3d::HeapProps(a_heap);
			const HRESULT          hr = g_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &a_desc, a_state, nullptr, __uuidof(ID3D12Resource),
                reinterpret_cast<void**>(r.GetAddressOf()));
			if (FAILED(hr)) {
				Log::Warn("render: CreateCommittedResource ({} bytes / {}x{} fmt {}) failed: {}", a_desc.Width, a_desc.Width, a_desc.Height, static_cast<int>(a_desc.Format),
					d3d::Hr(hr));
				return {};
			}
			return r;
		}

		bool Fail(std::string_view a_what, std::uint64_t a_key)
		{
			++g_failures;
			Log::Warn("render: dropped {} {:#018x}", a_what, a_key);
			return false;
		}

		bool OnMesh(const std::uint8_t* a_p, std::uint32_t a_size)
		{
			if (a_size < sizeof(proto::RenMesh)) {
				return Fail("short mesh message", 0);
			}
			proto::RenMesh m;
			std::memcpy(&m, a_p, sizeof(m));
			const std::uint64_t need = sizeof(m) + std::uint64_t(m.elementCount) * sizeof(proto::RenInputElement) + m.vertexBytes + m.indexBytes;
			if (m.elementCount == 0 || m.elementCount > kMaxElements || need > a_size) {
				return Fail(std::format("mesh with {} elements / {} bytes in a {}-byte message", m.elementCount, need, a_size), m.key);
			}
			auto mesh = std::make_shared<Mesh>();
			mesh->key = m.key;
			mesh->elements.resize(m.elementCount);
			std::memcpy(mesh->elements.data(), a_p + sizeof(m), m.elementCount * sizeof(proto::RenInputElement));
			if (m.streams == 0) {
				mesh->streams = 1;
				mesh->streamStride[0] = m.vertexStride;
				mesh->streamBytes[0] = m.vertexBytes;
			} else if (m.streams <= 4) {
				mesh->streams = m.streams;
				std::uint64_t sum = 0;
				for (std::uint32_t s = 0; s < m.streams; ++s) {
					mesh->streamStride[s] = m.streamStride[s];
					mesh->streamBytes[s] = m.streamBytes[s];
					sum += m.streamBytes[s];
				}
				if (sum != m.vertexBytes) {
					return Fail(std::format("mesh whose streams add up to {} of {} vertex bytes", sum, m.vertexBytes), m.key);
				}
			} else {
				return Fail(std::format("mesh with {} streams", m.streams), m.key);
			}
			for (std::uint32_t s = 0; s < mesh->streams; ++s) {
				if (mesh->streamStride[s] == 0 || mesh->streamStride[s] > 2048) {
					return Fail(std::format("mesh with stream {} stride {}", s, mesh->streamStride[s]), m.key);
				}
			}
			// Elements: resolve APPEND_ALIGNED offsets, check slots/formats/bounds.
			std::uint32_t append[4]{};
			for (auto& e : mesh->elements) {
				e.semantic[15] = 0;
				const auto info = d3d::Info(static_cast<DXGI_FORMAT>(e.format));
				if (e.inputSlot >= mesh->streams || info.kind == d3d::Kind::kUnknown || info.block) {
					return Fail(std::format("mesh element {}{} (format {}, slot {})", e.semantic, e.semanticIndex, e.format, e.inputSlot), m.key);
				}
				if (e.alignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT) {
					e.alignedByteOffset = append[e.inputSlot];
				}
				append[e.inputSlot] = e.alignedByteOffset + info.bytes;
				if (e.alignedByteOffset + info.bytes > mesh->streamStride[e.inputSlot]) {
					return Fail(std::format("mesh element {} beyond its stride", e.semantic), m.key);
				}
				if (Shaders::Canonical(e.semantic) == "BLENDINDICES") {
					mesh->hasBlendIndices = true;
				}
			}
			mesh->indexFormat = static_cast<DXGI_FORMAT>(m.indexFormat);
			const std::uint32_t isz = mesh->indexFormat == DXGI_FORMAT_R16_UINT ? 2 : mesh->indexFormat == DXGI_FORMAT_R32_UINT ? 4 : 0;
			if (isz == 0 || m.indexBytes == 0 || m.indexBytes % isz) {
				return Fail(std::format("mesh with index format {} / {} bytes", m.indexFormat, m.indexBytes), m.key);
			}
			mesh->indexCount = m.indexBytes / isz;
			mesh->indexBytes = m.indexBytes;
			mesh->topology = m.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

			std::uint64_t layout = Fnv(&mesh->topology, sizeof(mesh->topology));
			for (const auto& e : mesh->elements) {
				layout = Fnv(&e, sizeof(e), layout);
			}
			mesh->layoutHash = layout;

			std::uint64_t total = 0;
			for (std::uint32_t s = 0; s < mesh->streams; ++s) {
				mesh->streamOffset[s] = static_cast<std::uint32_t>(total);
				total = d3d::Align(total + mesh->streamBytes[s], 256);
			}
			mesh->indexOffset = static_cast<std::uint32_t>(total);
			total += m.indexBytes;

			mesh->buffer = Committed(D3D12_HEAP_TYPE_DEFAULT, d3d::BufferDesc(total), D3D12_RESOURCE_STATE_COMMON);
			auto staging = Committed(D3D12_HEAP_TYPE_UPLOAD, d3d::BufferDesc(total), D3D12_RESOURCE_STATE_GENERIC_READ);
			std::uint8_t* dst = nullptr;
			if (!mesh->buffer || !staging || FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&dst)))) {
				return Fail("mesh (no GPU memory)", m.key);
			}
			const std::uint8_t* src = a_p + sizeof(m) + m.elementCount * sizeof(proto::RenInputElement);
			{
				const std::uint8_t* streams[4]{};
				const std::uint8_t* at = src;
				for (std::uint32_t s = 0; s < mesh->streams; ++s) {
					streams[s] = at;
					at += mesh->streamBytes[s];
				}
				mesh->winding = RenderMesh::Winding(mesh->elements, streams, mesh->streamBytes, mesh->streamStride, mesh->streams, at, m.indexBytes, isz == 4,
					mesh->topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				mesh->hasBounds = RenderMesh::Bounds(mesh->elements, streams, mesh->streamBytes, mesh->streamStride, mesh->streams, mesh->boundsMin, mesh->boundsMax);
			}
			for (std::uint32_t s = 0; s < mesh->streams; ++s) {
				std::memcpy(dst + mesh->streamOffset[s], src, mesh->streamBytes[s]);
				src += mesh->streamBytes[s];
			}
			std::memcpy(dst + mesh->indexOffset, src, m.indexBytes);
			staging->Unmap(0, nullptr);
			mesh->uploadBytes = static_cast<std::uint32_t>(total);

			std::scoped_lock lock(g_lock);
			g_meshes[m.key] = mesh;
			g_pending.push_back({ mesh, nullptr, staging, {}, total });
			g_pendingBytes += total;
			++g_meshCount;
			Log::Debug("render: mesh {:#018x}: {} elements, {} streams, {} vertex bytes, {} indices, winding {}", m.key, m.elementCount, mesh->streams, m.vertexBytes,
				mesh->indexCount, mesh->winding);
			++g_winding[mesh->winding + 1];
			return true;
		}

		bool OnTexture(const std::uint8_t* a_p, std::uint32_t a_size)
		{
			if (a_size < sizeof(proto::RenTexture)) {
				return Fail("short texture message", 0);
			}
			proto::RenTexture t;
			std::memcpy(&t, a_p, sizeof(t));
			const auto fmt = d3d::Typed(static_cast<DXGI_FORMAT>(t.format));
			const auto info = d3d::Info(fmt);
			const std::uint32_t slices = std::max(1u, t.arraySize);
			if (info.kind == d3d::Kind::kUnknown || t.width == 0 || t.height == 0 || t.width > 16384 || t.height > 16384 || t.mipLevels == 0 || t.mipLevels > 16 ||
				slices > 64 || sizeof(t) + std::uint64_t(t.dataBytes) > a_size) {
				return Fail(std::format("texture {}x{} format {} mips {} slices {} ({} bytes)", t.width, t.height, t.format, t.mipLevels, slices, t.dataBytes), t.key);
			}
			std::uint64_t expected = 0;
			for (std::uint32_t s = 0; s < slices; ++s) {
				for (std::uint32_t mip = 0; mip < t.mipLevels; ++mip) {
					std::uint32_t rowBytes = 0, rows = 0;
					d3d::MipLayout(fmt, std::max(1u, t.width >> mip), std::max(1u, t.height >> mip), rowBytes, rows);
					expected += std::uint64_t(rowBytes) * rows;
				}
			}
			if (expected > t.dataBytes) {
				return Fail(std::format("texture with {} of {} expected bytes", t.dataBytes, expected), t.key);
			}

			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = t.width;
			desc.Height = t.height;
			desc.DepthOrArraySize = static_cast<UINT16>(slices);
			desc.MipLevels = static_cast<UINT16>(t.mipLevels);
			desc.Format = fmt;
			desc.SampleDesc.Count = 1;
			auto tex = std::make_shared<Texture>();
			tex->key = t.key;
			tex->format = fmt;
			tex->width = t.width;
			tex->height = t.height;
			tex->mips = t.mipLevels;
			tex->slices = slices;
			// What the texture is by content (er/MaterialRoles.h): a normal map sent as a diffuse, a box poster or a layer mask.
			tex->traits = MaterialRoles::Analyse(static_cast<std::uint32_t>(fmt), t.width, t.height, t.mipLevels, a_p + sizeof(t), t.dataBytes);
			tex->resource = Committed(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST);
			if (!tex->resource) {
				return Fail("texture (no GPU memory / unsupported)", t.key);
			}
			const UINT                                      n = slices * t.mipLevels;
			std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(n);
			std::vector<UINT>                               rows(n);
			std::vector<UINT64>                             rowSize(n);
			UINT64                                          total = 0;
			g_device->GetCopyableFootprints(&desc, 0, n, 0, fp.data(), rows.data(), rowSize.data(), &total);
			auto          staging = Committed(D3D12_HEAP_TYPE_UPLOAD, d3d::BufferDesc(total), D3D12_RESOURCE_STATE_GENERIC_READ);
			std::uint8_t* dst = nullptr;
			if (!staging || FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&dst)))) {
				return Fail("texture (no staging memory)", t.key);
			}
			// Source: D3D subresource order (mips of slice 0, then slice 1, ...), rows tightly packed.
			const std::uint8_t* src = a_p + sizeof(t);
			for (UINT sub = 0; sub < n; ++sub) {
				const std::uint32_t mip = sub % t.mipLevels;
				std::uint32_t       rowBytes = 0, nrows = 0;
				d3d::MipLayout(fmt, std::max(1u, t.width >> mip), std::max(1u, t.height >> mip), rowBytes, nrows);
				const auto copy = static_cast<std::size_t>(std::min<UINT64>(rowBytes, rowSize[sub]));
				for (std::uint32_t r = 0; r < nrows && r < rows[sub]; ++r) {
					std::memcpy(dst + fp[sub].Offset + std::uint64_t(r) * fp[sub].Footprint.RowPitch, src + std::uint64_t(r) * rowBytes, copy);
				}
				src += std::uint64_t(rowBytes) * nrows;
			}
			staging->Unmap(0, nullptr);
			tex->uploadBytes = static_cast<std::uint32_t>(total);

			tex->descriptor = AllocSlot();
			if (tex->descriptor < 0) {
				return Fail("texture (SRV heap full)", t.key);
			}
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = fmt;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			if (slices == 1) {
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srv.Texture2D.MipLevels = t.mipLevels;
			} else {
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				srv.Texture2DArray.MipLevels = t.mipLevels;
				srv.Texture2DArray.ArraySize = slices;
			}
			g_device->CreateShaderResourceView(tex->resource.Get(), &srv, Srv(*tex));

			std::scoped_lock lock(g_lock);
			g_textures[t.key] = tex;
			g_pending.push_back({ nullptr, tex, staging, std::move(fp), total });
			g_pendingBytes += total;
			++g_textureCount;
			Log::Debug("render: texture {:#018x}: {}x{} {} mips {} slices {}", t.key, t.width, t.height, static_cast<int>(fmt), t.mipLevels, slices);
			return true;
		}

		void OnForget(const std::uint8_t* a_p, std::uint32_t a_size)
		{
			if (a_size < sizeof(proto::RenForget)) {
				return;
			}
			proto::RenForget f;
			std::memcpy(&f, a_p, sizeof(f));
			std::scoped_lock lock(g_lock);
			g_meshes.erase(f.key);
			g_textures.erase(f.key);
			++g_forgets;
		}

		void OnClearAll()
		{
			std::scoped_lock lock(g_lock);
			const auto       meshes = g_meshes.size(), textures = g_textures.size();
			g_meshes.clear();
			g_textures.clear();
			g_pending.clear();
			g_pendingBytes = 0;
			++g_clears;
			Log::Info("render: guest cleared its assets ({} meshes, {} textures dropped)", meshes, textures);
		}

		// One pass over what the guest has published. False when there was nothing to do.
		bool Pump()
		{
			auto* ring = Link::Get().RenderRing();
			if (!ring) {
				return false;
			}
			auto&      head = *reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingHeadOff);
			auto&      tail = *reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingTailOff);
			const auto* data = ring + proto::kRenRingDataOff;
			constexpr std::uint64_t cap = proto::kRenRingDataBytes;
			const auto h = Atomic(head).load(std::memory_order_acquire);
			auto       t = Atomic(tail).load(std::memory_order_relaxed);
			if (t > h) {
				// A producer that restarted its counters: resynchronise.
				Atomic(tail).store(h, std::memory_order_release);
				return false;
			}
			bool did = false;
			while (t < h && !g_stop.load(std::memory_order_relaxed)) {
				if (g_pendingBytes.load() > kMaxPendingBytes) {
					break;  // Present hasn't caught up with the uploads yet
				}
				const std::uint64_t pos = t % cap;
				proto::RenMsgHeader hdr;
				std::memcpy(&hdr, data + pos, sizeof(hdr));
				if (hdr.type == proto::kRenPad) {
					t += cap - pos;
					Atomic(tail).store(t, std::memory_order_release);
					continue;
				}
				const std::uint64_t msg = d3d::Align(8 + std::uint64_t(hdr.payloadBytes), 8);
				if (pos + msg > cap || t + msg > h) {
					Log::Warn("render: corrupt render ring message (type {}, {} bytes at {}); skipping to the producer", hdr.type, hdr.payloadBytes, pos);
					Atomic(tail).store(h, std::memory_order_release);
					++g_failures;
					return true;
				}
				const auto* payload = data + pos + 8;
				switch (hdr.type) {
				case proto::kRenClearAll:
					OnClearAll();
					break;
				case proto::kRenMesh:
					OnMesh(payload, hdr.payloadBytes);
					break;
				case proto::kRenTexture:
					OnTexture(payload, hdr.payloadBytes);
					break;
				case proto::kRenForget:
					OnForget(payload, hdr.payloadBytes);
					break;
				default:
					Log::Debug("render: unknown render ring message {}", hdr.type);
					break;
				}
				g_bytesIn += msg;
				t += msg;
				Atomic(tail).store(t, std::memory_order_release);
				did = true;
			}
			return did;
		}

		void Worker()
		{
			Log::Info("render: asset worker running");
			while (!g_stop.load()) {
				if (!Pump()) {
					::Sleep(4);
				}
			}
		}
	}

	Texture::~Texture()
	{
		if (descriptor >= 0) {
			std::scoped_lock lock(g_srvLock);
			g_freeSlots.push_back(descriptor);
		}
	}

	bool Start(ID3D12Device* a_device)
	{
		if (g_thread.joinable()) {
			return true;
		}
		g_device = a_device;
		D3D12_DESCRIPTOR_HEAP_DESC hd{};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = kSrvSlots;
		if (FAILED(g_device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(g_srvHeap.GetAddressOf())))) {
			Log::Error("render: texture descriptor heap failed");
			g_device.Reset();
			return false;
		}
		g_srvInc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		g_freeSlots.clear();
		for (std::int32_t i = kSrvSlots - 1; i >= 0; --i) {
			g_freeSlots.push_back(i);
		}
		g_stop = false;
		g_thread = std::thread(&Worker);
		return true;
	}

	void Stop()
	{
		g_stop = true;
		if (g_thread.joinable()) {
			g_thread.join();
		}
	}

	std::shared_ptr<Mesh> FindMesh(std::uint64_t a_key)
	{
		std::scoped_lock lock(g_lock);
		const auto       it = g_meshes.find(a_key);
		return it != g_meshes.end() && it->second->ready.load(std::memory_order_acquire) ? it->second : nullptr;
	}

	std::shared_ptr<Texture> FindTexture(std::uint64_t a_key)
	{
		std::scoped_lock lock(g_lock);
		const auto       it = g_textures.find(a_key);
		return it != g_textures.end() && it->second->ready.load(std::memory_order_acquire) ? it->second : nullptr;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE Srv(const Texture& a_texture)
	{
		auto h = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += std::size_t(a_texture.descriptor) * g_srvInc;
		return h;
	}

	bool HasPendingUploads()
	{
		std::scoped_lock lock(g_lock);
		return !g_pending.empty();
	}

	void RecordUploads(ID3D12GraphicsCommandList* a_list, std::vector<std::shared_ptr<void>>& a_keep, std::uint64_t a_budgetBytes)
	{
		std::vector<Pending> take;
		{
			std::scoped_lock lock(g_lock);
			std::uint64_t    bytes = 0;
			while (!g_pending.empty() && (take.empty() || bytes + g_pending.front().bytes <= a_budgetBytes)) {
				bytes += g_pending.front().bytes;
				g_pendingBytes -= g_pending.front().bytes;
				take.push_back(std::move(g_pending.front()));
				g_pending.pop_front();
			}
		}
		if (take.empty()) {
			return;
		}
		std::vector<D3D12_RESOURCE_BARRIER> before, after;
		for (auto& p : take) {
			if (p.mesh) {
				before.push_back(d3d::Transition(p.mesh->buffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
				after.push_back(d3d::Transition(p.mesh->buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
					D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER));
			} else {
				after.push_back(d3d::Transition(p.texture->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
			}
		}
		if (!before.empty()) {
			a_list->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
		}
		for (auto& p : take) {
			if (p.mesh) {
				a_list->CopyBufferRegion(p.mesh->buffer.Get(), 0, p.staging.Get(), 0, p.bytes);
			} else {
				for (UINT sub = 0; sub < p.footprints.size(); ++sub) {
					D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
					dst.pResource = p.texture->resource.Get();
					dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					dst.SubresourceIndex = sub;
					src.pResource = p.staging.Get();
					src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					src.PlacedFootprint = p.footprints[sub];
					a_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
				}
			}
		}
		a_list->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
		for (auto& p : take) {
			// Draws later in this same command list may use it; the GPU runs the copy first.
			if (p.mesh) {
				p.mesh->ready.store(true, std::memory_order_release);
				a_keep.push_back(p.mesh);
			} else {
				p.texture->ready.store(true, std::memory_order_release);
				a_keep.push_back(p.texture);
			}
			a_keep.push_back(std::make_shared<ComPtr<ID3D12Resource>>(std::move(p.staging)));
			g_uploaded += p.bytes;
		}
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		return std::format("assets: {} meshes, {} textures live ({} / {} received, {} forgets, {} clears, {} dropped), {} pending uploads ({:.1f} MB), "
						   "{:.1f} MB received, {:.1f} MB uploaded; mesh winding {} inward / {} outward / {} unknown",
			g_meshes.size(), g_textures.size(), g_meshCount.load(), g_textureCount.load(), g_forgets.load(), g_clears.load(), g_failures.load(), g_pending.size(),
			g_pendingBytes.load() / 1048576.0, g_bytesIn.load() / 1048576.0, g_uploaded.load() / 1048576.0, g_winding[0].load(), g_winding[2].load(), g_winding[1].load());
	}
}
