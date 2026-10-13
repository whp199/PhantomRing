#include "Link.h"

#include "Log.h"

#include <bcrypt.h>

#include <algorithm>
#include <cstring>

namespace phantom::er
{
	namespace
	{
		using GuestRing = EntryRing<proto::GuestEvent, proto::kEventRingEntries>;
		using HostRing = EntryRing<proto::HostEvent, proto::kEventRingEntries>;
		static_assert(GuestRing::kBytes == proto::kEventRingBytes);

		std::int64_t g_qpcFreq = [] {
			LARGE_INTEGER f{};
			::QueryPerformanceFrequency(&f);
			return f.QuadPart ? f.QuadPart : 1;
		}();

		void CopyText(char* a_dst, std::size_t a_cap, std::string_view a_text)
		{
			const auto n = std::min(a_cap - 1, a_text.size());
			std::memcpy(a_dst, a_text.data(), n);
			a_dst[n] = 0;
		}

		std::string ReadText(const char* a_src, std::size_t a_cap)
		{
			char copy[128]{};
			std::memcpy(copy, a_src, std::min(a_cap, sizeof(copy) - 1));
			return std::string(copy, strnlen(copy, sizeof(copy) - 1));
		}

		const char* StatusName(proto::Status a_status)
		{
			switch (a_status) {
			case proto::Status::kAbsent:
				return "absent";
			case proto::Status::kBooting:
				return "booting";
			case proto::Status::kTitle:
				return "title";
			case proto::Status::kLoading:
				return "loading";
			case proto::Status::kReady:
				return "ready";
			case proto::Status::kFailed:
				return "FAILED";
			case proto::Status::kExiting:
				return "exiting";
			}
			return "?";
		}
	}

	Link& Link::Get()
	{
		static Link link;
		return link;
	}

	std::int64_t Link::Qpc()
	{
		LARGE_INTEGER t{};
		::QueryPerformanceCounter(&t);
		return t.QuadPart;
	}

	double Link::QpcToMs(std::int64_t a_ticks) { return static_cast<double>(a_ticks) * 1000.0 / static_cast<double>(g_qpcFreq); }

