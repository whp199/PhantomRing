#include "Console.h"

#include "Addresses.h"
#include "Buddy.h"
#include "BuddyOrders.h"
#include "Calibration.h"
#include "CameraDriver.h"
#include "Combat.h"
#include "Cqc.h"
#include "Config.h"
#include "ConsoleExpr.h"
#include "DebugDraw.h"
#include "Explosion.h"
#include "Fulton.h"
#include "TimeSkip.h"
#include "Game.h"
#include "HitHook.h"
#include "Hud.h"
#include "Host.h"
#include "InputBridge.h"
#include "Link.h"
#include "Log.h"
#include "Marking.h"
#include "Mem.h"
#include "MemDiff.h"
#include "MenuWatch.h"
#include "Decoys.h"
#include "Mines.h"
#include "Pacing.h"
#include "PacingProbe.h"
#include "Puppet.h"
#include "GeomProbe.h"
#include "Progression.h"
#include "Stealth.h"
#include "Renderer.h"
#include "Singletons.h"
#include "Throw.h"
#include "WorldShift.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <sstream>
#include <utility>
#include <vector>

namespace phantom::er::Console
{
	namespace
	{
		struct TypeInfo
		{
			std::string_view name;
			std::size_t      size;
		};
		constexpr TypeInfo kTypes[] = {
			{ "u8", 1 }, { "i8", 1 }, { "u16", 2 }, { "i16", 2 }, { "u32", 4 }, { "i32", 4 }, { "u64", 8 }, { "i64", 8 },
			{ "f32", 4 }, { "f64", 8 }, { "ptr", 8 }, { "vec4", 16 }, { "str", 64 }, { "wstr", 128 },
		};

		const TypeInfo* FindType(std::string_view a_name)
		{
			for (const auto& t : kTypes) {
				if (t.name == a_name) {
					return &t;
				}
			}
			return nullptr;
		}

		struct Symbol
		{
			std::string_view                        name;
			std::function<std::uint8_t*(const Frame&)> value;
			std::string_view                        type;  // default type for peek
			std::string_view                        help;
		};

		std::uint8_t* Inst(A a_slot) { return Addr().Instance(a_slot); }
		std::uint8_t* Off(std::uint8_t* a_p, std::uint32_t a_off) { return a_p ? a_p + a_off : nullptr; }

		const std::vector<Symbol>& Symbols()
		{
			using F = const Frame&;
			static const std::vector<Symbol> symbols = {
				{ "base", [](F) { return Addr().base; }, "u64", "eldenring.exe image base" },
				{ "wcm", [](F) { return game::WorldChrMan(); }, "ptr", "WorldChrMan instance" },
				{ "player", [](F f) { return f.player.chr; }, "ptr", "main player ChrIns (PlayerIns)" },
				{ "modules", [](F f) { return f.player.modules; }, "ptr", "player ChrInsModuleContainer" },
				{ "phys", [](F f) { return f.player.phys; }, "ptr", "player CSChrPhysicsModule" },
				{ "data", [](F f) { return f.player.data; }, "ptr", "player CSChrDataModule" },
				{ "chrctrl", [](F f) { return f.player.ctrl; }, "ptr", "player PlayerCtrl (ChrIns+0x58)" },
				{ "player.pos", [](F f) { return Off(f.player.phys, game::kPhysPosition); }, "vec4", "physics position (Havok)" },
				{ "player.quat", [](F f) { return Off(f.player.phys, game::kPhysOrientation); }, "vec4", "physics orientation xyzw" },
				{ "player.hp", [](F f) { return Off(f.player.data, game::kDataHp); }, "i32", "hp" },
				{ "player.maxhp", [](F f) { return Off(f.player.data, game::kDataMaxHp); }, "i32", "max hp" },
				{ "player.dbg", [](F f) { return Off(f.player.data, game::kDataDebug); }, "u8", "data debug byte (b0 NoDead b1 NoDamage)" },
				{ "player.flags", [](F f) { return Off(f.player.chr, game::kChrFlags1C5); }, "u8", "ChrIns+0x1C5 (b3 render b7 dead)" },
				{ "player.chrdbg", [](F f) { return Off(f.player.chr, game::kChrDebugFlags); }, "u32", "ChrIns+0x538 debug flags" },
				{ "player.team", [](F f) { return Off(f.player.chr, game::kChrTeam); }, "u8", "team type" },
				{ "cam", [](F) { return Inst(A::CSCamera); }, "ptr", "CSCamera instance" },
				{ "perscam1", [](F) { return game::PersCam1(); }, "ptr", "CSCamera.pers_cam_1 (render cam)" },
				{ "cam.pos", [](F) { return Off(game::PersCam1(), game::kCamMatrix + 0x30); }, "vec4", "render cam position row" },
				{ "cam.fwd", [](F) { return Off(game::PersCam1(), game::kCamMatrix + 0x20); }, "vec4", "render cam +0x30 row" },
				{ "cam.fov", [](F) { return Off(game::PersCam1(), game::kCamFov); }, "f32", "render cam fov" },
				{ "chrcam", [](F) { return game::ChrCamObject(); }, "ptr", "WorldChrMan.chr_cam" },
				{ "compasscam", [](F) { return game::GameRendCam(); }, "ptr", "GameRend's first camera: the HUD compass's heading (CSFeMan+0x1074) comes from it" },
				{ "padman", [](F) { return Inst(A::FD4PadManager); }, "ptr", "FD4PadManager" },
				{ "menuman", [](F) { return Inst(A::CSMenuMan); }, "ptr", "CSMenuMan" },
				{ "menu.blocking", [](F) { return Off(Inst(A::CSMenuMan), game::kMenuBlocking); }, "u64", "CSMenuMan+0x798: non-zero = blocking menu (E4)" },
				{ "remo", [](F) { return Inst(A::CSRemo); }, "ptr", "CSRemo (cutscenes; +8 = playing remo, +0x14C bit0) (E4)" },
				{ "actionbtn", [](F) { return Inst(A::CSActionButtonMan); }, "ptr", "CSActionButtonManImp (+0x20 prompt entry, +0x81 pressed) (E6)" },
				{ "dmgmod", [](F f) { return f.player.modules ? SafePtr(f.player.modules + 0x98) : nullptr; }, "ptr", "player CSPlayerDamageModule (E2)" },
				{ "havok", [](F) { return Inst(A::CSHavokMan); }, "ptr", "CSHavokMan" },
				{ "physworld", [](F) {
					 auto* havok = Inst(A::CSHavokMan);
					 return havok ? SafePtr(havok + 0x98) : nullptr;
				 }, "ptr", "CSPhysWorld" },
				{ "rendman", [](F) { return Inst(A::RendMan); }, "ptr", "RendMan" },
				{ "ezdraw", [](F) { return game::EzDraw(); }, "ptr", "RendMan's CSEzDraw" },
				{ "task", [](F) { return Inst(A::CSTask); }, "ptr", "CSTaskImp" },
				{ "flipper", [](F) { return Inst(A::CSFlipper); }, "ptr", "CSFlipper" },
				{ "fade", [](F) { return Inst(A::CSFade); }, "ptr", "CSFade" },
				{ "loading", [](F) { return Inst(A::CSNowLoadingHelper); }, "ptr", "CSNowLoadingHelper" },
				{ "gamedataman", [](F) { return Inst(A::GameDataMan); }, "ptr", "GameDataMan" },
				{ "gameman", [](F) { return Inst(A::GameMan); }, "ptr", "GameMan" },
				{ "fieldarea", [](F) { return Inst(A::FieldArea); }, "ptr", "FieldArea" },
				{ "time", [](F) { return Inst(A::WorldAreaTime); }, "ptr", "WorldAreaTime" },
				{ "locktgt", [](F) { return Inst(A::LockTgtMan); }, "ptr", "LockTgtMan" },
			};
			return symbols;
		}

