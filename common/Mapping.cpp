#include "Mapping.h"

#include "Log.h"

#include <cstring>
#include <cwchar>

namespace phantom
{
	namespace
	{
		std::uint32_t QueryUnixPid(const wchar_t* a_envOverride)
		{
			wchar_t env[32];
			const auto n = a_envOverride ? ::GetEnvironmentVariableW(a_envOverride, env, 32) : 0;
			if (n > 0 && n < 32) {
				return static_cast<std::uint32_t>(std::wcstoul(env, nullptr, 10));
			}
			if (!RunningUnderWine()) {
				// Real Windows: one kernel for both games, so the Windows pid is unique and lets the host
				// tell a restarted guest from the old one (er/Link.cpp) and the launcher find it.
				return static_cast<std::uint32_t>(::GetCurrentProcessId());
			}
			static constexpr unsigned char kCode[] = { 0x0f, 0x05, 0xc3 };  // syscall; ret
			void* stub = nullptr;
			for (std::uint64_t a = 0x710000000000ull; a < 0x7f0000000000ull && !stub; a += 0x10000000000ull) {
				stub = ::VirtualAlloc(reinterpret_cast<void*>(a), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
			}
			if (!stub) {
				return 0;
			}
			std::memcpy(stub, kCode, sizeof(kCode));
			::FlushInstructionCache(::GetCurrentProcess(), stub, sizeof(kCode));
			std::int64_t ret;
			__asm__ volatile("call *%[stub]" : "=a"(ret) : "a"(39LL), [stub] "r"(stub) : "rcx", "r11", "rdx", "rsi", "rdi", "r8", "r9", "r10", "memory");
			::VirtualFree(stub, 0, MEM_RELEASE);
			return ret > 0 ? static_cast<std::uint32_t>(ret) : 0;
		}
	}

	bool RunningUnderWine()
	{
		const auto ntdll = ::GetModuleHandleW(L"ntdll.dll");
		return ntdll && ::GetProcAddress(ntdll, "wine_get_version");
	}

	std::uint32_t UnixPid(const wchar_t* a_envOverride)
	{
		static const std::uint32_t pid = QueryUnixPid(a_envOverride);
		return pid;
	}

	bool Mapping::Open(const std::wstring& a_where, std::uint64_t a_bytes, bool a_create)
	{
		Close();
		where_ = a_where;
		const DWORD sizeHigh = static_cast<DWORD>(a_bytes >> 32);
		const DWORD sizeLow = static_cast<DWORD>(a_bytes & 0xFFFFFFFF);

		if (a_where.starts_with(L"file:")) {
			const std::wstring path = a_where.substr(5);
			file_ = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				a_create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file_ == INVALID_HANDLE_VALUE) {
				const DWORD err = ::GetLastError();
				if (a_create || (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)) {
					Log::Error("shared memory: can't open {} ({})", Narrow(path), err);
				}
				return false;
			}
			LARGE_INTEGER current{};
			::GetFileSizeEx(file_, &current);
			if (static_cast<std::uint64_t>(current.QuadPart) < a_bytes) {
				if (!a_create) {
					// The creator hasn't sized it yet; try again later.
					Close();
					return false;
				}
				LARGE_INTEGER want{};
				want.QuadPart = static_cast<LONGLONG>(a_bytes);
				if (!::SetFilePointerEx(file_, want, nullptr, FILE_BEGIN) || !::SetEndOfFile(file_)) {
					Log::Error("shared memory: can't size {} to {} MB ({})", Narrow(path), a_bytes >> 20, ::GetLastError());
					Close();
					return false;
				}
			}
			mapping_ = ::CreateFileMappingW(file_, nullptr, PAGE_READWRITE, sizeHigh, sizeLow, nullptr);
		} else {
			const std::wstring name = a_where.starts_with(L"name:") ? a_where.substr(5) : a_where;
			if (a_create) {
				mapping_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, sizeHigh, sizeLow, name.c_str());
			} else {
				mapping_ = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
			}
		}
		if (!mapping_) {
			const DWORD err = ::GetLastError();
			if (a_create || err != ERROR_FILE_NOT_FOUND) {
				Log::Error("shared memory: CreateFileMapping {} failed ({})", Narrow(a_where), err);
			}
			Close();
			return false;
		}
		base_ = static_cast<std::uint8_t*>(::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(a_bytes)));
		if (!base_) {
			Log::Error("shared memory: MapViewOfFile {} failed ({})", Narrow(a_where), ::GetLastError());
			Close();
			return false;
		}
		size_ = a_bytes;
		return true;
	}

	void Mapping::Close()
	{
		if (base_) {
			::UnmapViewOfFile(base_);
			base_ = nullptr;
		}
		if (mapping_) {
			::CloseHandle(mapping_);
			mapping_ = nullptr;
		}
		if (file_ != INVALID_HANDLE_VALUE) {
			::CloseHandle(file_);
			file_ = INVALID_HANDLE_VALUE;
		}
		size_ = 0;
	}
}
