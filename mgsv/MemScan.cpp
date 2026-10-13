#include "MemScan.h"

#include "Hook.h"
#include "Log.h"

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <sstream>
#include <vector>

namespace phantom::guest::memscan
{
	namespace
	{
		enum class Type
		{
			kU16,
			kI32,
			kF32,
		};

		std::mutex                  g_lock;
		Type                        g_type{ Type::kI32 };
		std::vector<std::uintptr_t> g_hits;

		bool ParseType(std::string_view a_name, Type& a_out)
		{
			if (a_name == "u16") {
				a_out = Type::kU16;
			} else if (a_name == "i32") {
				a_out = Type::kI32;
			} else if (a_name == "f32") {
				a_out = Type::kF32;
			} else {
				return false;
			}
			return true;
		}

		std::size_t Size(Type a_type) { return a_type == Type::kU16 ? 2 : 4; }

		bool Matches(Type a_type, const std::uint8_t* a_p, double a_value)
		{
			switch (a_type) {
			case Type::kU16:
				{
					std::uint16_t v;
					std::memcpy(&v, a_p, 2);
					return v == static_cast<std::uint16_t>(a_value);
				}
			case Type::kI32:
				{
					std::int32_t v;
					std::memcpy(&v, a_p, 4);
					return v == static_cast<std::int32_t>(a_value);
				}
			default:
				{
					float v;
					std::memcpy(&v, a_p, 4);
					return v == v && std::abs(static_cast<double>(v) - a_value) < 0.01;
				}
			}
		}

		std::string ValueAt(Type a_type, std::uintptr_t a_addr)
		{
			switch (a_type) {
			case Type::kU16:
				{
					const auto v = SafeRead<std::uint16_t>(reinterpret_cast<const void*>(a_addr));
					return v ? std::to_string(*v) : "?";
				}
			case Type::kI32:
				{
					const auto v = SafeRead<std::int32_t>(reinterpret_cast<const void*>(a_addr));
					return v ? std::to_string(*v) : "?";
				}
			default:
				{
					const auto v = SafeRead<float>(reinterpret_cast<const void*>(a_addr));
					return v ? std::format("{:.3f}", *v) : "?";
				}
			}
		}

		// Every committed, writable, non-guard region (private and image data alike).
		template <class F>
		void ForEachRegion(F&& a_fn)
		{
			std::uintptr_t addr = 0x10000;
			MEMORY_BASIC_INFORMATION mbi{};
			while (addr < 0x7FFFFFFF0000ull && ::VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
				const auto next = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
				const DWORD writable = PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
				if (mbi.State == MEM_COMMIT && (mbi.Protect & writable) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
					a_fn(static_cast<const std::uint8_t*>(mbi.BaseAddress), mbi.RegionSize);
				}
				if (next <= addr) {
					break;
				}
				addr = next;
			}
		}
	}

	bool Command(std::string_view a_cmd, std::string& a_out)
	{
		std::istringstream in{ std::string(a_cmd) };
		std::string        verb;
		in >> verb;
		if (verb == "scan") {
			std::string tname;
			double      value = 0;
			Type        type;
			if (!(in >> tname >> value) || !ParseType(tname, type)) {
				a_out = "usage: !scan <u16|i32|f32> <value>";
				return true;
			}
			std::vector<std::uintptr_t> hits;
			std::uint64_t               bytes = 0;
			const auto                  step = Size(type);
			ForEachRegion([&](const std::uint8_t* a_base, std::size_t a_size) {
				// Copy the region out under SEH-free reads (ReadProcessMemory on ourselves): a region can
				// be freed by a game thread while we look at it.
				std::vector<std::uint8_t> buf(std::min<std::size_t>(a_size, 64u << 20));
				for (std::size_t at = 0; at < a_size; at += buf.size()) {
					const std::size_t n = std::min(buf.size(), a_size - at);
					SIZE_T            got = 0;
					if (!::ReadProcessMemory(::GetCurrentProcess(), a_base + at, buf.data(), n, &got) || got < step) {
						continue;
					}
					bytes += got;
					for (std::size_t i = 0; i + step <= got; i += step) {
						if (Matches(type, buf.data() + i, value) && hits.size() < 4'000'000) {
							hits.push_back(reinterpret_cast<std::uintptr_t>(a_base + at + i));
						}
					}
				}
			});
			std::scoped_lock lock(g_lock);
			g_type = type;
			g_hits = std::move(hits);
			a_out = std::format("scan {} {}: {} hits in {:.0f} MB", tname, value, g_hits.size(), bytes / 1048576.0);
			return true;
		}
		if (verb == "rescan") {
			double value = 0;
			if (!(in >> value)) {
				a_out = "usage: !rescan <value>";
				return true;
			}
			std::scoped_lock            lock(g_lock);
			std::vector<std::uintptr_t> keep;
			std::uint8_t                v[4]{};
			for (const auto a : g_hits) {
				SIZE_T got = 0;
				if (::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<const void*>(a), v, Size(g_type), &got) && got == Size(g_type) &&
					Matches(g_type, v, value)) {
					keep.push_back(a);
				}
			}
			g_hits = std::move(keep);
			a_out = std::format("rescan {}: {} hits left", value, g_hits.size());
			return true;
		}
		if (verb == "scanlist") {
			std::scoped_lock lock(g_lock);
			a_out = std::format("{} hits", g_hits.size());
			for (std::size_t i = 0; i < g_hits.size() && i < 32; ++i) {
				a_out += std::format("\n  {:#x} = {}", g_hits[i], ValueAt(g_type, g_hits[i]));
			}
			return true;
		}
		if (verb == "peek" || verb == "poke") {
			std::string addrText, tname;
			double      value = 0;
			Type        type;
			if (!(in >> addrText >> tname) || !ParseType(tname, type) || (verb == "poke" && !(in >> value))) {
				a_out = std::format("usage: !{} <addr> <u16|i32|f32>{}", verb, verb == "poke" ? " <value>" : "");
				return true;
			}
			const auto addr = static_cast<std::uintptr_t>(std::strtoull(addrText.c_str(), nullptr, 0));
			if (verb == "poke") {
				std::uint8_t b[4]{};
				if (type == Type::kU16) {
					const auto v = static_cast<std::uint16_t>(value);
					std::memcpy(b, &v, 2);
				} else if (type == Type::kI32) {
					const auto v = static_cast<std::int32_t>(value);
					std::memcpy(b, &v, 4);
				} else {
					const auto v = static_cast<float>(value);
					std::memcpy(b, &v, 4);
				}
				SIZE_T put = 0;
				if (!::WriteProcessMemory(::GetCurrentProcess(), reinterpret_cast<void*>(addr), b, Size(type), &put)) {
					a_out = std::format("poke {:#x} failed ({})", addr, ::GetLastError());
					return true;
				}
				Log::Info("memscan: poked {:#x} = {}", addr, value);
			}
			a_out = std::format("{:#x} = {}", addr, ValueAt(type, addr));
			return true;
		}
		return false;
	}
}
