#include "Hook.h"

#include "Log.h"

#include <MinHook.h>

#include <cstring>
#include <mutex>

namespace phantom
{
	ModuleImage ModuleImage::Of(HMODULE a_module)
	{
		ModuleImage image;
		auto*       base = reinterpret_cast<std::uint8_t*>(a_module);
		if (!base) {
			return image;
		}
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		image.base = base;
		image.size = nt->OptionalHeader.SizeOfImage;
		const auto* section = IMAGE_FIRST_SECTION(nt);
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
			Section s;
			std::memcpy(s.name, section->Name, 8);
			s.start = base + section->VirtualAddress;
			s.size = section->Misc.VirtualSize;
			s.executable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
			image.sections.push_back(s);
		}
		return image;
	}

	Pattern Pattern::Parse(std::string_view a_text)
	{
		Pattern p;
		std::size_t i = 0;
		while (i < a_text.size()) {
			if (a_text[i] == ' ') {
				++i;
				continue;
			}
			if (a_text[i] == '?') {
				p.bytes.push_back(-1);
				i += (i + 1 < a_text.size() && a_text[i + 1] == '?') ? 2 : 1;
				continue;
			}
			auto hex = [](char c) -> int {
				if (c >= '0' && c <= '9') {
					return c - '0';
				}
				if (c >= 'a' && c <= 'f') {
					return c - 'a' + 10;
				}
				if (c >= 'A' && c <= 'F') {
					return c - 'A' + 10;
				}
				return -1;
			};
			const int hi = hex(a_text[i]);
			const int lo = i + 1 < a_text.size() ? hex(a_text[i + 1]) : -1;
			if (hi < 0 || lo < 0) {
				return {};
			}
			p.bytes.push_back(static_cast<std::int16_t>(hi * 16 + lo));
			i += 2;
		}
		return p;
	}

	namespace Scan
	{
		std::vector<std::uint8_t*> All(const ModuleImage& a_image, const Pattern& a_pattern, bool a_codeOnly, std::size_t a_limit)
		{
			std::vector<std::uint8_t*> hits;
			const auto& bytes = a_pattern.bytes;
			if (bytes.empty()) {
				return hits;
			}
			// Anchor on the first non-wildcard byte for a fast memchr-driven search.
			std::size_t anchor = 0;
			while (anchor < bytes.size() && bytes[anchor] < 0) {
				++anchor;
			}
			if (anchor == bytes.size()) {
				return hits;
			}
			const auto first = static_cast<unsigned char>(bytes[anchor]);
			for (const auto& section : a_image.sections) {
				if (a_codeOnly && !section.executable) {
					continue;
				}
				if (section.size < bytes.size()) {
					continue;
				}
				const std::uint8_t* begin = section.start;
				const std::uint8_t* end = section.start + section.size - bytes.size() + 1;
				const std::uint8_t* cur = begin + anchor;
				while (cur < end + anchor) {
					cur = static_cast<const std::uint8_t*>(std::memchr(cur, first, static_cast<std::size_t>(end + anchor - cur)));
					if (!cur) {
						break;
					}
					const std::uint8_t* start = cur - anchor;
					bool                match = true;
					for (std::size_t k = 0; k < bytes.size(); ++k) {
						if (bytes[k] >= 0 && start[k] != static_cast<std::uint8_t>(bytes[k])) {
							match = false;
							break;
						}
					}
					if (match) {
						hits.push_back(const_cast<std::uint8_t*>(start));
						if (hits.size() >= a_limit) {
							return hits;
						}
					}
					++cur;
				}
			}
			return hits;
		}

		std::uint8_t* Unique(const ModuleImage& a_image, std::string_view a_name, std::string_view a_pattern, bool a_codeOnly)
		{
			const auto hits = All(a_image, Pattern::Parse(a_pattern), a_codeOnly, 4);
			if (hits.size() == 1) {
				Log::Info("found {} at +{:#x}", a_name, a_image.Rva(hits[0]));
				return hits[0];
			}
			if (hits.empty()) {
				Log::Error("signature {} not found", a_name);
			} else {
				Log::Error("signature {} is ambiguous ({}+ matches)", a_name, hits.size());
			}
			return nullptr;
		}
	}

	void** FindVtable(const ModuleImage& a_image, std::string_view a_mangledName)
	{
		// Type descriptor: { void* vftable; void* spare; char name[]; } -- find the name string.
		std::uint8_t* typeDesc = nullptr;
		for (const auto& section : a_image.sections) {
			if (section.executable) {
				continue;
			}
			const auto* begin = section.start;
			const auto* end = section.start + section.size;
			for (auto* p = begin; p + a_mangledName.size() + 1 <= end;) {
				p = static_cast<const std::uint8_t*>(std::memchr(p, a_mangledName[0], static_cast<std::size_t>(end - p)));
				if (!p || p + a_mangledName.size() + 1 > end) {
					break;
				}
				if (std::memcmp(p, a_mangledName.data(), a_mangledName.size()) == 0 && p[a_mangledName.size()] == 0) {
					if (typeDesc) {
						Log::Warn("RTTI {}: several type descriptors", a_mangledName);
						return nullptr;
					}
					typeDesc = const_cast<std::uint8_t*>(p) - 16;
				}
				++p;
			}
		}
		if (!typeDesc) {
			return nullptr;
		}
		const auto tdRva = static_cast<std::uint32_t>(a_image.Rva(typeDesc));
		// Complete object locator (x64): { u32 signature=1; u32 offset; u32 cdOffset; u32 pTypeDescriptor (rva);
		// u32 pClassDescriptor; u32 pSelf (rva) }.
		void** found = nullptr;
		for (const auto& section : a_image.sections) {
			if (section.executable) {
				continue;
			}
			for (std::size_t off = 0; off + 24 <= section.size; off += 4) {
				const auto* col = reinterpret_cast<const std::uint32_t*>(section.start + off);
				if (col[0] != 1 || col[1] != 0 || col[3] != tdRva) {
					continue;
				}
				if (col[5] != static_cast<std::uint32_t>(a_image.Rva(col))) {
					continue;
				}
				// The vtable is preceded by a pointer to its COL.
				const auto colAddr = reinterpret_cast<std::uintptr_t>(col);
				for (const auto& vsec : a_image.sections) {
					if (vsec.executable) {
						continue;
					}
					for (std::size_t voff = 0; voff + 16 <= vsec.size; voff += 8) {
						const auto* slot = reinterpret_cast<const std::uintptr_t*>(vsec.start + voff);
						if (*slot == colAddr) {
							if (found) {
								Log::Warn("RTTI {}: several primary vtables", a_mangledName);
								return nullptr;
							}
							found = reinterpret_cast<void**>(const_cast<std::uintptr_t*>(slot + 1));
						}
					}
				}
			}
		}
		return found;
	}

	namespace Hooks
	{
		namespace
		{
			std::once_flag g_init;
			bool           g_ok{ false };
		}

		bool Init()
		{
			std::call_once(g_init, [] {
				const auto status = MH_Initialize();
				g_ok = status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
				if (!g_ok) {
					Log::Error("MinHook init failed: {}", MH_StatusToString(status));
				}
			});
			return g_ok;
		}

		bool Create(std::string_view a_name, void* a_target, void* a_detour, void** a_original)
		{
			if (!Init() || !a_target) {
				Log::Error("hook {}: no target", a_name);
				return false;
			}
			const auto status = MH_CreateHook(a_target, a_detour, a_original);
			if (status != MH_OK) {
				Log::Error("hook {}: {}", a_name, MH_StatusToString(status));
				return false;
			}
			Log::Info("hook {} prepared at {}", a_name, a_target);
			return true;
		}

		bool EnableAll()
		{
			const auto status = MH_EnableHook(MH_ALL_HOOKS);
			if (status != MH_OK) {
				Log::Error("enabling hooks: {}", MH_StatusToString(status));
				return false;
			}
			return true;
		}

		void DisableAll() { MH_DisableHook(MH_ALL_HOOKS); }

		void* SwapVtable(void** a_vtable, std::size_t a_index, void* a_detour)
		{
			if (!a_vtable) {
				return nullptr;
			}
			void** slot = a_vtable + a_index;
			DWORD  old = 0;
			if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
				return nullptr;
			}
			void* previous = *slot;
			*slot = a_detour;
			::VirtualProtect(slot, sizeof(void*), old, &old);
			return previous;
		}
	}
}
