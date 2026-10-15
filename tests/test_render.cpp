// Offline tests for the host renderer's stage C pieces (run with wine): every shader compiles with the
// d3dcompiler the game uses (Wine's, through vkd3d-shader), the HLSL constant blocks match the C++
// structs, the pipeline-stream parser and vkd3d-proton's shader hash, the near-camera fade rules and
// Elden Ring's G-buffer encodings mirrored on the CPU.
#include "Config.h"
#include "D3D12Util.h"
#include "DebugLayer.h"
#include "Log.h"
#include "RenderMesh.h"
#include "RenderNvg.h"
#include "RenderPso.h"
#include "RenderShadow.h"
#include "RenderShared.h"
#include "Retire.h"
#include "Shaders.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace phantom::er;
namespace proto = phantom::proto;

static int g_failures = 0;
#define CHECK(x)                                                    \
	do {                                                            \
		if (!(x)) {                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
			++g_failures;                                           \
		}                                                           \
	} while (0)

static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static proto::RenInputElement Element(const char* a_semantic, std::uint32_t a_index, DXGI_FORMAT a_format, std::uint32_t a_slot, std::uint32_t a_offset)
{
	proto::RenInputElement e{};
	std::strncpy(e.semantic, a_semantic, sizeof(e.semantic));
	e.semanticIndex = a_index;
	e.format = a_format;
	e.inputSlot = a_slot;
	e.alignedByteOffset = a_offset;
	return e;
}

// The constant buffer sizes the compiler laid out, by name (0 if reflection is unavailable).
static UINT CbufferSize(ID3DBlob* a_blob, const char* a_name)
{
	using ReflectFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
	HMODULE m = ::LoadLibraryW(L"d3dcompiler_47.dll");
	auto    reflect = m ? reinterpret_cast<ReflectFn>(reinterpret_cast<void*>(::GetProcAddress(m, "D3DReflect"))) : nullptr;
	if (!reflect) {
		return 0;
	}
	ID3D11ShaderReflection* r = nullptr;
	if (FAILED(reflect(a_blob->GetBufferPointer(), a_blob->GetBufferSize(), IID_ID3D11ShaderReflection, reinterpret_cast<void**>(&r))) || !r) {
		return 0;
	}
	UINT                     size = 0;
	D3D11_SHADER_BUFFER_DESC d{};
	if (auto* cb = r->GetConstantBufferByName(a_name); cb && SUCCEEDED(cb->GetDesc(&d))) {
		size = d.Size;
	}
	r->Release();
	return size;
}

