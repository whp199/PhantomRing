#include "Camera.h"

#include "Addresses.h"
#include "Hook.h"
#include "Log.h"
#include "Verify.h"

#include <windows.h>

#include <atomic>
#include <cstring>

extern "C"
{
	void* phantom_camera_trampoline = nullptr;
	void  phantom_camera_intercept();
	void  phantom_camera_observed(void* a_camera, const float* a_source, std::uintptr_t a_return, std::uintptr_t a_rsi) noexcept;
}

// Entry: rcx = camera, rdx = source {quat, pos}, [rsp] = return address, rsi = the caller's RSI.
// The setter is a leaf reached through a virtual call, so only the volatile registers it reads (rcx,
// rdx) must survive into the trampoline; rsi is non-volatile and untouched by our C++ (MS x64 ABI).
// Stack: entry rsp = 16n+8; two pushes + 0x28 leave it 16-aligned with 32 bytes of shadow space.
// (Port of MGS5VR's camera_bridge.asm to GNU as; MIT, see /THIRD-PARTY-NOTICES.md.)
asm(R"(
	.text
	.globl phantom_camera_intercept
	.def phantom_camera_intercept; .scl 2; .type 32; .endef
phantom_camera_intercept:
	push %rcx
	push %rdx
	sub $0x28, %rsp
	mov 0x38(%rsp), %r8
	mov %rsi, %r9
	call phantom_camera_observed
	add $0x28, %rsp
	pop %rdx
	pop %rcx
	jmp *phantom_camera_trampoline(%rip)
)");

namespace phantom::guest::camera
{
	namespace
	{
		std::atomic<FrameFn> g_onFrame{ nullptr };
		std::atomic<bool>    g_ownerChecked{ false };
		std::uintptr_t       g_rejectedOwner{ 0 };
	}

	bool Install(FrameFn a_onPlayerFrame)
	{
		if (!Matches(addr::kCameraSetter, "camera setter") || !Matches(addr::kCameraCallsite, "player camera callsite")) {
			Log::Error("camera: signatures differ -- no camera, root, motion or skeleton");
			return false;
		}
		if (!Matches(addr::kFocalWrite, "player camera focal write")) {
			Log::Warn("camera: focal-length field unverified -- FOV falls back to [Camera] FovOverrideDeg or 60 deg");
		}
		g_onFrame.store(a_onPlayerFrame);
		return Hooks::Create("camera setter", reinterpret_cast<void*>(addr::kCameraSetter.va), reinterpret_cast<void*>(&phantom_camera_intercept),
			&phantom_camera_trampoline);
	}
}

extern "C" void phantom_camera_observed(void* a_camera, const float* a_source, std::uintptr_t a_return, std::uintptr_t a_rsi) noexcept
{
	using namespace phantom;
	using namespace phantom::guest;
	if (a_return != addr::kCameraReturn) {
		return;  // some other camera (cutscene, menu, ...)
	}
	try {
		// Validate the owner the first time we see it (and whenever it changes): vtable and back-link.
		const auto vt = SafeRead<std::uintptr_t>(reinterpret_cast<const void*>(a_rsi));
		const auto cam = SafeRead<std::uintptr_t>(reinterpret_cast<const void*>(a_rsi + addr::kOwnerCamera));
		if (!vt || *vt != addr::kOwnerVtable || !cam || *cam != reinterpret_cast<std::uintptr_t>(a_camera)) {
			if (a_rsi != camera::g_rejectedOwner) {
				camera::g_rejectedOwner = a_rsi;
				Log::Warn("camera: callsite owner {:#x} failed validation (vtable {:#x})", a_rsi, vt ? *vt : 0);
			}
			return;
		}
		if (!camera::g_ownerChecked.exchange(true)) {
			Log::Info("camera: player camera owner {:#x}, camera {}", a_rsi, a_camera);
		}
		camera::Sample s;
		s.owner = a_rsi;
		s.cameraObject = reinterpret_cast<std::uintptr_t>(a_camera);
		std::memcpy(s.quat, a_source, sizeof(s.quat));
		std::memcpy(s.pos, a_source + 4, sizeof(s.pos));
		std::memcpy(s.root, reinterpret_cast<const void*>(a_rsi + addr::kOwnerRoot), sizeof(s.root));
		std::memcpy(&s.focalMm, static_cast<const std::uint8_t*>(a_camera) + addr::kCameraFocal, sizeof(float));
		if (const auto fn = camera::g_onFrame.load()) {
			fn(s);
		}
	} catch (...) {
	}
}
