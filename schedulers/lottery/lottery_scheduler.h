
#ifndef GHOST_SCHEDULERS_LOTTERY_LOTTERY_SCHEDULER_H
#define GHOST_SCHEDULERS_LOTTERY_LOTTERY_SCHEDULER_H

#include <unordered_set>
#include <set>
#include <map>
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

  std::ostream &operator<<(std::ostream &os, const LotteryTaskState &state);

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

  class RunCollection
  {
  public:
    RunCollection() = default;
    RunCollection(const RunCollection &) = delete;
    RunCollection &operator=(RunCollection &) = delete;

    void Erase(LotteryTask *task);
    void Add(LotteryTask *task);

    unsigned int NumTickets();

    LotteryTask *PickWinner(unsigned int winning_ticket);

    std::set<int>::iterator Begin();
    std::set<int>::iterator End();

    // Erase 'task' from the runqueue.
    //
    // Caller must ensure that 'task' is on the runqueue in the first place
    // (e.g. via task->queued()).
    // void Erase(FifoTask *task);

    size_t Size() const
    {
      absl::MutexLock lock(&mu_);
      return rq_.size();
    }

    bool Empty() const { return Size() == 0; }

  private:
    mutable absl::Mutex mu_;
    std::set<LotteryTask *> rq_ ABSL_GUARDED_BY(mu_);
  };

  class LotteryScheduler : public BasicDispatchScheduler<LotteryTask>
  {
  public:
    explicit LotteryScheduler(Enclave *enclave, CpuList cpulist,
                              std::shared_ptr<TaskAllocator<LotteryTask>> allocator);
    ~LotteryScheduler() final {}

    void Schedule(const Cpu &cpu, const StatusWord &agent_sw);

    void EnclaveReady() final;
    Channel &GetDefaultChannel() final { return *default_channel_; };

    bool Empty(const Cpu &cpu)
    {
      CpuState *cs = cpu_state(cpu);
      return cs->run_queue.Empty();
    }

    void DumpState(const Cpu &cpu, int flags) final;
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
    // void CpuTick(const Message &msg) final;

  private:
    void LotterySchedule(const Cpu &cpu, BarrierToken agent_barrier, bool prio_boost);
    void TaskOffCpu(LotteryTask *task, bool blocked, bool from_switchto);
    void TaskOnCpu(LotteryTask *task, Cpu cpu);
    void Migrate(LotteryTask *task, Cpu cpu, BarrierToken seqnum);
    Cpu AssignCpu(LotteryTask *task);
    void DumpAllTasks();
    long unsigned int ParkMillerRand();

    struct CpuState
    {
      LotteryTask *current = nullptr;
      std::unique_ptr<Channel> channel = nullptr;
      RunCollection run_queue;
      std::map<LotteryTask *, unsigned int> mp;
      unsigned int count = 0;
      int total_tickets = 0;
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

  std::unique_ptr<LotteryScheduler> MultiThreadedLotteryScheduler(Enclave *enclave, CpuList cpulist);

  class LotteryAgent : public LocalAgent
  {
  public:
    LotteryAgent(Enclave *enclave, Cpu cpu, LotteryScheduler *scheduler)
        : LocalAgent(enclave, cpu), scheduler_(scheduler) {}

    void AgentThread() override;
    Scheduler *AgentScheduler() const override { return scheduler_; }

  private:
    LotteryScheduler *scheduler_;
  };

  template <class EnclaveType>
  class FullLotteryAgent : public FullAgent<EnclaveType>
  {
  public:
    explicit FullLotteryAgent(AgentConfig config) : FullAgent<EnclaveType>(config)
    {
      scheduler_ =
          MultiThreadedLotteryScheduler(&this->enclave_, *this->enclave_.cpus());
      this->StartAgentTasks();
      this->enclave_.Ready();
    }

    ~FullLotteryAgent() override
    {
      this->TerminateAgentTasks();
    }

    std::unique_ptr<Agent> MakeAgent(const Cpu &cpu) override
    {
      return std::make_unique<LotteryAgent>(&this->enclave_, cpu, scheduler_.get());
    }

    void RpcHandler(int64_t req, const AgentRpcArgs &args,
                    AgentRpcResponse &response) override
    {
      switch (req)
      {
      case LotteryScheduler::kDebugRunqueue:
        scheduler_->debug_runqueue_ = true;
        response.response_code = 0;
        return;
      case LotteryScheduler::kCountAllTasks:
        response.response_code = scheduler_->CountAllTasks();
        return;
      default:
        response.response_code = -1;
        return;
      }
    }

  private:
    std::unique_ptr<LotteryScheduler> scheduler_;
  };

}
#endif