		const Symbol* FindSymbol(std::string_view a_name)
		{
			for (const auto& s : Symbols()) {
				if (s.name == a_name) {
					return &s;
				}
			}
			return nullptr;
		}

		ExprEnv Env(const Frame& a_f)
		{
			ExprEnv env;
			env.symbol = [&a_f](std::string_view a_name) -> std::optional<std::uint64_t> {
				if (const auto* s = FindSymbol(a_name)) {
					auto* p = s->value(a_f);
					return p ? std::optional<std::uint64_t>(reinterpret_cast<std::uint64_t>(p)) : std::nullopt;
				}
				if (a_name.starts_with("s.")) {  // any FD4 singleton slot by name: [s.CSFade]
					if (auto* slot = SingletonSlot(a_name.substr(2))) {
						return reinterpret_cast<std::uint64_t>(slot);
					}
				}
				return std::nullopt;
			};
			env.read64 = [](std::uint64_t a_address) { return SafeRead<std::uint64_t>(reinterpret_cast<const void*>(a_address)); };
			return env;
		}

		std::vector<std::string> Split(std::string_view a_text)
		{
			std::vector<std::string> out;
			std::istringstream       in{ std::string(a_text) };
			std::string              word;
			while (in >> word) {
				out.push_back(word);
			}
			return out;
		}

		std::string Join(const std::vector<std::string>& a_words, std::size_t a_from, std::size_t a_to)
		{
			std::string out;
			for (auto i = a_from; i < a_to && i < a_words.size(); ++i) {
				out += (out.empty() ? "" : " ") + a_words[i];
			}
			return out;
		}

		std::string Where(std::uint64_t a_address)
		{
			const auto* p = reinterpret_cast<const void*>(a_address);
			return Addr().InImage(p) ? std::format("{:#x} (eldenring+{:#x})", a_address, Addr().Rva(p)) : std::format("{:#x}", a_address);
		}

		std::string Format(const TypeInfo& a_type, const std::uint8_t* a_b)
		{
			auto get = [a_b]<class T>(T) {
				T v;
				std::memcpy(&v, a_b, sizeof(T));
				return v;
			};
			const auto n = a_type.name;
			if (n == "u8") return std::format("{} ({:#04x})", get(std::uint8_t{}), get(std::uint8_t{}));
			if (n == "i8") return std::format("{}", get(std::int8_t{}));
			if (n == "u16") return std::format("{} ({:#06x})", get(std::uint16_t{}), get(std::uint16_t{}));
			if (n == "i16") return std::format("{}", get(std::int16_t{}));
			if (n == "u32") return std::format("{} ({:#010x})", get(std::uint32_t{}), get(std::uint32_t{}));
			if (n == "i32") return std::format("{}", get(std::int32_t{}));
			if (n == "u64") return std::format("{} ({:#x})", get(std::uint64_t{}), get(std::uint64_t{}));
			if (n == "i64") return std::format("{}", get(std::int64_t{}));
			if (n == "f32") return std::format("{:.6g}", get(float{}));
			if (n == "f64") return std::format("{:.9g}", get(double{}));
			if (n == "vec4") {
				float v[4];
				std::memcpy(v, a_b, 16);
				return std::format("{:.4f} {:.4f} {:.4f} {:.4f}", v[0], v[1], v[2], v[3]);
			}
			if (n == "ptr") {
				const auto  p = get(std::uint64_t{});
				std::string rtti = p ? game::RttiName(reinterpret_cast<const void*>(p)) : std::string();
				return Where(p) + (rtti.empty() ? "" : " -> " + rtti);
			}
			if (n == "str") {
				const auto* s = reinterpret_cast<const char*>(a_b);
				return std::format("\"{}\"", std::string(s, strnlen(s, a_type.size)));
			}
			if (n == "wstr") {
				const auto*  w = reinterpret_cast<const wchar_t*>(a_b);
				std::wstring ws;
				for (std::size_t i = 0; i < a_type.size / 2 && w[i]; ++i) {
					ws += w[i];
				}
				return std::format("L\"{}\"", Narrow(ws));
			}
			return "?";
		}

		struct Ctx
		{
			const Frame& f;
			bool&        error;
			std::string  Fail(std::string a_msg)
			{
				error = true;
				return a_msg;
			}
		};

