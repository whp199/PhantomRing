// A small JSON reader for the recorded fixtures (tests/fixtures/quiet/): numbers, strings, booleans, null, arrays, objects. Test-only.
// Included by tests/test_host_combat.cpp before the grace / Quiet tests.
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace jsontest
{
	struct Value
	{
		enum class Type
		{
			kNull,
			kBool,
			kNum,
			kStr,
			kArr,
			kObj,
		};
		Type                                       type{ Type::kNull };
		bool                                       b{ false };
		double                                     num{ 0 };
		std::string                                str;
		std::vector<Value>                         arr;
		std::vector<std::pair<std::string, Value>> obj;

		const Value* Get(std::string_view a_key) const
		{
			for (const auto& [k, v] : obj) {
				if (k == a_key) {
					return &v;
				}
			}
			return nullptr;
		}
		bool   Has(std::string_view a_key) const { return Get(a_key) != nullptr; }
		double Num(std::string_view a_key, double a_default = 0) const
		{
			const auto* v = Get(a_key);
			return v && v->type == Type::kNum ? v->num : a_default;
		}
		bool Bool(std::string_view a_key, bool a_default = false) const
		{
			const auto* v = Get(a_key);
			return v && v->type == Type::kBool ? v->b : a_default;
		}
		bool IsNull() const { return type == Type::kNull; }
	};

	class Parser
	{
	public:
		explicit Parser(std::string a_text) : s_(std::move(a_text)) {}
		bool Parse(Value& a_out)
		{
			Ws();
			return One(a_out);
		}

	private:
		void Ws()
		{
			while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) {
				++i_;
			}
		}
		bool Lit(std::string_view a_word)
		{
			if (s_.compare(i_, a_word.size(), a_word) == 0) {
				i_ += a_word.size();
				return true;
			}
			return false;
		}
		bool Str(std::string& a_out)
		{
			if (i_ >= s_.size() || s_[i_] != '"') {
				return false;
			}
			++i_;
			while (i_ < s_.size() && s_[i_] != '"') {
				if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
					++i_;
				}
				a_out.push_back(s_[i_++]);
			}
			return i_++ < s_.size();
		}
		bool One(Value& a_v)
		{
			Ws();
			if (i_ >= s_.size()) {
				return false;
			}
			const char c = s_[i_];
			if (c == '{') {
				a_v.type = Value::Type::kObj;
				++i_;
				Ws();
				if (i_ < s_.size() && s_[i_] == '}') {
					++i_;
					return true;
				}
				while (true) {
					Ws();
					std::string k;
					if (!Str(k)) {
						return false;
					}
					Ws();
					if (i_ >= s_.size() || s_[i_++] != ':') {
						return false;
					}
					Value v;
					if (!One(v)) {
						return false;
					}
					a_v.obj.emplace_back(std::move(k), std::move(v));
					Ws();
					if (i_ < s_.size() && s_[i_] == ',') {
						++i_;
						continue;
					}
					return i_ < s_.size() && s_[i_++] == '}';
				}
			}
			if (c == '[') {
				a_v.type = Value::Type::kArr;
				++i_;
				Ws();
				if (i_ < s_.size() && s_[i_] == ']') {
					++i_;
					return true;
				}
				while (true) {
					Value v;
					if (!One(v)) {
						return false;
					}
					a_v.arr.push_back(std::move(v));
					Ws();
					if (i_ < s_.size() && s_[i_] == ',') {
						++i_;
						continue;
					}
					return i_ < s_.size() && s_[i_++] == ']';
				}
			}
			if (c == '"') {
				a_v.type = Value::Type::kStr;
				return Str(a_v.str);
			}
			if (Lit("true")) {
				a_v.type = Value::Type::kBool;
				a_v.b = true;
				return true;
			}
			if (Lit("false")) {
				a_v.type = Value::Type::kBool;
				return true;
			}
			if (Lit("null")) {
				return true;
			}
			if (Lit("NaN") || Lit("nan")) {
				a_v.type = Value::Type::kNum;
				a_v.num = NAN;
				return true;
			}
			char*      end = nullptr;
			const auto v = std::strtod(s_.c_str() + i_, &end);
			if (end == s_.c_str() + i_) {
				return false;
			}
			a_v.type = Value::Type::kNum;
			a_v.num = v;
			i_ = static_cast<std::size_t>(end - s_.c_str());
			return true;
		}

		std::string s_;
		std::size_t i_{ 0 };
	};

	inline std::optional<Value> Load(const std::filesystem::path& a_path)
	{
		std::ifstream in(a_path, std::ios::binary);
		if (!in) {
			return std::nullopt;
		}
		std::stringstream ss;
		ss << in.rdbuf();
		Value v;
		if (!Parser(ss.str()).Parse(v)) {
			return std::nullopt;
		}
		return v;
	}

	inline Vec3 V3(const Value* a_v)
	{
		if (!a_v || a_v->type != Value::Type::kArr || a_v->arr.size() < 3) {
			return { NAN, NAN, NAN };
		}
		return { static_cast<float>(a_v->arr[0].num), static_cast<float>(a_v->arr[1].num), static_cast<float>(a_v->arr[2].num) };
	}

	// The fixtures' folder: next to er/ (the shipped ini's folder is er/, given to the test as argv[1]).
	inline std::filesystem::path g_fixtures;
}
