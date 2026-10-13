#include "Version.h"

#include "phantomring_protocol.h"
#include "phantomring_version.h"  // generated: build/generated/, cmake/version.cmake

#include <algorithm>
#include <cstring>
#include <format>

namespace phantom::version
{
	namespace
	{
		struct Tag
		{
			char text[160]{};
		};

		// Built at compile time so the whole tag is one contiguous string in the DLL's .rdata (pr --version finds it).
		constexpr Tag MakeTag()
		{
			Tag         t{};
			std::size_t n = 0;
			auto        put = [&](const char* a_s) {
				while (*a_s && n + 1 < sizeof(t.text)) {
					t.text[n++] = *a_s++;
				}
			};
			put("PHANTOMRING_BUILD release=");
			put(PHANTOMRING_RELEASE);
			put(" commit=");
			put(PHANTOMRING_COMMIT);
			put(" protocol=");
			char        digits[12]{};
			int         k = 0;
			std::uint32_t v = proto::kVersion;
			do {
				digits[k++] = static_cast<char>('0' + v % 10);
				v /= 10;
			} while (v && k < 11);
			while (k && n + 1 < sizeof(t.text)) {
				t.text[n++] = digits[--k];
			}
			return t;
		}

		[[gnu::used]] constexpr Tag kTag = MakeTag();

		std::string Side(std::string_view a_release, std::uint32_t a_protocol, bool a_sameRelease)
		{
			if (a_release.empty()) {
				return a_protocol ? std::format("from before v{} (protocol {})", kFirstNamedRelease, a_protocol)
				                  : std::format("from before v{}", kFirstNamedRelease);
			}
			return a_sameRelease && a_protocol ? std::format("v{} (protocol {})", a_release, a_protocol) : std::format("v{}", a_release);
		}
	}

	std::string_view Release() { return PHANTOMRING_RELEASE; }

	std::string_view Commit() { return PHANTOMRING_COMMIT; }

	std::string Describe() { return std::format("v{} (commit {}, protocol {})", Release(), Commit(), proto::kVersion); }

	std::string_view BuildTag() { return kTag.text; }

	std::string MismatchText(std::string_view a_hostRelease, std::uint32_t a_hostProtocol, std::string_view a_guestRelease,
		std::uint32_t a_guestProtocol)
	{
		const bool same = a_hostRelease == a_guestRelease;
		return std::format("PhantomRing versions differ: Elden Ring plugin {}, MGSV plugin {} -- reinstall both", Side(a_hostRelease, a_hostProtocol, same),
			Side(a_guestRelease, a_guestProtocol, same));
	}

	void WriteField(char* a_dst, std::size_t a_size, std::string_view a_text)
	{
		if (!a_size) {
			return;
		}
		const auto n = std::min(a_text.size(), a_size - 1);
		std::memcpy(a_dst, a_text.data(), n);
		std::memset(a_dst + n, 0, a_size - n);
	}

	std::string ReadField(const char* a_src, std::size_t a_size)
	{
		std::size_t n = 0;
		while (n < a_size && a_src[n]) {
			++n;
		}
		std::string out(a_src, n);
		// Only printable ASCII reaches a log or the corner text (the field comes from the other process).
		std::replace_if(out.begin(), out.end(), [](char c) { return c < 0x20 || c > 0x7e; }, '?');
		return out;
	}
}
