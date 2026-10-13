#include "Singletons.h"

#include "Addresses.h"
#include "Hook.h"
#include "Log.h"
#include "Mem.h"

#include <cstring>
#include <unordered_map>

namespace phantom::er
{
	namespace
	{
		std::vector<SingletonInfo> g_found;

		const ModuleImage::Section* SectionOf(const ModuleImage& a_image, const char* a_name)
		{
			for (const auto& s : a_image.sections) {
				if (std::strcmp(s.name, a_name) == 0) {
					return &s;
				}
			}
			return nullptr;
		}

		bool In(const ModuleImage::Section* a_section, const void* a_p)
		{
			return a_section && a_p >= a_section->start && a_p < a_section->start + a_section->size;
		}

		std::string_view Simple(std::string_view a_name)
		{
			const auto colon = a_name.rfind(':');
			return colon == std::string_view::npos ? a_name : a_name.substr(colon + 1);
		}

		std::int32_t Rel32(const std::uint8_t* a_p)
		{
			std::int32_t v;
			std::memcpy(&v, a_p, 4);
			return v;
		}

		// The slots the address table resolved by AOB, and the singleton each must be.
		struct Expect
		{
			A           slot;
			const char* name;
		};
		constexpr Expect kExpected[] = {
			{ A::WorldChrMan, "CS::WorldChrMan" },
			{ A::CSMenuMan, "CS::CSMenuMan" },
			{ A::CSFlipper, "CS::CSFlipper" },
			{ A::CSCamera, "CS::CSCamera" },
			{ A::CSTask, "CS::CSTask" },
			{ A::CSTaskGroup, "CS::CSTaskGroup" },
			{ A::FD4PadManager, "FD4PadManager" },
			{ A::CSHavokMan, "CS::CSHavokMan" },
			{ A::RendMan, "CS::RendMan" },
			{ A::CSFade, "CS::CSFade" },
			{ A::CSNowLoadingHelper, "CS::CSNowLoadingHelper" },
			{ A::WorldAreaTime, "CS::WorldAreaTime" },
			{ A::LockTgtMan, "CS::LockTgtMan" },
		};
	}

	std::size_t FindSingletons(bool a_nameThem)
	{
		g_found.clear();
		const auto& addr = Addr();
		if (!addr.Has(A::GetSingletonName)) {
			Log::Warn("singleton finder: get_singleton_name unverified, skipped");
			return 0;
		}
		const auto image = ModuleImage::Of(reinterpret_cast<HMODULE>(addr.base));
		const auto* text = SectionOf(image, ".text");
		const auto* data = SectionOf(image, ".data");
		const auto* rdata = SectionOf(image, ".rdata");
		if (!text || !data || !rdata) {
			Log::Warn("singleton finder: sections missing");
			return 0;
		}
		ModuleImage textOnly = image;
		textOnly.sections = { *text };
		const auto sites = Scan::All(textOnly,
			Pattern::Parse("48 8B ?? ?? ?? ?? ?? 48 85 ?? 75 2E 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B C8 4C 8D 05 ?? ?? ?? ?? BA ?? ?? 00 00 48 8D 0D ?? ?? ?? ?? E8"),
			true, 0x4000);

		using NameFn = const char* (*)(const void*);
		const auto getName = addr.Fn<NameFn>(A::GetSingletonName);
		std::unordered_map<std::uint8_t*, std::uint8_t*> slotToTag;
		for (auto* site : sites) {
			auto* slot = site + 7 + Rel32(site + 3);
			auto* tag = site + 19 + Rel32(site + 15);
			auto* fn = site + 24 + Rel32(site + 20);
			const auto* path = reinterpret_cast<const char*>(site + 46 + Rel32(site + 42));
			if (!In(data, slot) || !In(data, tag) || fn != addr[A::GetSingletonName] || !In(rdata, path)) {
				continue;
			}
			const auto len = strnlen(path, 256);
			if (len < 14 || len == 256 || std::strcmp(path + len - 14, "FD4Singleton.h") != 0) {
				continue;
			}
			slotToTag.emplace(slot, tag);
		}
		if (!a_nameThem) {
			Log::Info("singleton finder (scan only): {} null-check sites, {} slots", sites.size(), slotToTag.size());
			return slotToTag.size();
		}
		for (const auto& [slot, tag] : slotToTag) {
			const char* name = getName(tag);
			if (name && addr.InImage(name)) {
				g_found.push_back({ std::string(name, strnlen(name, 128)), slot });
			}
		}
		Log::Info("singleton finder: {} null-check sites, {} singletons named", sites.size(), g_found.size());

		for (const auto& e : kExpected) {
			if (!addr.Has(e.slot)) {
				continue;
			}
			const auto* found = static_cast<const SingletonInfo*>(nullptr);
			for (const auto& s : g_found) {
				if (s.slot == addr[e.slot]) {
					found = &s;
				}
			}
			if (!found) {
				Log::Warn("singleton finder: slot +{:#x} ({}) has no FD4 null-check site", addr.Rva(addr[e.slot]), e.name);
			} else if (Simple(found->name) != Simple(e.name)) {
				Log::Warn("singleton finder: slot +{:#x} is named {}, expected {}", addr.Rva(addr[e.slot]), found->name, e.name);
			} else {
				Log::Debug("singleton finder: slot +{:#x} is {} as expected", addr.Rva(addr[e.slot]), found->name);
			}
		}
		return g_found.size();
	}

	const std::vector<SingletonInfo>& Singletons() { return g_found; }

	std::uint8_t* SingletonSlot(std::string_view a_name)
	{
		for (const auto& s : g_found) {
			if (s.name == a_name || Simple(s.name) == a_name) {
				return s.slot;
			}
		}
		return nullptr;
	}
}
