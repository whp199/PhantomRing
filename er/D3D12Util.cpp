#include "D3D12Util.h"

#include <algorithm>
#include <format>

namespace phantom::er::d3d
{
	FormatInfo Info(DXGI_FORMAT a_f)
	{
		using K = Kind;
		auto v = [](K k, std::uint8_t c, std::uint32_t b, bool un = false, float mx = 0) { return FormatInfo{ k, c, un, false, false, b, mx }; };
		auto bc = [](std::uint32_t b, std::uint8_t c, bool srgb = false) { return FormatInfo{ K::kFloat, c, true, true, srgb, b, 0 }; };
		switch (a_f) {
		case DXGI_FORMAT_R32G32B32A32_FLOAT: return v(K::kFloat, 4, 16);
		case DXGI_FORMAT_R32G32B32A32_UINT: return v(K::kUint, 4, 16);
		case DXGI_FORMAT_R32G32B32A32_SINT: return v(K::kSint, 4, 16);
		case DXGI_FORMAT_R32G32B32_FLOAT: return v(K::kFloat, 3, 12);
		case DXGI_FORMAT_R32G32B32_UINT: return v(K::kUint, 3, 12);
		case DXGI_FORMAT_R32G32B32_SINT: return v(K::kSint, 3, 12);
		case DXGI_FORMAT_R16G16B16A16_FLOAT: return v(K::kFloat, 4, 8);
		case DXGI_FORMAT_R16G16B16A16_UNORM: return v(K::kFloat, 4, 8, true, 65535.0f);
		case DXGI_FORMAT_R16G16B16A16_SNORM: return v(K::kFloat, 4, 8);
		case DXGI_FORMAT_R16G16B16A16_UINT: return v(K::kUint, 4, 8);
		case DXGI_FORMAT_R16G16B16A16_SINT: return v(K::kSint, 4, 8);
		case DXGI_FORMAT_R32G32_FLOAT: return v(K::kFloat, 2, 8);
		case DXGI_FORMAT_R32G32_UINT: return v(K::kUint, 2, 8);
		case DXGI_FORMAT_R32G32_SINT: return v(K::kSint, 2, 8);
		case DXGI_FORMAT_R10G10B10A2_UNORM: return v(K::kFloat, 4, 4, true, 1023.0f);
		case DXGI_FORMAT_R10G10B10A2_UINT: return v(K::kUint, 4, 4);
		case DXGI_FORMAT_R11G11B10_FLOAT: return v(K::kFloat, 3, 4);
		case DXGI_FORMAT_R8G8B8A8_UNORM: return v(K::kFloat, 4, 4, true, 255.0f);
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return { K::kFloat, 4, true, false, true, 4, 255.0f };
		case DXGI_FORMAT_R8G8B8A8_SNORM: return v(K::kFloat, 4, 4);
		case DXGI_FORMAT_R8G8B8A8_UINT: return v(K::kUint, 4, 4);
		case DXGI_FORMAT_R8G8B8A8_SINT: return v(K::kSint, 4, 4);
		case DXGI_FORMAT_B8G8R8A8_UNORM: return v(K::kFloat, 4, 4, true, 255.0f);
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return { K::kFloat, 4, true, false, true, 4, 255.0f };
		case DXGI_FORMAT_B8G8R8X8_UNORM: return v(K::kFloat, 4, 4, true, 255.0f);
		case DXGI_FORMAT_R16G16_FLOAT: return v(K::kFloat, 2, 4);
		case DXGI_FORMAT_R16G16_UNORM: return v(K::kFloat, 2, 4, true, 65535.0f);
		case DXGI_FORMAT_R16G16_SNORM: return v(K::kFloat, 2, 4);
		case DXGI_FORMAT_R16G16_UINT: return v(K::kUint, 2, 4);
		case DXGI_FORMAT_R16G16_SINT: return v(K::kSint, 2, 4);
		case DXGI_FORMAT_R32_FLOAT: return v(K::kFloat, 1, 4);
		case DXGI_FORMAT_R32_UINT: return v(K::kUint, 1, 4);
		case DXGI_FORMAT_R32_SINT: return v(K::kSint, 1, 4);
		case DXGI_FORMAT_R8G8_UNORM: return v(K::kFloat, 2, 2, true, 255.0f);
		case DXGI_FORMAT_R8G8_SNORM: return v(K::kFloat, 2, 2);
		case DXGI_FORMAT_R8G8_UINT: return v(K::kUint, 2, 2);
		case DXGI_FORMAT_R16_FLOAT: return v(K::kFloat, 1, 2);
		case DXGI_FORMAT_R16_UNORM: return v(K::kFloat, 1, 2, true, 65535.0f);
		case DXGI_FORMAT_R16_UINT: return v(K::kUint, 1, 2);
		case DXGI_FORMAT_R8_UNORM: return v(K::kFloat, 1, 1, true, 255.0f);
		case DXGI_FORMAT_R8_UINT: return v(K::kUint, 1, 1);
		case DXGI_FORMAT_A8_UNORM: return v(K::kFloat, 1, 1, true, 255.0f);
		case DXGI_FORMAT_BC1_UNORM: return bc(8, 4);
		case DXGI_FORMAT_BC1_UNORM_SRGB: return bc(8, 4, true);
		case DXGI_FORMAT_BC2_UNORM: return bc(16, 4);
		case DXGI_FORMAT_BC2_UNORM_SRGB: return bc(16, 4, true);
		case DXGI_FORMAT_BC3_UNORM: return bc(16, 4);
		case DXGI_FORMAT_BC3_UNORM_SRGB: return bc(16, 4, true);
		case DXGI_FORMAT_BC4_UNORM: return bc(8, 1);
		case DXGI_FORMAT_BC4_SNORM: return bc(8, 1);
		case DXGI_FORMAT_BC5_UNORM: return bc(16, 2);
		case DXGI_FORMAT_BC5_SNORM: return bc(16, 2);
		case DXGI_FORMAT_BC6H_UF16: return bc(16, 3);
		case DXGI_FORMAT_BC6H_SF16: return bc(16, 3);
		case DXGI_FORMAT_BC7_UNORM: return bc(16, 4);
		case DXGI_FORMAT_BC7_UNORM_SRGB: return bc(16, 4, true);
		default: return {};
		}
	}

