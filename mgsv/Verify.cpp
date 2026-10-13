#include "Verify.h"

#include "Log.h"

#include <windows.h>

#include <bcrypt.h>

#include <array>
#include <vector>

namespace phantom::guest
{
	ExeIdentity CheckExe()
	{
		ExeIdentity id;
		auto* base = reinterpret_cast<const std::uint8_t*>(::GetModuleHandleW(nullptr));
		if (!base) {
			return id;
		}
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
			return id;
		}
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
			return id;
		}
		id.base = reinterpret_cast<std::uintptr_t>(base);
		id.timeDateStamp = nt->FileHeader.TimeDateStamp;
		id.sizeOfImage = nt->OptionalHeader.SizeOfImage;
		id.matches = id.timeDateStamp == addr::kTimeDateStamp && id.sizeOfImage == addr::kSizeOfImage && id.base == addr::kImageBase;
		wchar_t path[MAX_PATH * 4];
		const auto n = ::GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
		id.path.assign(path, n);
		return id;
	}

	std::string Sha256File(const std::wstring& a_path)
	{
		BCRYPT_ALG_HANDLE  alg = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		std::string        out;
		HANDLE             file = ::CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
					FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			return out;
		}
		if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 && ::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
			std::vector<std::uint8_t> buffer(1 << 20);
			DWORD                     got = 0;
			bool                      ok = true;
			while (::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) && got) {
				if (::BCryptHashData(hash, buffer.data(), got, 0) < 0) {
					ok = false;
					break;
				}
			}
			std::array<std::uint8_t, 32> digest{};
			if (ok && ::BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0) {
				static constexpr char kHex[] = "0123456789abcdef";
				for (auto b : digest) {
					out.push_back(kHex[b >> 4]);
					out.push_back(kHex[b & 15]);
				}
			}
		}
		if (hash) {
			::BCryptDestroyHash(hash);
		}
		if (alg) {
			::BCryptCloseAlgorithmProvider(alg, 0);
		}
		::CloseHandle(file);
		return out;
	}

	namespace
	{
		bool Compare(std::uintptr_t a_va, std::initializer_list<std::uint8_t> a_bytes, const char* a_what)
		{
			std::array<std::uint8_t, 64> found{};
			SIZE_T                       read = 0;
			const auto                   n = a_bytes.size() < found.size() ? a_bytes.size() : found.size();
			if (!::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<const void*>(a_va), found.data(), n, &read) || read != n) {
				Log::Error("verify {}: {:#x} is not readable", a_what, a_va);
				return false;
			}
			std::size_t i = 0;
			for (auto b : a_bytes) {
				if (i >= n || found[i] != b) {
					std::string hex;
					for (std::size_t k = 0; k < n; ++k) {
						hex += std::format("{:02x} ", found[k]);
					}
					Log::Error("verify {}: bytes at {:#x} differ (found {}) -- feature disabled", a_what, a_va, hex);
					return false;
				}
				++i;
			}
			return true;
		}
	}

	bool Matches(const addr::Site& a_site, const char* a_what) { return Compare(a_site.va, a_site.bytes, a_what); }

	bool Matches(const addr::VarSite& a_site) { return Compare(a_site.site, a_site.bytes, a_site.name); }
}
