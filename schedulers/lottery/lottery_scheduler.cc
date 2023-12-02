#include "schedulers/lottery/lottery_scheduler.h"
#include <memory>
#include <random>

// debugging + timiing
#include <iostream>
#include <chrono>
#include <vector>

bool LOG_TOTAL_TASK_WINS_LOTTERY = false;
uint32_t SAMPLE_RATE = 50;
uint32_t TOTAL_SAMPLES = 20000000;

bool LOG_SCHEDULING_DECISION_TIME_TAKEN = false;

namespace ghost
{
    void RunCollection::Erase(LotteryTask *task)
    {
        if (task == nullptr)
            return;
        task->run_state = LotteryTaskState::kRunnable;
        absl::MutexLock lock(&mu_);
        rq_.erase(task);
    }
    void RunCollection::Add(LotteryTask *task)
    {
        CHECK_GE(task->cpu, 0);
        CHECK_EQ(task->run_state, LotteryTaskState::kRunnable);

        task->run_state = LotteryTaskState::kQueued;

        absl::MutexLock lock(&mu_);
        rq_.insert(task);
    }
    unsigned int RunCollection::NumTickets()
    {
        absl::MutexLock lock(&mu_);
        unsigned int acc = 0;
        std::vector<LotteryTask *> t;


        for (LotteryTask *v : rq_)
        {
            acc += v->num_tickets;
        }
        return acc;
    }

    LotteryTask *RunCollection::PickWinner(unsigned int winning_ticket)
    {
        absl::MutexLock lock(&mu_);
        unsigned int acc = 0;
        for (LotteryTask *v : rq_)
        {
            acc += v->num_tickets;
            if (acc >= winning_ticket)
            {
                v->run_state = LotteryTaskState::kRunnable;
                rq_.erase(v);
                return v;
            }
        }
        return nullptr;
    }

    void LotteryScheduler::DumpAllTasks()
    {
        fprintf(stderr, "task        state   cpu\n");
        allocator()->ForEachTask([](Gtid gtid, const LotteryTask *task)
                                 {
    absl::FPrintF(stderr, "%-12s%-8d%-8d%c%c\n", gtid.describe(),
                  task->run_state, task->cpu, task->preempted ? 'P' : '-');
    return true; });
    }

