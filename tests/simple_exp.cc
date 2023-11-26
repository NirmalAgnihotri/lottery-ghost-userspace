#include <stdio.h>

#include <atomic>
#include <memory>
#include <vector>

#include "lib/base.h"
#include "lib/ghost.h"
#include "shared/prio_table.h"

// A series of simple tests for ghOSt schedulers.

namespace ghost
{
  namespace
  {

    struct ScopedTime
    {
      ScopedTime() { start = absl::Now(); }
      ~ScopedTime()
      {
        printf(" took %0.2f ms\n", absl::ToDoubleMilliseconds(absl::Now() - start));
      }
      absl::Time start;
    };

    void SimpleExp()
    {
      printf("\nStarting simple worker\n");
      GhostThread t(GhostThread::KernelScheduler::kGhost, []
                    {
    fprintf(stderr, "hello world!\n");
    absl::SleepFor(absl::Milliseconds(10));
    fprintf(stderr, "fantastic nap!\n");
    // Verify that a ghost thread implicitly clones itself in the ghost
    // scheduling class.
    std::thread t2(
        [] { CHECK_EQ(sched_getscheduler(/*pid=*/0), SCHED_GHOST); });
    t2.join(); });

      t.Join();
      printf("\nFinished simple worker\n");
    }

    void UpdateSchedItem(PrioTable *table, uint32_t sidx, const Gtid &gtid, int num_tickets)
    {
      struct sched_item *si;

      si = table->sched_item(sidx);

      const uint32_t seq = si->seqcount.write_begin();
      si->sid = sidx;
      si->gpid = gtid.id();
      si->deadline = num_tickets;
      si->seqcount.write_end(seq);
      table->MarkUpdatedIndex(sidx, /* num_retries = */ 3);
    }

    void SimpleExpPrio(const std::unique_ptr<PrioTable> &table_, uint32_t start_idx)
    {
      printf("\nStarting simple worker\n");
      GhostThread t(GhostThread::KernelScheduler::kGhost, []
                    {
    fprintf(stderr, "hello world!\n");
    absl::SleepFor(absl::Milliseconds(10));
    fprintf(stderr, "fantastic nap!\n"); });

      UpdateSchedItem(table_.get(), 0, t.gtid(), 10);
      t.Join();

      printf("\nFinished simple worker\n");
    }

    void SimpleExpManyPrio(const std::unique_ptr<PrioTable> &table_, uint32_t start_idx, int num_threads)
    {
      std::vector<std::unique_ptr<GhostThread>> threads;

      threads.reserve(num_threads);
      for (int i = 0; i < num_threads; i++)
      {
        threads.emplace_back(
            new GhostThread(GhostThread::KernelScheduler::kGhost, [&]
                            {
          unsigned long long g = 0;

          for (int i = 0; i < 10000000000; i+=27) {
            g += i * (i-1) * (i+1);
            // absl::SleepFor(absl::Milliseconds(1));
          } 
          
          // Verify that a ghost thread implicitly clones itself in the ghost
          // scheduling class.
          std::thread t(
              [] { CHECK_EQ(sched_getscheduler(/*pid=*/0), SCHED_GHOST); });
          t.join();

          absl::SleepFor(absl::Milliseconds(10)); }));
      }
      for (int i = 0; i < num_threads; i++)
      {
        std::cout << "I IS " << i << std::endl;
        auto &t = threads[i];
        UpdateSchedItem(table_.get(), start_idx + i,
                        t->gtid(), (i + 1) * 10);
      }
      
      absl::SleepFor(absl::Milliseconds(4000));
      int mult = 1;
      for (int i = num_threads-1; i >= 0; i--)
      {
        std::cout << "I IS " << i << std::endl;
        auto &t = threads[i];
        UpdateSchedItem(table_.get(), start_idx + i,
                        t->gtid(), mult * 100);
        mult++;
      }


      for (auto &t : threads)
        t->Join();
    }    

    void SimpleExpMany(int num_threads)
    {
      std::vector<std::unique_ptr<GhostThread>> threads;

      threads.reserve(num_threads);
      for (int i = 0; i < num_threads; i++)
      {
        threads.emplace_back(
            new GhostThread(GhostThread::KernelScheduler::kGhost, []
                            {
          unsigned long long g = 0;

          for (int i = 0; i < 10000000000; i+=27) {
            g += i * (i-1) * (i+1);
            // absl::SleepFor(absl::Milliseconds(1));
          } 
          
          // Verify that a ghost thread implicitly clones itself in the ghost
          // scheduling class.
          std::thread t(
              [] { CHECK_EQ(sched_getscheduler(/*pid=*/0), SCHED_GHOST); });
          t.join();

          absl::SleepFor(absl::Milliseconds(10)); }));
      }

      for (auto &t : threads)
        t->Join();
    }

