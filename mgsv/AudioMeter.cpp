#include "AudioMeter.h"

#include "Config.h"
#include "Hook.h"
#include "Log.h"

#include <windows.h>
#include <objbase.h>
#include <mmsystem.h>
#include <mmreg.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <sstream>
#include <vector>

namespace phantom::guest::audio_meter
{
	namespace
	{
		// XAudio2 2.7 (the DirectX SDK June 2010 headers): CLSID_XAudio2 / CLSID_XAudio2_Debug, IXAudio2's vtable (IUnknown 0-2,
		// GetDeviceCount 3, GetDeviceDetails 4, Initialize 5, RegisterForCallbacks 6, UnregisterForCallbacks 7, CreateSourceVoice 8),
		// IXAudio2SourceVoice's (IXAudio2Voice 0-18, Start 19, Stop 20, SubmitSourceBuffer 21).
		constexpr GUID kClsidXAudio2{ 0x5a508685, 0xa254, 0x4fba, { 0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf } };
		constexpr GUID kClsidXAudio2Debug{ 0xdb05ea35, 0x0329, 0x4d4b, { 0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52 } };
		constexpr std::size_t kCreateSourceVoiceSlot = 8, kSubmitSlot = 21;

		struct Buffer  // XAUDIO2_BUFFER
		{
			UINT32      flags;
			UINT32      audioBytes;
			const BYTE* data;
			UINT32      playBegin, playLength, loopBegin, loopLength, loopCount;
			void*       context;
		};

		using CoCreateFn = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
		using CreateSourceVoiceFn = HRESULT(STDMETHODCALLTYPE*)(void*, void**, const WAVEFORMATEX*, UINT32, float, void*, const void*, const void*);
		using SubmitFn = HRESULT(STDMETHODCALLTYPE*)(void*, const Buffer*, const void*);
		CoCreateFn          g_coCreate{ nullptr };
		CreateSourceVoiceFn g_createSourceVoice{ nullptr };
		SubmitFn            g_submit{ nullptr };
		std::atomic<bool>   g_engineHooked{ false }, g_voiceHooked{ false };

		constexpr std::size_t kWindows = 512;
		struct Voice
		{
			void*                         voice{ nullptr };
			bool                          isFloat{ false };
			int                           bits{ 16 };
			int                           channels{ 2 };
			int                           rate{ 48000 };
			double                        sum{ 0 };
			std::uint64_t                 samples{ 0 };  // in the current window (all channels)
			std::uint64_t                 total{ 0 };    // windows finished
			std::array<float, kWindows>   dbfs{};
			std::array<float, kWindows>   peak{};
			float                         curPeak{ 0 };
		};
		std::mutex         g_lock;
		std::vector<Voice> g_voices;
		std::uint64_t      g_mark{ 0 };  // the busiest voice's window count at `mark`
		void*              g_markVoice{ nullptr };

		Voice* Find(void* a_voice)
		{
			for (auto& v : g_voices) {
				if (v.voice == a_voice) {
					return &v;
				}
			}
			return nullptr;
		}

		HRESULT STDMETHODCALLTYPE SubmitDetour(void* a_voice, const Buffer* a_buffer, const void* a_wma)
		{
			if (a_buffer && a_buffer->data && a_buffer->audioBytes) {
				std::scoped_lock lock(g_lock);
				if (auto* v = Find(a_voice); v && (v->isFloat ? v->bits == 32 : v->bits == 16)) {
					const std::size_t n = a_buffer->audioBytes / (v->bits / 8);
					const std::size_t window = static_cast<std::size_t>(v->rate) * static_cast<std::size_t>(std::max(1, v->channels)) / 2;
					for (std::size_t i = 0; i < n; ++i) {
						float s;
						if (v->isFloat) {
							std::memcpy(&s, a_buffer->data + i * 4, 4);
						} else {
							std::int16_t q;
							std::memcpy(&q, a_buffer->data + i * 2, 2);
							s = static_cast<float>(q) / 32768.0f;
						}
						if (!std::isfinite(s)) {
							continue;
						}
						v->sum += static_cast<double>(s) * s;
						v->curPeak = std::max(v->curPeak, std::fabs(s));
						if (++v->samples >= window) {
							const double rms = std::sqrt(v->sum / static_cast<double>(v->samples));
							v->dbfs[v->total % kWindows] = static_cast<float>(20.0 * std::log10(std::max(rms, 1e-9)));
							v->peak[v->total % kWindows] = static_cast<float>(20.0 * std::log10(std::max<double>(v->curPeak, 1e-9)));
							++v->total;
							v->sum = 0;
							v->samples = 0;
							v->curPeak = 0;
						}
					}
				}
			}
			return g_submit(a_voice, a_buffer, a_wma);
		}

