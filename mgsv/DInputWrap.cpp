#include "DInputWrap.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <atomic>

namespace phantom::guest::dinput
{
	namespace
	{
		std::atomic<unsigned long> g_hidden{ 0 };

		// Local copies: the guest doesn't link dxguid/uuid.
		constexpr GUID kIIDUnknown{ 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

		bool IsGameController(DWORD a_devType)
		{
			const auto type = GET_DIDEVICE_TYPE(a_devType);
			return type >= DI8DEVTYPE_JOYSTICK && type <= DI8DEVTYPE_1STPERSON;
		}

		struct EnumContext
		{
			LPDIENUMDEVICESCALLBACKW callback;
			LPVOID                   ref;
		};

		BOOL CALLBACK FilterCallback(LPCDIDEVICEINSTANCEW a_device, LPVOID a_ref)
		{
			auto* ctx = static_cast<EnumContext*>(a_ref);
			if (a_device && IsGameController(a_device->dwDevType)) {
				g_hidden.fetch_add(1, std::memory_order_relaxed);
				return DIENUM_CONTINUE;
			}
			return ctx->callback(a_device, ctx->ref);
		}

		class Wrapper final : public IDirectInput8W
		{
		public:
			explicit Wrapper(IDirectInput8W* a_real) :
				real_(a_real) {}

			// IUnknown
			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_iid, void** a_out) override
			{
				if (!a_out) {
					return E_POINTER;
				}
				if (IsEqualGUID(a_iid, kIIDUnknown) || IsEqualGUID(a_iid, kIID_IDirectInput8W)) {
					AddRef();
					*a_out = static_cast<IDirectInput8W*>(this);
					return S_OK;
				}
				return real_->QueryInterface(a_iid, a_out);
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG left = --refs_;
				if (left == 0) {
					real_->Release();
					delete this;
				}
				return left;
			}

			// IDirectInput8W
			HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID a_guid, LPDIRECTINPUTDEVICE8W* a_device, LPUNKNOWN a_outer) override
			{
				return real_->CreateDevice(a_guid, a_device, a_outer);
			}
			HRESULT STDMETHODCALLTYPE EnumDevices(DWORD a_type, LPDIENUMDEVICESCALLBACKW a_callback, LPVOID a_ref, DWORD a_flags) override
			{
				if (!a_callback) {
					return real_->EnumDevices(a_type, a_callback, a_ref, a_flags);
				}
				if (a_type == DI8DEVCLASS_GAMECTRL || (a_type >= DI8DEVTYPE_JOYSTICK && a_type <= DI8DEVTYPE_1STPERSON)) {
					return DI_OK;  // there are no game controllers
				}
				EnumContext ctx{ a_callback, a_ref };
				return real_->EnumDevices(a_type, &FilterCallback, &ctx, a_flags);
			}
			HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID a_guid) override { return real_->GetDeviceStatus(a_guid); }
			HRESULT STDMETHODCALLTYPE RunControlPanel(HWND a_owner, DWORD a_flags) override { return real_->RunControlPanel(a_owner, a_flags); }
			HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE a_instance, DWORD a_version) override { return real_->Initialize(a_instance, a_version); }
			HRESULT STDMETHODCALLTYPE FindDevice(REFGUID a_class, LPCWSTR a_name, LPGUID a_out) override { return real_->FindDevice(a_class, a_name, a_out); }
			HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCWSTR a_user, LPDIACTIONFORMATW a_format, LPDIENUMDEVICESBYSEMANTICSCBW a_callback,
				LPVOID a_ref, DWORD a_flags) override
			{
				// Action-mapping enumeration would list the pads too; MGSV doesn't use it (no such
				// import path in the exe), so answer "nothing".
				return DI_OK;
			}
			HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK a_callback, LPDICONFIGUREDEVICESPARAMSW a_params, DWORD a_flags,
				LPVOID a_ref) override
			{
				return real_->ConfigureDevices(a_callback, a_params, a_flags, a_ref);
			}

		private:
			IDirectInput8W*    real_;
			std::atomic<ULONG> refs_{ 1 };
		};
	}

	IUnknown* WrapW(IUnknown* a_real)
	{
		return a_real ? static_cast<IDirectInput8W*>(new Wrapper(static_cast<IDirectInput8W*>(a_real))) : nullptr;
	}

	unsigned long HiddenDevices() { return g_hidden.load(); }
}
