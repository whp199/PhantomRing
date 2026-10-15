cmake_minimum_required(VERSION 3.25)

# The one version source is the VERSION file (e.g. 0.2.0-alpha): CMake's project version is its numeric part; both DLLs
# (common/Version.h), `pr --version` and the packages' BUILD.txt read the whole of it.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PHANTOM_RELEASE LIMIT_COUNT 1)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" PHANTOM_VERSION_CORE "${PHANTOM_RELEASE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION)
project(PhantomRing VERSION ${PHANTOM_VERSION_CORE} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Everything static: the DLLs must load in a stock Proton prefix with no MinGW runtime around.
set(PHANTOM_STATIC_LINK -static -static-libgcc -static-libstdc++)

# ---- MinHook (git submodule, v1.3.4) --------------------------------------------------------------
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/extern/minhook/src/hook.c")
	message(FATAL_ERROR "extern/minhook is empty: clone with --recursive, or run `git submodule update --init`")
endif()
add_library(minhook STATIC
	extern/minhook/src/buffer.c
	extern/minhook/src/hook.c
	extern/minhook/src/trampoline.c
	extern/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC extern/minhook/include)

# ---- version: VERSION + the commit id, regenerated on every build (cmake/version.cmake) ---------------
set(PHANTOM_VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/phantomring_version.h)
add_custom_target(phantom_version
	COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${PHANTOM_VERSION_HEADER} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake
	BYPRODUCTS ${PHANTOM_VERSION_HEADER}
	VERBATIM)

# ---- shared core: mapping, rings, log, ini, hooks, version --------------------------------------
add_library(phantom_common STATIC
	common/Mapping.cpp
	common/Log.cpp
	common/Ini.cpp
	common/Hook.cpp
	common/Platform.cpp
	common/Version.cpp)
add_dependencies(phantom_common phantom_version)
target_include_directories(phantom_common PUBLIC common protocol)
target_include_directories(phantom_common PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(phantom_common PUBLIC minhook)
target_compile_definitions(phantom_common PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_compile_options(phantom_common PUBLIC -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

# ---- Elden Ring host plugin (me3 native) ---------------------------------------------------------
file(GLOB_RECURSE PHANTOM_HOST_SOURCES CONFIGURE_DEPENDS er/*.cpp er/*.h er/*.S)
if(PHANTOM_HOST_SOURCES)
	add_library(PhantomRingHost SHARED ${PHANTOM_HOST_SOURCES})
	target_include_directories(PhantomRingHost PRIVATE er)
	target_link_libraries(PhantomRingHost PRIVATE phantom_common d3d12 dxgi d3dcompiler ole32 bcrypt user32 ws2_32)
	target_link_options(PhantomRingHost PRIVATE ${PHANTOM_STATIC_LINK} -Wl,--no-insert-timestamp)
	set_target_properties(PhantomRingHost PROPERTIES PREFIX "" OUTPUT_NAME "PhantomRingHost")
	# The DLL reads its ini from next to itself; er/PhantomRingHost.ini is the edited copy (re-copied on change).
	configure_file(er/PhantomRingHost.ini ${CMAKE_CURRENT_BINARY_DIR}/PhantomRingHost.ini COPYONLY)
endif()

# ---- MGSV guest plugin (dinput8.dll proxy) --------------------------------------------------------
file(GLOB_RECURSE PHANTOM_GUEST_SOURCES CONFIGURE_DEPENDS mgsv/*.cpp mgsv/*.h mgsv/*.S)
if(PHANTOM_GUEST_SOURCES)
	add_library(PhantomRingGuest SHARED ${PHANTOM_GUEST_SOURCES} mgsv/dinput8.def)
	target_include_directories(PhantomRingGuest PRIVATE mgsv)
	target_link_libraries(PhantomRingGuest PRIVATE phantom_common d3d11 dxgi d3dcompiler ole32 bcrypt user32 ws2_32)
	target_link_options(PhantomRingGuest PRIVATE ${PHANTOM_STATIC_LINK} -Wl,--no-insert-timestamp)
	set_target_properties(PhantomRingGuest PROPERTIES PREFIX "" OUTPUT_NAME "dinput8")
endif()

# ---- tests (run under wine) -------------------------------------------------------------------
add_executable(test_common tests/test_common.cpp)
target_link_libraries(test_common PRIVATE phantom_common)
target_link_options(test_common PRIVATE ${PHANTOM_STATIC_LINK})

add_executable(layout_dump tests/layout_dump.cpp)
target_link_libraries(layout_dump PRIVATE phantom_common)
target_link_options(layout_dump PRIVATE ${PHANTOM_STATIC_LINK})

# Host plugin: console expressions, anchor math, kinematic controller, link + console without a game
# (all of er/ but its DllMain), and an inert-load check (the DLL in a non-Elden Ring process must log
# "exe mismatch" and do nothing).
set(PHANTOM_HOST_TEST_SOURCES ${PHANTOM_HOST_SOURCES})
list(FILTER PHANTOM_HOST_TEST_SOURCES EXCLUDE REGEX "/er/main\\.cpp$")
add_executable(test_host tests/test_host.cpp ${PHANTOM_HOST_TEST_SOURCES})
target_include_directories(test_host PRIVATE er)
target_link_libraries(test_host PRIVATE phantom_common bcrypt user32)
target_link_options(test_host PRIVATE ${PHANTOM_STATIC_LINK})

# Combat (er/CombatLogic, er/Params): shapes, damage, pacing, params; real regulation data when
# tools/re/er_params.py has extracted it into build/er_params.
add_executable(test_host_combat tests/test_host_combat.cpp ${PHANTOM_HOST_TEST_SOURCES})
target_include_directories(test_host_combat PRIVATE er)
target_link_libraries(test_host_combat PRIVATE phantom_common bcrypt user32)
target_link_options(test_host_combat PRIVATE ${PHANTOM_STATIC_LINK})

# Renderer stage C (er/Shaders, er/RenderShared, er/RenderPso): shaders compile with Wine's d3dcompiler,
# constant blocks match, pipeline-stream parsing, near-camera fades, the depth mapping.
add_executable(test_render tests/test_render.cpp ${PHANTOM_HOST_TEST_SOURCES})
target_include_directories(test_render PRIVATE er)
target_link_libraries(test_render PRIVATE phantom_common bcrypt user32 dxguid)
target_link_options(test_render PRIVATE ${PHANTOM_STATIC_LINK})

add_executable(test_host_load tests/test_host_load.cpp)
target_link_options(test_host_load PRIVATE ${PHANTOM_STATIC_LINK})

# The host's own address verification run against eldenring.exe on disk (mapped, never executed):
#   wine build/verify_er_addresses.exe 'Z:\...\ELDEN RING\Game\eldenring.exe'
add_executable(verify_er_addresses tests/verify_er_addresses.cpp er/Addresses.cpp er/Singletons.cpp)
target_include_directories(verify_er_addresses PRIVATE er)
target_link_libraries(verify_er_addresses PRIVATE phantom_common bcrypt)
target_link_options(verify_er_addresses PRIVATE ${PHANTOM_STATIC_LINK} -Wl,--image-base=0x10000000)

set(PHANTOM_WINEPREFIX "$ENV{WINEPREFIX}" CACHE PATH "Wine prefix for the offline tests (empty: wine's default)")
# An empty prefix must not reach wine as WINEPREFIX= (wine refuses it): leave the variable out then.
if(PHANTOM_WINEPREFIX)
	set(PHANTOM_WINE_ENV "WINEPREFIX=${PHANTOM_WINEPREFIX}")
else()
	set(PHANTOM_WINE_ENV "--unset=WINEPREFIX")
endif()

# Host + common tests, run from build/ so their scratch files stay there:
#   cmake --build build --target check_host
add_custom_target(check_host
	COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all wine $<TARGET_FILE:test_common>
	COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all wine $<TARGET_FILE:test_host> ${CMAKE_SOURCE_DIR}/er/PhantomRingHost.ini
	COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all wine $<TARGET_FILE:test_host_combat> ${CMAKE_SOURCE_DIR}/er/PhantomRingHost.ini er_params
	COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all wine $<TARGET_FILE:test_render>
	COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all wine $<TARGET_FILE:test_host_load> $<TARGET_FILE:PhantomRingHost>
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	DEPENDS test_common test_host test_host_combat test_render test_host_load PhantomRingHost
	USES_TERMINAL)

# ---- MGSV guest tests (run under wine: cmake --build build --target check_guest) ---------------------
if(PHANTOM_GUEST_SOURCES)
	add_executable(test_guest_logic tests/test_guest_logic.cpp mgsv/Logic.cpp)
	target_include_directories(test_guest_logic PRIVATE mgsv)
	target_link_options(test_guest_logic PRIVATE ${PHANTOM_STATIC_LINK})

	# MGSV's dive as kGuestDiving (mgsv/Dive.h, roadmap 2.9)
	add_executable(test_guest_dive tests/test_guest_dive.cpp mgsv/Dive.cpp)
	target_include_directories(test_guest_dive PRIVATE mgsv)
	target_link_options(test_guest_dive PRIVATE ${PHANTOM_STATIC_LINK})

	# CQC holds and throws (ROADMAP 2.2): the intent tracker behind kGEvCqc.
	add_executable(test_guest_cqc tests/test_guest_cqc.cpp mgsv/CqcLogic.cpp)
	target_include_directories(test_guest_cqc PRIVATE mgsv protocol)
	target_link_options(test_guest_cqc PRIVATE ${PHANTOM_STATIC_LINK})

	# C4's detonator logic (mgsv/C4Logic.cpp, docs/GUEST.md 18.8a)
	add_executable(test_guest_c4 tests/test_guest_c4.cpp mgsv/C4Logic.cpp)
	target_include_directories(test_guest_c4 PRIVATE mgsv)
	target_link_options(test_guest_c4 PRIVATE ${PHANTOM_STATIC_LINK})

	add_executable(test_guest_actorproxy tests/test_guest_actorproxy.cpp mgsv/ActorProxy.cpp mgsv/Logic.cpp)
	target_include_directories(test_guest_actorproxy PRIVATE mgsv)
	target_link_libraries(test_guest_actorproxy PRIVATE phantom_common)
	target_link_options(test_guest_actorproxy PRIVATE ${PHANTOM_STATIC_LINK})

	# D-Horse treadmill's pure half (mgsv/HorseLogic.cpp, docs/GUEST.md 8.1a)
	add_executable(test_guest_horse tests/test_guest_horse.cpp mgsv/HorseLogic.cpp mgsv/Logic.cpp)
	target_include_directories(test_guest_horse PRIVATE mgsv)
	target_link_options(test_guest_horse PRIVATE ${PHANTOM_STATIC_LINK})

	add_executable(test_guest_link tests/test_guest_link.cpp mgsv/Link.cpp mgsv/Config.cpp)
	target_include_directories(test_guest_link PRIVATE mgsv)
	target_link_libraries(test_guest_link PRIVATE phantom_common)
	target_link_options(test_guest_link PRIVATE ${PHANTOM_STATIC_LINK} -municode)

	add_executable(test_guest_watch tests/test_guest_watch.cpp mgsv/Watch.cpp)
	target_include_directories(test_guest_watch PRIVATE mgsv)
	target_link_libraries(test_guest_watch PRIVATE phantom_common)
	target_link_options(test_guest_watch PRIVATE ${PHANTOM_STATIC_LINK})

	add_executable(test_guest_proxy tests/test_guest_proxy.cpp mgsv/DInputWrap.cpp)
	target_include_directories(test_guest_proxy PRIVATE mgsv)
	target_compile_definitions(test_guest_proxy PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
	target_link_options(test_guest_proxy PRIVATE ${PHANTOM_STATIC_LINK} -municode)
	add_dependencies(test_guest_proxy PhantomRingGuest)

	# The Lua pump against a real Lua 5.1.5 (MGSV's version) with a mock engine. Needs Lua's sources:
	#   -DPHANTOM_LUA51_SRC=<lua-5.1.5>/src   (luaconf.h or luaconf.h.in in it)
	set(PHANTOM_LUA51_SRC "" CACHE PATH "Lua 5.1.5 src/ directory, for test_guest_pump (optional)")
	set(PHANTOM_GUEST_PUMP_TEST ${CMAKE_COMMAND} -E echo "PHANTOM_LUA51_SRC not set: guest pump test skipped")
	if(PHANTOM_LUA51_SRC AND EXISTS "${PHANTOM_LUA51_SRC}/lapi.c")
		file(GLOB PHANTOM_LUA51_SOURCES "${PHANTOM_LUA51_SRC}/*.c")
		list(FILTER PHANTOM_LUA51_SOURCES EXCLUDE REGEX "/(lua|luac|print)\\.c$")
		set(PHANTOM_LUA51_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/lua51-include")
		if(NOT EXISTS "${PHANTOM_LUA51_SRC}/luaconf.h")
			configure_file("${PHANTOM_LUA51_SRC}/luaconf.h.in" "${PHANTOM_LUA51_INCLUDE}/luaconf.h" COPYONLY)
		endif()
		add_library(lua51_test STATIC ${PHANTOM_LUA51_SOURCES})
		target_include_directories(lua51_test PUBLIC "${PHANTOM_LUA51_INCLUDE}" "${PHANTOM_LUA51_SRC}")
		target_compile_options(lua51_test PRIVATE -w)
		add_executable(test_guest_pump tests/test_guest_pump.cpp mgsv/Pump.cpp mgsv/Lua.cpp mgsv/GuestScript.cpp mgsv/State.cpp mgsv/Link.cpp
			mgsv/Config.cpp mgsv/Pad.cpp mgsv/Offline.cpp mgsv/Verify.cpp mgsv/Logic.cpp mgsv/ActorProxy.cpp)
		target_include_directories(test_guest_pump PRIVATE mgsv)
		target_compile_definitions(test_guest_pump PRIVATE PHANTOM_GUEST_TESTING)
		target_link_libraries(test_guest_pump PRIVATE phantom_common lua51_test bcrypt)
		target_link_options(test_guest_pump PRIVATE ${PHANTOM_STATIC_LINK})
		set(PHANTOM_GUEST_PUMP_TEST ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all wine $<TARGET_FILE:test_guest_pump>)
		set(PHANTOM_GUEST_PUMP_DEP test_guest_pump)
	endif()

	# The proxy must win over Wine's builtin dinput8, exactly as in the game (WINEDLLOVERRIDES).
	# The embedded guest Lua against a mock engine (stock Lua 5.1 = MGSV's dialect), when available.
	find_program(PHANTOM_LUA51 NAMES lua5.1 lua51)
	set(PHANTOM_GUEST_LUA_TEST ${CMAKE_COMMAND} -E echo "lua5.1 not found: guest script test skipped")
	if(PHANTOM_LUA51)
		set(PHANTOM_GUEST_LUA_TEST ${PHANTOM_LUA51} ${CMAKE_SOURCE_DIR}/tests/test_guest_script.lua ${CMAKE_SOURCE_DIR}/mgsv/GuestScript.cpp)
	endif()
	add_custom_target(check_guest
		COMMAND ${PHANTOM_GUEST_LUA_TEST}
		COMMAND ${PHANTOM_GUEST_PUMP_TEST}
		COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all wine $<TARGET_FILE:test_guest_logic>
		COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all wine $<TARGET_FILE:test_guest_dive>
		COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all wine $<TARGET_FILE:test_guest_c4>
		COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all wine $<TARGET_FILE:test_guest_link> ${CMAKE_SOURCE_DIR}/mgsv/PhantomRingGuest.ini
		COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all wine $<TARGET_FILE:test_guest_watch>
		COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all wine $<TARGET_FILE:test_guest_actorproxy>
		COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all wine $<TARGET_FILE:test_guest_cqc>
		COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all wine $<TARGET_FILE:test_guest_horse>
		COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all "WINEDLLOVERRIDES=dinput8=n,b"
			wine $<TARGET_FILE:test_guest_proxy> $<TARGET_FILE:PhantomRingGuest>
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		DEPENDS test_guest_logic test_guest_dive test_guest_c4 test_guest_actorproxy test_guest_cqc test_guest_horse test_guest_link test_guest_watch test_guest_proxy PhantomRingGuest ${PHANTOM_GUEST_PUMP_DEP}
		USES_TERMINAL)
endif()

# ---- Windows players (docs/WINDOWS.md): platform decisions, Steam lookup, guest install, launcher, the
# named mapping between two processes, the hidden MGSV window. Run under wine with
# PHANTOMRING_PLATFORM=windows where a test needs the Windows paths:
#   cmake --build build --target check_windows
add_executable(test_windows tests/test_windows.cpp er/SteamLibrary.cpp er/GuestInstall.cpp er/Launcher.cpp er/Link.cpp)
target_include_directories(test_windows PRIVATE er)
target_link_libraries(test_windows PRIVATE phantom_common bcrypt)
target_link_options(test_windows PRIVATE ${PHANTOM_STATIC_LINK} -municode)

add_executable(test_windows_link tests/test_windows_link.cpp er/SteamLibrary.cpp er/GuestInstall.cpp er/Launcher.cpp er/Link.cpp mgsv/Link.cpp)
target_include_directories(test_windows_link PRIVATE er)
target_link_libraries(test_windows_link PRIVATE phantom_common bcrypt)
target_link_options(test_windows_link PRIVATE ${PHANTOM_STATIC_LINK})

if(PHANTOM_GUEST_SOURCES)
	# Image base away from 0x140000000: the guest's MGSV addresses must be unmapped here.
	add_executable(test_guest_window tests/test_guest_window.cpp mgsv/WindowGuard.cpp mgsv/WindowHidden.cpp mgsv/Verify.cpp)
	target_include_directories(test_guest_window PRIVATE mgsv)
	target_link_libraries(test_guest_window PRIVATE phantom_common bcrypt user32)
	target_link_options(test_guest_window PRIVATE ${PHANTOM_STATIC_LINK} -Wl,--image-base=0x10000000)
	set(PHANTOM_GUEST_WINDOW_TEST ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all PHANTOMRING_PLATFORM=windows
		wine $<TARGET_FILE:test_guest_window> ${CMAKE_SOURCE_DIR}/mgsv/PhantomRingGuest.ini)
	set(PHANTOM_GUEST_WINDOW_DEP test_guest_window)
endif()

add_custom_target(check_windows
	COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all wine $<TARGET_FILE:test_windows> ${CMAKE_SOURCE_DIR}/er/PhantomRingHost.ini
	COMMAND ${CMAKE_COMMAND} -E env ${PHANTOM_WINE_ENV} WINEDEBUG=-all PHANTOMRING_PLATFORM=windows wine $<TARGET_FILE:test_windows_link>
	COMMAND ${PHANTOM_GUEST_WINDOW_TEST}
	COMMAND python3 -I ${CMAKE_SOURCE_DIR}/tools/check_imports.py $<TARGET_FILE:PhantomRingHost> $<TARGET_FILE:PhantomRingGuest>
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	DEPENDS test_windows test_windows_link ${PHANTOM_GUEST_WINDOW_DEP} PhantomRingHost PhantomRingGuest
	USES_TERMINAL)
