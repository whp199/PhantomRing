#include "Shaders.h"

#include "Log.h"

#include <d3dcompiler.h>

#include <cstring>
#include <format>
#include <mutex>

namespace phantom::er::Shaders
{
	namespace
	{
		using d3d::ComPtr;
		using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

		CompileFn             g_compile{ nullptr };
		std::mutex            g_lock;
		ComPtr<ID3DBlob>      g_ps[4];
		bool                  g_psFailed[4]{};

		// Shared by every stage. FrameCB / DrawCB must match er/RenderShared.h.
		constexpr char kCommon[] = R"(
cbuffer FrameCB : register(b0)
{
	float4 gAnchor[3];    // anchor-relative MGSV world -> ER world, 3x4 rows
	float4 gCamRight;     // xyz; w = projection x scale (signed: screen flip)
	float4 gCamUp;        // xyz; w = projection y scale
	float4 gCamFwd;       // xyz; w = near plane
	float4 gCamPos;       // xyz
	float4 gLightDir;     // xyz to the light (ER world); w = intensity
	float4 gLightColor;   // rgb; w = exposure
	float4 gAmbientUp;    // rgb sky ambient
	float4 gAmbientDown;  // rgb ground ambient
	float4 gOutput;       // x encoding (0 sRGB, 1 none, 2 PQ/HDR10, 3 scRGB), y paper white nits, z debug view, w dissolve (0 solid .. 1 gone)
	float4 gDepth;        // Elden Ring scene depth test: x on, y its near plane, zw its size / back-buffer size
	float4 gDepth2;       // x its far plane, y relative bias, z 1 = standard Z (else reversed)
	float4 gClip;         // xy: clip z = x * viewZ + y (Elden Ring's reversed Z); zw: NDC jitter
	float4 gPrevAnchor[3];  // last Elden Ring frame's anchor (velocity)
	float4 gPrevCamRight;   // last frame's camera, like gCam*
	float4 gPrevCamUp;
	float4 gPrevCamFwd;
	float4 gPrevCamPos;
	float4 gGbuf;         // stage C: x light-set index / 255, y RT1.w, z RT1.z (translucency), w albedo scale
	float4 gMat;          // x gloss scale, y gloss bias, z metal scale, w alpha-test reference
	float4 gFade;         // xy unused; zw velocity scale (NDC delta -> ER units)
	float4 gMisc;         // xy velocity bias, z frame index, w 1 = project with Elden Ring's view constants (b4)
};
cbuffer DrawCB : register(b1)
{
	uint gFlags;
	uint gTexMask;
	uint gMatrixCount;
	uint gOwner;
	float4 gParams0;
	float4 gParams1;
	float gDrawFade;      // near-camera fade of this draw's owner (1 solid .. 0 gone)
	float2 gDrawNear;     // per-pixel near-camera fade: gone at x metres from the camera, solid from y
	float gDrawPad;
};
cbuffer Bones : register(b2)
{
	float4 gBones[768];
};
cbuffer PrevBones : register(b3)
{
	float4 gPrevBones[768];
};
// Elden Ring's own view constants (er/RenderView.h), when gMisc.w is set: [3] origin, [24..27] camera-
// relative view-projection rows, [28] previous origin, [29..32] previous rows.
cbuffer ErView : register(b4)
{
	float4 gEr[64];
};

struct VSOut
{
	float4 pos : SV_Position;
	float3 wpos : TEXCOORD0;
	float3 nrm : TEXCOORD1;
	float4 tan : TEXCOORD2;
	float2 uv : TEXCOORD3;
	float4 dbg : TEXCOORD4;
	float4 cur : TEXCOORD5;   // this frame's clip position without jitter (velocity)
	float4 prev : TEXCOORD6;  // last frame's clip position of the same point
	float2 uv1 : TEXCOORD7;   // the second UV set (TEXCOORD1; the first when the mesh has none): MGSV's box poster
};
)";