		HRESULT STDMETHODCALLTYPE CreateSourceVoiceDetour(void* a_engine, void** a_voice, const WAVEFORMATEX* a_format, UINT32 a_flags, float a_ratio, void* a_callback,
			const void* a_sends, const void* a_effects)
		{
			const HRESULT hr = g_createSourceVoice(a_engine, a_voice, a_format, a_flags, a_ratio, a_callback, a_sends, a_effects);
			if (SUCCEEDED(hr) && a_voice && *a_voice && a_format) {
				Voice v;
				v.voice = *a_voice;
				v.channels = a_format->nChannels;
				v.rate = static_cast<int>(a_format->nSamplesPerSec);
				v.bits = a_format->wBitsPerSample;
				v.isFloat = a_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
				if (a_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && a_format->cbSize >= 22) {
					GUID sub;
					std::memcpy(&sub, reinterpret_cast<const BYTE*>(a_format) + sizeof(WAVEFORMATEX) + 6, sizeof(GUID));
					v.isFloat = sub.Data1 == WAVE_FORMAT_IEEE_FLOAT;  // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT's first field
				}
				{
					std::scoped_lock lock(g_lock);
					if (g_voices.size() < 64) {
						g_voices.push_back(v);
					}
				}
				// Every source voice shares the implementation's vtable: one slot swap meters them all.
				if (!g_voiceHooked.exchange(true)) {
					auto** vtable = *reinterpret_cast<void***>(*a_voice);
					if (auto* old = Hooks::SwapVtable(vtable, kSubmitSlot, reinterpret_cast<void*>(&SubmitDetour))) {
						g_submit = reinterpret_cast<SubmitFn>(old);
					} else {
						Log::Warn("audio meter: SubmitSourceBuffer's slot could not be swapped -- no meter");
					}
				}
				Log::Info("audio meter: source voice {} -- {} ch {} Hz {}-bit {}", *a_voice, v.channels, v.rate, v.bits, v.isFloat ? "float" : "PCM");
			}
			return hr;
		}

		HRESULT WINAPI CoCreateDetour(REFCLSID a_clsid, LPUNKNOWN a_outer, DWORD a_ctx, REFIID a_iid, LPVOID* a_out)
		{
			const HRESULT hr = g_coCreate(a_clsid, a_outer, a_ctx, a_iid, a_out);
			if (SUCCEEDED(hr) && a_out && *a_out && (IsEqualGUID(a_clsid, kClsidXAudio2) || IsEqualGUID(a_clsid, kClsidXAudio2Debug)) &&
				!g_engineHooked.exchange(true)) {
				auto** vtable = *reinterpret_cast<void***>(*a_out);
				if (auto* old = Hooks::SwapVtable(vtable, kCreateSourceVoiceSlot, reinterpret_cast<void*>(&CreateSourceVoiceDetour))) {
					g_createSourceVoice = reinterpret_cast<CreateSourceVoiceFn>(old);
					Log::Info("audio meter: MGSV's XAudio2 engine {} -- its source voices are metered (`!sound meter`)", *a_out);
				} else {
					Log::Warn("audio meter: CreateSourceVoice's slot could not be swapped -- no meter");
				}
			}
			return hr;
		}

		Voice* Busiest()
		{
			Voice* best = nullptr;
			for (auto& v : g_voices) {
				if (!best || v.total > best->total) {
					best = &v;
				}
			}
			return best && best->total ? best : nullptr;
		}

		std::string Windows(const Voice& a_v, std::uint64_t a_from, std::uint64_t a_to)
		{
			std::string        out;
			std::vector<float> d;
			float              peak = -200.0f;
			for (std::uint64_t i = std::max<std::uint64_t>(a_from, a_to > kWindows ? a_to - kWindows : 0); i < a_to; ++i) {
				d.push_back(a_v.dbfs[i % kWindows]);
				peak = std::max(peak, a_v.peak[i % kWindows]);
				out += std::format("{}{:.0f}", out.empty() ? "" : " ", a_v.dbfs[i % kWindows]);
			}
			if (d.empty()) {
				return "no windows yet";
			}
			auto sorted = d;
			std::sort(sorted.begin(), sorted.end());
			return std::format("{} windows of 0.5 s: loudest {:.1f} dBFS, median {:.1f}, peak {:.1f} dBFS\n  {}", d.size(), sorted.back(),
				sorted[sorted.size() / 2], peak, out);
		}
	}

	void Install()
	{
		if (!Cfg().audioMeter) {
			return;
		}
		auto* ole = ::GetModuleHandleW(L"ole32.dll");
		if (!ole) {
			ole = ::LoadLibraryW(L"ole32.dll");
		}
		auto* target = ole ? reinterpret_cast<void*>(::GetProcAddress(ole, "CoCreateInstance")) : nullptr;
		if (!target || !Hooks::Create("CoCreateInstance (audio meter)", target, &CoCreateDetour, &g_coCreate)) {
			Log::Warn("audio meter: CoCreateInstance not hooked -- no meter");
			return;
		}
		Log::Info("audio meter: waiting for MGSV's XAudio2 engine");
	}

	std::string Command(const std::string& a_args)
	{
		std::istringstream in(a_args);
		std::string        verb;
		in >> verb;
		std::scoped_lock lock(g_lock);
		auto*            v = Busiest();
		if (!v) {
			return g_engineHooked ? "audio meter: no samples yet" : "audio meter: off ([Audio] Meter = false, or no XAudio2 engine seen)";
		}
		if (verb == "mark") {
			g_mark = v->total;
			g_markVoice = v->voice;
			return std::format("audio meter: mark at window {}", g_mark);
		}
		if (verb == "since") {
			if (g_markVoice != v->voice) {
				return "audio meter: no mark on this voice (`!sound meter mark` first)";
			}
			return "audio meter since the mark: " + Windows(*v, g_mark, v->total);
		}
		int n = 20;
		if (!verb.empty()) {
			n = std::clamp(std::atoi(verb.c_str()), 1, 256);
		}
		return std::format("audio meter (voice {}, {} voices): ", v->voice, g_voices.size()) + Windows(*v, v->total > static_cast<std::uint64_t>(n) ? v->total - n : 0, v->total);
	}
}
