#include "schedulers/lottery/lottery_scheduler.h"
#include <memory>
#include <iostream>
#include <random>

namespace ghost
{
    void RunCollection::Remove(LotteryTask *task)
    {
        task->run_state = FifoTaskState::kRunnable;
        absl::MutexLock lock(&mu_);
        rq_.erase(task);
    }
    void RunCollection::Add(LotteryTask *task)
    {
        CHECK_GE(task->cpu, 0);
        CHECK_EQ(task->run_state, FifoTaskState::kRunnable);

        task->run_state = FifoTaskState::kQueued;

        absl::MutexLock lock(&mu_);
        rq_.insert(task);
    }
    unsigned int RunCollection::NumTickets()
    {
        absl::MutexLock lock(&mu_);
        unsigned int acc = 0;
        for (const LotteryTask *&v : rq_)
        {
            acc += v->num_tickets
        }

        return acc;
    }

    LotteryTask *RunCollection::PickWinner(unsisgned int winning_ticket)
    {
        absl::MutexLock lock(&mu_);
        unsigned int acc = 0;
        for (const LotteryTask *&v : rq_)
        {
            acc += v->num_tickets;
            if (acc >= v->num_tickets)
                return v;
        }
        return nullptr;
    }

    LotteryScheduler::LotteryScheduler(Enclave *enclave, CpuList cpulist,
                                       std::shared_ptr<TaskAllocator<LotteryTask>> allocator) : BasicDispatchScheduler(enclave, std::move(cpulist),
                                                                                                                       std::move(allocator))
    {
        for (const Cpu &cpu : cpus())
        {
            int node = 0;
            CpuState *cs = cpu_state(cpu);
            cs->channel = enclave->MakeChannel(GHOST_MAX_QUEUE_ELEMS, node, MachineTopology()->ToCpuList({cpu}));
            if (!default_channel_)
            {
                default_channel_ = cs->channel.get();
            }
        }
    }

    // Implicitly thread-safe because it is only called from one agent associated
    // with the default queue.
    Cpu LotteryScheduler::AssignCpu(LotteryTask *task)
    {
        static auto begin = cpus().begin();
        static auto end = cpus().end();
        static auto next = end;

        if (next == end)
        {
            next = begin;
        }
        return next++;
    }

    void LotteryScheduler::LotterySchedule(const Cpu &cpu, BarrierToken agent_barrier)
    {
        CpuState *cs = cpu_state(cpu);

        // get the total number of tickets
        unsigned int num_tickets = cs->run_queue.NumTickets();

        // run the lottery
        std::random_device rd;
        std::mt19937 gen(rd()); // Mersenne Twister engine
        // Define a uniform distribution for integers between 1 and num_tickets
        std::uniform_int_distribution<int> distribution(1, num_tickets);

        int winning_ticket = distribution(gen);
        int curr_tickets = 0;

        LotteryTask *next = cs->run_queue.PickWinner();

        cs->run_queue.Erase(next);
        GHOST_DPRINT(3, stderr, "LotterySchedule %s on %s cpu %d ",
                     next ? next->gtid.describe() : "idling",
                     prio_boost ? "prio-boosted" : "", cpu.id());
        RunRequest *req = enclave()->GetRunRequest(cpu);

        if (next)
        {
            while (next->status_word.on_cpu())
            {
                Pause();
            }

            req->Open({
                .target = next->gtid,
                .target_barrier = next->seqnum,
                .agent_barrier = agent_barrier,
                .commit_flags = COMMIT_AT_TXN_COMMIT,
            });

            if (req->Commit())
            {
                // Txn commit succeeded and 'next' is oncpu.
                TaskOnCpu(next, cpu);
            }
            else
            {
                GHOST_DPRINT(3, stderr, "FifoSchedule: commit failed (state=%d)",
                             req->state());

                if (next == cs->current)
                {
                    TaskOffCpu(next, /*blocked=*/false, /*from_switchto=*/false);
                }

                cs->run_queue.Add(next);
            }
        }
        else
        {
            req->LocalYield(agent_barrier, flags);
        }
    }

