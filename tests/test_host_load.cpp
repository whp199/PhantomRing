// Loads PhantomRingHost.dll into a process that is not Elden Ring (run with wine, next to the DLL):
// it must stay inert and log "exe mismatch" instead of touching anything or crashing.
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc, char** argv)
{
	const char* dll = argc > 1 ? argv[1] : "PhantomRingHost.dll";
	std::string log = dll;
	const auto  slash = log.find_last_of("\\/");
	log = (slash == std::string::npos ? std::string() : log.substr(0, slash + 1)) + "PhantomRingHost.log";
	std::remove(log.c_str());

	HMODULE module = ::LoadLibraryA(dll);
	if (!module) {
		std::printf("FAIL: LoadLibrary(%s) error %lu\n", dll, ::GetLastError());
		return 1;
	}
	std::string text;
	for (int i = 0; i < 50; ++i) {  // the DLL works on its own thread
		::Sleep(100);
		std::ifstream     in(log);
		std::stringstream s;
		s << in.rdbuf();
		text = s.str();
		if (text.find("exe mismatch") != std::string::npos) {
			break;
		}
	}
	::Sleep(300);  // give a misbehaving init thread a chance to crash us
	::FreeLibrary(module);
	std::printf("%s", text.c_str());
	if (text.find("exe mismatch") == std::string::npos || text.find("shared memory") != std::string::npos) {
		std::printf("FAIL: expected an inert 'exe mismatch' log\n");
		return 1;
	}
	std::printf("OK: inert in a non-Elden Ring process\n");
	return 0;
}