		// er/RenderMarkers.h: camera-facing dots and balls, rings on a surface. Vertex = RenderMarkers::Vertex.
		constexpr char kMarkerVs[] = R"(
struct MarkerIn
{
	float3 pos : POSITION;
	float2 corner : TEXCOORD0;
	float4 color : COLOR0;
	float4 shape : TEXCOORD1;
};
struct MarkerOut
{
	float4 pos : SV_Position;
	float2 corner : TEXCOORD0;
	float4 color : COLOR0;
	float4 shape : TEXCOORD1;
	float  viewZ : TEXCOORD2;
};
MarkerOut VSMarker(MarkerIn i)
{
	MarkerOut o;
	float3 d = i.pos - gCamPos.xyz;
	float4 c = float4(dot(d, gCamRight.xyz) * gCamRight.w, dot(d, gCamUp.xyz) * gCamUp.w, 0, dot(d, gCamFwd.xyz));
	o.pos = float4(c.x, c.y, gClip.x * c.w + gClip.y, c.w);
	o.corner = i.corner;
	o.color = i.color;
	o.shape = i.shape;
	o.viewZ = c.w;
	return o;
}
)";
		constexpr char kMarkerPs[] = R"(
struct MarkerOut
{
	float4 pos : SV_Position;
	float2 corner : TEXCOORD0;
	float4 color : COLOR0;
	float4 shape : TEXCOORD1;
	float  viewZ : TEXCOORD2;
};
float4 PSMarker(MarkerOut i) : SV_Target
{
	if (gDepth.x > 0.5) {
		float d = tSceneDepth.Load(int3(int2(i.pos.xy * gDepth.zw), 0)).r;
		float n = gDepth.y;
		float f = gDepth2.x;
		float erZ = gDepth2.z > 0.5 ? n * f / max(f - d * (f - n), 1e-6) : n * f / max(d * (f - n) + n, 1e-6);
		// Its slack: a landing ring or a mine lies in the grass and on the ground's bumps, which must not hide it (walls do).
		float slack = i.shape.w;
		if (i.viewZ > erZ * (1.0 + gDepth2.y) + slack) {
			discard;
		}
	}
	float r = length(i.corner);
	float3 rgb = i.color.rgb;
	float a = i.color.a;
	if (i.shape.x < 0.5) {
		// Dot: a bright core with a soft halo.
		if (r > 1.0) discard;
		float core = saturate((0.55 - r) / 0.2);
		float halo = saturate(1.0 - r) * 0.55;
		a *= max(core, halo);
		rgb = lerp(rgb, float3(1, 1, 1), core * 0.5);
	} else if (i.shape.x < 1.5) {
		// Ball: a sphere impostor lit from above the camera.
		if (r > 1.0) discard;
		float3 nrm = float3(i.corner.x, i.corner.y, sqrt(saturate(1.0 - r * r)));
		float3 l = normalize(float3(-0.3, 0.8, 0.5));
		float diff = saturate(dot(nrm, l));
		float spec = pow(saturate(dot(reflect(-l, nrm), float3(0, 0, 1))), 24.0) * 0.35;
		rgb = rgb * (0.3 + 0.7 * diff) + spec;
		a *= saturate((1.0 - r) * 6.0);
	} else {
		// Ring: between the inner share and the edge, soft on both sides.
		float inner = i.shape.y;
		float w = max(1.0 - inner, 0.05);
		float t = (r - inner) / w;
		if (r > 1.0 || t < 0.0) discard;
		a *= saturate(t * 4.0) * saturate((1.0 - r) * 12.0);
	}
	return Encode(rgb * a, a);
}
)";

		constexpr char kVertexTail[] = R"(
float3x4 Bone(uint i)
{
	i = min(i, gMatrixCount - 1);
	return float3x4(gBones[i * 3], gBones[i * 3 + 1], gBones[i * 3 + 2]);
}

float3x4 PrevBone(uint i)
{
	i = min(i, gMatrixCount - 1);
	return float3x4(gPrevBones[i * 3], gPrevBones[i * 3 + 1], gPrevBones[i * 3 + 2]);
}

