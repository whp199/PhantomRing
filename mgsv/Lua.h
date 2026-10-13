#pragma once
// Running our Lua inside MGSV: a lua_pcall hook that, whenever the engine's outermost pcall on a thread
// returns (depth 1: no engine Lua is mid-flight on this thread), calls our pump with that lua_State.
// Same technique as MGS5VR's native_actions.cpp, proven on this exact exe (research §3.4).
//
// Everything that could run a metamethod (global lookups, tostring of tables, our own functions) runs
// inside a protected call: an error outside one would reach lua_atpanic and end the game. Unprotected
// API use is limited to stack shuffling, raw registry access and reading numbers/strings/booleans.

#include <string>
#include <string_view>

namespace phantom::guest::lua
{
	// Verifies the Lua API bytes and creates (not enables) the pcall hook. False: no Lua features.
	bool Install();
	[[nodiscard]] bool Available();

	using PumpFn = void (*)(void* a_L);
	void SetPump(PumpFn a_pump);

#ifdef PHANTOM_GUEST_TESTING
	// tests/test_guest_pump.cpp runs the real pump against a stock Lua 5.1 instead of MGSV's.
	struct TestApi
	{
		void* pcall;
		void* loadBuffer;
		void* getTop;
		void* setTop;
		void* toLString;
		void* type;
		void* toNumber;
		void* toBoolean;
		void* pushValue;
		void* pushString;
		void* rawGetI;
		void* ref;
		void* unref;
	};
	void UseApiForTests(const TestApi& a_api);
#endif

	// Thin wrapper over the engine's own Lua API; valid only inside the pump (MGSV's Lua thread).
	// Never throws; every call that runs Lua code goes through the original (unhooked) lua_pcall.
	class Ctx
	{
	public:
		explicit Ctx(void* a_L) :
			L_(a_L) {}

		int    Top() const;
		void   SetTop(int a_top) const;
		int    Type(int a_index) const;
		double Number(int a_index, double a_default = 0.0) const;
		bool   Boolean(int a_index) const;
		// Strings and numbers as text, booleans as true/false, nil as "nil", anything else as "<type n>".
		std::string String(int a_index) const;
		void*       GlobalState() const;  // L->l_G: identifies the VM

		// Compiles and runs a chunk, leaving a_results values on the stack (unless it fails: then the
		// stack is restored and a_error holds the message). Returns the Lua status (0 = ok).
		int Run(std::string_view a_code, const char* a_chunkName, int a_results, std::string* a_error) const;
		// Compiles a chunk and leaves the function on the stack (status 0), or restores the stack.
		int Load(std::string_view a_code, const char* a_chunkName, std::string* a_error) const;
		// Pops the top value into the registry; returns its reference (negative: nothing stored).
		int  Ref() const;
		void Unref(int a_ref) const;
		// Calls registry[a_ref](a_arg). On success the a_results values are the top of the stack; the
		// caller restores it with SetTop. On failure the stack is restored and a_error is set.
		int CallRef(int a_ref, std::string_view a_arg, int a_results, std::string* a_error) const;
		// Calls registry[a_ref](copy of the value at a_valueIndex); same contract.
		int CallRefWithValue(int a_ref, int a_valueIndex, int a_results, std::string* a_error) const;

	private:
		int   Finish(int a_status, int a_top, std::string* a_error) const;
		void* L_;
	};
}
