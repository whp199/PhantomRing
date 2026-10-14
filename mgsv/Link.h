#pragma once
// The guest side of the shared mapping: attach, heartbeat, host-restart detection, and the producer
// sides MGSV owns (GuestState, PoseBlock, guest events, render ring). Everything here is non-blocking:
// MGSV keeps running whether or not Elden Ring is there.

#include "phantomring_protocol.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace phantom::guest::link
{
	// Starts the worker thread (attach loop + heartbeat + "!" console commands). a_where: "file:..."/"name:...".
	void Start(const std::wstring& a_where);

	// Boot status shown by the host while MGSV is starting (and in the dev tools).
	void SetStatus(proto::Status a_status, std::string_view a_text);
	proto::Status CurrentStatus();
	std::string   CurrentStatusText();

	[[nodiscard]] bool Attached();
	[[nodiscard]] bool HostAlive();  // attached and the host's heartbeat is fresh

	// Game-thread liveness: the worker only heartbeats while the game itself ticks (or is booting).
	void NoteGameTick();

	// Producers (any game thread; serialized internally).
	void PublishState(const proto::GuestState& a_state);
	void PublishPose(const proto::PoseBlock& a_pose);
	bool PushEvent(const proto::GuestEvent& a_event);

	// Consumers.
	bool ReadPad(proto::PadState& a_out);
	bool ReadHostState(proto::HostState& a_out);
	bool PopHostEvent(proto::HostEvent& a_out);
	// The ActorTable (host -> guest seqlock): its seq and the first `count` records only (ActorProxy). False while detached,
	// before the host's first table, or when the host kept writing through every attempt.
	bool ReadActors(proto::ActorRecord* a_out, std::uint32_t a_max, std::uint32_t& a_count, std::uint32_t& a_seq);

	// The dev console mailbox (nullptr while detached). Lua requests are served by the Lua pump; lines
	// starting with '!' are guest commands served by the worker (they work before Lua is up).
	proto::Console* GuestConsole();
	// Serves '!' console lines on the worker thread; returns the response text (status 0).
	using CommandHandler = std::string (*)(std::string_view a_line);
	void SetCommandHandler(CommandHandler a_handler);
	// Bumps every time the guest (re)attaches to a host session: the frame code restarts the
	// odometer (new odoEpoch) and resends whatever the host may have dropped.
	std::uint32_t AttachGeneration();

	// Phase 2 producers (render ring, draw triple buffer): hold the render lock while writing. Returns the
	// mapping base, or nullptr (lock released) while detached. Attach takes the same lock to send
	// kRenClearAll and reset the draw triple buffer, so a re-attach never interleaves with a capture write.
	std::uint8_t* LockRender(std::unique_lock<std::mutex>& a_lock);

	// Called every worker iteration (~30 Hz), between the game's frames' writes (e.g. the life bridge).
	void SetWorkerTick(void (*a_tick)());

	// Unix pid of this process under Wine/Proton (the launcher may pass it as PHANTOMRING_GUEST_UNIX_PID);
	// the Windows pid on real Windows; 0 when unknown.
	std::uint32_t UnixPid();
	[[nodiscard]] bool RunningUnderWine();
	std::wstring DefaultMapping();
}
