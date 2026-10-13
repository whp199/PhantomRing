#pragma once

// Which PhantomRing this is: the VERSION file (the one version source, e.g. "0.2.0-alpha") and the commit it was
// built from, both baked in at build time by cmake/version.cmake. Both plugins print Describe() in their log's first
// line; BuildTag() is also a plain string in the DLL that `pr --version` / `pr doctor` / the packagers read
// (tools/phantomring/version.py).
//
// The two plugins must speak the same protocol (protocol/phantomring_protocol.h kVersion). Each writes its release
// into the shared header (Header.hostRelease / guestRelease, guestProtocol) so a mismatch can be told to the player
// in words: MismatchText().

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace phantom::version
{
	// The release that first wrote Header.hostRelease/guestRelease/guestProtocol: an empty field is older than it.
	inline constexpr std::string_view kFirstNamedRelease = "0.2.0-alpha";

	std::string_view Release();   // "0.2.0-alpha"
	std::string_view Commit();    // "0123456789ab", "0123456789ab-dirty" (uncommitted changes), "unknown" (no git)
	std::string      Describe();  // "v0.2.0-alpha (commit 0123456789ab, protocol 11)"
	std::string_view BuildTag();  // "PHANTOMRING_BUILD release=0.2.0-alpha commit=0123456789ab protocol=11"

	// "PhantomRing versions differ: Elden Ring plugin v0.2.0-alpha, MGSV plugin v0.3.0 -- reinstall both". A release
	// that is the same on both sides gets its protocol number (development builds); an empty release reads as "from
	// before v0.2.0-alpha"; a protocol of 0 is unknown.
	std::string MismatchText(std::string_view a_hostRelease, std::uint32_t a_hostProtocol, std::string_view a_guestRelease,
		std::uint32_t a_guestProtocol);

	// Fixed-size header text fields: NUL-padded (truncated to a_size - 1), and read back up to the first NUL.
	void        WriteField(char* a_dst, std::size_t a_size, std::string_view a_text);
	std::string ReadField(const char* a_src, std::size_t a_size);
}
