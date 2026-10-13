#include "Tasks.h"

#include "Addresses.h"
#include "Log.h"
#include "Mem.h"

#include <atomic>
#include <exception>

namespace phantom::er
{
	namespace
	{
		struct Task;

		// FD4TaskData: {FD4Time{vptr, f32 dt, pad}, u32 group, i32 seed} (fromsoftware-rs fd4/task.rs).
		struct TaskData
		{
			void*         timeVtable;
			float         dt;
			std::uint32_t pad;
			std::uint32_t group;
			std::int32_t  seed;
		};

		// get_runtime_class must return a DLRuntimeClass&. fromsoftware-rs never implements it (the
		// game doesn't call it on the recurring path); a zeroed block is safer than a crash if it does.
		alignas(16) std::uint8_t g_fakeRuntimeClass[0x80]{};

		void* GetRuntimeClass(Task*) { return g_fakeRuntimeClass; }
		void* Destructor(Task* a_self, std::uint32_t) { return a_self; }  // tasks are never freed
		void  Execute(Task* a_self, const TaskData* a_data);

		void* g_vtable[] = { reinterpret_cast<void*>(&GetRuntimeClass), reinterpret_cast<void*>(&Destructor), reinterpret_cast<void*>(&Execute) };

		// Layout the game sees: vtable pointer + FD4TaskBase's u32 at +8 (kept 0, as fromsoftware-rs).
		struct Task
		{
			void**                     vtable{ g_vtable };
			std::uint64_t              unk8{ 0 };
			TaskFn                     fn{ nullptr };
			const char*                name{ "" };
			TaskGroup                  group{};
			std::atomic<std::uint64_t> runs{ 0 };
			bool                       failed{ false };
		};

		Task g_tasks[16];
		int  g_taskCount = 0;

		void Execute(Task* a_self, const TaskData* a_data)
		{
			a_self->runs.fetch_add(1, std::memory_order_relaxed);
			if (a_self->failed) {
				return;
			}
			// No C++ exception may unwind into the game's frames.
			try {
				a_self->fn(a_data ? a_data->dt : 0.0f);
			} catch (const std::exception& e) {
				a_self->failed = true;
				Log::Error("task {} threw ({}); disabled", a_self->name, e.what());
			} catch (...) {
				a_self->failed = true;
				Log::Error("task {} threw; disabled", a_self->name);
			}
		}
	}

	bool WaitForSystemInit(std::uint32_t a_timeoutMs)
	{
		const auto& addr = Addr();
		if (!addr.HasAll(A::GlobalHinstance, A::CSTask, A::RegisterTask, A::VtTaskImp)) {
			Log::Error("tasks: system-init addresses unverified; no per-frame callbacks");
			return false;
		}
		const auto start = ::GetTickCount64();
		auto       nextLog = start + 10000;
		for (;;) {
			const auto hinstance = SafeRead<void*>(addr[A::GlobalHinstance]);
			auto*      task = addr.Instance(A::CSTask);
			if (hinstance && *hinstance && HasVtable(task, addr[A::VtTaskImp])) {
				Log::Info("system init done after {} ms", ::GetTickCount64() - start);
				return true;
			}
			const auto now = ::GetTickCount64();
			if (a_timeoutMs && now - start > a_timeoutMs) {
				Log::Error("tasks: system init not reached after {} ms", a_timeoutMs);
				return false;
			}
			if (now > nextLog) {
				Log::Info("still waiting for system init ({} s)", (now - start) / 1000);
				nextLog = now + 10000;
			}
			::Sleep(50);
		}
	}

	bool RegisterTask(TaskGroup a_group, TaskFn a_fn, const char* a_name)
	{
		const auto& addr = Addr();
		auto*       taskImp = addr.Instance(A::CSTask);
		if (!addr.Has(A::RegisterTask) || !HasVtable(taskImp, addr[A::VtTaskImp]) || g_taskCount >= static_cast<int>(std::size(g_tasks))) {
			Log::Error("task {}: can't register", a_name);
			return false;
		}
		auto& task = g_tasks[g_taskCount++];
		task.fn = a_fn;
		task.name = a_name;
		task.group = a_group;
		using RegisterFn = void (*)(void* a_taskImp, std::uint32_t a_group, Task* a_task);
		addr.Fn<RegisterFn>(A::RegisterTask)(taskImp, static_cast<std::uint32_t>(a_group), &task);
		Log::Info("task {} registered in group {}", a_name, static_cast<std::uint32_t>(a_group));
		return true;
	}

	std::uint64_t TaskRuns(TaskGroup a_group)
	{
		std::uint64_t runs = 0;
		for (int i = 0; i < g_taskCount; ++i) {
			if (g_tasks[i].group == a_group) {
				runs += g_tasks[i].runs.load(std::memory_order_relaxed);
			}
		}
		return runs;
	}
}
