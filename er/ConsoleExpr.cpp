#include "ConsoleExpr.h"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <format>

namespace phantom::er
{
	namespace
	{
		class Parser
		{
		public:
			Parser(std::string_view a_text, const ExprEnv& a_env) : text_(a_text), env_(a_env) {}

			std::expected<std::uint64_t, std::string> Run()
			{
				auto value = Sum();
				if (!value) {
					return value;
				}
				Skip();
				if (pos_ != text_.size()) {
					return std::unexpected(std::format("unexpected '{}' at {}", text_[pos_], pos_));
				}
				return value;
			}

		private:
			using Result = std::expected<std::uint64_t, std::string>;

			void Skip()
			{
				while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
					++pos_;
				}
			}

			bool Eat(char a_c)
			{
				Skip();
				if (pos_ < text_.size() && text_[pos_] == a_c) {
					++pos_;
					return true;
				}
				return false;
			}

			Result Sum()
			{
				auto lhs = Prod();
				while (lhs) {
					if (Eat('+')) {
						auto rhs = Prod();
						if (!rhs) {
							return rhs;
						}
						lhs = *lhs + *rhs;
					} else if (Eat('-')) {
						auto rhs = Prod();
						if (!rhs) {
							return rhs;
						}
						lhs = *lhs - *rhs;
					} else {
						break;
					}
				}
				return lhs;
			}

			Result Prod()
			{
				auto lhs = Unary();
				while (lhs && Eat('*')) {
					auto rhs = Unary();
					if (!rhs) {
						return rhs;
					}
					lhs = *lhs * *rhs;
				}
				return lhs;
			}

			Result Unary()
			{
				if (Eat('-')) {
					auto v = Unary();
					if (v) {
						return std::uint64_t{ 0 } - *v;
					}
					return v;
				}
				return Atom();
			}

			Result Atom()
			{
				Skip();
				if (pos_ >= text_.size()) {
					return std::unexpected(std::string("expression ends early"));
				}
				const char c = text_[pos_];
				if (c == '[' || c == '(') {
					++pos_;
					auto inner = Sum();
					if (!inner) {
						return inner;
					}
					if (!Eat(c == '[' ? ']' : ')')) {
						return std::unexpected(std::format("missing '{}' at {}", c == '[' ? ']' : ')', pos_));
					}
					if (c == '(') {
						return inner;
					}
					if (!env_.read64) {
						return std::unexpected(std::string("no memory access"));
					}
					const auto value = env_.read64(*inner);
					if (!value) {
						return std::unexpected(std::format("can't read [{:#x}]", *inner));
					}
					return *value;
				}
				if (std::isdigit(static_cast<unsigned char>(c))) {
					const auto start = pos_;
					while (pos_ < text_.size() && std::isalnum(static_cast<unsigned char>(text_[pos_]))) {
						++pos_;
					}
					const auto number = ParseInteger(text_.substr(start, pos_ - start));
					if (!number) {
						return std::unexpected(std::format("bad number '{}'", text_.substr(start, pos_ - start)));
					}
					return *number;
				}
				if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
					const auto start = pos_;
					while (pos_ < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '_' || text_[pos_] == '.')) {
						++pos_;
					}
					const auto name = text_.substr(start, pos_ - start);
					const auto value = env_.symbol ? env_.symbol(name) : std::nullopt;
					if (!value) {
						return std::unexpected(std::format("unknown or unavailable symbol '{}'", name));
					}
					return *value;
				}
				return std::unexpected(std::format("unexpected '{}' at {}", c, pos_));
			}

			std::string_view text_;
			const ExprEnv&   env_;
			std::size_t      pos_{ 0 };
		};
	}

	std::expected<std::uint64_t, std::string> EvalExpr(std::string_view a_text, const ExprEnv& a_env)
	{
		return Parser(a_text, a_env).Run();
	}

	std::optional<std::uint64_t> ParseInteger(std::string_view a_text)
	{
		bool negative = false;
		if (!a_text.empty() && a_text.front() == '-') {
			negative = true;
			a_text.remove_prefix(1);
		}
		int base = 10;
		if (a_text.size() > 2 && a_text[0] == '0' && (a_text[1] == 'x' || a_text[1] == 'X')) {
			base = 16;
			a_text.remove_prefix(2);
		}
		if (a_text.empty()) {
			return std::nullopt;
		}
		std::uint64_t value = 0;
		const auto [end, ec] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), value, base);
		if (ec != std::errc() || end != a_text.data() + a_text.size()) {
			return std::nullopt;
		}
		return negative ? std::uint64_t{ 0 } - value : value;
	}

	std::optional<double> ParseNumber(std::string_view a_text)
	{
		if (const auto integer = ParseInteger(a_text)) {
			const bool negative = !a_text.empty() && a_text.front() == '-';
			return negative ? -static_cast<double>(std::uint64_t{ 0 } - *integer) : static_cast<double>(*integer);
		}
		const std::string text(a_text);
		char*             end = nullptr;
		const double      value = std::strtod(text.c_str(), &end);
		if (text.empty() || !end || *end != '\0') {
			return std::nullopt;
		}
		return value;
	}
}
