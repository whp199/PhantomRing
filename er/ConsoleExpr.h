#pragma once

// Address expressions for the dev console (er/Console.cpp):
//
//   expr   := sum
//   sum    := prod (('+' | '-') prod)*
//   prod   := unary ('*' unary)*
//   unary  := '-' unary | atom
//   atom   := number | symbol | '[' expr ']' | '(' expr ')'
//
// numbers: 0x1E508, 123; symbols: names the host resolves (wcm, player, player.pos, ...), may contain
// '.' and '_'; [x] reads the u64 at x (through the caller's safe reader, so a bad pointer is an
// error, never a crash). Example: [[wcm]+0x1E508]+0x190. Pure: the game is reached only through
// the two callbacks.

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace phantom::er
{
	struct ExprEnv
	{
		std::function<std::optional<std::uint64_t>(std::string_view)> symbol;  // nullopt: unknown/unavailable
		std::function<std::optional<std::uint64_t>(std::uint64_t)>    read64;  // nullopt: unreadable
	};

	std::expected<std::uint64_t, std::string> EvalExpr(std::string_view a_text, const ExprEnv& a_env);

	// Parses "12", "0x1f", "-3", "1.5e3" (as double), used for poke values and call arguments.
	std::optional<double>        ParseNumber(std::string_view a_text);
	std::optional<std::uint64_t> ParseInteger(std::string_view a_text);
}