		// "<expr...> [type] [count]": the trailing words that parse as a type / count are options.
		std::string Peek(Ctx& c, const std::vector<std::string>& w)
		{
			if (w.size() < 2) {
				return c.Fail("usage: peek <expr> [type] [count]");
			}
			std::size_t end = w.size();
			std::size_t count = 1;
			const TypeInfo* type = nullptr;
			if (end >= 3 && FindType(w[end - 2]) && ParseInteger(w[end - 1])) {
				count = std::clamp<std::size_t>(*ParseInteger(w[end - 1]), 1, 64);
				type = FindType(w[end - 2]);
				end -= 2;
			} else if (FindType(w[end - 1])) {
				type = FindType(w[end - 1]);
				end -= 1;
			}
			const auto text = Join(w, 1, end);
			if (!type) {
				const auto* sym = FindSymbol(text);
				type = FindType(sym ? sym->type : "u64");
			}
			const auto address = EvalExpr(text, Env(c.f));
			if (!address) {
				return c.Fail(address.error());
			}
			std::string out;
			for (std::size_t i = 0; i < count; ++i) {
				std::uint8_t buffer[128]{};
				const auto   at = *address + i * type->size;
				const bool   ok = SafeReadBytes(reinterpret_cast<const void*>(at), buffer, type->size) ||
				                (type->name.ends_with("str") && SafeReadBytes(reinterpret_cast<const void*>(at), buffer, 16));
				if (!ok) {
					return out + c.Fail(std::format("can't read {} bytes at {:#x}", type->size, at));
				}
				out += std::format("{} {}: {}\n", Where(at), type->name, Format(*type, buffer));
			}
			return out;
		}

		std::string Poke(Ctx& c, const std::vector<std::string>& w)
		{
			std::size_t typeAt = 0;
			for (std::size_t i = w.size(); i-- > 2;) {
				if (FindType(w[i])) {
					typeAt = i;
					break;
				}
			}
			if (!typeAt || typeAt + 1 >= w.size()) {
				return c.Fail("usage: poke <expr> <type> <value>   (vec4: x y z w)");
			}
			const auto* type = FindType(w[typeAt]);
			const auto  address = EvalExpr(Join(w, 1, typeAt), Env(c.f));
			if (!address) {
				return c.Fail(address.error());
			}
			std::vector<double> values;
			for (auto i = typeAt + 1; i < w.size(); ++i) {
				std::string item = w[i];
				std::ranges::replace(item, ',', ' ');
				for (const auto& part : Split(item)) {
					const auto v = ParseNumber(part);
					if (!v) {
						return c.Fail(std::format("bad value '{}'", part));
					}
					values.push_back(*v);
				}
			}
			std::uint8_t bytes[16]{};
			auto         put = [&bytes]<class T>(T v) { std::memcpy(bytes, &v, sizeof(T)); };
			const auto   n = type->name;
			const auto   integer = ParseInteger(w[typeAt + 1]);
			const auto   asInt = integer ? *integer : static_cast<std::uint64_t>(static_cast<std::int64_t>(values[0]));
			if (n == "f32") put(static_cast<float>(values[0]));
			else if (n == "f64") put(values[0]);
			else if (n == "vec4") {
				float v[4]{};
				for (std::size_t i = 0; i < 4 && i < values.size(); ++i) {
					v[i] = static_cast<float>(values[i]);
				}
				std::memcpy(bytes, v, 16);
			} else if (n == "str" || n == "wstr") {
				return c.Fail("poke: strings not supported");
			} else {
				put(asInt);  // little-endian: the low bytes are what a smaller type writes
			}
			if (!SafeWriteBytes(reinterpret_cast<void*>(*address), bytes, type->size)) {
				return c.Fail(std::format("can't write {} bytes at {:#x}", type->size, *address));
			}
			Log::Info("console: poke {} {} {}", Where(*address), n, Join(w, typeAt + 1, w.size()));
			return std::format("wrote {} at {}", Format(*type, bytes), Where(*address));
		}

		std::string Vt(Ctx& c, const std::vector<std::string>& w)
		{
			const auto address = EvalExpr(Join(w, 1, w.size()), Env(c.f));
			if (!address) {
				return c.Fail(address.error());
			}
			const auto vptr = SafeRead<std::uint64_t>(reinterpret_cast<const void*>(*address));
			if (!vptr) {
				return c.Fail(std::format("can't read {:#x}", *address));
			}
			const auto name = game::RttiNameOfVtable(reinterpret_cast<const void*>(*vptr));
			return std::format("object {}: vtable {} {}", Where(*address), Where(*vptr), name.empty() ? "(no RTTI)" : name);
		}

		std::string Chr(Ctx& c, const std::vector<std::string>& w)
		{
			const std::size_t max = w.size() > 1 ? std::clamp<std::size_t>(ParseInteger(w[1]).value_or(16), 1, 64) : 16;
			std::string       out;
			for (const auto& ch : game::Characters(max)) {
				out += std::format("{} {:016x} {} team {:3} npc {:9} type {:2} hp {:5}/{:5}{} d {:6.1f} pos ({:.2f}, {:.2f}, {:.2f})\n", static_cast<void*>(ch.chr),
					ch.handle, ch.isPlayer ? "P" : (ch.isEnemy ? "E" : "C"), ch.team, ch.npcParam, ch.chrType, ch.hp, ch.maxHp, ch.dead ? " DEAD" : "",
					ch.distance, ch.pos.x, ch.pos.y, ch.pos.z);
			}
			return out.empty() ? c.Fail("no characters (WorldChrMan unavailable?)") : out;
		}