    void SpinFor(absl::Duration d)
    {
      while (d > absl::ZeroDuration())
      {
        absl::Time a = MonotonicNow();
        absl::Time b;

        // Try to minimize the contribution of arithmetic/Now() overhead.
        for (int i = 0; i < 150; i++)
        {
          b = MonotonicNow();
        }

        absl::Duration t = b - a;

        // Don't count preempted time
        if (t < absl::Microseconds(200))
        {
          d -= t;
        }
      }
    }

    void BusyExpRunFor(int num_threads, absl::Duration d)
    {
      std::vector<std::unique_ptr<GhostThread>> threads;

      threads.reserve(num_threads);
      for (int i = 0; i < num_threads; i++)
      {
        threads.emplace_back(
            new GhostThread(GhostThread::KernelScheduler::kGhost, [&]
                            {
          // Time start = Now();
          // while (Now() - start < d) {}
          SpinFor(d); }));
      }

      for (auto &t : threads)
        t->Join();
    }

    void TaskDeparted()
    {
      printf("\nStarting simple worker\n");
      GhostThread t(GhostThread::KernelScheduler::kGhost, []
                    {
    fprintf(stderr, "hello world!\n");
    absl::SleepFor(absl::Milliseconds(10));

    fprintf(stderr, "fantastic nap! departing ghOSt now for CFS...\n");
    const sched_param param{};
    CHECK_EQ(sched_setscheduler(/*pid=*/0, SCHED_OTHER, &param), 0);
    CHECK_EQ(sched_getscheduler(/*pid=*/0), SCHED_OTHER);
    fprintf(stderr, "hello from CFS!\n"); });

      t.Join();
      printf("\nFinished simple worker\n");
    }

    void TaskDepartedMany(int num_threads)
    {
      std::vector<std::unique_ptr<GhostThread>> threads;

      threads.reserve(num_threads);
      for (int i = 0; i < num_threads; i++)
      {
        threads.emplace_back(
            new GhostThread(GhostThread::KernelScheduler::kGhost, []
                            {
          absl::SleepFor(absl::Milliseconds(10));

          const sched_param param{};
          CHECK_EQ(sched_setscheduler(/*pid=*/0, SCHED_OTHER, &param), 0);
          CHECK_EQ(sched_getscheduler(/*pid=*/0), SCHED_OTHER); }));
      }

      for (auto &t : threads)
        t->Join();
    }

    void TaskDepartedManyRace(int num_threads)
    {
      RemoteThreadTester().Run(
          [] { // ghost threads
            absl::SleepFor(absl::Nanoseconds(1));
          },
          [](GhostThread *t) { // remote, per-thread work
            const sched_param param{};
            CHECK_EQ(sched_setscheduler(t->tid(), SCHED_OTHER, &param), 0);
            CHECK_EQ(sched_getscheduler(t->tid()), SCHED_OTHER);
          });
    }

  } // namespace
} // namespace ghost

int main()
{
  const int kPrioMax = 51200;

  auto table_ = std::make_unique<ghost::PrioTable>(
      kPrioMax, 3,
      ghost::PrioTable::StreamCapacity::kStreamCapacity19);
  int start_idx = 0;

  // {
  //   printf("SimpleExp\n");
  //   ghost::ScopedTime time;
  //   ghost::SimpleExp();
  // }
  // {
  //   printf("SimpleExpMany\n");
  //   ghost::ScopedTime time;
  //   ghost::SimpleExpMany(2);
  // }
  // {
  //   printf("SimpleExpPRIOOne\n");
  //   ghost::ScopedTime time;
  //   ghost::SimpleExpPrio(table_, start_idx);
  // }
  {
    printf("SimpleExpPRIOMany\n");
    ghost::ScopedTime time;
    ghost::SimpleExpManyPrio(table_, start_idx, 2);
  }
  // {
  //   printf("BusyExp\n");
  //   ghost::ScopedTime time;
  //   ghost::BusyExpRunFor(100, absl::Milliseconds(10));
  // }
  // {
  //   printf("TaskDeparted\n");
  //   ghost::ScopedTime time;
  //   ghost::TaskDeparted();
  // }
  // {
  //   printf("TaskDepartedMany\n");
  //   ghost::ScopedTime time;
  //   ghost::TaskDepartedMany(1000);
  // }
  // {
  //   printf("TaskDepartedManyRace\n");
  //   ghost::ScopedTime time;
  //   ghost::TaskDepartedManyRace(1000);
  // }
  return 0;
}
