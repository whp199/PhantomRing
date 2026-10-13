#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <windows.h>

namespace phantom
{
	// A loaded module's image as sections, for signature scans.
	struct ModuleImage
	{
		std::uint8_t* base{ nullptr };
		std::size_t   size{ 0 };

		struct Section
		{
			char          name[9]{};
			std::uint8_t* start{ nullptr };
			std::size_t   size{ 0 };
			bool          executable{ false };
		};
		std::vector<Section> sections;

		static ModuleImage Of(HMODULE a_module);
		[[nodiscard]] std::uintptr_t Rva(const void* a_address) const
		{
			return reinterpret_cast<std::uintptr_t>(a_address) - reinterpret_cast<std::uintptr_t>(base);
		}
		[[nodiscard]] bool Contains(const void* a_address) const
		{
			return a_address >= base && a_address < base + size;
		}
	};

	// "48 8B 05 ?? ?? ?? ?? 48 85 C0" style patterns ('?' or '??' is a wildcard byte).
	struct Pattern
	{
		std::vector<std::int16_t> bytes;  // -1 = wildcard
		static Pattern Parse(std::string_view a_text);
	};

	namespace Scan
	{
		// Every match in the module's executable sections (or all sections when a_codeOnly is false).
		std::vector<std::uint8_t*> All(const ModuleImage& a_image, const Pattern& a_pattern, bool a_codeOnly = true, std::size_t a_limit = 16);
		// The single match, or nullptr if there are none or more than one (logs which).
		std::uint8_t* Unique(const ModuleImage& a_image, std::string_view a_name, std::string_view a_pattern, bool a_codeOnly = true);
		// Resolves a RIP-relative operand: disp32 at a_at + a_dispOffset, relative to a_at + a_instrLen.
		inline std::uint8_t* Rip(std::uint8_t* a_at, std::size_t a_dispOffset, std::size_t a_instrLen)
		{
			if (!a_at) {
				return nullptr;
			}
			std::int32_t disp;
			std::memcpy(&disp, a_at + a_dispOffset, 4);
			return a_at + a_instrLen + disp;
		}
	}

	// RTTI: the vtable of a class by its MSVC type-descriptor name (".?AVCSCamera@CS@@").
	// Walks .rdata for the RTTICompleteObjectLocator whose type descriptor matches and that has
	// offset 0 (the primary vtable). Returns nullptr if not found or ambiguous.
	void** FindVtable(const ModuleImage& a_image, std::string_view a_mangledName);

	// MinHook wrapper. Hooks are created disabled; EnableAll() turns on every created hook at once.
	namespace Hooks
	{
		bool Init();
		// Detour a function; a_original receives the trampoline.
		bool Create(std::string_view a_name, void* a_target, void* a_detour, void** a_original);
		template <class F>
		bool Create(std::string_view a_name, void* a_target, F* a_detour, F** a_original)
		{
			return Create(a_name, a_target, reinterpret_cast<void*>(a_detour), reinterpret_cast<void**>(a_original));
		}
		bool EnableAll();
		void DisableAll();

		// Swap a vtable slot (no code patched: safe against code-integrity checks). Returns the old
		// function, or nullptr on failure.
		void* SwapVtable(void** a_vtable, std::size_t a_index, void* a_detour);
	}

	// Safe reads of game memory that may be stale (returns nullopt on access violation).
	template <class T>
	std::optional<T> SafeRead(const void* a_address)
	{
		T value{};
		SIZE_T read = 0;
		if (!a_address || !::ReadProcessMemory(::GetCurrentProcess(), a_address, &value, sizeof(T), &read) || read != sizeof(T)) {
			return std::nullopt;
		}
		return value;
	}
}