	bool Link::Open(const std::wstring& a_where)
	{
		if (!map_.Open(a_where, proto::kMappingBytes, true)) {
			return false;
		}
		auto* h = Hdr();
		// Readers check magic first: clear it while the header is being rewritten.
		Atomic(h->magic).store(0, std::memory_order_release);
		if (::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&nonce_), sizeof(nonce_), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0 || nonce_ == 0) {
			nonce_ = static_cast<std::uint64_t>(Qpc()) ^ (static_cast<std::uint64_t>(::GetCurrentProcessId()) << 48) ^ 0x9E3779B97F4A7C15ull;
		}
		h->version = proto::kVersion;
		h->qpcFrequency = g_qpcFreq;
		// Unix pid (Windows pids collide across Proton prefixes): common/Mapping.h UnixPid().
		h->hostUnixPid = UnixPid(L"PHANTOMRING_HOST_UNIX_PID");
		Atomic(h->hostHeartbeatQpc).store(Qpc(), std::memory_order_relaxed);
		h->hostStatus = proto::Status::kBooting;
		CopyText(h->hostStatusText, sizeof(h->hostStatusText), "Elden Ring starting");
		ResetHostRegions();
		Atomic(h->sessionNonce).store(nonce_, std::memory_order_release);
		Atomic(h->magic).store(proto::kMagic, std::memory_order_release);
		Log::Info("shared memory {} ({} MB) attached, session {:016x}, host unix pid {}", Narrow(a_where), proto::kMappingBytes >> 20, nonce_, h->hostUnixPid);
		return true;
	}

	void Link::ResetHostRegions()
	{
		auto* base = map_.Base();
		std::memset(base + proto::kOffHostState, 0, sizeof(proto::HostState));
		std::memset(base + proto::kOffPad, 0, sizeof(proto::PadState));
		std::memset(base + proto::kOffHostEvents, 0, HostRing::kDataOff);  // head + tail; guest re-syncs on the new nonce
		std::memset(base + proto::kOffActors, 0, sizeof(proto::ActorTable));
		GuestRing{ base + proto::kOffGuestEvents }.Drain();
		DrainRender();
		auto* console = HostConsole();
		Atomic(console->respSeq).store(Atomic(console->reqSeq).load(std::memory_order_acquire), std::memory_order_release);
		drawFront_ = 2;
		drawValid_ = false;
	}

	bool Link::GuestAliveNow() const
	{
		if (!Valid()) {
			return false;
		}
		auto* h = Hdr();
		if (Atomic(h->guestNonce).load(std::memory_order_acquire) != nonce_) {
			return false;
		}
		const auto beat = Atomic(h->guestHeartbeatQpc).load(std::memory_order_acquire);
		return beat != 0 && QpcToMs(Qpc() - beat) < static_cast<double>(proto::kHeartbeatTimeoutMs);
	}

	double Link::GuestHeartbeatAgeMs() const
	{
		if (!Valid()) {
			return -1;
		}
		const auto beat = Atomic(Hdr()->guestHeartbeatQpc).load(std::memory_order_acquire);
		return beat ? QpcToMs(Qpc() - beat) : -1;
	}

	proto::Status Link::GuestStatus() const
	{
		return Valid() ? Atomic(Hdr()->guestStatus).load(std::memory_order_acquire) : proto::Status::kAbsent;
	}

	std::string Link::GuestStatusText() const
	{
		return Valid() ? ReadText(Hdr()->guestStatusText, sizeof(Hdr()->guestStatusText)) : std::string();
	}

	void Link::Tick()
	{
		if (!Valid()) {
			return;
		}
		auto* h = Hdr();
		Atomic(h->hostHeartbeatQpc).store(Qpc(), std::memory_order_release);

		const bool attached = Atomic(h->guestNonce).load(std::memory_order_acquire) == nonce_;
		const auto pid = Atomic(h->guestUnixPid).load(std::memory_order_acquire);
		// A new guest process may stamp the nonce straight away; its pid tells us it restarted.
		if (attached && (!guestAttached_ || pid != guestPid_)) {
			guestPid_ = pid;
			OnGuestAttached();
		} else if (!attached && guestAttached_) {
			Log::Info("guest left this session (nonce changed)");
		}
		guestAttached_ = attached;

		const bool alive = GuestAliveNow();
		if (alive != guestAlive_) {
			Log::Info("guest {} (heartbeat age {:.0f} ms)", alive ? "alive" : "LOST", GuestHeartbeatAgeMs());
		}
		guestAlive_ = alive;

		// Without a renderer (off, or its hooks failed) keep the guest's render ring flowing by
		// discarding it; er/RenderAssets consumes it otherwise.
		if (!renderConsumer_.load()) {
			DrainRender();
		}

		// A producer that restarted its ring (head went backwards) would otherwise stall us forever.
		GuestRing ring{ map_.Base() + proto::kOffGuestEvents };
		if (Atomic(ring.HeadRef()).load(std::memory_order_acquire) < Atomic(ring.TailRef()).load(std::memory_order_relaxed)) {
			ring.Drain();
		}

		const auto status = GuestStatus();
		auto       text = GuestStatusText();
		if (attached && (status != lastGuestStatus_ || text != lastGuestText_)) {
			// Boot progress of MGSV; shown to the player once the overlay exists (phase 5).
			Log::Info("guest status: {} \"{}\"", StatusName(status), text);
			lastGuestStatus_ = status;
			lastGuestText_ = std::move(text);
		}
	}

	void Link::DrainRender()
	{
		auto& head = *reinterpret_cast<std::uint64_t*>(map_.Base() + proto::kOffRenderRing + proto::kRenRingHeadOff);
		auto& tail = *reinterpret_cast<std::uint64_t*>(map_.Base() + proto::kOffRenderRing + proto::kRenRingTailOff);
		const auto h = Atomic(head).load(std::memory_order_acquire);
		const auto t = Atomic(tail).load(std::memory_order_relaxed);
		if (h != t) {
			renderDiscarded_ += h > t ? h - t : 0;
			Atomic(tail).store(h, std::memory_order_release);
		}
	}

	void Link::OnGuestAttached()
	{
		++attaches_;
		GuestRing{ map_.Base() + proto::kOffGuestEvents }.Drain();
		drawFront_ = 2;
		drawValid_ = false;
		Log::Info("guest attached to session {:016x} (attach #{}, guest unix pid {})", nonce_, attaches_,
			Atomic(Hdr()->guestUnixPid).load(std::memory_order_acquire));
	}

	void Link::SetHostStatus(proto::Status a_status, std::string_view a_text)
	{
		if (!Valid()) {
			return;
		}
		auto* h = Hdr();
		if (h->hostStatus != a_status || std::string_view(h->hostStatusText) != a_text) {
			CopyText(h->hostStatusText, sizeof(h->hostStatusText), a_text);
			Atomic(h->hostStatus).store(a_status, std::memory_order_release);
			Log::Info("host status: {} \"{}\"", StatusName(a_status), a_text);
		}
	}

	void Link::WriteHostState(const proto::HostState& a_state)
	{
		if (Valid()) {
			SeqWrite(At<proto::HostState>(proto::kOffHostState), a_state);
		}
	}

	bool Link::ReadGuestState(proto::GuestState& a_out) const
	{
		return Valid() && guestAttached_ && SeqRead(At<proto::GuestState>(proto::kOffGuestState), a_out);
	}

	bool Link::ReadPose(proto::PoseBlock& a_out) const
	{
		if (!Valid() || !guestAttached_ || !SeqRead(At<proto::PoseBlock>(proto::kOffPose), a_out)) {
			return false;
		}
		a_out.boneCount = std::min(a_out.boneCount, proto::kMaxPoseBones);
		return true;
	}

	void Link::WritePad(const proto::PadState& a_pad)
	{
		if (Valid()) {
			SeqWrite(At<proto::PadState>(proto::kOffPad), a_pad);
		}
	}

	void Link::WriteActors(const proto::ActorTable& a_table, std::uint32_t a_count)
	{
		if (!Valid()) {
			return;
		}
		// Same seqlock as SeqWrite, but only the used part of the table is copied.
		auto*      dst = At<proto::ActorTable>(proto::kOffActors);
		auto       seq = Atomic(dst->seq);
		const auto s = seq.load(std::memory_order_relaxed);
		seq.store(s + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		dst->count = a_count;
		std::memcpy(dst->actors, a_table.actors, sizeof(proto::ActorRecord) * a_count);
		seq.store(s + 2, std::memory_order_release);
	}

	bool Link::PopGuestEvent(proto::GuestEvent& a_out)
	{
		return Valid() && guestAttached_ && GuestRing{ map_.Base() + proto::kOffGuestEvents }.Pop(a_out);
	}

	bool Link::PushHostEvent(const proto::HostEvent& a_event)
	{
		return Valid() && guestAttached_ && HostRing{ map_.Base() + proto::kOffHostEvents }.Push(a_event);
	}

	bool Link::AcquireDraw()
	{
		if (!Valid() || !guestAttached_) {
			return false;
		}
		auto* ctl = At<proto::DrawCtl>(proto::kOffDrawCtl);
		TripleReader reader{ &ctl->state, drawFront_ };
		if (!reader.Acquire()) {
			return false;
		}
		drawFront_ = reader.front;
		drawValid_ = true;
		return true;
	}

	const std::uint8_t* Link::FrontDrawBytes(std::uint32_t& a_bytes) const
	{
		a_bytes = 0;
		if (!Valid() || !drawValid_ || drawFront_ > 2) {
			return nullptr;
		}
		const auto* hdrs = At<proto::DrawSlotHdr>(proto::kOffDrawCtl + sizeof(proto::DrawCtl));
		a_bytes = std::min<std::uint32_t>(hdrs[drawFront_].bytes, static_cast<std::uint32_t>(proto::kDrawSlotBytes));
		return map_.Base() + proto::kOffDrawSlots + drawFront_ * proto::kDrawSlotBytes;
	}

	const proto::DrawList* Link::FrontDraw() const
	{
		std::uint32_t bytes = 0;
		const auto*   p = FrontDrawBytes(bytes);
		if (!p || bytes < sizeof(proto::DrawList)) {
			return nullptr;
		}
		const auto* list = reinterpret_cast<const proto::DrawList*>(p);
		const std::uint64_t need = sizeof(proto::DrawList) + std::uint64_t(list->drawCount) * sizeof(proto::DrawRecord) + std::uint64_t(list->matrixCount) * 48;
		return need <= bytes ? list : nullptr;
	}
}
