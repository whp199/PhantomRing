#include "MemDiff.h"

#include "Addresses.h"
#include "Game.h"
#include "InputBridge.h"
#include "Mem.h"
#include "Singletons.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <set>

#include <windows.h>

namespace phantom::er::MemDiff
{
	namespace
	{
		constexpr std::size_t kSingletonBytes = 0x4000;
		constexpr std::size_t kChildBytes = 0x800;
		constexpr std::size_t kMaxChildren = 30000;
		constexpr std::size_t kMaxSectionBytes = 64u << 20;

		struct Region
		{
			std::string   name;  // "exe .data", "CS::FD4PadManager", "CS::FD4PadManager+0x18 ->"
			std::uint8_t* at{ nullptr };
			std::size_t   bytes{ 0 };
		};

		struct Snapshot
		{
			std::string                            label;
			std::vector<std::vector<std::uint8_t>> data;  // per region; empty = unreadable at that time
		};

		std::vector<Region>   g_regions;
		std::vector<Snapshot> g_snaps;

		bool Readable(const void* a_p, std::size_t a_bytes)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (!::VirtualQuery(a_p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
				return false;
			}
			const auto end = static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
			return static_cast<const std::uint8_t*>(a_p) + a_bytes <= end;
		}

		// A heap object pointer: 8-aligned, outside the exe, in committed private read/write memory.
		bool HeapObject(std::uint64_t a_v)
		{
			if (a_v < 0x10000 || (a_v & 7) || a_v >= 0x7FFFFFFF0000ull || Addr().InImage(reinterpret_cast<void*>(a_v))) {
				return false;
			}
			MEMORY_BASIC_INFORMATION mbi{};
			return ::VirtualQuery(reinterpret_cast<void*>(a_v), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
			       (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
		}

		std::string Reset()
		{
			g_regions.clear();
			g_snaps.clear();
			auto* base = Addr().base;
			if (!base) {
				return "no image";
			}
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			const auto* sec = IMAGE_FIRST_SECTION(nt);
			for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
				const auto& s = sec[i];
				if ((s.Characteristics & IMAGE_SCN_MEM_WRITE) && s.Misc.VirtualSize && s.Misc.VirtualSize <= kMaxSectionBytes) {
					char name[9]{};
					std::memcpy(name, s.Name, 8);
					g_regions.push_back({ std::format("exe {} +{:#x}", name, s.VirtualAddress), base + s.VirtualAddress, s.Misc.VirtualSize });
				}
			}
			std::set<std::uint64_t> seen;
			std::size_t             children = 0;
			for (const auto& s : Singletons()) {
				auto* obj = SafePtr(s.slot);
				if (!obj || !seen.insert(reinterpret_cast<std::uint64_t>(obj)).second) {
					continue;
				}
				std::size_t bytes = kSingletonBytes;
				while (bytes > 0x100 && !Readable(obj, bytes)) {
					bytes /= 2;
				}
				std::vector<std::uint64_t> q(bytes / 8);
				if (!SafeReadBytes(obj, q.data(), bytes)) {
					continue;
				}
				g_regions.push_back({ s.name, obj, bytes });
				for (std::size_t i = 0; i < q.size() && children < kMaxChildren; ++i) {
					if (HeapObject(q[i]) && seen.insert(q[i]).second && Readable(reinterpret_cast<void*>(q[i]), kChildBytes)) {
						g_regions.push_back({ std::format("{}+{:#x} ->", s.name, i * 8), reinterpret_cast<std::uint8_t*>(q[i]), kChildBytes });
						++children;
					}
				}
			}
			std::size_t total = 0;
			for (const auto& r : g_regions) {
				total += r.bytes;
			}
			return std::format("snap: {} regions ({} singleton children), {:.1f} MB per snapshot", g_regions.size(), children, total / 1048576.0);
		}

		std::string Take(const std::string& a_label)
		{
			if (g_regions.empty()) {
				Reset();
			}
			Snapshot s;
			s.label = a_label;
			s.data.resize(g_regions.size());
			std::size_t unreadable = 0;
			for (std::size_t i = 0; i < g_regions.size(); ++i) {
				s.data[i].resize(g_regions[i].bytes);
				if (!SafeReadBytes(g_regions[i].at, s.data[i].data(), g_regions[i].bytes)) {
					s.data[i].clear();
					++unreadable;
				}
			}
			g_snaps.push_back(std::move(s));
			return std::format("snap #{} '{}' taken ({} regions, {} unreadable)", g_snaps.size() - 1, a_label, g_regions.size(), unreadable);
		}

		// The full list goes to PhantomRingHost.snapdiff.txt next to the DLL (the console answer holds 4 KB).
		std::string WriteFull(const std::string& a_text)
		{
			HMODULE self = nullptr;
			wchar_t path[MAX_PATH]{};
			if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&WriteFull),
					&self) ||
				!::GetModuleFileNameW(self, path, MAX_PATH)) {
				return "";
			}
			std::wstring p(path);
			p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"PhantomRingHost.snapdiff.txt";
			HANDLE f = ::CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f == INVALID_HANDLE_VALUE) {
				return "";
			}
			DWORD written = 0;
			::WriteFile(f, a_text.data(), static_cast<DWORD>(a_text.size()), &written, nullptr);
			::CloseHandle(f);
			return " (all in PhantomRingHost.snapdiff.txt)";
		}

		std::string Diff(const std::string& a_a, const std::string& a_b, std::size_t a_max, int a_va = -1, int a_vb = -1)
		{
			std::vector<const Snapshot*> as, bs;
			for (const auto& s : g_snaps) {
				if (s.label == a_a) {
					as.push_back(&s);
				} else if (s.label == a_b) {
					bs.push_back(&s);
				}
			}
			if (as.empty() || bs.empty()) {
				return std::format("need snapshots labelled '{}' and '{}' ({} taken)", a_a, a_b, g_snaps.size());
			}
			std::string out;
			std::size_t hits = 0;
			for (std::size_t r = 0; r < g_regions.size(); ++r) {
				auto usable = [r](const Snapshot* s) { return s->data[r].size() == g_regions[r].bytes; };
				if (!std::all_of(as.begin(), as.end(), usable) || !std::all_of(bs.begin(), bs.end(), usable)) {
					continue;
				}
				for (std::size_t o = 0; o < g_regions[r].bytes; ++o) {
					const auto va = as[0]->data[r][o];
					const auto vb = bs[0]->data[r][o];
					if (va == vb || (a_va >= 0 && va != a_va) || (a_vb >= 0 && vb != a_vb)) {
						continue;
					}
					bool ok = true;
					for (const auto* s : as) {
						ok = ok && s->data[r][o] == va;
					}
					for (const auto* s : bs) {
						ok = ok && s->data[r][o] == vb;
					}
					if (!ok) {
						continue;
					}
					++hits;
					auto*      at = g_regions[r].at + o;
					const auto where = Addr().InImage(at) ? std::format("eldenring+{:#x}", Addr().Rva(at)) : std::format("{}", static_cast<void*>(at));
					out += std::format("{} +{:#x} ({}): {} {:#04x} -> {} {:#04x}\n", g_regions[r].name, o, where, a_a, va, a_b, vb);
				}
			}
			const auto file = WriteFull(out);
			std::size_t cut = 0;
			for (std::size_t i = 0; i < a_max && cut != std::string::npos; ++i) {
				cut = out.find('\n', cut ? cut + 1 : 0);
			}
			return std::format("{} byte(s) follow '{}' ({}x) vs '{}' ({}x){}\n{}", hits, a_a, as.size(), a_b, bs.size(), file, cut == std::string::npos ? out : out.substr(0, cut + 1));
		}
	}

	namespace
	{
		// Object trees keyed by pointer path, re-walked at every take: survives the objects being reallocated (a load).
		struct TreeSnap
		{
			std::string                                       label;
			std::map<std::string, std::vector<std::uint8_t>> objects;  // "+0x50>+0x18" -> bytes
		};
		std::vector<TreeSnap> g_trees;
		std::uint8_t*         g_treeRoot{ nullptr };  // nullptr = the main player

		bool ClassObject(std::uint64_t a_v)
		{
			if (!HeapObject(a_v)) {
				return false;
			}
			const auto vt = SafeRead<std::uint8_t*>(reinterpret_cast<void*>(a_v));
			return vt && Addr().InImage(*vt);
		}

		std::string TreeTake(const std::string& a_label, int a_depth, std::size_t a_bytes)
		{
			auto* root = g_treeRoot ? g_treeRoot : game::MainPlayer().chr;
			if (!root) {
				return "no root (no player)";
			}
			TreeSnap snap;
			snap.label = a_label;
			std::vector<std::pair<std::string, std::uint8_t*>> level{ { "chr", root } };
			std::set<std::uint8_t*>                             seen{ root };
			for (int d = 0; d <= a_depth && !level.empty() && snap.objects.size() < 6000; ++d) {
				std::vector<std::pair<std::string, std::uint8_t*>> next;
				for (const auto& [path, obj] : level) {
					std::vector<std::uint8_t> bytes(a_bytes);
					if (!SafeReadBytes(obj, bytes.data(), a_bytes)) {
						continue;
					}
					for (std::size_t o = 0; o + 8 <= a_bytes && d < a_depth; o += 8) {
						std::uint64_t v;
						std::memcpy(&v, bytes.data() + o, 8);
						auto* child = reinterpret_cast<std::uint8_t*>(v);
						if (ClassObject(v) && seen.insert(child).second) {
							next.push_back({ std::format("{}>+{:#x}", path, o), child });
						}
					}
					snap.objects.emplace(path, std::move(bytes));
				}
				level = std::move(next);
			}
			g_trees.push_back(std::move(snap));
			return std::format("tree #{} '{}': {} objects (depth {}, {:#x} bytes each)", g_trees.size() - 1, a_label, g_trees.back().objects.size(), a_depth, a_bytes);
		}

		// i32 fields that hold one value in every <a> snapshot and another in every <b> one (same path).
		std::string TreeDiff(const std::string& a_a, const std::string& a_b, std::size_t a_max, bool a_small)
		{
			std::vector<const TreeSnap*> as, bs;
			for (const auto& s : g_trees) {
				if (s.label == a_a) {
					as.push_back(&s);
				} else if (s.label == a_b) {
					bs.push_back(&s);
				}
			}
			if (as.empty() || bs.empty()) {
				return std::format("need trees labelled '{}' and '{}'", a_a, a_b);
			}
			std::string out;
			std::size_t hits = 0;
			for (const auto& [path, bytes0] : as[0]->objects) {
				auto get = [&path](const TreeSnap* s) -> const std::vector<std::uint8_t>* {
					const auto it = s->objects.find(path);
					return it == s->objects.end() ? nullptr : &it->second;
				};
				bool all = true;
				for (const auto* s : as) all = all && get(s);
				for (const auto* s : bs) all = all && get(s);
				if (!all) {
					continue;
				}
				for (std::size_t o = 0; o + 4 <= bytes0.size(); o += 4) {
					std::int32_t va, vb;
					std::memcpy(&va, bytes0.data() + o, 4);
					std::memcpy(&vb, get(bs[0])->data() + o, 4);
					if (va == vb || (a_small && (va < -1 || va > 255 || vb < -1 || vb > 255))) {
						continue;
					}
					bool ok = true;
					for (const auto* s : as) {
						std::int32_t v;
						std::memcpy(&v, get(s)->data() + o, 4);
						ok = ok && v == va;
					}
					for (const auto* s : bs) {
						std::int32_t v;
						std::memcpy(&v, get(s)->data() + o, 4);
						ok = ok && v == vb;
					}
					if (ok) {
						++hits;
						out += std::format("{}+{:#x}: {} -> {}\n", path, o, va, vb);
					}
				}
			}
			const auto file = WriteFull(out);
			return std::format("{} i32 field(s) follow '{}' ({}x) vs '{}' ({}x){}{}\n{}", hits, a_a, as.size(), a_b, bs.size(), a_small ? " (values -1..255)" : "", file,
				out.substr(0, std::min<std::size_t>(out.size(), a_max * 60)));
		}
	}

	std::string Command(const std::vector<std::string>& w)
	{
		if (w.size() > 1 && w[1] == "tree") {
			// snap tree root <address|player> | take <label> [depth [bytes]] | diff <a> <b> [max] [small] | clear
			const std::string sub = w.size() > 2 ? w[2] : "";
			if (sub == "root" && w.size() > 3) {
				g_treeRoot = w[3] == "player" ? nullptr : reinterpret_cast<std::uint8_t*>(std::strtoull(w[3].c_str(), nullptr, 0));
				return std::format("tree root {}", g_treeRoot ? std::format("{}", static_cast<void*>(g_treeRoot)) : std::string("= the main player"));
			}
			if (sub == "take" && w.size() > 3) {
				return TreeTake(w[3], w.size() > 4 ? std::atoi(w[4].c_str()) : 3, w.size() > 5 ? std::strtoull(w[5].c_str(), nullptr, 0) : 0x400);
			}
			if (sub == "diff" && w.size() > 4) {
				return TreeDiff(w[3], w[4], w.size() > 5 ? std::strtoul(w[5].c_str(), nullptr, 10) : 80, w.size() > 6 && w[6] == "small");
			}
			if (sub == "clear") {
				g_trees.clear();
				return "trees cleared";
			}
			return std::format("snap tree root <address|player> | take <label> [depth [bytes]] | diff <a> <b> [max] [small] | clear ({} trees)", g_trees.size());
		}
		const std::string sub = w.size() > 1 ? w[1] : "";
		if (sub == "reset") {
			return Reset();
		}
		if (sub == "take" && w.size() > 2) {
			return Take(w[2]);
		}
		if (sub == "diff" && w.size() > 3) {
			// snap diff <a> <b> [max] [value in a] [value in b]
			return Diff(w[2], w[3], w.size() > 4 ? std::strtoul(w[4].c_str(), nullptr, 10) : 80, w.size() > 5 ? std::atoi(w[5].c_str()) : -1,
				w.size() > 6 ? std::atoi(w[6].c_str()) : -1);
		}
		if (sub == "add" && w.size() > 2) {
			// snap add <address> [bytes]: one more region (an object found with peek/vt), kept for the next snapshots
			auto* at = reinterpret_cast<std::uint8_t*>(std::strtoull(w[2].c_str(), nullptr, 0));
			const std::size_t bytes = w.size() > 3 ? std::strtoull(w[3].c_str(), nullptr, 0) : 0x1000;
			if (!Readable(at, bytes)) {
				return std::format("{} +{:#x} is not readable", static_cast<void*>(at), bytes);
			}
			g_regions.push_back({ std::format("added {}", static_cast<void*>(at)), at, bytes });
			for (auto& snap : g_snaps) {
				snap.data.emplace_back();  // older snapshots never saw it
			}
			return std::format("snap: region {} added ({} regions)", g_regions.size() - 1, g_regions.size());
		}
		if (sub == "clear") {
			g_regions.clear();
			g_snaps.clear();
			return "snap: cleared";
		}
		return std::format("snap reset | add <address> [bytes] | take <label> | diff <a> <b> [max] | clear  ({} regions, {} snapshots)", g_regions.size(), g_snaps.size());
	}

	std::string Input(const std::vector<std::string>& w)
	{
		INPUT in[2]{};
		UINT  n = 0;
		if (w.size() > 2 && (w[1] == "key" || w[1] == "keydown" || w[1] == "keyup")) {
			// keydown / keyup separately: Elden Ring reads the keyboard's state once a frame, so a down+up in one call is
			// never seen (no controller connected: the title screen then needs the keyboard).
			const auto vk = static_cast<WORD>(std::strtoul(w[2].c_str(), nullptr, 0));
			for (auto& i : in) {
				i.type = INPUT_KEYBOARD;
				i.ki.wVk = vk;
				i.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
			}
			in[0].ki.dwFlags = w[1] == "keyup" ? KEYEVENTF_KEYUP : 0;
			in[1].ki.dwFlags = KEYEVENTF_KEYUP;
			n = w[1] == "key" ? 2 : 1;
		} else if (w.size() > 2 && w[1] == "virtualpad") {
			return InputBridge::SetVirtualPad(w[2] == "on");
		} else if (w.size() > 3 && w[1] == "mouse") {
			in[0].type = INPUT_MOUSE;
			in[0].mi.dx = std::strtol(w[2].c_str(), nullptr, 10);
			in[0].mi.dy = std::strtol(w[3].c_str(), nullptr, 10);
			in[0].mi.dwFlags = MOUSEEVENTF_MOVE;
			n = 1;
		} else {
			return "input key|keydown|keyup <vk> | mouse <dx> <dy> | virtualpad on|off";
		}
		const UINT sent = ::SendInput(n, in, sizeof(INPUT));
		return std::format("SendInput: {}/{} event(s) (foreground window {})", sent, n, static_cast<void*>(::GetForegroundWindow()));
	}
}
