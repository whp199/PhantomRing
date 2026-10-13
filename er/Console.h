#pragma once

// Dev console, host side (proto Console @kOffHostConsole): a tiny command language for live RE,
// executed on the FrameBegin task (game thread). `help` lists the commands; every memory access is
// guarded (a bad address is an error message, never a crash).

#include <string>
#include <string_view>

namespace phantom::er
{
	struct Frame;

	namespace Console
	{
		void        Poll(const Frame& a_frame);
		std::string Execute(std::string_view a_line, const Frame& a_frame, bool& a_error);  // also used by tests/logs
	}
}