    void LotteryScheduler::DumpState(const Cpu &cpu, int flags)
    {
        if (flags & Scheduler::kDumpAllTasks)
        {
            DumpAllTasks();
        }
        CpuState *cs = cpu_state(cpu);

        if (!(flags & Scheduler::kDumpStateEmptyRQ) && !cs->current &&
            cs->run_queue.Empty())
        {
            return;
        }

        const LotteryTask *current = cs->current;
        const RunCollection *rq = &cs->run_queue;
        absl::FPrintF(stderr, "SchedState[%d]: %s rq_l=%lu\n", cpu.id(),
                      current ? current->gtid.describe() : "none", rq->Size());
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

    void LotteryScheduler::EnclaveReady()
    {
        for (const Cpu &cpu : cpus())
        {
            CpuState *cs = cpu_state(cpu);
            Agent *agent = enclave()->GetAgent(cpu);

            // AssociateTask may fail if agent barrier is stale.
            while (!cs->channel->AssociateTask(agent->gtid(), agent->barrier(),
                                               /*status=*/nullptr))
            {
                CHECK_EQ(errno, ESTALE);
            }
        }

        // Enable tick msg delivery here instead of setting AgentConfig.tick_config_
        // because the agent subscribing the default channel (mostly the
        // channel/agent for the front CPU in the enclave) can get CpuTick messages
        // for another CPU in the enclave while this function is trying to associate
        // each agent to its corresponding channel.
        enclave()->SetDeliverTicks(true);
    }

    void LotteryScheduler::DiscoveryStart()
    {
        in_discovery_ = true;
    }

    void LotteryScheduler::DiscoveryComplete()
    {
        for (auto &scraper : orchs_)
        {
            scraper.second->RefreshAllSchedParams(kSchedCallbackFunc);
        }
        in_discovery_ = false;
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

    long unsigned int LotteryScheduler::ParkMillerRand()
    {
        static long unsigned int seed = 1;
        long unsigned int hi, lo;
        lo = 16807 * (seed & 0xFFFF);
        hi = 16807 * (seed >> 16);

        lo += (hi & 0x7FFF) << 16;
        lo += hi >> 15;

        if (lo > 0x7FFFFFFF)
            lo -= 0x7FFFFFFF;

        return (seed = lo);
    }

    void LotteryScheduler::LotterySchedule(const Cpu &cpu, BarrierToken agent_barrier, bool prio_boost)
    {
        CpuState *cs = cpu_state(cpu);
        cs->count += 1;
        LotteryTask *next = nullptr;
        if (!prio_boost)
        {
            
            // get the total number of tickets
            unsigned int num_tickets = cs->run_queue.NumTickets();


            auto start = std::chrono::high_resolution_clock::now();
            // run the lottery
            int winning_ticket = num_tickets > 0 ? 1 + (ParkMillerRand() % num_tickets) : 1;
            // std::cout << "Winning: " << winning_ticket << std::endl;
            if (cs->current != nullptr)
                cs->run_queue.Add(cs->current);
            next = cs->run_queue.PickWinner(winning_ticket);

            auto end = std::chrono::high_resolution_clock::now();

            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

            if (LOG_SCHEDULING_DECISION_TIME_TAKEN && (cs -> count)% SAMPLE_RATE == 0 && (cs -> count) < SAMPLE_RATE * TOTAL_SAMPLES) {
                auto num_processes = cs -> run_queue.Size();
                std:: string log = "COUNT: " + std::to_string(num_processes) + ",";
                log += std::to_string(duration.count()) + "\n";
                write_log(log);
            }

            // std::cout << next << " has won lottery" << std::endl;
            cs->mp[next] += 1;
            if (LOG_TOTAL_TASK_WINS_LOTTERY && (cs -> count)% SAMPLE_RATE == 0 && (cs -> count) < SAMPLE_RATE * TOTAL_SAMPLES) {
                std::string to_write = "";
                for (auto [k, v] : cs->mp)
                {
                   uintptr_t t = reinterpret_cast<uintptr_t>(k);
                   if (t == 0) continue;
                   to_write += std::to_string(t) + "," + std::to_string(v) + "\n";
                }
                to_write + "\n";
                write_log(to_write);
            }

        }
        else
        // cs->run_queue.Erase(next);
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
                GHOST_DPRINT(3, stderr, "LotterySchedule: commit failed (state=%d)",
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
            int flags = 0;
            if (prio_boost && (cs->current || !cs->run_queue.Empty()))
            {
                flags = RTLA_ON_IDLE;
            }
            req->LocalYield(agent_barrier, flags);
        }
    }

    void LotteryScheduler::Schedule(const Cpu &cpu, const StatusWord &agent_sw)
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

    void LotteryScheduler::UpdateSchedParams()
    {
        for (auto &scraper : orchs_)
        {
            scraper.second->RefreshSchedParams(kSchedCallbackFunc);
        }
    }

    void LotteryScheduler::SchedParamsCallback(Orchestrator &orch, const SchedParams *sp, Gtid oldgtid)
    {
        Gtid gtid = sp->GetGtid();

        // TODO: Get it off cpu if it is running. Assumes that oldgpid wasn't moved
        // around to a later spot in the PrioTable. Get it off the runqueue if it is
        // queued. Implement Scheduler::EraseFromRunqueue(oldgpid);
        CHECK(!oldgtid || (oldgtid == gtid));

        if (!gtid)
        { // empty sched_item.
            return;
        }

        LotteryTask *task = allocator()->GetTask(gtid);
        if (!task)
        {
            // We are too early (i.e. haven't seen MSG_TASK_NEW for gtid) in which
            // case ignore the update for now. We'll grab the latest SchedParams
            // from shmem in the MSG_TASK_NEW handler.
            //
            // We are too late (i.e have already seen MSG_TASK_DEAD for gtid) in
            // which case we just ignore the update.
            return;
        }
        task->sp = sp;
        task->num_tickets = sp->GetDeadline();
    }

    void LotteryScheduler::HandleNewGtid(LotteryTask *task, pid_t tgid)
    {
        CHECK_GE(tgid, 0);

        if (orchs_.find(tgid) == orchs_.end())
        {
            auto orch = std::make_unique<Orchestrator>();
            if (!orch->Init(tgid))
            {
                // If the task's group leader has already exited and closed the PrioTable
                // fd while we are handling TaskNew, it is possible that we cannot find
                // the PrioTable.
                // Just set has_work so that we schedule this task and allow it to exit.
                // We also need to give it an sp; various places call task->sp->qos_.
                static SchedParams dummy_sp;
                task->sp = &dummy_sp;
                return;
            }
            auto pair = std::make_pair(tgid, std::move(orch));
            orchs_.insert(std::move(pair));
        }
    }

    void LotteryScheduler::TaskNew(LotteryTask *task, const Message &msg)
    {
        const ghost_msg_payload_task_new *payload =
            static_cast<const ghost_msg_payload_task_new *>(msg.payload());

        task->seqnum = msg.seqnum();
        task->run_state = LotteryTaskState::kBlocked;

        // tgid = task group ID, one for all threads in a process
        // We have one orchestrator per process since there is one priotable per process as all threads share the same mem region
        // gtid = ghost task ID
        const Gtid gtid(payload->gtid);
        const pid_t tgid = gtid.tgid();
        HandleNewGtid(task, tgid);
        // for (auto &scraper : orchs_)
        // {
        //     scraper.second->RefreshAllSchedParams(kSchedCallbackFunc);
        // }
        // task->num_tickets = 10;

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

        if (!in_discovery_) {
            auto iter = orchs_.find(tgid);
            if (iter != orchs_.end())
            {
                iter->second->GetSchedParams(task->gtid, kSchedCallbackFunc);
            }
        }
    }
    void LotteryScheduler::Migrate(LotteryTask *task, Cpu cpu, BarrierToken seqnum)
    {
        CHECK_EQ(task->run_state, LotteryTaskState::kRunnable);
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
        // task->num_tickets += 1000;
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
        // task->num_tickets = 10;
    }

    std::unique_ptr<LotteryScheduler> MultiThreadedLotteryScheduler(Enclave *enclave,
                                                                    CpuList cpulist)
    {
        auto allocator = std::make_shared<ThreadSafeMallocTaskAllocator<LotteryTask>>();
        auto scheduler = std::make_unique<LotteryScheduler>(enclave, std::move(cpulist),
                                                            std::move(allocator));
        return scheduler;
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
            scheduler_->UpdateSchedParams();
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
    std::ostream &operator<<(std::ostream &os, const LotteryTaskState &state)
    {
        switch (state)
        {
        case LotteryTaskState::kBlocked:
            return os << "kBlocked";
        case LotteryTaskState::kRunnable:
            return os << "kRunnable";
        case LotteryTaskState::kQueued:
            return os << "kQueued";
        case LotteryTaskState::kOnCpu:
            return os << "kOnCpu";
        }
    }

}