// View-space position projected by a camera's axes; z = 0 (callers fill in the depth mapping), w = view z.
float4 Project(float3 w, float3 camPos, float4 r, float4 u, float4 f)
{
	float3 d = w - camPos;
	float3 v = float3(dot(d, r.xyz), dot(d, u.xyz), dot(d, f.xyz));
	return float4(v.x * r.w, v.y * u.w, 0, v.z);
}

VSOut Finish(float3 p, float3 n, float4 t, float2 uv, float2 uv1, uint4 bi, float4 bw, uint4 bi2, float4 bw2)
{
	float3x4 m;
	float3x4 mp;
	if ((gFlags & 2) != 0) {
		float s = bw.x + bw.y + bw.z + bw.w + bw2.x + bw2.y + bw2.z + bw2.w;
		if (s < 0.001) {
			bw = float4(1, 0, 0, 0);
			s = 1;
		}
		bw /= s;
		bw2 /= s;
		m = Bone(bi.x) * bw.x + Bone(bi.y) * bw.y + Bone(bi.z) * bw.z + Bone(bi.w) * bw.w;
		m += Bone(bi2.x) * bw2.x + Bone(bi2.y) * bw2.y + Bone(bi2.z) * bw2.z + Bone(bi2.w) * bw2.w;
		mp = PrevBone(bi.x) * bw.x + PrevBone(bi.y) * bw.y + PrevBone(bi.z) * bw.z + PrevBone(bi.w) * bw.w;
		mp += PrevBone(bi2.x) * bw2.x + PrevBone(bi2.y) * bw2.y + PrevBone(bi2.z) * bw2.z + PrevBone(bi2.w) * bw2.w;
	} else {
		m = Bone(0);
		mp = PrevBone(0);
	}
	float3x4 a = float3x4(gAnchor[0], gAnchor[1], gAnchor[2]);
	float3x4 ap = float3x4(gPrevAnchor[0], gPrevAnchor[1], gPrevAnchor[2]);
	float3 ms = mul(m, float4(p, 1));
	float3 w = mul(a, float4(ms, 1));
	float3 wp = mul(ap, float4(mul(mp, float4(p, 1)), 1));
	float3 nw = mul((float3x3)a, mul((float3x3)m, n));
	float3 tw = mul((float3x3)a, mul((float3x3)m, t.xyz));

	VSOut o;
	if (gMisc.w > 0.5) {
		// Exactly Elden Ring's matrices: its TAA jitter, depth mapping, previous frame (or a cascade's light view).
		float4 d = float4(w - gEr[3].xyz, 1);
		o.pos = float4(dot(gEr[24], d), dot(gEr[25], d), dot(gEr[26], d), dot(gEr[27], d));
		float4 dp = float4(wp - gEr[28].xyz, 1);
		o.cur = o.pos;
		o.prev = float4(dot(gEr[29], dp), dot(gEr[30], dp), dot(gEr[31], dp), dot(gEr[32], dp));
	} else {
		float4 c = Project(w, gCamPos.xyz, gCamRight, gCamUp, gCamFwd);
		// Elden Ring's reversed Z: depth = gClip.x + gClip.y / viewZ (forward stages: 0 and near, infinite far).
		o.pos = float4(c.x + gClip.z * c.w, c.y + gClip.w * c.w, gClip.x * c.w + gClip.y, c.w);
		o.cur = c;
		o.prev = Project(wp, gPrevCamPos.xyz, gPrevCamRight, gPrevCamUp, gPrevCamFwd);
	}
	o.wpos = w;
	o.nrm = nw;
	o.tan = float4(tw, t.w < 0 ? -1.0 : 1.0);
	o.uv = uv;
	o.uv1 = uv1;
	o.dbg = float4((float)bi.x, bw.x, 0, 0);
	return o;
}
)";

		constexpr char kPixel[] = R"(
