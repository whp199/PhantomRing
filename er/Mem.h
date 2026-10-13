#pragma once

// Guarded access to game memory. MinGW has no __try/__except, so reads and writes of anything that
// may be stale go through Read/WriteProcessMemory on our own process: a bad address is an error
// return, never an access violation. Hot paths validate an object once (vtable check) and then
// touch its fields directly.

#include "Hook.h"

#include <cstdint>
#include <optional>

#include <windows.h>

namespace phantom::er
{
	using phantom::SafeRead;

	template <class T>
	bool SafeWrite(void* a_address, const T& a_value)
	{
		SIZE_T written = 0;
		return a_address && ::WriteProcessMemory(::GetCurrentProcess(), a_address, &a_value, sizeof(T), &written) && written == sizeof(T);
	}

	inline bool SafeReadBytes(const void* a_address, void* a_out, std::size_t a_bytes)
	{
		SIZE_T read = 0;
		return a_address && ::ReadProcessMemory(::GetCurrentProcess(), a_address, a_out, a_bytes, &read) && read == a_bytes;
	}

	inline bool SafeWriteBytes(void* a_address, const void* a_in, std::size_t a_bytes)
	{
		SIZE_T written = 0;
		return a_address && ::WriteProcessMemory(::GetCurrentProcess(), a_address, a_in, a_bytes, &written) && written == a_bytes;
	}

	inline std::uint8_t* SafePtr(const void* a_address)
	{
		const auto value = SafeRead<std::uint8_t*>(a_address);
		return value ? *value : nullptr;
	}

	// True if the object at a_object has exactly the vtable a_vtable.
	inline bool HasVtable(const void* a_object, const void* a_vtable)
	{
		if (!a_object || !a_vtable) {
			return false;
		}
		const auto vptr = SafeRead<const void*>(a_object);
		return vptr && *vptr == a_vtable;
	}
}
