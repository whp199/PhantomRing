#include "Lua.h"

#include "Addresses.h"
#include "Hook.h"
#include "Log.h"
#include "Verify.h"

#include <atomic>
#include <cstring>

namespace phantom::guest::lua
{
	namespace
	{
		using PcallFn = int (*)(void*, int, int, int);
		using LoadBufferFn = int (*)(void*, const char*, std::size_t, const char*);
		using GetTopFn = int (*)(void*);
		using SetTopFn = void (*)(void*, int);
		using ToLStringFn = const char* (*)(void*, int, std::size_t*);
		using TypeFn = int (*)(void*, int);
		using ToNumberFn = double (*)(void*, int);
		using ToBooleanFn = int (*)(void*, int);
		using PushValueFn = void (*)(void*, int);
		using PushStringFn = void (*)(void*, const char*);
		using RawGetIFn = void (*)(void*, int, int);
		using RefFn = int (*)(void*, int);
		using UnrefFn = void (*)(void*, int, int);

		struct Api
		{
			PcallFn      pcall{};  // the trampoline (original) once hooked
			LoadBufferFn loadBuffer{};
			GetTopFn     getTop{};
			SetTopFn     setTop{};
			ToLStringFn  toLString{};
			TypeFn       type{};
			ToNumberFn   toNumber{};
			ToBooleanFn  toBoolean{};
			PushValueFn  pushValue{};
			PushStringFn pushString{};
			RawGetIFn    rawGetI{};
			RefFn        ref{};
			UnrefFn      unref{};
		} g_api;

		std::atomic<PumpFn> g_pump{ nullptr };
		std::atomic<bool>   g_available{ false };
		thread_local int    t_depth = 0;

		int Detour(void* a_L, int a_args, int a_results, int a_handler)
		{
			++t_depth;
			const int status = g_api.pcall(a_L, a_args, a_results, a_handler);
			if (t_depth == 1) {
				if (const auto pump = g_pump.load(std::memory_order_acquire)) {
					try {
						pump(a_L);
					} catch (...) {
						Log::Error("lua pump threw; ignored");
					}
				}
			}
			--t_depth;
			return status;
		}

		template <class T>
		T Fn(std::uintptr_t a_va)
		{
			return reinterpret_cast<T>(a_va);
		}
	}

	bool Install()
	{
		using namespace addr;
		const bool ok = Matches(kLuaPcall, "lua_pcall") && Matches(kLuaLoadBuffer, "luaL_loadbuffer") && Matches(kLuaGetTop, "lua_gettop") &&
		                Matches(kLuaSetTop, "lua_settop") && Matches(kLuaToLString, "lua_tolstring") && Matches(kLuaType, "lua_type") &&
		                Matches(kLuaToNumber, "lua_tonumber") && Matches(kLuaToBoolean, "lua_toboolean") && Matches(kLuaPushValue, "lua_pushvalue") &&
		                Matches(kLuaPushString, "lua_pushstring") && Matches(kLuaRawGetI, "lua_rawgeti") && Matches(kLuaLRef, "luaL_ref") &&
		                Matches(kLuaLUnref, "luaL_unref");
		if (!ok) {
			Log::Error("lua: API bytes differ -- Lua pump, console, boot automation and warps disabled");
			return false;
		}
		g_api.loadBuffer = Fn<LoadBufferFn>(kLuaLoadBuffer.va);
		g_api.getTop = Fn<GetTopFn>(kLuaGetTop.va);
		g_api.setTop = Fn<SetTopFn>(kLuaSetTop.va);
		g_api.toLString = Fn<ToLStringFn>(kLuaToLString.va);
		g_api.type = Fn<TypeFn>(kLuaType.va);
		g_api.toNumber = Fn<ToNumberFn>(kLuaToNumber.va);
		g_api.toBoolean = Fn<ToBooleanFn>(kLuaToBoolean.va);
		g_api.pushValue = Fn<PushValueFn>(kLuaPushValue.va);
		g_api.pushString = Fn<PushStringFn>(kLuaPushString.va);
		g_api.rawGetI = Fn<RawGetIFn>(kLuaRawGetI.va);
		g_api.ref = Fn<RefFn>(kLuaLRef.va);
		g_api.unref = Fn<UnrefFn>(kLuaLUnref.va);
		if (!Hooks::Create("lua_pcall", reinterpret_cast<void*>(kLuaPcall.va), &Detour, &g_api.pcall)) {
			return false;
		}
		g_available.store(true);
		return true;
	}

