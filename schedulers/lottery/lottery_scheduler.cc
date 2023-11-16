#include "schedulers/lottery/lottery_scheduler.h"
#include <memory>
#include <iostream>
namespace ghost
{
    LotteryScheduler::LotteryScheduler(Enclave *enclave, CpuList cpulist,
                                       std::shared_ptr<TaskAllocator<LotteryTask>> allocator) : BasicDispatchScheduler(enclave, std::move(cpulist),
                                                                                                                       std::move(allocator))
    {
    }

    void LotteryScheduler::Schedule(const Cpu &cpu, const StatusWord &sw)
    {
        std::cout << "HELLO";
    }

    void LotteryScheduler::TaskNew(LotteryTask *task, const Message &msg)
    {
        const ghost_msg_payload_task_new *payload =
            static_cast<const ghost_msg_payload_task_new *>(msg.payload());

        task->seqnum = msg.seqnum();
        task->run_state = LotteryTaskState::kBlocked;
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
    void FifoScheduler::Migrate(FifoTask *task, Cpu cpu, BarrierToken seqnum)
    {
        CHECK_EQ(task->run_state, FifoTaskState::kRunnable);
        CHECK_EQ(task->cpu, -1);

        CpuState *cs = cpu_state(cpu);
        const Channel *channel = cs->channel.get();
        CHECK(channel->AssociateTask(task->gtid, seqnum, /*status=*/nullptr));

        GHOST_DPRINT(3, stderr, "Migrating task %s to cpu %d", task->gtid.describe(),
                     cpu.id());
        task->cpu = cpu.id();

        // Make task visible in the new runqueue *after* changing the association
        // (otherwise the task can get oncpu while producing into the old queue).
        cs->run_queue.Enqueue(task);

        // Get the agent's attention so it notices the new task.
        enclave()->GetAgent(cpu)->Ping();
    }

    void FifoScheduler::TaskRunnable(FifoTask *task, const Message &msg)
    {
    }

    void FifoScheduler::TaskDeparted(FifoTask *task, const Message &msg)
    {
    }

    void FifoScheduler::TaskDead(FifoTask *task, const Message &msg)
    {
    }

    void FifoScheduler::TaskYield(FifoTask *task, const Message &msg)
    {
    }

    void FifoScheduler::TaskBlocked(FifoTask *task, const Message &msg)
    {
    }

    void FifoScheduler::TaskPreempted(FifoTask *task, const Message &msg)
    {
    }

    void FifoScheduler::TaskSwitchto(FifoTask *task, const Message &msg)
    {
    }

}
