
#ifndef GHOST_SCHEDULERS_LOTTERY_LOTTERY_SCHEDULER_H
#define GHOST_SCHEDULERS_LOTTERY_LOTTERY_SCHEDULER_H

#include <deque>
#include <memory>

#include "lib/agent.h"
#include "lib/scheduler.h"

namespace ghost
{
#define DEFAULT_TICKET_ALLOC = 10

    enum class LotteryTaskState
    {
        kBlocked,  // not on runqueue.
        kRunnable, // transitory state:
                   // 1. kBlocked->kRunnable->kQueued
                   // 2. kQueued->kRunnable->kOnCpu
        kQueued,   // on runqueue.
        kOnCpu,    // running on cpu.
    };

    struct LotteryTask : public Task<>
    {
        int num_tickets = 0;
        explicit LotteryTask(Gtid lot_task_gtid, ghost_sw_info sw_info)
            : Task<>(lot_task_gtid, sw_info) {}
        ~LotteryTask() override {}

        inline bool blocked() const { return run_state == LotteryTaskState::kBlocked; }
        inline bool queued() const { return run_state == LotteryTaskState::kQueued; }
        inline bool oncpu() const { return run_state == LotteryTaskState::kOnCpu; }

        // N.B. _runnable() is a transitory state typically used during runqueue
        // manipulation. It is not expected to be used from task msg callbacks.
        //
        // If you are reading this then you probably want to take a closer look
        // at queued() instead.
        inline bool _runnable() const
        {
            return run_state == LotteryTaskState::kRunnable;
        }

        LotteryTaskState run_state = LotteryTaskState::kBlocked;
        int cpu = -1;

        // Whether the last execution was preempted or not.
        bool preempted = false;

        // A task's priority is boosted on a kernel preemption or a !deferrable
        // wakeup - basically when it may be holding locks or other resources
        // that prevent other tasks from making progress.
        // TODO - will likely be ticket boost in lottery
        //   bool prio_boost = false;
    };

    class LotteryScheduler : public BasicDispatchScheduler<LotteryTask>
    {
    public:
        explicit LotteryScheduler(Enclave *enclave, CpuList cpulist,
                                  std::shared_ptr<TaskAllocator<LotteryTask>> allocator);
        ~LotteryScheduler() final {}

        void Schedule(const Cpu &cpu, const StatusWord &sw);

        void EnclaveReady() final;
        Channel &GetDefaultChannel() final { return *default_channel_; };

        bool Empty(const Cpu &cpu)
        {
            CpuState *cs = cpu_state(cpu);
            return cs->run_queue.empty();
        }

        // void DumpState(const Cpu &cpu, int flags) final;
        std::atomic<bool> debug_runqueue_ = false;

        int CountAllTasks()
        {
            int num_tasks = 0;
            allocator()->ForEachTask([&num_tasks](Gtid gtid, const LotteryTask *task)
                                     {
      ++num_tasks;
      return true; });
            return num_tasks;
        }

        static constexpr int kDebugRunqueue = 1;
        static constexpr int kCountAllTasks = 2;

    protected:
        void TaskNew(LotteryTask *task, const Message &msg) final;
        void TaskRunnable(LotteryTask *task, const Message &msg) final;
        void TaskDeparted(LotteryTask *task, const Message &msg) final;
        void TaskDead(LotteryTask *task, const Message &msg) final;
        void TaskYield(LotteryTask *task, const Message &msg) final;
        void TaskBlocked(LotteryTask *task, const Message &msg) final;
        void TaskPreempted(LotteryTask *task, const Message &msg) final;
        void TaskSwitchto(LotteryTask *task, const Message &msg) final;

    private:
        //   void FifoSchedule(const Cpu& cpu, BarrierToken agent_barrier,
        //                     bool prio_boosted);
        //   void TaskOffCpu(FifoTask* task, bool blocked, bool from_switchto);
        //   void TaskOnCpu(FifoTask* task, Cpu cpu);
        void Migrate(LotteryTask *task, Cpu cpu, BarrierToken seqnum);
        Cpu AssignCpu(LotteryTask *task);

        struct CpuState
        {
            LotteryTask *current = nullptr;
            std::unique_ptr<Channel> channel = nullptr;
            std::vector<LotteryTask> run_queue;
            int total_tickets;
        } ABSL_CACHELINE_ALIGNED;

        inline CpuState *cpu_state(const Cpu &cpu) { return &cpu_states_[cpu.id()]; }

        inline CpuState *cpu_state_of(const LotteryTask *task)
        {
            CHECK_GE(task->cpu, 0);
            CHECK_LT(task->cpu, MAX_CPUS);
            return &cpu_states_[task->cpu];
        }

        CpuState cpu_states_[MAX_CPUS];
        Channel *default_channel_ = nullptr;
    };
}
#endif