	DXGI_FORMAT Typed(DXGI_FORMAT a_f)
	{
		switch (a_f) {
		case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
		case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
		case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
		case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
		case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
		case DXGI_FORMAT_BC6H_TYPELESS: return DXGI_FORMAT_BC6H_UF16;
		case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
		case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
		case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
		case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
		case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
		default: return a_f;
		}
	}

	DXGI_FORMAT RtvFormat(DXGI_FORMAT a_f)
	{
		switch (a_f) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		default: return a_f;
		}
	}

	const char* Name(DXGI_FORMAT a_f)
	{
		switch (a_f) {
		case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
		case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
		case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
		case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
		case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
		case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
		case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
		case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
		case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
		case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
		case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
		case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
		case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
		case DXGI_FORMAT_BC1_UNORM: return "BC1_UNORM";
		case DXGI_FORMAT_BC3_UNORM: return "BC3_UNORM";
		case DXGI_FORMAT_BC5_UNORM: return "BC5_UNORM";
		case DXGI_FORMAT_BC7_UNORM: return "BC7_UNORM";
		default: return "?";
		}
	}

	void MipLayout(DXGI_FORMAT a_format, std::uint32_t a_w, std::uint32_t a_h, std::uint32_t& a_rowBytes, std::uint32_t& a_rows)
	{
		const auto info = Info(Typed(a_format));
		if (info.block) {
			a_rowBytes = std::max(1u, (a_w + 3) / 4) * info.bytes;
			a_rows = std::max(1u, (a_h + 3) / 4);
		} else {
			a_rowBytes = a_w * info.bytes;
			a_rows = a_h;
		}
	}

	std::string Hr(HRESULT a_hr)
	{
		const char* name = "";
		switch (static_cast<std::uint32_t>(a_hr)) {
		case 0x887A0005: name = " (DXGI_ERROR_DEVICE_REMOVED)"; break;
		case 0x887A0006: name = " (DXGI_ERROR_DEVICE_HUNG)"; break;
		case 0x887A0007: name = " (DXGI_ERROR_DEVICE_RESET)"; break;
		case 0x80070057: name = " (E_INVALIDARG)"; break;
		case 0x8007000E: name = " (E_OUTOFMEMORY)"; break;
		case 0x80004002: name = " (E_NOINTERFACE)"; break;
		case 0x80004005: name = " (E_FAIL)"; break;
		default: break;
		}
		return std::format("{:#010x}{}", static_cast<std::uint32_t>(a_hr), name);
	}
}