		std::string Call(Ctx& c, const std::vector<std::string>& w)
		{
			auto nums = [&w](std::size_t a_from, std::size_t a_n) -> std::optional<std::vector<float>> {
				std::vector<float> v;
				for (std::size_t i = a_from; i < a_from + a_n; ++i) {
					const auto x = i < w.size() ? ParseNumber(w[i]) : std::nullopt;
					if (!x) {
						return std::nullopt;
					}
					v.push_back(static_cast<float>(*x));
				}
				return v;
			};
			const auto& p = c.f.player;
			const std::string what = w.size() > 1 ? w[1] : "";
			if (what == "setpos") {
				const auto v = nums(2, 3);
				if (!v || !p) {
					return c.Fail("usage: call setpos x y z   (needs a player)");
				}
				game::SetPosition(p, { (*v)[0], (*v)[1], (*v)[2] });
				Log::Info("console: SetPosition({}, {}, {})", (*v)[0], (*v)[1], (*v)[2]);
				return "ChrIns::SetPosition called";
			}
			if (what == "setyaw") {
				const auto v = nums(2, 1);
				if (!v || !p) {
					return c.Fail("usage: call setyaw radians   (needs a player)");
				}
				game::SetYaw(p, (*v)[0]);
				Log::Info("console: SetYaw({})", (*v)[0]);
				return "ChrIns::SetYaw called";
			}
			if (what == "ray") {
				const auto v = nums(2, 6);
				if (!v) {
					return c.Fail("usage: call ray x y z dx dy dz [filter]   (filter: ray_filter, or e.g. 0x2000016 = [Puppet] ground_filter)");
				}
				// An optional 8th word: the cast_ray filter to try (docs/HOST.md "Collision filters"), else [Puppet] ray_filter.
				std::uint32_t filter = Cfg().rayFilter;
				if (w.size() > 8) {
					const auto f = ParseNumber(w[8]);
					if (!f || *f < 0 || *f > 4294967295.0) {
						return c.Fail("call ray: the filter is a number (e.g. 0x2000016)");
					}
					filter = static_cast<std::uint32_t>(*f);
				}
				Vec3       hit;
				const bool ok = game::CastRay({ (*v)[0], (*v)[1], (*v)[2] }, { (*v)[3], (*v)[4], (*v)[5] }, hit, p.chr, filter);
				return ok ? std::format("hit ({:.3f}, {:.3f}, {:.3f}) [filter {:#x}]", hit.x, hit.y, hit.z, filter)
				          : std::format("no hit [filter {:#x}]", filter);
			}
			if (what == "rayall" || what == "raykeep") {
				// rayall: every hit along the ray with its material (what the game's collector drops: class 2 and 5); raykeep: the
				// closest hit with material classes kept (a 9th word: the class bits, default [Puppet] ground_keep).
				const auto v = nums(2, 6);
				if (!v) {
					return c.Fail(std::format("usage: call {} x y z dx dy dz [filter]{} [layers]", what, what == "raykeep" ? " [keep bits]" : ""));
				}
				std::uint32_t filter = Cfg().rayFilter;
				if (w.size() > 8) {
					const auto f = ParseNumber(w[8]);
					if (!f || *f < 0 || *f > 4294967295.0) {
						return c.Fail("the filter is a number (e.g. 0x2000016)");
					}
					filter = static_cast<std::uint32_t>(*f);
				}
				const Vec3 from{ (*v)[0], (*v)[1], (*v)[2] }, delta{ (*v)[3], (*v)[4], (*v)[5] };
				// The broad-phase layers to look in (cast_ray: 0xFB): raykeep's 10th word, rayall's 9th.
				const std::size_t layersAt = what == "raykeep" ? 10 : 9;
				std::uint8_t      layers = what == "raykeep" ? static_cast<std::uint8_t>(Cfg().groundLayers) : game::kCastRayLayers;
				if (w.size() > layersAt) {
					const auto l = ParseNumber(w[layersAt]);
					if (!l || *l < 0 || *l > 255) {
						return c.Fail("the broad-phase layers are a byte (cast_ray: 0xFB; all: 0xFF)");
					}
					layers = static_cast<std::uint8_t>(*l);
				}
				if (what == "raykeep") {
					std::uint32_t keep = Cfg().groundKeep;
					if (w.size() > 9) {
						const auto k = ParseNumber(w[9]);
						if (!k || *k < 0 || *k > 4294967295.0) {
							return c.Fail("the keep bits are a number (0x24 = classes 2 and 5)");
						}
						keep = static_cast<std::uint32_t>(*k);
					}
					if (!game::CanCastRayKeep()) {
						return c.Fail("Snake's own ray query is off (an address failed: see the log)");
					}
					Vec3       hit;
					const bool ok = game::CastRayKeep(from, delta, hit, p.chr, filter, keep, layers);
					return ok ? std::format("hit ({:.3f}, {:.3f}, {:.3f}) [filter {:#x}, keep {:#x}, layers {:#04x}]", hit.x, hit.y, hit.z, filter, keep, layers)
					          : std::format("no hit [filter {:#x}, keep {:#x}, layers {:#04x}]", filter, keep, layers);
				}
				game::RayHit hits[24];
				const int    n = game::CastRayAll(from, delta, p.chr, filter, hits, static_cast<int>(std::size(hits)), layers);
				if (n < 0) {
					return c.Fail("Snake's own ray query is off (an address failed: see the log)");
				}
				std::string out = std::format("{} hit(s) [filter {:#x}, layers {:#04x}; broad-phase layer masks {}]", n, filter, layers, game::BroadPhaseLayers());
				for (int i = 0; i < std::min<int>(n, static_cast<int>(std::size(hits))); ++i) {
					const auto& h = hits[i];
					out += std::format("\n  {:5.3f} ({:.3f}, {:.3f}, {:.3f}) n ({:.2f}, {:.2f}, {:.2f}) body {:#x} layer {:#x} (filter info {:#x}) "
									   "hkmat {} key {:#x} material {}{}",
						h.fraction, h.pos.x, h.pos.y, h.pos.z, h.normal.x, h.normal.y, h.normal.z, h.body, h.shapeFilter & 0x7F, h.shapeFilter,
						h.hkMaterial, h.shapeKey, h.material,
						h.material >= 0 && (h.material / 100 == 2 || h.material / 100 == 5) ? " (dropped by the game's collector)" : "");
				}
				return out;
			}
			if (what == "kill") {
				if (w.size() < 3 || w[2] != "yes" || !p) {
					return c.Fail("usage: call kill yes   (kills the ER player through ChrIns::Kill)");
				}
				game::Kill(p);
				Log::Info("console: ChrIns::Kill");
				return "ChrIns::Kill called";
			}
			return c.Fail("call: setpos x y z | setyaw rad | ray|rayall|raykeep x y z dx dy dz [filter] | kill yes");
		}

		struct FlagDef
		{
			std::string_view name;
			int              where;  // 0 = ChrIns+0x1C5, 1 = data+0x19B, 2 = ChrIns+0x538, 3 = ChrIns+0x1C4
			std::uint8_t     mask;
			std::string_view help;
		};
		constexpr FlagDef kFlags[] = {
			{ "render", 0, 0x08, "ChrIns+0x1C5 b3 enable_render (clear = hidden)" },
			{ "dead", 0, 0x80, "ChrIns+0x1C5 b7 death flag (read only advice)" },
			{ "nodead", 1, 0x01, "data+0x19B b0 NoDead" },
			{ "nodamage", 1, 0x02, "data+0x19B b1 NoDamage" },
			{ "nohit", 2, 0x08, "ChrIns+0x538 b3 no hit" },
			{ "noattack", 2, 0x10, "ChrIns+0x538 b4 no attack/actions" },
			{ "nomove", 2, 0x20, "ChrIns+0x538 b5 no move" },
			{ "nogravity", 3, 0x20, "ChrIns+0x1C4 b5 no gravity" },
		};