Texture2D tAlbedo : register(t0);
Texture2D tNormal : register(t1);
Texture2D tSrm : register(t2);
Texture2D tExtra : register(t3);
Texture2D tExtra2 : register(t4);
Texture2D tSceneDepth : register(t6);
SamplerState sLinear : register(s0);

float3 SrgbToLinear(float3 c) { return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045)); }
float3 LinearToSrgb(float3 c)
{
	c = saturate(c);
	return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, step(c, 0.0031308));
}
float3 Pq(float3 nits)
{
	float3 y = pow(saturate(nits / 10000.0), 0.1593017578125);
	return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}
float3 Hue(float h)
{
	return saturate(abs(frac(h + float3(0, 2.0 / 3.0, 1.0 / 3.0)) * 6 - 3) - 1);
}

float4 Encode(float3 c, float a)
{
	float mode = gOutput.x;
	if (mode < 0.5) {
		return float4(LinearToSrgb(c), a);
	}
	if (mode < 1.5) {
		return float4(saturate(c), a);
	}
	if (mode < 2.5) {
		float3 r2020 = float3(dot(float3(0.6274, 0.3293, 0.0433), c), dot(float3(0.0691, 0.9195, 0.0114), c), dot(float3(0.0164, 0.0880, 0.8956), c));
		return float4(Pq(r2020 * gOutput.y), a);
	}
	return float4(c * gOutput.y / 80.0, a);
}

// 4x4 Bayer threshold (0.5/16 .. 15.5/16) from the pixel position: a static pattern, so Elden Ring's TAA
// resolves the screen door into a soft fade instead of shimmering.
float Bayer4(float2 p)
{
	uint2 q = uint2(p) & 3;
	uint xy = q.x ^ q.y;
	uint v = ((xy & 1) << 3) | ((q.y & 1) << 2) | (xy & 2) | ((q.y & 2) >> 1);
	return ((float)v + 0.5) / 16.0;
}

// Screen-door fades that keep depth and self-occlusion right (alpha blending a whole character would
// not): the death dissolve (interleaved gradient noise) and MGSV's near-camera fade, per owner
// (gDrawFade, e.g. D-Walker right behind the camera) and per pixel by distance to the camera.
void ScreenDoor(VSOut i)
{
	if (gOutput.w > 0.0) {
		float ign = frac(52.9829189 * frac(dot(floor(i.pos.xy), float2(0.06711056, 0.00583715))));
		if (ign < gOutput.w) {
			discard;
		}
	}
	float fade = gDrawFade;
	if (gDrawNear.y > gDrawNear.x) {
		fade = min(fade, saturate((length(i.wpos - gCamPos.xyz) - gDrawNear.x) / (gDrawNear.y - gDrawNear.x)));
	}
	if (fade < 1.0 && fade <= Bayer4(i.pos.xy)) {
		discard;
	}
}

struct Surface
{
	float3 albedo;  // linear
	float  alpha;
	float3 n;       // ER world, facing the viewer's side
	float  spec, rough, metal;
};

