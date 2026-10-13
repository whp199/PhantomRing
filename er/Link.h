#pragma once

// Host side of the shared memory (protocol/phantomring_protocol.h): opens (or creates) the mapping,
// stamps magic/version and a fresh session nonce, resets every host-owned region, heartbeats each
// frame, and tells whether the guest (MGSV) is alive in *this* session. Lifetime: the whole process.

#include "Mapping.h"
#include "phantomring_protocol.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace phantom::er
{
	class Link
	{
	public:
		static Link& Get();

		bool Open(const std::wstring& a_where);
		[[nodiscard]] bool Valid() const { return map_.Valid(); }

		// Once per ER frame (FrameBegin): heartbeat, guest liveness, status text changes, ring resync.
		void Tick();

		[[nodiscard]] bool          GuestAlive() const { return guestAlive_; }
		[[nodiscard]] bool          GuestAliveNow() const;  // thread-safe (input hook)
		[[nodiscard]] std::uint64_t Nonce() const { return nonce_; }
		[[nodiscard]] double        GuestHeartbeatAgeMs() const;
		[[nodiscard]] proto::Status GuestStatus() const;
		[[nodiscard]] std::string   GuestStatusText() const;
		[[nodiscard]] std::uint32_t AttachCount() const { return attaches_; }
		[[nodiscard]] std::uint64_t RenderBytesDiscarded() const { return renderDiscarded_; }
		// Empty, or the player-readable text when MGSV's plugin speaks another protocol (it never attaches then); Tick's thread.
		[[nodiscard]] const std::string& VersionMismatch() const { return versionMismatch_; }

		void SetHostStatus(proto::Status a_status, std::string_view a_text);

		void WriteHostState(const proto::HostState& a_state);
		bool ReadGuestState(proto::GuestState& a_out) const;
		bool ReadPose(proto::PoseBlock& a_out) const;
		void WritePad(const proto::PadState& a_pad);  // single writer: InputBridge serializes callers
		void WriteActors(const proto::ActorTable& a_table, std::uint32_t a_count);

		bool PopGuestEvent(proto::GuestEvent& a_out);
		bool PushHostEvent(const proto::HostEvent& a_event);

		[[nodiscard]] proto::Console* HostConsole() const { return At<proto::Console>(proto::kOffHostConsole); }
		// The whole mapping, for readers that keep their own cursor (er/HudFrames: the HUD triple buffer).
		[[nodiscard]] std::uint8_t* MapBase() const { return Valid() ? map_.Base() : nullptr; }

		// The render ring (guest -> host byte ring). While a consumer (er/RenderAssets) owns it, Tick()
		// stops discarding it.
		[[nodiscard]] std::uint8_t* RenderRing() const { return Valid() ? map_.Base() + proto::kOffRenderRing : nullptr; }
		void                        SetRenderConsumer(bool a_on) { renderConsumer_.store(a_on); }

		// Draw slots (guest -> host triple buffer): takes the newest published DrawList if there is
		// one; FrontDraw() is the current front (nullptr before the first frame / after a re-attach).
		bool                                 AcquireDraw();
		[[nodiscard]] const proto::DrawList* FrontDraw() const;
		[[nodiscard]] const std::uint8_t*    FrontDrawBytes(std::uint32_t& a_bytes) const;

		static std::int64_t Qpc();
		static double       QpcToMs(std::int64_t a_ticks);

	private:
		template <class T>
		T* At(std::uint64_t a_off) const
		{
			return reinterpret_cast<T*>(map_.Base() + a_off);
		}
		[[nodiscard]] proto::Header* Hdr() const { return At<proto::Header>(proto::kOffHeader); }
		void                         ResetHostRegions();
		void                         OnGuestAttached();
		void                         DrainRender();

		Mapping       map_;
		std::uint64_t nonce_{ 0 };
		bool          guestAlive_{ false };
		bool          guestAttached_{ false };
		std::uint32_t attaches_{ 0 };
		std::uint32_t guestPid_{ 0 };
		std::uint64_t renderDiscarded_{ 0 };
		std::atomic<bool> renderConsumer_{ false };
		std::uint32_t drawFront_{ 2 };
		bool          drawValid_{ false };
		proto::Status lastGuestStatus_{ proto::Status::kAbsent };
		std::string   lastGuestText_;
		std::int64_t  unattachedSince_{ 0 };  // QPC: a heartbeating guest not attached since (CheckGuestVersion)
		std::string   versionMismatch_;
		void          CheckGuestVersion(bool a_attached);
	};
}
