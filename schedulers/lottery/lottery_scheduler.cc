#include "schedulers/lottery/lottery_scheduler.h"
#include <memory>
#include <iostream>
#include <random>

namespace ghost
{
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
        int num_tickets = 0;
        for (auto task : cs->run_queue)
        {
            num_tickets += task->num_tickets;
        }

        // run the lottery
        std::random_device rd;
        std::mt19937 gen(rd()); // Mersenne Twister engine

        // Define a uniform distribution for integers between 1 and 10
        std::uniform_int_distribution<int> distribution(1, num_tickets);

        // Generate a random number between 1 and 10
        int ticket = distribution(gen);
        int curr_tickets = 0;

        LotteryTask *next;
        for (int i = 0; i < cs->run_queue.size(); ++i)
        {
            if (cs->run_queue[i]->num_tickets ==)
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
        cs->run_queue.push_back(task);

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
            cs->run_queue.push_back(task);
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
            cs->run_queue.erase(task);
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
        cs->run_queue.push_back(task);
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
        cs->run_queue.push_back(task);

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

}