Surface Shade(VSOut i, bool front)
{
	Surface s;
	float4 albedo = float4(0.55, 0.55, 0.55, 1);
	if ((gTexMask & 1) != 0) {
		albedo = tAlbedo.Sample(sLinear, i.uv);
	}
	if ((gFlags & 1) != 0) {
		clip(albedo.a - gMat.w);
	}
	if ((gFlags & 4) != 0) {
		albedo.rgb = SrgbToLinear(albedo.rgb);
	}
	if ((gFlags & 32) != 0 && (gTexMask & 24) == 24) {
		// MGSV's camo (D-Walker's panels): the white base painted with the camo pattern where the mask is set.
		float3 camo = tExtra.Sample(sLinear, i.uv).rgb;
		if ((gFlags & 4) != 0) {
			camo = SrgbToLinear(camo);
		}
		albedo.rgb = lerp(albedo.rgb, albedo.rgb * camo, tExtra2.Sample(sLinear, i.uv).r);
	}
	if ((gFlags & 64) != 0 && (gTexMask & 16) != 0) {
		// MGSV's box poster (the anime girl): its picture (t4) lies on the box's second UV set, on a plain light-grey
		// background (243, 242, 243) everywhere else; it replaces the cardboard where the picture is.
		float3 poster = tExtra2.Sample(sLinear, i.uv1).rgb;
		float3 d = abs(poster - float3(0.953, 0.949, 0.953));
		float  cover = saturate((max(d.r, max(d.g, d.b)) - 0.012) * 25.0);
		if ((gFlags & 4) != 0) {
			poster = SrgbToLinear(poster);
		}
		albedo.rgb = lerp(albedo.rgb, poster, cover);
	}
	s.albedo = albedo.rgb;
	s.alpha = albedo.a;

	float3 n = normalize(i.nrm);
	if (!front) {
		n = -n;
	}
	if ((gTexMask & 2) != 0 && dot(i.tan.xyz, i.tan.xyz) > 1e-8) {
		float4 t = tNormal.Sample(sLinear, i.uv);
		float2 xy = ((gFlags & 8) != 0 ? t.ag : t.rg) * 2 - 1;
		float z = sqrt(saturate(1 - dot(xy, xy)));
		float3 T = normalize(i.tan.xyz - n * dot(n, i.tan.xyz));
		float3 B = cross(n, T) * i.tan.w;
		n = normalize(T * xy.x + B * xy.y + n * z);
	}
	s.n = n;

	// Fox "SRM" map: R specular, G roughness, B metalness (the recorded textures: fabric G 0.75-0.9 B 0,
	// polished metal parts G 0.2 B 0.8).
	s.spec = 0.25;
	s.rough = 0.65;
	s.metal = 0;
	if ((gTexMask & 4) != 0) {
		float4 m = tSrm.Sample(sLinear, i.uv);
		s.spec = m.r;
		s.rough = m.g;
		s.metal = m.b;
	}
	return s;
}

// Stages A/B: lit here (one directional light + hemisphere ambient), encoded for the target.
float4 PSMain(VSOut i, bool front : SV_IsFrontFace) : SV_Target
{
	ScreenDoor(i);
	if (gDepth.x > 0.5) {
		float d = tSceneDepth.Load(int3(int2(i.pos.xy * gDepth.zw), 0)).r;
		float n = gDepth.y;
		float f = gDepth2.x;
		float erZ = gDepth2.z > 0.5 ? n * f / max(f - d * (f - n), 1e-6) : n * f / max(d * (f - n) + n, 1e-6);
		float myZ = i.cur.w;  // view z (perspective-correct)
		if (myZ > erZ * (1.0 + gDepth2.y)) {
			discard;
		}
	}
	Surface s = Shade(i, front);
	float3 n = s.n;

	float view = gOutput.z;
	if (view > 0.5) {
		float3 dc = s.albedo;
		if (view > 1.5 && view < 2.5) dc = n * 0.5 + 0.5;
		if (view > 2.5 && view < 3.5) dc = float3(frac(i.uv), 0);
		if (view > 3.5 && view < 4.5) dc = front ? float3(0, 1, 0) : float3(1, 0, 0);
		if (view > 4.5 && view < 5.5) dc = Hue(i.dbg.x * 0.137);
		if (view > 5.5 && view < 6.5) dc = float3(s.spec, s.rough, s.metal);
		return Encode(dc, 1);
	}

	float3 L = normalize(gLightDir.xyz);
	float3 V = normalize(gCamPos.xyz - i.wpos);
	float3 H = normalize(L + V);
	float ndl = saturate(dot(n, L));
	float hemi = n.y * 0.5 + 0.5;
	float3 ambient = lerp(gAmbientDown.rgb, gAmbientUp.rgb, hemi);
	float power = exp2(11.0 * (1.0 - s.rough) + 1.0);
	float3 f0 = lerp(float3(0.04, 0.04, 0.04) * (s.spec * 2.0), s.albedo, s.metal);
	float fres = pow(1.0 - saturate(dot(n, V)), 5.0);
	float3 specCol = f0 + (1.0 - f0) * fres * (1.0 - s.rough);
	float3 diffuse = s.albedo * (1.0 - s.metal);
	float3 light = gLightColor.rgb * gLightDir.w;
	float3 c = diffuse * (ndl * light + ambient) + specCol * pow(saturate(dot(n, H)), power) * (power + 8.0) / 25.0 * ndl * light + specCol * ambient * 0.25;
	c *= gLightColor.w;
	float alpha = (gFlags & 16) != 0 ? s.alpha : 1.0;
	return Encode(c, alpha);
}

