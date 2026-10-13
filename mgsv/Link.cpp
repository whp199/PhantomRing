#include "Link.h"

#include "Log.h"
#include "Mapping.h"
#include "Platform.h"
#include "Version.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>

namespace phantom::guest::link
{
	namespace
	{
		using namespace phantom::proto;

		Mapping*                   g_map{ nullptr };  // worker thread only; never unmapped (other threads may be reading it)
		std::atomic<std::uint8_t*> g_base{ nullptr };
		std::atomic<std::uint64_t> g_nonce{ 0 };
		std::atomic<std::uint32_t> g_generation{ 0 };
		std::mutex                 g_produce;  // one producer at a time per slot/ring; also excludes re-attach
		std::mutex                 g_render;   // render ring + draw triple buffer (capture); also excludes re-attach

		std::mutex  g_statusLock;
		Status      g_status{ Status::kBooting };
		std::string g_statusText{ "MGSV: starting" };

		std::atomic<std::int64_t> g_lastGameTick{ 0 };
		std::int64_t              g_qpcFreq{ 1 };
		std::atomic<CommandHandler> g_command{ nullptr };
		std::atomic<void (*)()>     g_workerTick{ nullptr };

		std::int64_t Qpc()
		{
			LARGE_INTEGER t;
			::QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		Header* Hdr(std::uint8_t* a_base) { return reinterpret_cast<Header*>(a_base + kOffHeader); }

		template <class T>
		T* At(std::uint8_t* a_base, std::uint64_t a_off)
		{
			return reinterpret_cast<T*>(a_base + a_off);
		}

		using GuestRing = EntryRing<GuestEvent, kEventRingEntries>;
		using HostRing = EntryRing<HostEvent, kEventRingEntries>;

		void WriteStatus(std::uint8_t* a_base)
		{
			auto*            h = Hdr(a_base);
			std::scoped_lock lock(g_statusLock);
			const auto       n = std::min(g_statusText.size(), sizeof(h->guestStatusText) - 1);
			std::memcpy(h->guestStatusText, g_statusText.data(), n);
			h->guestStatusText[n] = 0;
			Atomic(h->guestStatus).store(g_status, std::memory_order_release);
		}

		// Byte ring producer for the one message the guest sends there today (phase 2 adds meshes).
		bool PushRender(std::uint8_t* a_base, RenType a_type)
		{
			auto*      ring = a_base + kOffRenderRing;
			auto&      headRef = *reinterpret_cast<std::uint64_t*>(ring + kRenRingHeadOff);
			auto&      tailRef = *reinterpret_cast<std::uint64_t*>(ring + kRenRingTailOff);
			const auto head = Atomic(headRef).load(std::memory_order_relaxed);
			const auto tail = Atomic(tailRef).load(std::memory_order_acquire);
			constexpr std::uint64_t kMsg = sizeof(RenMsgHeader);  // no payload; 8 bytes never straddle the end
			if (head + kMsg - tail > kRenRingDataBytes) {
				return false;
			}
			const RenMsgHeader msg{ a_type, 0 };
			std::memcpy(ring + kRenRingDataOff + head % kRenRingDataBytes, &msg, sizeof(msg));
			Atomic(headRef).store(head + kMsg, std::memory_order_release);
			return true;
		}

		void Attach(std::uint8_t* a_base, std::uint64_t a_nonce)
		{
			auto*            h = Hdr(a_base);
			std::scoped_lock lock(g_produce, g_render);
			Atomic(h->guestNonce).store(0, std::memory_order_release);  // detached while our producer sides reset
			// The draw triple buffer restarts too: middle 0, our writer on 1, the host's reader on 2.
			Atomic(At<DrawCtl>(a_base, kOffDrawCtl)->state).store(0, std::memory_order_release);
			Atomic(At<HudCtl>(a_base, kOffHudCtl)->state).store(0, std::memory_order_release);  // HUD layer too (Hud.cpp)
			HostRing{ a_base + kOffHostEvents }.Drain();                // whatever a previous host left is stale
			// A previous MGSV process's last GuestState (kGuestReady, its camera) and pose must not be taken for
			// ours while this one is still booting: publish empty ones before the host trusts our slots.
			static const GuestState kNoState{};
			static const PoseBlock  kNoPose{};
			SeqWrite(At<GuestState>(a_base, kOffGuestState), kNoState);
			SeqWrite(At<PoseBlock>(a_base, kOffPose), kNoPose);
			if (!PushRender(a_base, kRenClearAll)) {
				Log::Warn("link: render ring full, kRenClearAll not sent");
			}
			// Console: answer nothing left over from a previous session.
			auto* console = At<Console>(a_base, kOffGuestConsole);
			Atomic(console->respSeq).store(Atomic(console->reqSeq).load(std::memory_order_acquire), std::memory_order_release);
			h->guestUnixPid = UnixPid();
			WriteStatus(a_base);
			Atomic(h->guestHeartbeatQpc).store(Qpc(), std::memory_order_release);
			g_generation.fetch_add(1, std::memory_order_acq_rel);
			Atomic(h->guestNonce).store(a_nonce, std::memory_order_release);  // last: the host trusts our slots from here
			g_nonce.store(a_nonce, std::memory_order_release);
			Log::Info("link: attached to host session {:#018x} (host unix pid {})", a_nonce, h->hostUnixPid);
		}

		void ServeCommand(std::uint8_t* a_base)
		{
			auto*      c = At<Console>(a_base, kOffGuestConsole);
			const auto req = Atomic(c->reqSeq).load(std::memory_order_acquire);
			if (req == Atomic(c->respSeq).load(std::memory_order_acquire) || c->request[0] != '!') {
				return;
			}
			const auto  handler = g_command.load();
			std::string line(c->request, strnlen(c->request, sizeof(c->request)));
			std::string out = handler ? handler(line) : std::string("no command handler");
			const auto  n = std::min(out.size(), sizeof(c->response) - 1);
			std::memcpy(c->response, out.data(), n);
			c->response[n] = 0;
			c->status = 0;
			Atomic(c->respSeq).store(req, std::memory_order_release);
		}

		// Identity of the file behind a "file:" mapping (volume serial + file index), to notice the
		// launcher replacing it; nullopt for named mappings or a missing file.
		std::optional<std::uint64_t> FileId(const std::wstring& a_where)
		{
			if (!a_where.starts_with(L"file:")) {
				return std::nullopt;
			}
			HANDLE f = ::CreateFileW(a_where.substr(5).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (f == INVALID_HANDLE_VALUE) {
				return std::nullopt;
			}
			BY_HANDLE_FILE_INFORMATION info{};
			const bool                 ok = ::GetFileInformationByHandle(f, &info);
			::CloseHandle(f);
			if (!ok) {
				return std::nullopt;
			}
			return (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32 | info.nFileIndexLow) ^ (static_cast<std::uint64_t>(info.dwVolumeSerialNumber) << 48);
		}

		DWORD WINAPI Worker(void* a_where)
		{
			const std::wstring where = *static_cast<std::wstring*>(a_where);
			delete static_cast<std::wstring*>(a_where);
			bool         loggedWait = false, loggedVersion = false;
			std::optional<std::uint64_t> mappedId;
			std::int64_t lastHostSeen = Qpc();
			for (;;) {
				auto* base = g_base.load(std::memory_order_acquire);
				if (!base) {
					auto* map = new Mapping;
					if (!map->Open(where, kMappingBytes, false)) {
						delete map;
						if (!loggedWait) {
							Log::Info("link: waiting for {} (created by Elden Ring's plugin, or by the Linux launcher)", Narrow(where));
							loggedWait = true;
						}
						::Sleep(1000);
						continue;
					}
					// A previous view (remap below) stays mapped on purpose: pad/event readers on game
					// threads may still be inside it. Remaps only happen when the file was replaced.
					g_map = map;
					mappedId = FileId(where);
					base = g_map->Base();
					Atomic(Hdr(base)->guestNonce).store(0, std::memory_order_release);  // a new guest: nothing attached yet
					g_base.store(base, std::memory_order_release);
					lastHostSeen = Qpc();
					Log::Info("link: mapped {}", Narrow(where));
				}
				auto*      h = Hdr(base);
				const auto now = Qpc();
				WriteStatus(base);

				const bool booting = CurrentStatus() != Status::kReady;
				const auto tick = g_lastGameTick.load(std::memory_order_acquire);
				if (booting || (now - tick) < 10 * g_qpcFreq) {
					Atomic(h->guestHeartbeatQpc).store(now, std::memory_order_release);
				}

				const auto magic = Atomic(h->magic).load(std::memory_order_acquire);
				const auto version = h->version;
				const auto nonce = Atomic(h->sessionNonce).load(std::memory_order_acquire);
				// Who we are, even while refusing to attach: the host tells the player (common/Version.h).
				Atomic(h->guestProtocol).store(kVersion, std::memory_order_release);
				if (phantom::version::ReadField(h->guestRelease, sizeof(h->guestRelease)) != phantom::version::Release()) {
					phantom::version::WriteField(h->guestRelease, sizeof(h->guestRelease), phantom::version::Release());
				}
				if (magic == kMagic && version != kVersion) {
					if (!loggedVersion) {
						Log::Error("link: {} -- not attaching (this guest: {})",
							phantom::version::MismatchText(phantom::version::ReadField(h->hostRelease, sizeof(h->hostRelease)), version,
								phantom::version::Release(), kVersion),
							phantom::version::Describe());
						loggedVersion = true;
					}
				} else if (magic == kMagic && nonce != 0) {
					if (nonce != g_nonce.load(std::memory_order_acquire) || Atomic(h->guestNonce).load(std::memory_order_acquire) != nonce) {
						Attach(base, nonce);
					}
					ServeCommand(base);
				}

				// The launcher may have recreated the file under us (we would keep mapping the unlinked
				// one): if the host has been silent for a while, map the path again.
				const auto hostBeat = Atomic(h->hostHeartbeatQpc).load(std::memory_order_acquire);
				if (hostBeat && (now - hostBeat) < kHeartbeatTimeoutMs * g_qpcFreq / 1000) {
					lastHostSeen = now;
				} else if (now - lastHostSeen > 5 * g_qpcFreq) {
					lastHostSeen = now;
					const auto id = FileId(where);
					if (id && mappedId && *id != *mappedId) {
						Log::Info("link: {} was replaced while the host was away, mapping the new file", Narrow(where));
						std::scoped_lock lock(g_produce);
						g_base.store(nullptr, std::memory_order_release);
						g_nonce.store(0, std::memory_order_release);
						loggedWait = false;
						continue;
					}
				}
				if (const auto tick = g_workerTick.load(std::memory_order_acquire)) {
					tick();
				}
				::Sleep(33);
			}
		}
	}

	bool RunningUnderWine() { return phantom::RunningUnderWine(); }

	std::uint32_t UnixPid() { return phantom::UnixPid(L"PHANTOMRING_GUEST_UNIX_PID"); }

	std::wstring DefaultMapping() { return platform::DefaultMapping(platform::Detect().wine); }  // the host's default too

	void Start(const std::wstring& a_where)
	{
		LARGE_INTEGER f;
		::QueryPerformanceFrequency(&f);
		g_qpcFreq = f.QuadPart > 0 ? f.QuadPart : 1;
		g_lastGameTick.store(Qpc());
		Log::Info("link: unix pid {}, mapping {}", UnixPid(), Narrow(a_where));
		if (HANDLE t = ::CreateThread(nullptr, 0, &Worker, new std::wstring(a_where), 0, nullptr)) {
			::CloseHandle(t);
		} else {
			Log::Error("link: can't start the worker thread ({})", ::GetLastError());
		}
	}

	void SetStatus(proto::Status a_status, std::string_view a_text)
	{
		std::scoped_lock lock(g_statusLock);
		if (g_status != a_status || g_statusText != a_text) {
			Log::Info("status: {} ({})", a_text, static_cast<int>(a_status));
		}
		g_status = a_status;
		g_statusText.assign(a_text);
	}

	proto::Status CurrentStatus()
	{
		std::scoped_lock lock(g_statusLock);
		return g_status;
	}

	std::string CurrentStatusText()
	{
		std::scoped_lock lock(g_statusLock);
		return g_statusText;
	}

	bool Attached()
	{
		auto* base = g_base.load(std::memory_order_acquire);
		const auto nonce = g_nonce.load(std::memory_order_acquire);
		return base && nonce && Atomic(Hdr(base)->sessionNonce).load(std::memory_order_acquire) == nonce;
	}

	bool HostAlive()
	{
		auto* base = g_base.load(std::memory_order_acquire);
		if (!base || !Attached()) {
			return false;
		}
		const auto beat = Atomic(Hdr(base)->hostHeartbeatQpc).load(std::memory_order_acquire);
		return beat && (Qpc() - beat) < proto::kHeartbeatTimeoutMs * g_qpcFreq / 1000;
	}

	void NoteGameTick() { g_lastGameTick.store(Qpc(), std::memory_order_release); }

	std::uint32_t AttachGeneration() { return g_generation.load(std::memory_order_acquire); }

	void SetWorkerTick(void (*a_tick)()) { g_workerTick.store(a_tick, std::memory_order_release); }

	std::uint8_t* LockRender(std::unique_lock<std::mutex>& a_lock)
	{
		a_lock = std::unique_lock<std::mutex>(g_render);
		auto* base = g_base.load(std::memory_order_acquire);
		if (!base || !Attached()) {
			a_lock.unlock();
			return nullptr;
		}
		return base;
	}

	void PublishState(const proto::GuestState& a_state)
	{
		std::scoped_lock lock(g_produce);
		if (auto* base = g_base.load(std::memory_order_acquire); base && g_nonce.load()) {
			SeqWrite(At<GuestState>(base, kOffGuestState), a_state);
		}
	}

	void PublishPose(const proto::PoseBlock& a_pose)
	{
		std::scoped_lock lock(g_produce);
		if (auto* base = g_base.load(std::memory_order_acquire); base && g_nonce.load()) {
			SeqWrite(At<PoseBlock>(base, kOffPose), a_pose);
		}
	}

	bool PushEvent(const proto::GuestEvent& a_event)
	{
		std::scoped_lock lock(g_produce);
		auto*            base = g_base.load(std::memory_order_acquire);
		return base && g_nonce.load() && GuestRing{ base + kOffGuestEvents }.Push(a_event);
	}

	bool ReadPad(proto::PadState& a_out)
	{
		auto* base = g_base.load(std::memory_order_acquire);
		return base && Attached() && SeqRead(At<PadState>(base, kOffPad), a_out);
	}

	bool ReadHostState(proto::HostState& a_out)
	{
		auto* base = g_base.load(std::memory_order_acquire);
		return base && Attached() && SeqRead(At<HostState>(base, kOffHostState), a_out);
	}

	bool PopHostEvent(proto::HostEvent& a_out)
	{
		// Single consumer (the frame tick); the lock only keeps it from racing the re-attach Drain().
		std::scoped_lock lock(g_produce);
		auto*            base = g_base.load(std::memory_order_acquire);
		return base && Attached() && HostRing{ base + kOffHostEvents }.Pop(a_out);
	}

	proto::Console* GuestConsole()
	{
		auto* base = g_base.load(std::memory_order_acquire);
		return base && Attached() ? At<Console>(base, kOffGuestConsole) : nullptr;
	}

	void SetCommandHandler(CommandHandler a_handler) { g_command.store(a_handler); }
}