    void LotteryScheduler::Schedule(const Cpu &cpu, const StatusWord &sw)
    {
        BarrierToken agent_barrier = agent_sw.barrier();
        CpuState *cs = cpu_state(cpu);

        GHOST_DPRINT(3, stderr, "Schedule: agent_barrier[%d] = %d\n", cpu.id(),
                     agent_barrier);

        Message msg;
        while (!(msg = Peek(cs->channel.get())).empty())
        {
            DispatchMessage(msg);
            Consume(cs->channel.get(), msg);
        }
        LotterySchedule(cpu, agent_barrier, agent_sw.boosted_priority());
    }

    void LotteryScheduler::TaskNew(LotteryTask *task, const Message &msg)
    {
        const ghost_msg_payload_task_new *payload =
            static_cast<const ghost_msg_payload_task_new *>(msg.payload());

        task->seqnum = msg.seqnum();
        task->run_state = LotteryTaskState::kBlocked;
        task->num_tickets = 10;
        if (payload->runnable)
        {
            task->run_state = LotteryTaskState::kRunnable;
            Cpu cpu = AssignCpu(task);
            Migrate(task, cpu, msg.seqnum());
        }
        else
        {
            // Wait until task becomes runnable to avoid race between migration
            // and MSG_TASK_WAKEUP showing up on the default channel.
        }
    }
    void LotteryScheduler::Migrate(LotteryTask *task, Cpu cpu, BarrierToken seqnum)
    {
        CHECK_EQ(task->run_state, FifoTaskState::kRunnable);
        CHECK_EQ(task->cpu, -1);

        CpuState *cs = cpu_state(cpu);
        const Channel *channel = cs->channel.get();
        CHECK(channel->AssociateTask(task->gtid, seqnum, /*status=*/nullptr));
        cs->total_tickets += task->num_tickets;
        GHOST_DPRINT(3, stderr, "Migrating task %s to cpu %d", task->gtid.describe(),
                     cpu.id());
        task->cpu = cpu.id();

        // Make task visible in the new runqueue *after* changing the association
        // (otherwise the task can get oncpu while producing into the old queue).
        cs->run_queue.Add(task);

        // Get the agent's attention so it notices the new task.
        enclave()->GetAgent(cpu)->Ping();
    }

    void LotteryScheduler::TaskRunnable(LotteryTask *task, const Message &msg)
    {
        const ghost_msg_payload_task_wakeup *payload =
            static_cast<const ghost_msg_payload_task_wakeup *>(msg.payload());
        CHECK(task->blocked());
        task->run_state = LotteryTaskState::kRunnable;
        if (task->cpu < 0)
        {
            // There cannot be any more messages pending for this task after a
            // MSG_TASK_WAKEUP (until the agent puts it oncpu) so it's safe to
            // migrate.
            Cpu cpu = AssignCpu(task);
            Migrate(task, cpu, msg.seqnum());
        }
        else
        {
            CpuState *cs = cpu_state_of(task);
            cs->run_queue.Add(task);
        }
    }

    void LotteryScheduler::TaskDeparted(LotteryTask *task, const Message &msg)
    {
        const ghost_msg_payload_task_departed *payload =
            static_cast<const ghost_msg_payload_task_departed *>(msg.payload());

        if (task->oncpu() || payload->from_switchto)
        {
            TaskOffCpu(task, /*blocked=*/false, payload->from_switchto);
        }
        else if (task->queued())
        {
            CpuState *cs = cpu_state_of(task);
            cs->run_queue.Erase(task);
            cs->total_tickets -= task->num_tickets;
        }
        else
        {
            CHECK(task->blocked());
        }

        if (payload->from_switchto)
        {
            Cpu cpu = topology()->cpu(payload->cpu);
            enclave()->GetAgent(cpu)->Ping();
        }

        allocator()->FreeTask(task);
    }