// Stage C: Elden Ring's G-buffer encoding (its G-buffer pixel shaders and the lighting passes that read
// them, docs/HOST.md "Stage C"): RT0 world normal * 0.5 + 0.5, RT1 metalness / gloss / translucency /
// vertex AO, RT2 albedo (sRGB target: linear values) / light-set index, RT3 and RT4 zero for opaque
// materials (their alpha is a fade value; blending is off).
struct GbufOut
{
	float4 n : SV_Target0;
	float4 m : SV_Target1;
	float4 a : SV_Target2;
	float2 x : SV_Target3;
	float4 e : SV_Target4;
};

GbufOut PSGbuffer(VSOut i, bool front : SV_IsFrontFace)
{
	ScreenDoor(i);
	Surface s = Shade(i, front);
	// ER's lighting takes gloss (1 = mirror): perceptual roughness = 1 - sqrt(gloss).
	float gloss = saturate(1.0 - s.rough);
	gloss = saturate(gloss * gloss * gMat.x + gMat.y);
	GbufOut o;
	o.n = float4(s.n * 0.5 + 0.500488758, 0);
	o.m = float4(saturate(s.metal * gMat.z), gloss, gGbuf.z, gGbuf.y);
	o.a = float4(saturate(s.albedo * gGbuf.w), gGbuf.x);
	o.x = float2(0, 0);
	o.e = float4(0, 0, 0, 0);
	float view = gOutput.z;
	if (view > 0.5) {
		// Debug views through ER's lighting: 1 flat grey albedo, 2 no normal map, 6 SRM as albedo.
		if (view < 1.5) o.a.rgb = float3(0.5, 0.5, 0.5);
		if (view > 1.5 && view < 2.5) o.n.rgb = normalize(i.nrm) * (front ? 0.5 : -0.5) + 0.500488758;
		if (view > 5.5 && view < 6.5) o.a.rgb = float3(s.spec, s.rough, s.metal);
	}
	return o;
}

// Stage C shadows: only alpha-tested draws need a pixel shader in Elden Ring's shadow atlas.
void PSShadow(VSOut i)
{
	if ((gTexMask & 1) != 0) {
		clip(tAlbedo.Sample(sLinear, i.uv).a - gMat.w);
	}
}

