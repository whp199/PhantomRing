#include "Params.h"

#include "Addresses.h"
#include "Log.h"
#include "Mem.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>

#include <windows.h>

namespace phantom::er::params
{
	// ---- pure parsing -----------------------------------------------------------------------------
	std::optional<Header> ParseHeader(const std::uint8_t* a_h)
	{
		const std::uint8_t bigEndian = a_h[0x2C];
		const std::uint8_t format = a_h[0x2D];
		if (bigEndian != 0 || !(format & 0x04)) {
			return std::nullopt;  // ER params are little-endian with 64-bit offsets
		}
		Header h;
		std::memcpy(&h.rowCount, a_h + 0xA, 2);
		h.nameByOffset = (format & 0x80) != 0;
		if (h.nameByOffset) {
			std::memcpy(&h.nameOffset, a_h + 0x10, 4);
		} else {
			const char* name = reinterpret_cast<const char*>(a_h + 0xC);
			h.inlineName.assign(name, strnlen(name, 0x20));
		}
		return h;
	}

	bool ParseRows(const Header& a_header, const std::uint8_t* a_descs, ParsedFile& a_out)
	{
		a_out.rowCount = a_header.rowCount;
		a_out.is64 = true;
		a_out.rows.clear();
		a_out.rows.reserve(a_header.rowCount);
		std::vector<std::uint64_t> offsets;
		offsets.reserve(a_header.rowCount);
		for (std::size_t i = 0; i < a_header.rowCount; ++i) {
			std::uint32_t id;
			std::uint64_t data;
			std::memcpy(&id, a_descs + i * kRowDescBytes, 4);
			std::memcpy(&data, a_descs + i * kRowDescBytes + 8, 8);
			a_out.rows.emplace_back(id, data);
			offsets.push_back(data);
		}
		std::ranges::sort(offsets);
		std::uint64_t stride = std::numeric_limits<std::uint64_t>::max();
		for (std::size_t i = 1; i < offsets.size(); ++i) {
			if (offsets[i] > offsets[i - 1]) {
				stride = std::min(stride, offsets[i] - offsets[i - 1]);
			}
		}
		a_out.rowStride = stride == std::numeric_limits<std::uint64_t>::max() || stride > 0x100000 ? 0 : static_cast<std::uint32_t>(stride);
		return a_header.rowCount > 0;
	}

	std::optional<ParsedFile> Parse(const std::uint8_t* a_bytes, std::size_t a_size)
	{
		if (!a_bytes || a_size < kHeaderBytes) {
			return std::nullopt;
		}
		const auto header = ParseHeader(a_bytes);
		if (!header || kHeaderBytes + header->DescBytes() > a_size) {
			return std::nullopt;
		}
		ParsedFile out;
		if (!ParseRows(*header, a_bytes + kHeaderBytes, out)) {
			return std::nullopt;
		}
		if (header->nameByOffset) {
			if (header->nameOffset >= a_size) {
				return std::nullopt;
			}
			const char* name = reinterpret_cast<const char*>(a_bytes + header->nameOffset);
			out.structName.assign(name, strnlen(name, std::min<std::size_t>(64, a_size - header->nameOffset)));
		} else {
			out.structName = header->inlineName;
		}
		return out;
	}

	// ---- the live tables ------------------------------------------------------------------------
	namespace
	{
		constexpr std::uint32_t kHolders = 0x80;        // SoloParamRepository: holders[194] from here
		constexpr std::uint32_t kHolderStride = 0x48;   // {u32 count; ParamResCap* caps[8]}
		constexpr int           kHolderCount = 194;
		constexpr std::uint32_t kResName = 0x08;        // FD4ResCap: FD4BasicHashString name
		constexpr std::uint32_t kParamResCap = 0x80;    // ParamResCap -> FD4ParamResCap*
		constexpr std::uint32_t kParamFile = 0x80;      // FD4ParamResCap -> ParamFile*

		std::uint8_t* Repository()
		{
			const auto& a = Addr();
			if (!a.HasAll(A::SoloParamRepository, A::VtSoloParamRepositoryImp, A::VtParamResCap)) {
				return nullptr;
			}
			auto* repo = a.Instance(A::SoloParamRepository);
			return HasVtable(repo, a[A::VtSoloParamRepositoryImp]) ? repo : nullptr;
		}

		// FD4BasicHashString {vtable; DLString (MSVC basic_string<wchar_t, DLAllocator>); u32 hash; ...}.
		// Where MSVC puts the stateful allocator isn't pinned down for this build, so both orders are
		// tried; the one with a sane size/capacity wins. Empty when neither decodes.
		std::wstring ResName(std::uint8_t* a_resCap)
		{
			std::uint8_t raw[0x38]{};
			if (!SafeReadBytes(a_resCap + kResName, raw, sizeof(raw))) {
				return {};
			}
			for (const std::uint32_t buf : { 0x10u, 0x08u }) {
				std::uint64_t size, cap;
				std::memcpy(&size, raw + buf + 0x10, 8);
				std::memcpy(&cap, raw + buf + 0x18, 8);
				if (size == 0 || size > 64 || cap < size || cap < 7 || cap > 4096) {
					continue;
				}
				wchar_t text[65]{};
				if (cap < 8) {
					std::memcpy(text, raw + buf, size * sizeof(wchar_t));
				} else {
					std::uint8_t* ptr;
					std::memcpy(&ptr, raw + buf, 8);
					if (!SafeReadBytes(ptr, text, size * sizeof(wchar_t))) {
						continue;
					}
				}
				std::wstring name(text, size);
				if (std::ranges::all_of(name, [](wchar_t c) { return c >= 0x20 && c < 0x7F; })) {
					return name;
				}
			}
			return {};
		}