static void TestShaders()
{
	CHECK(Shaders::Init());
	auto forward = Shaders::PixelShader(Shaders::Pixel::kForward);
	auto gbuffer = Shaders::PixelShader(Shaders::Pixel::kGbuffer);
	auto velocity = Shaders::PixelShader(Shaders::Pixel::kVelocity);
	auto shadow = Shaders::PixelShader(Shaders::Pixel::kShadow);
	CHECK(shadow);
	CHECK(forward);
	CHECK(gbuffer);
	CHECK(velocity);
	CHECK(Shaders::MarkerShader(false) && Shaders::MarkerShader(true));  // er/RenderMarkers.h: throw arc, grenades, mines
	CHECK(RenderNvg::ShadersCompile());  // er/RenderNvg.h: MGSV's night vision

	// Snake's body layout as the guest publishes it (GUEST.md 15.2, merged streams), and a rigid one.
	const std::vector<proto::RenInputElement> skinned = {
		Element("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0),
		Element("NORMAL", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 0),
		Element("TANGENT", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 8),
		Element("BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 20),
		Element("BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 1, 24),
		Element("TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 1, 28),
	};
	const std::vector<proto::RenInputElement> rigid = {
		Element("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0),
		Element("NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12),
		Element("TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16),
	};
	auto vs1 = Shaders::VertexShader(skinned);
	auto vs2 = Shaders::VertexShader(rigid);
	CHECK(vs1);
	CHECK(vs2);

	// The HLSL blocks must match the structs the renderer uploads.
	if (gbuffer) {
		const UINT frame = CbufferSize(gbuffer.Get(), "FrameCB");
		const UINT draw = CbufferSize(gbuffer.Get(), "DrawCB");
		if (frame) {
			CHECK(frame == sizeof(render::FrameConstants));
			CHECK(draw == render::kDrawConstants * 4);
		} else {
			std::printf("note: D3DReflect unavailable, constant block sizes not checked\n");
		}
	}
	if (vs1) {
		const UINT frame = CbufferSize(vs1.Get(), "FrameCB");
		if (frame) {
			CHECK(frame == sizeof(render::FrameConstants));
		}
	}
}

static void TestPso()
{
	// vkd3d-proton names dumps by FNV-1 (multiply then xor) over the bytecode; a dumped DXIL file named
	// 00057fb3ed23c711 hashed to its own name (checked on the live dump, 2026-10-12).
	const std::uint8_t empty[1]{};
	CHECK(RenderPso::ShaderHash(empty, 0) == 14695981039346656037ull);
	const std::uint8_t a[1] = { 'a' };
	CHECK(RenderPso::ShaderHash(a, 1) == ((14695981039346656037ull * 1099511628211ull) ^ 'a'));

	// A d3dx12-style stream: root signature, PS, render-target formats, DSV format, depth-stencil.
	struct alignas(void*) RootSub
	{
		D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE };
		void*                               root{ reinterpret_cast<void*>(0x1234) };
	};
	struct alignas(void*) PsSub
	{
		D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS };
		D3D12_SHADER_BYTECODE               code{};
	};
	struct alignas(void*) RtSub
	{
		D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS };
		D3D12_RT_FORMAT_ARRAY               formats{};
	};
	struct alignas(void*) DsvSub
	{
		D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT };
		DXGI_FORMAT                         format{ DXGI_FORMAT_D32_FLOAT_S8X24_UINT };
	};
	struct alignas(void*) DsSub
	{
		D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL };
		D3D12_DEPTH_STENCIL_DESC            desc{};
	};
	struct Stream
	{
		RootSub root;
		PsSub   ps;
		RtSub   rt;
		DsvSub  dsv;
		DsSub   ds;
	} stream;
	static const std::uint8_t code[4] = { 1, 2, 3, 4 };
	stream.ps.code = { code, sizeof(code) };
	stream.rt.formats.NumRenderTargets = 5;
	const DXGI_FORMAT gb[5] = { DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT };
	std::memcpy(stream.rt.formats.RTFormats, gb, sizeof(gb));
	stream.ds.desc.DepthEnable = TRUE;
	stream.ds.desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	stream.ds.desc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
	stream.ds.desc.StencilEnable = TRUE;
	stream.ds.desc.StencilWriteMask = 0xFF;
	stream.ds.desc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
	const D3D12_PIPELINE_STATE_STREAM_DESC desc{ sizeof(stream), &stream };
	const auto                             info = RenderPso::ParseStream(desc);
	CHECK(info.root == reinterpret_cast<void*>(0x1234));
	CHECK(info.ps == RenderPso::ShaderHash(code, sizeof(code)));
	CHECK(info.psBytes == 4);
	CHECK(info.numRt == 5);
	CHECK(info.rtv[2] == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	CHECK(info.rtv[4] == DXGI_FORMAT_R16G16B16A16_FLOAT);
	CHECK(info.dsv == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
	CHECK(info.depth && info.depthWrite && info.stencil);
	CHECK(info.stencilWrite == 0xFF && info.stencilPass == D3D12_STENCIL_OP_REPLACE);
	CHECK(RenderPso::Formats(info).starts_with("5 RT 24/28/29/49/10 ds 20 z7w"));
}

static void TestFades()
{
	// Snake per pixel only; D-Walker also as a whole by its nearest part (defaults 0.5..1.5 m).
	RenderSnapshot s;
	s.camera.pos = { 0, 0, 0 };
	s.camera.valid = true;
	const float identity[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
	std::memcpy(s.anchor, identity, sizeof(identity));
	proto::DrawRecord snake{};
	snake.owner = proto::kOwnerPlayer;
	snake.matrixCount = 1;
	snake.firstMatrix = 0;
	proto::DrawRecord walker{};
	walker.owner = proto::kOwnerBuddy;
	walker.matrixCount = 2;
	walker.firstMatrix = 1;
	s.draws = { snake, walker };
	// Snake's root 0.2 m from the camera (owner fade must stay 1); D-Walker's parts at 3 m and 1 m.
	const float mats[36] = { 1, 0, 0, 0.2f, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 3, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1 };
	s.matrices.assign(mats, mats + 36);
	const auto fades = render::Fades(s);
	CHECK(fades.size() == 2);
	CHECK(Near(fades[0].owner, 1.0f));
	CHECK(Near(fades[0].range[0], Cfg().render.nearFade[0]) && Near(fades[0].range[1], Cfg().render.nearFade[1]));
	const auto& o = Cfg().render.nearFadeOther;
	CHECK(Near(fades[1].owner, std::clamp((1.0f - o[0]) / (o[1] - o[0]), 0.0f, 1.0f)));

	// D-Walker 2.2 m from the camera, right on the line to Snake (anchor 4 m ahead): an occluder.
	s.anchor[11] = 4.0f;
	const float between[36] = { 1, 0, 0, 0.2f, 0, 1, 0, 0, 0, 0, 1, 4, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, -2, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, -2.2f };
	s.matrices.assign(between, between + 36);  // matrices are anchor-relative: parts at z 2 / 1.8 in the world
	auto f2 = render::Fades(s);
	CHECK(Near(f2[1].owner, Cfg().render.occluderFade));
	CHECK(Near(f2[0].owner, 1.0f));
	s.riding = true;  // never while Snake rides it
	f2 = render::Fades(s);
	CHECK(f2[1].owner > Cfg().render.occluderFade + 0.1f);
	// First person (MGSV's iron sights / scope): Snake's arms and gun in front of the camera are never faded; D-Walker still is.
	s.firstPerson = true;
	f2 = render::Fades(s);
	CHECK(f2[0].range[0] == 0.0f && f2[0].range[1] == 0.0f);
	CHECK(Near(f2[1].range[0], Cfg().render.nearFadeOther[0]) && Near(f2[1].range[1], Cfg().render.nearFadeOther[1]));
}

static void TestFrame()
{
	// Stage C uses Elden Ring's finite reversed Z: depth(near) = 1, depth(far) = 0.
	RenderSnapshot s;
	s.camera.right = { 1, 0, 0 };
	s.camera.up = { 0, 1, 0 };
	s.camera.fwd = { 0, 0, 1 };
	s.camera.pos = { 0, 0, 0 };
	s.camera.fovY = 1.0f;
	s.camera.nearZ = 0.1f;
	s.camera.farZ = 10000.0f;
	s.camera.valid = true;
	render::FrameConstants fc{};
	render::FillFrame(fc, s, 3840, 2160, true);
	auto depth = [&](float z) { return fc.clip[0] + fc.clip[1] / z; };
	CHECK(Near(depth(0.1f), 1.0f));
	CHECK(Near(depth(10000.0f), 0.0f));
	CHECK(Near(fc.camUp[3], 1.0f / std::tan(0.5f)));
	CHECK(Near(fc.camRight[3], fc.camUp[3] * 2160.0f / 3840.0f));
	// No previous frame: the previous camera is the current one (zero velocity).
	CHECK(Near(fc.prevCamUp[3], fc.camUp[3]));
	// The forward stages keep the infinite far plane (depth = near / z).
	render::FrameConstants ff{};
	render::FillFrame(ff, s, 3840, 2160, false);
	CHECK(ff.clip[0] == 0.0f && Near(ff.clip[1], 0.1f));
	// Light-set index and gloss mapping as the shaders encode them.
	// gbuffer_light_set = 0 (Elden Ring's world set: lit whenever its world is on screen; its character sets 3/4 are lit
	// only while one of its characters is, VERIFIED live), carried as 0/255.
	CHECK(Cfg().render.gbufferLightSet == 0);
	CHECK(Near(fc.gbuf[0] * 255.0f, 0.0f));
}

static void TestWinding()
{
	// A box face at z = 1 with its normal +z: (0,0,1) (1,0,1) (0,1,1): cross(b - a, c - a) = +z (outward).
	struct V
	{
		float p[3];
		std::uint16_t n[4];  // half floats
	};
	auto half = [](float a_f) -> std::uint16_t { return a_f == 1.0f ? 0x3C00 : a_f == -1.0f ? 0xBC00 : 0; };
	V v[4] = {};
	const float pos[4][3] = { { 0, 0, 1 }, { 1, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 } };
	for (int i = 0; i < 4; ++i) {
		std::memcpy(v[i].p, pos[i], 12);
		v[i].n[2] = half(1.0f);
	}
	const std::vector<proto::RenInputElement> el = {
		Element("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0),
		Element("NORMAL", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 12),
	};
	const std::uint8_t* streams[1] = { reinterpret_cast<const std::uint8_t*>(v) };
	const std::uint32_t bytes[1] = { sizeof(v) };
	const std::uint32_t strides[1] = { sizeof(V) };
	const std::uint16_t outward[12] = { 0, 1, 2, 1, 3, 2, 0, 1, 2, 1, 3, 2 };
	const std::uint16_t inward[12] = { 0, 2, 1, 1, 2, 3, 0, 2, 1, 1, 2, 3 };
	CHECK(RenderMesh::Winding(el, streams, bytes, strides, 1, reinterpret_cast<const std::uint8_t*>(outward), sizeof(outward), false, false) == 1);
	CHECK(RenderMesh::Winding(el, streams, bytes, strides, 1, reinterpret_cast<const std::uint8_t*>(inward), sizeof(inward), false, false) == -1);
	// No normals: unknown.
	const std::vector<proto::RenInputElement> noNormal = { Element("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0) };
	CHECK(RenderMesh::Winding(noNormal, streams, bytes, strides, 1, reinterpret_cast<const std::uint8_t*>(outward), sizeof(outward), false, false) == 0);
	float f[4];
	const std::uint8_t unorm[4] = { 255, 0, 128, 255 };
	CHECK(RenderMesh::Decode(DXGI_FORMAT_R8G8B8A8_UNORM, unorm, f) && Near(f[0], 1.0f) && Near(f[1], -1.0f) && std::fabs(f[2]) < 0.01f);
	// Bind-space bounds (the occluder test carries their centre with each matrix).
	float lo[3]{}, hi[3]{};
	CHECK(RenderMesh::Bounds(el, streams, bytes, strides, 1, lo, hi));
	CHECK(Near(lo[0], 0) && Near(lo[1], 0) && Near(lo[2], 1) && Near(hi[0], 1) && Near(hi[1], 1) && Near(hi[2], 1));
	const std::vector<proto::RenInputElement> noPosition = { Element("NORMAL", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 12) };
	CHECK(!RenderMesh::Bounds(noPosition, streams, bytes, strides, 1, lo, hi));
	// The front-face rule: [Render] front_ccw is for outward meshes; inward ones (MGSV's) get the opposite.
	CHECK(render::FrontCcw(true, 1) != render::FrontCcw(true, -1));
	CHECK(render::FrontCcw(true, 0) == render::FrontCcw(true, 1));
}

// d3d::WaitFence with a fake fence: a stale signal on the auto-reset event (left by an earlier timed-out
// wait) must not count as "the GPU is done".
struct FakeFence
{
	std::uint64_t value{ 0 };
	int           registrations{ 0 };
	int           completeAt{ -1 };  // this registration completes the fence
	bool          fail{ false };
	std::uint64_t GetCompletedValue() { return value; }
	HRESULT       SetEventOnCompletion(std::uint64_t a_value, HANDLE a_event)
	{
		if (fail) {
			return E_FAIL;
		}
		if (++registrations == completeAt) {
			value = a_value;
			::SetEvent(a_event);
		}
		return S_OK;
	}
};

static void TestWaitFence()
{
	HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
	CHECK(event != nullptr);
	{
		FakeFence f;
		::SetEvent(event);  // the stale signal
		const auto t0 = ::GetTickCount64();
		CHECK(!phantom::er::d3d::WaitFence(&f, event, 1, 100));
		CHECK(::GetTickCount64() - t0 >= 90);
		CHECK(f.registrations >= 2);  // woke on the stale signal, re-checked, re-registered
	}
	{
		FakeFence f;
		f.completeAt = 1;
		CHECK(phantom::er::d3d::WaitFence(&f, event, 5, 100) && f.value == 5);
	}
	{
		FakeFence f;
		f.value = 7;  // already done: no registration at all
		CHECK(phantom::er::d3d::WaitFence(&f, event, 7, 100) && f.registrations == 0);
	}
	{
		FakeFence f;
		f.fail = true;
		CHECK(!phantom::er::d3d::WaitFence(&f, event, 1, 100));
	}
	::CloseHandle(event);
}

// A watched target that changes stays alive until a submission after the change has completed.
static void TestRetire()
{
	Retire<std::shared_ptr<int>> r;
	auto                         gbuffer = std::make_shared<int>(1);
	std::weak_ptr<int>           alive = gbuffer;
	r.Push(std::move(gbuffer), 8);  // the last submission was 7 when ER recreated it: 8 may still use it
	CHECK(r.Collect(7) == 0 && !alive.expired());
	CHECK(r.Collect(8) == 1 && alive.expired());
	r.Push(std::make_shared<int>(2), 9);
	r.Push(std::make_shared<int>(3), 12);
	CHECK(r.Collect(10) == 1 && r.Size() == 1);
	r.Clear();
	CHECK(r.Size() == 0);
}

// Snake only goes into the sun-cascade atlas (rendered every frame), not into atlases ER may reuse.
static void TestSunCascadeAtlas()
{
	const D3D12_VIEWPORT cascades[] = { { 0, 0, 2048, 2048, 0, 1 }, { 2048, 0, 1024, 1024, 0, 1 }, { 3072, 0, 1024, 1024, 0, 1 } };
	CHECK(RenderShadow::SunCascadeAtlas(4096, 2048, cascades, 3));
	CHECK(RenderShadow::SunCascadeAtlas(4096, 2048, cascades + 0, 1));        // only the first cascade this frame
	CHECK(!RenderShadow::SunCascadeAtlas(4096, 2048, cascades + 1, 2));       // the near ones alone: not proof
	const D3D12_VIEWPORT low[] = { { 0, 0, 1024, 1024, 0, 1 } };
	CHECK(RenderShadow::SunCascadeAtlas(2048, 1024, low, 1));                 // a lower shadow quality, same layout
	const D3D12_VIEWPORT page[] = { { 512, 0, 512, 512, 0, 1 } };
	CHECK(!RenderShadow::SunCascadeAtlas(2048, 2048, page, 1));               // another R16 atlas: a light's page
	CHECK(!RenderShadow::SunCascadeAtlas(4096, 2048, nullptr, 0));
	CHECK(!RenderShadow::SunCascadeAtlas(0, 0, cascades, 3));
}

// [Render] debug_layer (er/DebugLayer.h): which validation messages reach the log; inert while off (the default).
static void TestDebugLayer()
{
	using namespace phantom::er;
	CHECK(!RenderConfig{}.debugLayer && RenderConfig{}.gpuValidation);
	CHECK(!std::strcmp(DebugLayer::SeverityName(D3D12_MESSAGE_SEVERITY_ERROR), "ERROR"));
	CHECK(!std::strcmp(DebugLayer::SeverityName(D3D12_MESSAGE_SEVERITY_CORRUPTION), "CORRUPTION"));
	CHECK(!std::strcmp(DebugLayer::SeverityName(99), "?"));

	DebugLayer::MessageFilter f(3, 5);
	bool first = true;
	CHECK(!f.Take(D3D12_MESSAGE_SEVERITY_INFO, 7, &first) && !first);  // info: counted, never logged
	CHECK(f.Take(D3D12_MESSAGE_SEVERITY_ERROR, 100, &first) && !first);
	CHECK(f.Take(D3D12_MESSAGE_SEVERITY_ERROR, 100));
	CHECK(f.Take(D3D12_MESSAGE_SEVERITY_ERROR, 100));
	CHECK(!f.Take(D3D12_MESSAGE_SEVERITY_ERROR, 100, &first) && first);  // the 4th of an id: dropped, said once
	CHECK(!f.Take(D3D12_MESSAGE_SEVERITY_ERROR, 100, &first) && !first);
	CHECK(f.Take(D3D12_MESSAGE_SEVERITY_WARNING, 200));
	CHECK(f.Take(D3D12_MESSAGE_SEVERITY_CORRUPTION, 300));
	CHECK(!f.Take(D3D12_MESSAGE_SEVERITY_ERROR, 400, &first) && !first);  // total cap (5) reached: dropped silently
	CHECK(f.Count(D3D12_MESSAGE_SEVERITY_ERROR) == 6 && f.Count(D3D12_MESSAGE_SEVERITY_INFO) == 1 && f.Logged() == 5 && f.Dropped() == 3);
	CHECK(f.Summary().starts_with("1 corruption, 6 errors, 1 warnings, 1 info (4 ids; 5 logged, 3 not logged)"));

	// Off: no device kept, nothing polled.
	CHECK(!DebugLayer::Enabled());
	DebugLayer::Attach(nullptr);
	DebugLayer::Poll();
	CHECK(DebugLayer::Describe() == "debug layer: off");
}

#include "test_host_materials.inc"

int main()
{
	phantom::Log::Init(L"test_render.log", "test");
	TestShaders();
	TestPso();
	TestFades();
	TestFrame();
	TestWinding();
	TestWaitFence();
	TestRetire();
	TestSunCascadeAtlas();
	TestDebugLayer();
	TestMaterialRoles();
	std::printf("test_render: %s (%d failures)\n", g_failures ? "FAILED" : "ok", g_failures);
	return g_failures ? 1 : 0;
}