		std::string Flag(Ctx& c, const std::vector<std::string>& w)
		{
			const auto& p = c.f.player;
			if (!p) {
				return c.Fail("no player");
			}
			auto byteOf = [&p](int a_where) -> std::uint8_t* {
				switch (a_where) {
				case 0:
					return p.chr + game::kChrFlags1C5;
				case 1:
					return p.data + game::kDataDebug;
				case 2:
					return p.chr + game::kChrDebugFlags;
				default:
					return p.chr + game::kChrFlags1C4;
				}
			};
			if (w.size() < 2) {
				std::string out;
				for (const auto& f : kFlags) {
					out += std::format("{:10} {}  {}\n", f.name, game::GetBit(byteOf(f.where), f.mask) ? "on " : "off", f.help);
				}
				return out;
			}
			for (const auto& f : kFlags) {
				if (f.name == w[1]) {
					if (w.size() < 3) {
						return std::format("{} {}", f.name, game::GetBit(byteOf(f.where), f.mask) ? "on" : "off");
					}
					const bool on = w[2] == "on" || w[2] == "1";
					game::SetBit(byteOf(f.where), f.mask, on);
					Log::Info("console: flag {} {}", f.name, on ? "on" : "off");
					return std::format("{} -> {}", f.name, on ? "on" : "off");
				}
			}
			return c.Fail("unknown flag (flag with no arguments lists them)");
		}

		std::string Sym(Ctx& c)
		{
			std::string out;
			for (const auto& s : Symbols()) {
				auto* v = s.value(c.f);
				out += std::format("{:13} {:18} {}\n", s.name, v ? Where(reinterpret_cast<std::uint64_t>(v)) : std::string("(unavailable)"), s.help);
			}
			out += "s.<Name>      any FD4 singleton slot (see `single`)\n";
			return out;
		}

		std::string Single(const std::vector<std::string>& w)
		{
			std::string out;
			for (const auto& s : Singletons()) {
				if (w.size() > 1 && s.name.find(w[1]) == std::string::npos) {
					continue;
				}
				out += std::format("{:40} slot {} -> {}\n", s.name, Where(reinterpret_cast<std::uint64_t>(s.slot)),
					Where(reinterpret_cast<std::uint64_t>(SafePtr(s.slot))));
			}
			return out.empty() ? std::string("no singletons (finder not run or no match)") : out;
		}

		std::string Cam(const Frame& a_f)
		{
			auto* cam = game::PersCam1();
			if (!cam) {
				return "pers_cam_1 unavailable";
			}
			const auto* m = reinterpret_cast<const float*>(cam + game::kCamMatrix);
			return std::format("pers_cam_1 {} ({})\n right {:.3f} {:.3f} {:.3f}\n up    {:.3f} {:.3f} {:.3f}\n fwd   {:.3f} {:.3f} {:.3f}\n pos   {:.3f} {:.3f} {:.3f}\n"
							   " fov {:.4f} aspect {:.4f} near {:.4f} far {:.1f}\n driving {} maxDist {:.2f}\n guest cam ({:.2f}, {:.2f}, {:.2f}) q ({:.3f} {:.3f} {:.3f} {:.3f}) fov {:.3f}",
				static_cast<void*>(cam), game::RttiName(cam), m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14],
				*reinterpret_cast<const float*>(cam + game::kCamFov), *reinterpret_cast<const float*>(cam + game::kCamAspect),
				*reinterpret_cast<const float*>(cam + game::kCamNear), *reinterpret_cast<const float*>(cam + game::kCamFar), CameraDriver::Driving(),
				CameraDriver::MaxDistance(), a_f.guest.camPos[0], a_f.guest.camPos[1], a_f.guest.camPos[2], a_f.guest.camQuat[0], a_f.guest.camQuat[1],
				a_f.guest.camQuat[2], a_f.guest.camQuat[3], a_f.guest.camFovY);
		}