	bool Available() { return g_available.load(); }

#ifdef PHANTOM_GUEST_TESTING
	void UseApiForTests(const TestApi& a_api)
	{
		g_api.pcall = reinterpret_cast<PcallFn>(a_api.pcall);
		g_api.loadBuffer = reinterpret_cast<LoadBufferFn>(a_api.loadBuffer);
		g_api.getTop = reinterpret_cast<GetTopFn>(a_api.getTop);
		g_api.setTop = reinterpret_cast<SetTopFn>(a_api.setTop);
		g_api.toLString = reinterpret_cast<ToLStringFn>(a_api.toLString);
		g_api.type = reinterpret_cast<TypeFn>(a_api.type);
		g_api.toNumber = reinterpret_cast<ToNumberFn>(a_api.toNumber);
		g_api.toBoolean = reinterpret_cast<ToBooleanFn>(a_api.toBoolean);
		g_api.pushValue = reinterpret_cast<PushValueFn>(a_api.pushValue);
		g_api.pushString = reinterpret_cast<PushStringFn>(a_api.pushString);
		g_api.rawGetI = reinterpret_cast<RawGetIFn>(a_api.rawGetI);
		g_api.ref = reinterpret_cast<RefFn>(a_api.ref);
		g_api.unref = reinterpret_cast<UnrefFn>(a_api.unref);
		g_available.store(true);
	}
#endif

	void SetPump(PumpFn a_pump) { g_pump.store(a_pump, std::memory_order_release); }

	int  Ctx::Top() const { return g_api.getTop(L_); }
	void Ctx::SetTop(int a_top) const { g_api.setTop(L_, a_top); }
	int  Ctx::Type(int a_index) const { return g_api.type(L_, a_index); }

	double Ctx::Number(int a_index, double a_default) const
	{
		return Type(a_index) == addr::kLuaTNumber ? g_api.toNumber(L_, a_index) : a_default;
	}

	bool Ctx::Boolean(int a_index) const { return g_api.toBoolean(L_, a_index) != 0; }

	std::string Ctx::String(int a_index) const
	{
		const int t = Type(a_index);
		if (t == addr::kLuaTString || t == addr::kLuaTNumber) {
			// lua_tolstring converts a number in place; work on a copy so the slot keeps its type.
			g_api.pushValue(L_, a_index);
			std::size_t n = 0;
			const char* s = g_api.toLString(L_, -1, &n);
			std::string out = s ? std::string(s, n) : std::string();
			g_api.setTop(L_, -2);
			return out;
		}
		if (t == addr::kLuaTBoolean) {
			return Boolean(a_index) ? "true" : "false";
		}
		if (t == addr::kLuaTNil || t < 0) {
			return "nil";
		}
		return "<type " + std::to_string(t) + ">";
	}

	void* Ctx::GlobalState() const
	{
		// lua_State: ... +0x20 global_State* l_G (Lua 5.1 x64; research §3.1). L comes straight from the
		// engine's own lua_pcall, so it is live; this runs on every outermost pcall, keep it a plain load.
		void* g = nullptr;
		std::memcpy(&g, static_cast<const std::uint8_t*>(L_) + 0x20, sizeof(g));
		return g;
	}

	int Ctx::Finish(int a_status, int a_top, std::string* a_error) const
	{
		if (a_status != 0) {
			if (a_error) {
				*a_error = Type(-1) == addr::kLuaTString ? String(-1) : std::string("(non-string error)");
			}
			SetTop(a_top);
		}
		return a_status;
	}

	int Ctx::Load(std::string_view a_code, const char* a_chunkName, std::string* a_error) const
	{
		const int top = Top();
		return Finish(g_api.loadBuffer(L_, a_code.data(), a_code.size(), a_chunkName), top, a_error);
	}

	int Ctx::Run(std::string_view a_code, const char* a_chunkName, int a_results, std::string* a_error) const
	{
		const int top = Top();
		int       status = g_api.loadBuffer(L_, a_code.data(), a_code.size(), a_chunkName);
		if (status == 0) {
			status = g_api.pcall(L_, 0, a_results, 0);
		}
		return Finish(status, top, a_error);
	}

	int Ctx::Ref() const { return g_api.ref(L_, addr::kLuaRegistryIndex); }

	void Ctx::Unref(int a_ref) const
	{
		if (a_ref >= 0) {
			g_api.unref(L_, addr::kLuaRegistryIndex, a_ref);
		}
	}

	int Ctx::CallRef(int a_ref, std::string_view a_arg, int a_results, std::string* a_error) const
	{
		const int top = Top();
		g_api.rawGetI(L_, addr::kLuaRegistryIndex, a_ref);
		if (Type(-1) != addr::kLuaTFunction) {
			SetTop(top);
			if (a_error) {
				*a_error = "reference is not a function";
			}
			return -1;
		}
		const std::string arg(a_arg);
		g_api.pushString(L_, arg.c_str());
		return Finish(g_api.pcall(L_, 1, a_results, 0), top, a_error);
	}

	int Ctx::CallRefWithValue(int a_ref, int a_valueIndex, int a_results, std::string* a_error) const
	{
		const int top = Top();
		const int abs = a_valueIndex < 0 ? top + a_valueIndex + 1 : a_valueIndex;
		g_api.rawGetI(L_, addr::kLuaRegistryIndex, a_ref);
		if (Type(-1) != addr::kLuaTFunction) {
			SetTop(top);
			if (a_error) {
				*a_error = "reference is not a function";
			}
			return -1;
		}
		g_api.pushValue(L_, abs);
		return Finish(g_api.pcall(L_, 1, a_results, 0), top, a_error);
	}
}
