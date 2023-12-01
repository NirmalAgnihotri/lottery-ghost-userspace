#include <stdio.h>

#include <atomic>
#include <memory>
#include <vector>
#include <unordered_set>

#include "lib/base.h"
#include "lib/ghost.h"
#include "shared/prio_table.h"

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

    void TwoThreadsDoublePriority(const std::unique_ptr<PrioTable> &table_, uint32_t start_idx) {
      std::vector<std::unique_ptr<GhostThread>> threads;
      threads.reserve(2);

       for (int i = 0; i < 2; i++)
      {
        threads.emplace_back(
            new GhostThread(GhostThread::KernelScheduler::kGhost, [&]
            {
              unsigned long long g = 0;

              for (int i = 0; i < 10000000000; i+=27) {
                g += i * (i-1) * (i+1);
              } 

              std::thread t(
                  [] { CHECK_EQ(sched_getscheduler(/*pid=*/0), SCHED_GHOST); });
              t.join();
            })
          );
      }

      for (int i = 0; i < 2; i++) {
          auto &t = threads[i];
          if (i == 0) {
          UpdateSchedItem(table_.get(), start_idx + i,
                          t->gtid(), 10);
          }

          if (i == 1) {
            UpdateSchedItem(table_.get(), start_idx + i, t->gtid(), 10 * 10);
          }

      }

      for (auto &t : threads)
        t->Join();
    }
  
    void ManyThreadsSamePriority(const std::unique_ptr<PrioTable> &table_, uint32_t start_idx, uint32_t num_threads ) {
      std::vector<std::unique_ptr<GhostThread>> threads;
      threads.reserve(num_threads);

       for (int i = 0; i < num_threads; i++)
      {
        threads.emplace_back(
            new GhostThread(GhostThread::KernelScheduler::kGhost, [&]
                            {
          unsigned long long g = 0;

          for (int i = 1; i < 10000000; i+=27) {
            // std::cout << "ITERATION: " << i << std::endl;
            g += i * (i-1) * (i+1);
          } 
          
          std::thread t(
              [] { CHECK_EQ(sched_getscheduler(/*pid=*/0), SCHED_GHOST); });
          t.join();

        }));
      }


      for (int i = 0; i < num_threads; i++)
      {
        // std::cout << "I IS " << i << std::endl;
        auto &t = threads[i];
        UpdateSchedItem(table_.get(), start_idx + i,
                        t->gtid(), 10);
      }

      for (auto &t : threads)
        t->Join();


    }

    void TwoThreadsInvertPriority(const std::unique_ptr<PrioTable> &table_, uint32_t start_idx) {
      std::vector<std::unique_ptr<GhostThread>> threads;

      threads.reserve(2);
      for (int i = 0; i < 2; i++)
      {
        threads.emplace_back(
            new GhostThread(GhostThread::KernelScheduler::kGhost, [&]
                            {
          unsigned long long g = 0;

          for (int i = 0; i < 10000000000; i+=27) {
            g += i * (i-1) * (i+1);
          } 
          
          std::thread t(
              [] { CHECK_EQ(sched_getscheduler(/*pid=*/0), SCHED_GHOST); });
          t.join();

        }));
      }

      for (int i = 0; i < 2; i++)
      {
        // std::cout << "I IS " << i << std::endl;
        auto &t = threads[i];
        UpdateSchedItem(table_.get(), start_idx + i,
                        t->gtid(), (i + 1) * 10);
      }

      absl::SleepFor(absl::Milliseconds(10000));

      std::cout << "switching.." << std::endl;
      int mult = 1;
      for (int i = 1; i >= 0; i--)
      {
        // std::cout << "I IS " << i << std::endl;
        auto &t = threads[i];
        UpdateSchedItem(table_.get(), start_idx + i,
                        t->gtid(), 20 - 10*i);
        mult++;
      }

      absl::SleepFor(absl::Milliseconds(20000));

      std::cout << "DONE" << std::endl;

      for (auto &t : threads)
        t->Join();

    }

    void AddThreadsFactor10(const std::unique_ptr<PrioTable> &table_, uint32_t start_idx) {
      std::vector<std::unique_ptr<GhostThread>> threads;
      uint32_t num_threads = 3500;
      threads.reserve(num_threads);

      std::set<int> stops = {500, 1000, 1500, 2000, 2500, 3000, 3500};

       for (int i = 0; i < num_threads; i++)
      {
        threads.emplace_back(
            new GhostThread(GhostThread::KernelScheduler::kGhost, [&]
                            {
          unsigned long long g = 0;

          for (int i = 1; i < 10000000; i+=27) {
            // std::cout << "ITERATION: " << i << std::endl;
            g += i * (i-1) * (i+1);
          } 

          while (true) {
            i += 1;
          }
          
          // std::thread t(
          //     [] { CHECK_EQ(sched_getscheduler(/*pid=*/0), SCHED_GHOST); });
          // t.join();

        }));

        auto &t = threads[i];
        UpdateSchedItem(table_.get(), start_idx + i,
                        t->gtid(), 10);

        if (stops.count(i+1)) {
          std::cout << "Sampling...." << std:: endl;
          absl::SleepFor(absl::Milliseconds(20000)); 
        }
          
      }

    }
  } //namespace
} //ghost namespace


int main()
{
  const int kPrioMax = 51200;

  auto table_ = std::make_unique<ghost::PrioTable>(
      kPrioMax, 3,
      ghost::PrioTable::StreamCapacity::kStreamCapacity19);
  int start_idx = 0;

  // {
  //   printf("TwoThreadsDoublePriority\n");
  //   ghost::ScopedTime time;
  //   ghost::TwoThreadsDoublePriority(table_, start_idx);
  // }

  // {
  //   printf("ManyThreadsSamePriority\n");
  //   ghost::ScopedTime time;
  //   ghost::ManyThreadsSamePriority(table_, start_idx, 2);
  // }

  // {
  //   printf("TwoThreadsInvertPriority\n");
  //   ghost::ScopedTime time;
  //   ghost::TwoThreadsInvertPriority(table_, start_idx);
  // }

  {
    printf("AddThreadsFactor10\n");
    ghost::ScopedTime time;
    ghost::AddThreadsFactor10(table_, start_idx);
  }
  return 0;
}