    void LotteryScheduler::TaskDead(LotteryTask *task, const Message &msg)
    {
        CHECK(task->blocked());
        allocator()->FreeTask(task);
    }

    void LotteryScheduler::TaskYield(LotteryTask *task, const Message &msg)
    {
        const ghost_msg_payload_task_yield *payload =
            static_cast<const ghost_msg_payload_task_yield *>(msg.payload());
        TaskOffCpu(task, /*blocked=*/false, payload->from_switchto);
        CpuState *cs = cpu_state_of(task);
        cs->run_queue.Add(task);
        if (payload->from_switchto)
        {
            Cpu cpu = topology()->cpu(payload->cpu);
            enclave()->GetAgent(cpu)->Ping();
        }
    }

    void LotteryScheduler::TaskBlocked(LotteryTask *task, const Message &msg)
    {
        const ghost_msg_payload_task_blocked *payload =
            static_cast<const ghost_msg_payload_task_blocked *>(msg.payload());
        TaskOffCpu(task, /*blocked=*/true, payload->from_switchto);
        if (payload->from_switchto)
        {
            Cpu cpu = topology()->cpu(payload->cpu);
            enclave()->GetAgent(cpu)->Ping();
        }
    }

    void LotteryScheduler::TaskPreempted(LotteryTask *task, const Message &msg)
    {
        const ghost_msg_payload_task_preempt *payload =
            static_cast<const ghost_msg_payload_task_preempt *>(msg.payload());
        TaskOffCpu(task, /*blocked=*/false, payload->from_switchto);
        task->preempted = true;

        CpuState *cs = cpu_state_of(task);
        cs->run_queue.Add(task);

        if (payload->from_switchto)
        {
            Cpu cpu = topology()->cpu(payload->cpu);
            enclave()->GetAgent(cpu)->Ping();
        }
    }

    void LotteryScheduler::TaskSwitchto(LotteryTask *task, const Message &msg)
    {
        TaskOffCpu(task, /*blocked=*/true, /*from_switchto=*/false);
    }

    void LotteryScheduler::TaskOffCpu(LotteryTask *task, bool blocked,
                                      bool from_switchto)
    {
        GHOST_DPRINT(3, stderr, "Task %s offcpu %d", task->gtid.describe(),
                     task->cpu);
        CpuState *cs = cpu_state_of(task);

        if (task->oncpu())
        {
            CHECK_EQ(cs->current, task);
            cs->current = nullptr;
        }
        else
        {
            CHECK(from_switchto);
            CHECK_EQ(task->run_state, LotteryTaskState::kBlocked);
        }

        task->run_state =
            blocked ? LotteryTaskState::kBlocked : LotteryTaskState::kRunnable;
    }

    void LotteryScheduler::TaskOnCpu(LotteryTask *task, Cpu cpu)
    {
        CpuState *cs = cpu_state(cpu);
        cs->current = task;

        GHOST_DPRINT(3, stderr, "Task %s oncpu %d", task->gtid.describe(), cpu.id());

        task->run_state = LotteryTaskState::kOnCpu;
        task->cpu = cpu.id();
        task->preempted = false;
    }

    void LotteryAgent::AgentThread()
    {
        gtid().assign_name("Agent:" + std::to_string(cpu().id()));
        if (verbose() > 1)
        {
            printf("Agent tid:=%d\n", gtid().tid());
        }
        SignalReady();
        WaitForEnclaveReady();

        PeriodicEdge debug_out(absl::Seconds(1));

        while (!Finished() || !scheduler_->Empty(cpu()))
        {
            scheduler_->Schedule(cpu(), status_word());

            if (verbose() && debug_out.Edge())
            {
                static const int flags = verbose() > 1 ? Scheduler::kDumpStateEmptyRQ : 0;
                if (scheduler_->debug_runqueue_)
                {
                    scheduler_->debug_runqueue_ = false;
                    scheduler_->DumpState(cpu(), Scheduler::kDumpAllTasks);
                }
                else
                {
                    scheduler_->DumpState(cpu(), flags);
                }
            }
        }
    }

}