		// findq <expr> [max]: every 8-aligned qword equal to the value in committed, writable private memory
		// (instances of a class by its vtable, references to an object). Read through ReadProcessMemory in
		// 1 MB chunks so a page freed meanwhile is skipped, never faulted on. Stalls the frame (~1-3 s).
		std::string FindQword(Ctx& a_c, const std::vector<std::string>& a_w)
		{
			if (a_w.size() < 2) {
				return a_c.Fail("findq <expr> [max]");
			}
			const auto value = EvalExpr(Join(a_w, 1, 2), Env(a_c.f));
			if (!value) {
				return a_c.Fail("bad expression");
			}
			const std::size_t max = a_w.size() > 2 ? std::strtoul(a_w[2].c_str(), nullptr, 0) : 32;
			std::vector<std::uint64_t> buf((1u << 20) / 8);
			std::string                out;
			std::size_t                hits = 0;
			std::uint64_t              scanned = 0;
			MEMORY_BASIC_INFORMATION   mbi{};
			for (auto* p = static_cast<std::uint8_t*>(nullptr); ::VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi); p = static_cast<std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
				const bool rw = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mbi.Protect & PAGE_GUARD);
				if (!rw) {
					continue;
				}
				auto* base = static_cast<std::uint8_t*>(mbi.BaseAddress);
				for (std::size_t off = 0; off < mbi.RegionSize && hits < max; off += buf.size() * 8) {
					const std::size_t n = std::min<std::size_t>(buf.size() * 8, mbi.RegionSize - off);
					SIZE_T            got = 0;
					if (!::ReadProcessMemory(::GetCurrentProcess(), base + off, buf.data(), n, &got)) {
						continue;
					}
					scanned += got;
					for (std::size_t i = 0; i < got / 8 && hits < max; ++i) {
						if (buf[i] == *value) {
							++hits;
							out += std::format("  {}\n", Where(reinterpret_cast<std::uint64_t>(base + off + i * 8)));
						}
					}
				}
				if (hits >= max) {
					break;
				}
			}
			return std::format("{} hit(s) for {:#x} in {} MB{}\n{}", hits, *value, scanned >> 20, hits >= max ? " (stopped at max)" : "", out);
		}

		constexpr std::string_view kHelp =
			"peek <expr> [type] [count]   types u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 ptr vec4 str wstr\n"
			"poke <expr> <type> <value>   vec4 value: x y z w\n"
			"vt <expr>                    vtable + RTTI name of the object at expr\n"
			"sym | single [filter]        named roots | every FD4 singleton\n"
			"chr [n]                      characters by distance\n"
			"status | cam | calib | buddy | ui | ez\n"
			"combat [status | route auto|bullet|hp|ini | targets | hit n [dmg [poise]] | sleep n [power] | team n]\n"
			"combat shoot <rifle|handgun|sniper|buck|stun|tranq|rocket|grenade|flame> x y z [share] | strike x y z | aimat x y z | self <id> [off]\n"
			"boom [status | lethal|stun|sleep|smoke|flame|breaker|shrapnel [radius] | mode auto|bullet|sfx|off|ini | restore | pool]   ER explosion 5 m ahead\n"
			"boom look <kind> <bullet> [sfx] | set <key> <value> | raw <bullet> x y z   looks, breaker tuning, an unmodified ER bullet\n"
			"throw [status | arc on|off|auto|ini | meshes on|off]   Snake's thrown grenades we fly (er/Throw.h); arc on = draw it without MGSV aiming\n"
			"mines [detonate <id>]                   Snake's placed explosives (er/Mines.h)\n"
			"decoys [add [m] [plain|active|shock] | pop <id>]  Snake's decoys (er/Decoys.h)\n"
			"stealth [hide on|off | speffect <id>]   Snake in MGSV's cardboard box (er/Stealth.h)\n"
			"cqc [status | grab | throw | choke | interrogate | release | on | off | set <key> <v>]   CQC holds and throws on Elden Ring enemies (er/Cqc.h)\n"
			"fulton [status | nearest [fail] | 0x<handle> [fail] | forget]   MGSV's Fulton on an Elden Ring enemy, as kGEvFulton (er/Fulton.h)\n"
			"mark [status | binoculars on|off|auto | nearest | clear]   binocular marks over Elden Ring characters (er/Marking.h)\n"
			"timeskip [status | <minutes> | clear]   MGSV's Phantom Cigar on Elden Ring's clock, as kGEvTimeSkip (er/TimeSkip.h)\n"
			"geom [learn [use] | near|all [radius] | ladders [radius] | dump <ins>]   map assets around the player (er/GeomProbe.h)\n"
			"progression [off|level|weapon|both]   the Tarnished's level and right-hand armament, Snake's damage factor ([Progression]; switch live)\n"
			"call setpos x y z | setyaw rad | ray x y z dx dy dz [filter] | rayall|raykeep (every hit + material | kept classes) | kill yes\n"
			"flag [name [on|off]]         render nodead nodamage nohit noattack nomove nogravity\n"
			"puppet auto|off | motion kinematic|root_motion | hop on|off | set <key> <value>   menu auto|open|closed   debug on|off\n"
			"tarnished [on|off|toggle]    Tarnished mode (what View + Start held 1 s toggles): the Tarnished plays, Snake parks\n"
			"render [status | keys | set <key> <value> | probe]   phase 2 renderer\n"
			"hud [status | msg [info|good|warn|error] <text> | aim on|off|auto | set <key> <value>]   overlay (MGSV HUD layer, reticle, corner text)\n"
			"draw [skeleton|buddy|tracers on|off]   CSEzDraw developer views\n"
			"snap reset|take <label>|diff <a> <b> [max]|clear   bytes that follow a state you switch (live RE)\n"
			"pacing [set smooth|draws on|off | set delay auto|<steps> | set extrapolate <steps> | set motion <frames>]   frame pacing (MGSV's frames blended)\n"
			"pacing probe start [frames] | stop | dump [file]   per-frame pacing measurements -> CSV next to the DLL\n"
			"input key|keydown|keyup <vk> | mouse <dx> <dy> | wheel <n> | mbutton left|right|middle down|up   SendInput inside Elden Ring's Wine desktop; input kbm [on|off|sens <v>]: keyboard + mouse as Snake's pad\n"
			"findq <expr> [max]           qwords equal to expr in writable memory (e.g. instances by vtable: findq base+0x33BC768)\n"
			"expressions: + - * ( ) and [x] = read u64 at x; e.g. peek [[wcm]+0x1E508]+0x190 ptr\n";
	}

	std::string Execute(std::string_view a_line, const Frame& a_f, bool& a_error)
	{
		a_error = false;
		Ctx        c{ a_f, a_error };
		const auto w = Split(a_line);
		if (w.empty()) {
			return c.Fail("empty command (try help)");
		}
		const auto& cmd = w[0];
		if (cmd == "help") {
			return std::string(kHelp);
		}
		if (cmd == "peek") {
			return Peek(c, w);
		}
		if (cmd == "poke") {
			return Poke(c, w);
		}
		if (cmd == "vt") {
			return Vt(c, w);
		}
		if (cmd == "chr") {
			return Chr(c, w);
		}
		if (cmd == "call") {
			return Call(c, w);
		}
		if (cmd == "flag") {
			return Flag(c, w);
		}
		if (cmd == "sym") {
			return Sym(c);
		}
		if (cmd == "single") {
			return Single(w);
		}
		if (cmd == "status") {
			return Host::StatusLine();
		}
		if (cmd == "hithook") {
			if (w.size() > 2 && w[1] == "study") {
				HitHook::SetStudy(w[2] == "on");
				return std::format("hit study {}", w[2]);
			}
			return HitHook::DumpStudy();
		}
		if (cmd == "cam" && w.size() >= 4 && w[1] == "set") {
			// cam set <fov_scale|fov_offset|eye_height|collision> <value>: live camera tuning (not saved)
			auto&       c = Cfg();
			const float v = std::strtof(w[3].c_str(), nullptr);
			if (w[2] == "fov_scale") {
				c.fovScale = v;
			} else if (w[2] == "fov_offset") {
				c.fovOffset = v;
			} else if (w[2] == "eye_height") {
				c.eyeHeight = v;
			} else if (w[2] == "collision") {
				c.cameraCollision = v != 0.0f;
			} else if (w[2] == "collision_radius") {
				c.collisionRadius = v;
			} else if (w[2] == "collision_ease_out") {
				c.collisionEaseOut = v;
			} else if (w[2] == "collision_margin") {
				c.collisionMargin = v;
			} else if (w[2] == "vertical_smoothing") {
				c.verticalSmoothing = v;
			} else if (w[2] == "sync_compass") {
				c.syncCompassCam = v != 0.0f;
			} else {
				return "cam set <fov_scale|fov_offset|eye_height|collision|collision_radius|collision_ease_out|collision_margin|vertical_smoothing|sync_compass> <value>";
			}
			return std::format("cam: {} = {}", w[2], v);
		}
		if (cmd == "cam") {
			return Cam(a_f);
		}
		if (cmd == "calib") {
			return Calibration::Describe();
		}
		if (cmd == "buddy") {
			return Buddy::Describe() + "\n" + BuddyOrders::Describe();
		}
		if (cmd == "combat") {
			return Combat::Console(w, a_f);
		}
		if (cmd == "boom") {
			return Explosion::Console(w, a_f);
		}
		if (cmd == "throw") {
			return Throw::Console(w, a_f) + "\n" + WorldShift::Describe();
		}
		if (cmd == "mines") {
			return Mines::Console(w, a_f);
		}
		if (cmd == "decoys") {
			return Decoys::Console(w, a_f);
		}
		if (cmd == "stealth") {
			return Stealth::Console(w, a_f);
		}
		if (cmd == "cqc") {
			return Cqc::Console(w, a_f);
		}
		if (cmd == "fulton") {
			std::string rest;
			for (std::size_t i = 1; i < w.size(); ++i) {
				rest += (i > 1 ? " " : "") + std::string(w[i]);
			}
			return Fulton::Command(a_f, rest);
		}
		if (cmd == "mark") {
			std::string rest;
			for (std::size_t i = 1; i < w.size(); ++i) {
				rest += (i > 1 ? " " : "") + std::string(w[i]);
			}
			return Marking::Command(a_f, rest);
		}
		if (cmd == "timeskip") {
			std::string rest;
			for (std::size_t i = 1; i < w.size(); ++i) {
				rest += (i > 1 ? " " : "") + std::string(w[i]);
			}
			return TimeSkip::Command(rest);
		}
		if (cmd == "geom") {
			return GeomProbe::Command(w, a_f);
		}
		if (cmd == "progression") {
			if (w.size() > 1) {
				// progression off|level|weapon|both: [Progression] damage_scaling live (not saved)
				if (w[1] != "off" && w[1] != "level" && w[1] != "weapon" && w[1] != "both") {
					return "progression [off|level|weapon|both]";
				}
				Cfg().progression = w[1];
				Log::Info("console: [Progression] damage_scaling = {}", w[1]);
			}
			return Progression::Command(a_f);
		}
		if (cmd == "ui") {
			return MenuWatch::DumpUi() + "\n" + MenuWatch::Describe();
		}
		if (cmd == "press" && w.size() > 1) {
			// press <a+b+x+y+start+back+lb+rb+lt+rt+up+down+left+right+l3+r3|none> [ms] [lx ly [rx ry]]: Elden Ring's XInput user 0
			static constexpr std::pair<std::string_view, std::uint16_t> kNames[] = { { "a", 0x1000 }, { "b", 0x2000 }, { "x", 0x4000 },
				{ "y", 0x8000 }, { "start", 0x0010 }, { "back", 0x0020 }, { "lb", 0x0100 }, { "rb", 0x0200 }, { "up", 0x0001 },
				{ "down", 0x0002 }, { "left", 0x0004 }, { "right", 0x0008 }, { "l3", 0x0040 }, { "r3", 0x0080 }, { "none", 0 } };
			std::uint16_t buttons = 0;
			std::uint8_t  lt = 0, rt = 0;
			std::string_view names = w[1];
			while (!names.empty()) {
				const auto plus = names.find('+');
				const auto name = names.substr(0, plus);
				bool known = name == "lt" || name == "rt";
				lt = name == "lt" ? 255 : lt;
				rt = name == "rt" ? 255 : rt;
				for (const auto& [n, b] : kNames) {
					if (n == name) {
						buttons |= b;
						known = true;
					}
				}
				if (!known) {
					return std::format("unknown button '{}'", name);
				}
				names = plus == std::string_view::npos ? std::string_view() : names.substr(plus + 1);
			}
			const unsigned ms = w.size() > 2 ? static_cast<unsigned>(std::strtoul(w[2].c_str(), nullptr, 10)) : 150u;
			const auto     lx = w.size() > 3 ? static_cast<std::int16_t>(std::strtol(w[3].c_str(), nullptr, 10)) : std::int16_t(0);
			const auto     ly = w.size() > 4 ? static_cast<std::int16_t>(std::strtol(w[4].c_str(), nullptr, 10)) : std::int16_t(0);
			const auto rx = w.size() > 5 ? static_cast<std::int16_t>(std::strtol(w[5].c_str(), nullptr, 10)) : std::int16_t(0);
			const auto ry = w.size() > 6 ? static_cast<std::int16_t>(std::strtol(w[6].c_str(), nullptr, 10)) : std::int16_t(0);
			InputBridge::Inject(buttons, ms ? ms : 150, lx, ly, lt, rt, rx, ry);
			return std::format("injected {:#06x} lt {} rt {} for {} ms (left {}, {} right {}, {})", buttons, lt, rt, ms ? ms : 150, lx, ly, rx, ry);
		}
		if (cmd == "ez") {
			return DebugDraw::Available() ? std::format("CSEzDraw at {}", static_cast<void*>(game::EzDraw())) : std::string("no CSEzDraw");
		}
		if (cmd == "puppet" && w.size() > 2 && w[1] == "hide") {
			// Live test of the ways to hide the Tarnished ([Puppet] hide_mode): render_flag | fade | none.
			Cfg().hideMode = w[2];
			return std::format("hide_mode = {}", Cfg().hideMode);
		}
		if (cmd == "puppet" && w.size() > 2 && w[1] == "motion") {
			return Puppet::SetMotion(w[2], a_f);
		}
		if (cmd == "puppet" && w.size() > 1 && w[1] == "hop") {
			// Dive = hop (roadmap 2.9) switched live: puppet hop on|off ([Puppet] hop).
			if (w.size() > 2) {
				Cfg().hop = w[2] == "on" || w[2] == "1" || w[2] == "true";
			}
			return std::format("hop = {} (height {:.2f} m, length {:.2f} m, step {:.2f} m)", Cfg().hop ? "on" : "off", Cfg().hopHeight, Cfg().hopLength,
				Cfg().hopStep);
		}
		if (cmd == "puppet" && w.size() > 1 && w[1] == "set") {
			// Live tuning of Snake's body (the [Puppet] keys of the same names; not saved): puppet set <key> <value>.
			auto&      cfg = Cfg();
			const auto v = w.size() > 3 ? ParseNumber(w[3]) : std::nullopt;
			const std::string key = w.size() > 2 ? w[2] : "";
			const auto u32 = [&](std::uint32_t& a_field) { a_field = static_cast<std::uint32_t>(std::clamp(*v, 0.0, 4294967295.0)); };
			if (!v) {
				return c.Fail("usage: puppet set ground_keep|ground_layers|ground_filter|wall_filter|wall_filter_also|ground_probes|step_up|snap_down|radius|"
				              "jump_apex|jump_air_step|jump_air_control <value>");
			}
			if (key == "ground_keep") {
				u32(cfg.groundKeep);
			} else if (key == "ground_layers") {
				u32(cfg.groundLayers);
			} else if (key == "ground_filter") {
				u32(cfg.groundFilter);
			} else if (key == "wall_filter") {
				u32(cfg.wallFilter);
			} else if (key == "wall_filter_also") {
				u32(cfg.wallFilterAlso);
			} else if (key == "ground_probes") {
				cfg.groundProbes = *v != 0.0;
			} else if (key == "step_up") {
				cfg.stepUp = static_cast<float>(*v);
			} else if (key == "snap_down") {
				cfg.snapDown = static_cast<float>(*v);
			} else if (key == "radius") {
				cfg.radius = static_cast<float>(*v);
			} else if (key == "jump_apex") {
				cfg.jumpApex = static_cast<float>(*v);
			} else if (key == "jump_air_step") {
				cfg.jumpAirStep = static_cast<float>(*v);
			} else if (key == "jump_air_control") {
				cfg.jumpAirControl = static_cast<float>(*v);
			} else {
				return c.Fail(std::format("puppet set: unknown key '{}'", key));
			}
			Log::Info("console: [Puppet] {} = {}", key, w[3]);
			return std::format("[Puppet] {} = {} (ground_filter {:#x} keep {:#x} layers {:#x}, wall_filter {:#x} + {:#x}, probes {}, step_up {:.2f}, snap_down {:.2f}, "
			                   "radius {:.2f}, jump_apex {:.2f}, jump_air_step {:.2f})",
				key, w[3], cfg.groundFilter, cfg.groundKeep, cfg.groundLayers, cfg.wallFilter, cfg.wallFilterAlso, cfg.groundProbes, cfg.stepUp, cfg.snapDown, cfg.radius,
				cfg.jumpApex, cfg.jumpAirStep);
		}
		if (cmd == "puppet" && w.size() > 1) {
			Puppet::SetOverride(w[1] == "off" ? Puppet::Override::kOff : Puppet::Override::kAuto);
			return Puppet::Describe();
		}
		if (cmd == "tarnished") {
			return Host::TarnishedCommand(w.size() > 1 ? std::string_view(w[1]) : std::string_view());
		}
		if (cmd == "menu" && w.size() > 1) {
			MenuWatch::SetForce(w[1] == "open" ? MenuWatch::Force::kOpen : (w[1] == "closed" ? MenuWatch::Force::kClosed : MenuWatch::Force::kAuto));
			return MenuWatch::Describe();
		}
		if (cmd == "render") {
			return Renderer::Command(a_line.substr(std::min(a_line.size(), a_line.find("render") + 6)));
		}
		if (cmd == "findq") {
			return FindQword(c, w);
		}
		if (cmd == "hud") {
			return Hud::Command(a_line.substr(std::min(a_line.size(), a_line.find("hud") + 3)));
		}
		if (cmd == "draw") {
			// draw [skeleton|buddy|tracers on|off]: the CSEzDraw developer views (off for players by default)
			auto& cfg = Cfg();
			if (w.size() > 2) {
				const bool on = w[2] == "on" || w[2] == "1" || w[2] == "true";
				if (w[1] == "skeleton") cfg.drawSkeleton = on;
				else if (w[1] == "buddy") cfg.drawBuddy = on;
				else if (w[1] == "tracers") cfg.drawTracers = on;
				else return c.Fail("draw [skeleton|buddy|tracers on|off]");
			}
			return std::format("debug draw {} (CSEzDraw {}): skeleton {} buddy {} tracers {}", cfg.debugDraw ? "on" : "off ([Features] debug_draw)",
				DebugDraw::Available() ? "present" : "missing", cfg.drawSkeleton, cfg.drawBuddy, cfg.drawTracers);
		}
		if (cmd == "pacing") {
			return w.size() > 1 && w[1] == "probe" ? PacingProbe::Command(w) : Pacing::Command(w);
		}
		if (cmd == "snap") {
			return MemDiff::Command(w);
		}
		if (cmd == "input") {
			return MemDiff::Input(w);
		}
		if (cmd == "debug" && w.size() > 1) {
			Log::SetDebug(w[1] == "on");
			return std::format("debug log {}", Log::DebugEnabled() ? "on" : "off");
		}
		return c.Fail(std::format("unknown command '{}' (try help)", cmd));
	}

	void Poll(const Frame& a_f)
	{
		if (!Cfg().console || !Link::Get().Valid()) {
			return;
		}
		auto*      con = Link::Get().HostConsole();
		const auto req = Atomic(con->reqSeq).load(std::memory_order_acquire);
		if (req == Atomic(con->respSeq).load(std::memory_order_relaxed)) {
			return;
		}
		char line[proto::kConsoleTextBytes + 1]{};
		std::memcpy(line, con->request, proto::kConsoleTextBytes);
		bool       error = false;
		std::string out;
		try {
			out = Execute(std::string_view(line, strnlen(line, proto::kConsoleTextBytes)), a_f, error);
		} catch (const std::exception& e) {
			error = true;
			out = std::format("internal error: {}", e.what());
		}
		Log::Debug("console: {} -> {}{}", line, error ? "ERROR " : "", out.substr(0, 200));
		const auto n = std::min(out.size(), sizeof(con->response) - 1);
		std::memcpy(con->response, out.data(), n);
		con->response[n] = 0;
		con->status = error ? 1 : 0;
		Atomic(con->respSeq).store(req, std::memory_order_release);
	}
}