// Stage C velocity: Elden Ring's object-velocity pass writes (NDC now - NDC last frame) * scale + bias
// into its R16G16_FLOAT target; ours does the same for Snake's pixels (depth-tested against his own
// depth in the scene depth), so TAA does not smear him.
float2 PSVelocity(VSOut i, bool front : SV_IsFrontFace) : SV_Target
{
	ScreenDoor(i);
	if ((gFlags & 1) != 0 && (gTexMask & 1) != 0) {
		clip(tAlbedo.Sample(sLinear, i.uv).a - gMat.w);
	}
	float2 now = i.cur.xy / i.cur.w;
	float2 then = i.prev.xy / max(i.prev.w, 1e-4);
	return (now - then) * gFade.zw + gMisc.xy;
}
)";

		struct Use
		{
			const char* canonical;
			std::uint32_t index;
		};

		ComPtr<ID3DBlob> CompileText(std::string_view a_src, const char* a_entry, const char* a_target)
		{
			ComPtr<ID3DBlob> code, errors;
			const HRESULT    hr = g_compile(a_src.data(), a_src.size(), "PhantomRing.hlsl", nullptr, nullptr, a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
				   code.GetAddressOf(), errors.GetAddressOf());
			if (FAILED(hr) || !code) {
				const std::string text = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : std::string("(no message)");
				Log::Error("render: shader {} ({}) failed: {} {}", a_entry, a_target, d3d::Hr(hr), text);
				return {};
			}
			if (errors && errors->GetBufferSize() > 1) {
				Log::Debug("render: shader {} warnings: {}", a_entry, std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
			}
			return code;
		}

		std::string Semantic(const proto::RenInputElement& a_e)
		{
			char name[17]{};
			std::memcpy(name, a_e.semantic, 16);
			return name;
		}
	}

	bool Init()
	{
		if (g_compile) {
			return true;
		}
		for (const wchar_t* dll : { L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll" }) {
			if (HMODULE m = ::LoadLibraryW(dll)) {
				g_compile = reinterpret_cast<CompileFn>(reinterpret_cast<void*>(::GetProcAddress(m, "D3DCompile")));
				if (g_compile) {
					Log::Info("render: shader compiler {}", Narrow(dll));
					return true;
				}
			}
		}
		Log::Error("render: no d3dcompiler with D3DCompile; renderer off");
		return false;
	}

	std::string_view Canonical(const char* a_semantic)
	{
		for (const char* name : { "POSITION", "NORMAL", "TANGENT", "BINORMAL", "COLOR", "TEXCOORD", "BLENDWEIGHT", "BLENDINDICES" }) {
			if (std::strncmp(a_semantic, name, 16) == 0) {
				return name;
			}
		}
		return {};
	}

	std::string VertexSource(std::span<const proto::RenInputElement> a_elements)
	{
		std::string decl, body;
		bool        have[8]{};  // pos nrm tan bin uv uv1; bw bi bw2/bi2 handled separately
		bool        haveBi = false;
		for (std::size_t k = 0; k < a_elements.size(); ++k) {
			const auto& e = a_elements[k];
			const auto  name = Semantic(e);
			const auto  canon = Canonical(name.c_str());
			if (canon.empty()) {
				continue;
			}
			const auto info = d3d::Info(static_cast<DXGI_FORMAT>(e.format));
			if (info.kind == d3d::Kind::kUnknown) {
				continue;
			}
			const char* type = info.kind == d3d::Kind::kUint ? "uint4" : info.kind == d3d::Kind::kSint ? "int4" : "float4";
			const std::string var = std::format("a{}", k);
			const bool        isFloat = info.kind == d3d::Kind::kFloat;
			const std::uint32_t idx = e.semanticIndex;
			auto add = [&](const std::string& a_code) {
				decl += std::format("\t{} {} : {}{};\n", type, var, canon, idx);
				body += a_code;
			};
			if (canon == "POSITION" && idx == 0 && isFloat && !have[0]) {
				have[0] = true;
				add(std::format("\tp = v.{}.xyz;\n", var));
			} else if (canon == "NORMAL" && idx == 0 && isFloat && !have[1]) {
				have[1] = true;
				add(std::format("\tn = v.{}.xyz{};\n", var, info.unorm ? " * 2 - 1" : ""));
			} else if (canon == "TANGENT" && idx == 0 && isFloat && !have[2]) {
				have[2] = true;
				add(std::format("\tt = v.{}{};\n\tif ({} < 4) t.w = 1;\n", var, info.unorm ? " * 2 - 1" : "", static_cast<int>(info.comps)));
			} else if (canon == "BINORMAL" && idx == 0 && isFloat && !have[3]) {
				have[3] = true;
				add(std::format("\tb = v.{}.xyz{};\n\thaveB = true;\n", var, info.unorm ? " * 2 - 1" : ""));
			} else if (canon == "TEXCOORD" && idx == 0 && isFloat && !have[4]) {
				have[4] = true;
				add(std::format("\tuv = v.{}.xy;\n", var));
			} else if (canon == "TEXCOORD" && idx == 1 && isFloat && !have[5]) {
				have[5] = true;
				add(std::format("\tuv1 = v.{}.xy;\n\thaveUv1 = true;\n", var));
			} else if (canon == "BLENDWEIGHT" && idx <= 1) {
				const char* dst = idx == 0 ? "bw" : "bw2";
				add(isFloat ? std::format("\t{} = v.{};\n", dst, var) : std::format("\t{} = (float4)v.{} / 255.0;\n", dst, var));
				if (info.comps < 4) {
					body += std::format("\t{}.{} = 0;\n", dst, info.comps == 1 ? "yzw" : info.comps == 2 ? "zw" : "w");
				}
			} else if (canon == "BLENDINDICES" && idx <= 1) {
				const char* dst = idx == 0 ? "bi" : "bi2";
				haveBi = true;
				add(isFloat ? std::format("\t{} = (uint4)round(v.{} * {:.1f});\n", dst, var, info.unormMax > 0 ? info.unormMax : 1.0f) : std::format("\t{} = (uint4)v.{};\n", dst, var));
			}
		}
		std::string src = kCommon;
		src += "struct VSIn\n{\n" + decl + (decl.empty() ? "\tuint vid : SV_VertexID;\n" : "") + "};\n";
		src += kVertexTail;
		src += "VSOut VSMain(VSIn v)\n{\n\tfloat3 p = 0; float3 n = float3(0, 1, 0); float4 t = 0; float3 b = 0; bool haveB = false; float2 uv = 0;\n"
			   "\tfloat2 uv1 = 0; bool haveUv1 = false; uint4 bi = 0; uint4 bi2 = 0; float4 bw = float4(1, 0, 0, 0); float4 bw2 = 0;\n";
		src += body;
		src += "\tif (haveB) t.w = dot(cross(n, t.xyz), b) < 0 ? -1 : 1;\n";
		if (!haveBi) {
			src += "\tbw = float4(1, 0, 0, 0); bw2 = 0;\n";
		}
		src += "\tif (!haveUv1) uv1 = uv;\n\treturn Finish(p, n, t, uv, uv1, bi, bw, bi2, bw2);\n}\n";
		return src;
	}

	ComPtr<ID3DBlob> Compile(std::string_view a_source, const char* a_entry, const char* a_target)
	{
		return g_compile ? CompileText(a_source, a_entry, a_target) : ComPtr<ID3DBlob>{};
	}

	ComPtr<ID3DBlob> PixelShader(Pixel a_kind)
	{
		static const char* const kEntries[] = { "PSMain", "PSGbuffer", "PSVelocity", "PSShadow" };
		const auto               k = static_cast<int>(a_kind);
		std::scoped_lock         lock(g_lock);
		if (!g_ps[k] && !g_psFailed[k] && g_compile) {
			g_ps[k] = CompileText(std::string(kCommon) + kPixel, kEntries[k], "ps_5_0");
			g_psFailed[k] = !g_ps[k];
		}
		return g_ps[k];
	}

	ComPtr<ID3DBlob> MarkerShader(bool a_pixel)
	{
		if (!g_compile) {
			return {};
		}
		return a_pixel ? CompileText(std::string(kCommon) + kPixel + kMarkerPs, "PSMarker", "ps_5_0")
		               : CompileText(std::string(kCommon) + kMarkerVs, "VSMarker", "vs_5_0");
	}

	ComPtr<ID3DBlob> VertexShader(std::span<const proto::RenInputElement> a_elements)
	{
		if (!g_compile) {
			return {};
		}
		const auto src = VertexSource(a_elements);
		auto       blob = CompileText(src, "VSMain", "vs_5_0");
		if (!blob) {
			Log::Error("render: vertex shader source was:\n{}", src.substr(std::strlen(kCommon)));
		}
		return blob;
	}
}
