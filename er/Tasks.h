#pragma once

// Per-frame callbacks through Elden Ring's own task system (no code patched): a task object shaped
// like fromsoftware-rs RecurringTask (vtable {get_runtime_class, dtor, execute(FD4TaskData*)}) is
// handed to register_task for a CSTaskGroupIndex. Groups verified against the exe's task table
// (docs/research/elden-ring-runtime-re.md §3.9).

#include <cstdint>

namespace phantom::er
{
	enum class TaskGroup : std::uint32_t
	{
		kFrameBegin = 0,
		kChrInsPrePhysics = 78,
		kCameraStep = 106,
		kDrawParamUpdate = 107,
		kWorldChrManPostPhysics = 117,
		kDmgManPost = 122,               // timeline: after WorldChrMan_PostPhysics, right before MoveMap_PostPhysics_1 (123)
		kDrawPre = 144,
		kFrameEnd = 168,
	};

	using TaskFn = void (*)(float a_dt);

	// Blocks until system init is done (global_hinstance set, CSTask created), up to a_timeoutMs
	// (0 = forever). False if the needed addresses aren't verified or it timed out.
	bool WaitForSystemInit(std::uint32_t a_timeoutMs);

	// Registers a_fn to run every frame in a_group. Tasks live for the whole process.
	bool RegisterTask(TaskGroup a_group, TaskFn a_fn, const char* a_name);

	// Executions per task so far (dev console "status").
	std::uint64_t TaskRuns(TaskGroup a_group);
}