		std::uint8_t* FileOf(std::uint8_t* a_resCap)
		{
			auto* fprc = SafePtr(a_resCap + kParamResCap);
			return fprc ? SafePtr(fprc + kParamFile) : nullptr;
		}
	}

	bool Available() { return Repository() != nullptr; }

	bool Table::Bind()
	{
		file_ = nullptr;
		resCap_ = nullptr;
		rows_.clear();
		auto* repo = Repository();
		if (!repo) {
			error_ = "SoloParamRepository unverified or not created yet";
			return false;
		}
		const auto& a = Addr();
		std::string tried;
		// a_byName: the holder's resource name must read back as ours. Without it (the fallback, for when
		// the name can't be decoded) only the documented index is tried; the struct name and row stride
		// are checked either way.
		auto        attempt = [&](int a_index, bool a_byName) -> bool {
            auto*      holder = repo + kHolders + static_cast<std::uint32_t>(a_index) * kHolderStride;
            const auto count = SafeRead<std::uint32_t>(holder).value_or(0);
            auto*      rc = count >= 1 && count <= 8 ? SafePtr(holder + 8) : nullptr;
            if (!HasVtable(rc, a[A::VtParamResCap])) {
                return false;
            }
            const auto name = ResName(rc);
            if (a_byName ? name != spec_.resName : a_index != spec_.holderIndex) {
                return false;
            }
            auto*        file = FileOf(rc);
            std::uint8_t header[kHeaderBytes];
            if (!file || !SafeReadBytes(file, header, sizeof(header))) {
                tried = std::format("holder {}: no file", a_index);
                return false;
            }
            const auto h = ParseHeader(header);
            if (!h || h->rowCount == 0) {
                tried = std::format("holder {}: not a 64-bit PARAM", a_index);
                return false;
            }
            std::string structName = h->inlineName;
            if (h->nameByOffset) {
                char buf[33]{};
                if (!SafeReadBytes(file + h->nameOffset, buf, 32)) {
                    tried = std::format("holder {}: struct name unreadable", a_index);
                    return false;
                }
                structName = buf;
            }
            if (structName != spec_.structName) {
                tried = std::format("holder {}: struct {} (want {})", a_index, structName, spec_.structName);
                return false;
            }
            std::vector<std::uint8_t> descs(h->DescBytes());
            ParsedFile                parsed;
            if (!SafeReadBytes(file + kHeaderBytes, descs.data(), descs.size()) || !ParseRows(*h, descs.data(), parsed)) {
                tried = std::format("holder {}: row table unreadable", a_index);
                return false;
            }
            if (parsed.rowStride != spec_.rowSize) {
                tried = std::format("holder {}: row stride {:#x} (want {:#x})", a_index, parsed.rowStride, spec_.rowSize);
                return false;
            }
            for (const auto& [id, offset] : parsed.rows) {
                rows_.emplace(id, offset);
            }
            file_ = file;
            resCap_ = rc;
            Log::Info("params: {} = holder {} (name '{}', {} rows, struct {}, file {})", Narrow(spec_.resName), a_index, Narrow(name), parsed.rowCount, structName,
                static_cast<void*>(file));
            return true;
		};
		if (attempt(spec_.holderIndex, true)) {
			return true;
		}
		for (int i = 0; i < kHolderCount; ++i) {
			if (i != spec_.holderIndex && attempt(i, true)) {
				return true;
			}
		}
		if (attempt(spec_.holderIndex, false)) {
			Log::Warn("params: {} bound by its documented holder index (resource names unreadable)", Narrow(spec_.resName));
			return true;
		}
		error_ = tried.empty() ? "no holder named " + Narrow(spec_.resName) : tried;
		return false;
	}

	std::uint8_t* Table::Row(std::uint32_t a_id)
	{
		if (file_ && (!resCap_ || FileOf(resCap_) != file_)) {
			Log::Info("params: {} file changed (regulation reload?); re-binding", Narrow(spec_.resName));
			file_ = nullptr;
		}
		if (!file_) {
			const auto now = ::GetTickCount64();
			if (lastTry_ && now - lastTry_ < 2000) {
				return nullptr;
			}
			lastTry_ = now;
			if (!Bind()) {
				return nullptr;
			}
		}
		const auto it = rows_.find(a_id);
		return it == rows_.end() ? nullptr : file_ + it->second;
	}

	std::string Table::Describe() const
	{
		return file_ ? std::format("{}: {} rows at {}", Narrow(spec_.resName), rows_.size(), static_cast<void*>(file_))
		             : std::format("{}: {}", Narrow(spec_.resName), error_);
	}

	// ---- patches ----------------------------------------------------------------------------------
	bool Patch::Write(std::uint8_t* a_row, std::uint32_t a_offset, const void* a_bytes, std::size_t a_size)
	{
		if (!a_row || !a_bytes || a_size == 0) {
			return false;
		}
		auto* at = a_row + a_offset;
		const bool known = std::ranges::any_of(saved_, [&](const Saved& s) { return s.at == at && s.bytes.size() == a_size; });
		if (!known) {
			Saved s{ at, std::vector<std::uint8_t>(a_size) };
			if (!SafeReadBytes(at, s.bytes.data(), a_size)) {
				return false;
			}
			saved_.push_back(std::move(s));
		}
		return SafeWriteBytes(at, a_bytes, a_size);
	}

	void Patch::Restore()
	{
		// Newest first, so overlapping patches unwind in order.
		for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
			SafeWriteBytes(it->at, it->bytes.data(), it->bytes.size());
		}
		saved_.clear();
	}
}
