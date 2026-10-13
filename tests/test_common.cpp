// Smoke tests for the shared core (run with wine): rings, seqlock, triple buffer, mapping, scans.
#include "Hook.h"
#include "Ini.h"
#include "Log.h"
#include "Mapping.h"

#include <cstdio>
#include <fstream>

using namespace phantom;

static int g_failures = 0;
#define CHECK(x)                                                              \
	do {                                                                      \
		if (!(x)) {                                                           \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);           \
			++g_failures;                                                     \
		}                                                                     \
	} while (0)

struct Payload
{
	std::uint32_t seq;
	std::uint32_t a;
	double        b;
};

int main()
{
	Log::Init(L"test_common.log", "test");

	// mapping (file-backed and named) + seqlock across two views of the same file
	{
		Mapping w, r;
		CHECK(w.Open(L"file:phantom_test.bin", 1 << 20, true));
		CHECK(r.Open(L"file:phantom_test.bin", 1 << 20, false));
		auto* dst = reinterpret_cast<Payload*>(w.Base());
		std::memset(dst, 0, sizeof(Payload));
		Payload out{};
		CHECK(!SeqRead(reinterpret_cast<Payload*>(r.Base()), out));
		SeqWrite(dst, Payload{ 0, 7, 2.5 });
		CHECK(SeqRead(reinterpret_cast<Payload*>(r.Base()), out));
		CHECK(out.a == 7 && out.b == 2.5 && out.seq == 2);
		Mapping n;
		CHECK(n.Open(L"name:Local\\PhantomRingTest", 1 << 16, true));
		Mapping n2;
		CHECK(n2.Open(L"name:Local\\PhantomRingTest", 1 << 16, false));
		n.Base()[5] = 42;
		CHECK(n2.Base()[5] == 42);
	}

	// entry ring
	{
		alignas(64) static std::uint8_t mem[EntryRing<std::uint32_t, 8>::kBytes]{};
		EntryRing<std::uint32_t, 8> ring{ mem };
		for (std::uint32_t i = 0; i < 8; ++i) {
			CHECK(ring.Push(i));
		}
		CHECK(!ring.Push(99));
		std::uint32_t v = 0;
		for (std::uint32_t i = 0; i < 8; ++i) {
			CHECK(ring.Pop(v) && v == i);
		}
		CHECK(!ring.Pop(v));
	}

	// triple buffer: reader always gets the latest published slot, slots never collide
	{
		std::uint32_t state = 0;
		TripleWriter  w{ &state };
		TripleReader  r{ &state };
		std::uint32_t contents[3] = {};
		CHECK(!r.Acquire());
		for (std::uint32_t frame = 1; frame <= 10; ++frame) {
			contents[w.back] = frame;
			w.Publish();
			if (frame % 3 == 0) {
				CHECK(r.Acquire());
				CHECK(contents[r.front] == frame);
				CHECK(r.front != w.back);
			}
		}
	}

	// patterns + scanning our own image
	{
		const auto p = Pattern::Parse("48 8B ?? ? 05");
		CHECK(p.bytes.size() == 5 && p.bytes[2] == -1 && p.bytes[3] == -1 && p.bytes[4] == 5);
		const auto image = ModuleImage::Of(::GetModuleHandleW(nullptr));
		CHECK(!image.sections.empty());
		static const std::uint8_t marker[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x13, 0x37, 0xC0, 0xDE, 0x99 };
		const auto hits = Scan::All(image, Pattern::Parse("DE AD BE EF 13 37 C0 DE ??"), false);
		bool       found = false;
		for (auto* h : hits) {
			found |= h == marker;
		}
		CHECK(found);
	}

	// ini
	{
		std::ofstream("test.ini") << "\xEF\xBB\xBF[Main]\nfoo = 12 ; comment\nBar=yes\n[Other]\nx=1.5\n";
		Ini ini;
		CHECK(ini.Load(L"test.ini"));
		CHECK(ini.GetInt("main", "FOO", 0) == 12);
		CHECK(ini.GetBool("Main", "bar", false));
		CHECK(ini.GetFloat("Other", "x", 0) == 1.5);
		CHECK(ini.Get("Other", "missing", "d") == "d");
	}

	std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
	return g_failures ? 1 : 0;
}
