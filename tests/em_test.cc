#include <stdio.h>

#include <atomic>
#include <memory>
#include <vector>
#include <cmath>

#include <cstdlib>

#include <iostream>
#include <fstream>
#include <algorithm>

#include <fstream>
#include <mutex>

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

        void UpdateSchedItem(PrioTable *table, uint32_t sidx, const Gtid &gtid, int num_tickets) {
            struct sched_item *si;

            si = table->sched_item(sidx);

            const uint32_t seq = si->seqcount.write_begin();
            si->sid = sidx;
            si->gpid = gtid.id();
            si->deadline = num_tickets;
            si->seqcount.write_end(seq);
            table->MarkUpdatedIndex(sidx, /* num_retries = */ 3);
        }

        // Function to perform matrix multiplication
        std::vector<std::vector<int>> multiplyMatrices(const std::vector<std::vector<int>>& mat1, const std::vector<std::vector<int>>& mat2) {
            size_t rows1 = mat1.size();
            size_t cols1 = mat1[0].size();
            size_t rows2 = mat2.size();
            size_t cols2 = mat2[0].size();

            if (cols1 != rows2) {
                cols1 = rows2;
                // std::cerr << "Error: Matrix dimensions mismatch for multiplication.\n";
                return {};
            }

            std::vector<std::vector<int>> result(rows1, std::vector<int>(cols2, 0));

            for (size_t i = 0; i < rows1; ++i) {
                for (size_t j = 0; j < cols2; ++j) {
                    for (size_t k = 0; k < cols1; ++k) {
                        result[i][j] += mat1[i][k] * mat2[k][j];
                    }
                }
            }

            return result;
        }

        double compute_py_with_x(std::vector<int> &x, int &y, std::vector<double> &ps) {
            double acc = 1;

            for (int i = 0; i < x.size(); i++) {
                double p_val = ps[i];
                int x_val = x[i];

                // std::cout << p_val << " " << x_val << std::endl;

                acc *= pow((1-p_val), x_val);
            }

            if (y == 0) return acc;
            return 1 - acc;
        }

        double compute_llh(std::vector<std::vector<int>> &xs, std::vector<int> &ys, std::vector<double> &ps) {
            long double llh = 0;
            for (int i = 0; i < ys.size(); i++) {
                int y = ys[i];
                llh += log(compute_py_with_x(xs[i], y, ps));
            }

            return llh/ys.size();
        }

        void do_update(std::vector<std::vector<int>> &xs, std::vector<int> &ys, std::vector<double> &ps) {
            std::vector<double> new_ps;
            for (int i =0; i < ps.size(); i++) {
                double acc = 0;
                unsigned int t_i = 0;

                for (int j = 0; j < xs.size(); j++) {
                    std::vector<int> x = xs[j];
                    int y = ys[j];

                    if (x[i] == 1) t_i += 1;

                    double num = y * x[i] * ps[i];

                    double m_acc = 1;
                    for (int k = 0; k < x.size(); k++) {
                        m_acc *= pow(1 - ps[k], x[k]);
                    }
                    double den = 1 - m_acc;
                    acc += num/den;
                }
                // std::cout << t_i << std::endl;
                new_ps.push_back(acc/t_i);
            }
            ps = new_ps;
        }

        std::vector<std::vector<int>> read_xs() {

            std::vector<std::vector<int>> xs;

            const std::string file_path = "x_values.txt";
            std::ifstream file(file_path);

            if (!file.is_open()) {
                std::cout << "Unable to open the file: " << file_path << std::endl;
                return xs; // Return an error code
            }

            
            std::string line;

            while (std::getline(file, line)) {
                std::vector<int> vector;

                // Process each character in the line
                for (char c : line) {
                    if (c == '0' || c == '1') {
                        // Convert the character to an integer and add to the vector
                        vector.push_back(c - '0');
                    }
                }

                // Add the vector to the vectorList
                xs.push_back(vector);
            }

            file.close();

            return xs;
        }

        std::vector<int> read_ys() {
            std::vector<int> ys;

            const std::string file_path = "y_values.txt";
            std::ifstream file(file_path);

            if (!file.is_open()) {
                std::cout << "Unable to open the file: " << file_path << std::endl;
                return ys; // Return an error code
            }

            std::string line;
            while(std::getline(file, line)) {
                for (char c: line) {
                    if (c == '0' || c == '1') {
                        ys.push_back(c - '0');
                    }
                }
            }

            file.close();
            return ys;
        } 

        std::vector<std::vector<double>> read_ps(int num_threads, int size_p) {
            std::vector<std::vector<double>> ps;
            std::srand(static_cast<unsigned int>(std::time(nullptr)));
            for (int i = 0; i < num_threads; i++) {
                std::vector<double> p;
                for (int j = 0; j < size_p; j++) {
                    // int random_value = std::rand();
                    // // Normalize the random value to be between 0 and 1
                    // double random_normalized = static_cast<double>(random_value) / RAND_MAX;
                    p.push_back(((i+1) * 217)/10000.0);
                }
                ps.push_back(p);
            }
            return ps;
        }

        void RunEmExperiment(const std::unique_ptr<PrioTable> &table_, uint32_t start_idx) {
            std::vector<std::unique_ptr<GhostThread>> threads;
            uint32_t numthreads = 20;
            threads.reserve(numthreads);

            std::vector<std::vector<int>> xs = read_xs();

            std:: cout << xs.size() << " " << xs[0].size() << std::endl;
            
            std::vector<int> ys = read_ys();
            std::vector<std::vector<double>> initial_ps = read_ps(numthreads, xs[0].size());
            
            std::vector<double> llhs(numthreads, 0);
            std::vector<double> llh_diffs(numthreads, 1000);
            for (int i = 0; i < numthreads; i++) {
                threads.emplace_back(
                    new GhostThread(GhostThread::KernelScheduler::kGhost, [&, i]{
                        unsigned int thread_idx = i;
                        std::vector<double> ps = initial_ps[thread_idx];
                        auto end_time = std::chrono::high_resolution_clock::now();
                        while (true) {
                            // compute differences and update global data strcutures
                            double llh = compute_llh(xs, ys, ps);
                            double diff = abs(abs(llh) - abs(llhs[thread_idx]));
                            llh_diffs[thread_idx] =diff;
                            llhs[thread_idx] = llh;     
                            
                            //  update ps'
                            do_update(xs, ys, ps);

                            end_time = std::chrono::high_resolution_clock::now();
                        }
                        return 0;
                    })
                );
            }

            for (int i = 0; i < numthreads; i++) {
                    auto &t = threads[i];
                    UpdateSchedItem(table_.get(), start_idx + i, t->gtid(), 10000);
            }

            auto start_time = std::chrono::high_resolution_clock::now();

            std::chrono::milliseconds desired_duration(60000);
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            while (duration <= desired_duration) {
                for (int i = 0; i < numthreads; i++) {
                    auto &t = threads[i];
                    double diff = llh_diffs[i];
                    int tickets = std::max(static_cast<int>(std::round(diff * 10000)) ,1);
                    UpdateSchedItem(table_.get(), start_idx + i, t->gtid(), tickets);
                }

                end_time = std::chrono::high_resolution_clock::now();
                duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            }

            

            for (double &d: llhs) {
                std:: cout << d << std::endl;
            }

            auto minElement = std::min_element(llhs.begin(), llhs.end());
            // Find maximum element
            auto maxElement = std::max_element(llhs.begin(), llhs.end());
            // Calculate mean using std::accumulate
            double mean = std::accumulate(llhs.begin(), llhs.end(), 0.0) / llhs.size();

            std::cout <<"Duration " << std::to_string(duration.count()) << std::endl;
            std::cout << "MIN: " << *minElement << " MAX: " << *maxElement << " MEAN: " << mean << std::endl;
            for (auto &t : threads) {
                t ->Join();
            }

        }
    
        void RunPremptiveEM(const std::unique_ptr<PrioTable> &table_, uint32_t start_idx) {
            std::vector<std::unique_ptr<GhostThread>> threads;
            uint32_t numthreads = 20;
            threads.reserve(numthreads);

            std::vector<std::vector<int>> xs = read_xs();
            std::vector<int> ys = read_ys();
            std::vector<std::vector<double>> initial_ps = read_ps(numthreads, xs[0].size());
            
            std::vector<double> llhs(numthreads, 0);
            std::vector<double> llh_diffs(numthreads, 1000);
            bool should_run = true;
            for (int i = 0; i < numthreads; i++) {
                int j = i;
                threads.emplace_back(
                    new GhostThread(GhostThread::KernelScheduler::kGhost, [&, i]{
                        unsigned int thread_idx = i;
                        std::vector<double> ps = initial_ps[thread_idx];
                        auto end_time = std::chrono::high_resolution_clock::now();
                        double diff = 1000000;
                        while (should_run && diff > 0.00001) {
                            // compute differences and update global data strcutures
                            double llh = compute_llh(xs, ys, ps);
                            double diff = abs(abs(llh) - abs(llhs[thread_idx]));
                            llh_diffs[thread_idx] =diff;
                            llhs[thread_idx] = llh;     
                            
                            //  update ps'
                            do_update(xs, ys, ps);

                            end_time = std::chrono::high_resolution_clock::now();
                        }
                        return 0;
                    })
                );
            }

            auto start_time = std::chrono::high_resolution_clock::now();

            std::chrono::milliseconds desired_duration(55000);
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            while (duration <= desired_duration) {
                for (int i = 0; i < numthreads; i++) {
                    auto &t = threads[i];
                }

                end_time = std::chrono::high_resolution_clock::now();
                duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            }

            should_run = false;

            for (auto &t : threads) {
                t ->Join();
            }

        
            for (double &d: llhs) {
                std:: cout << d << std::endl;
            }

            auto minElement = std::min_element(llhs.begin(), llhs.end());
            // Find maximum element
            auto maxElement = std::max_element(llhs.begin(), llhs.end());
            // Calculate mean using std::accumulate
            double mean = std::accumulate(llhs.begin(), llhs.end(), 0.0) / llhs.size();

            std::cout <<"Duration " << std::to_string(duration.count()) << std::endl;
            std::cout << "MIN: " << *minElement << " MAX: " << *maxElement << " MEAN: " << mean << std::endl;
        }

        void RunGenericTest(const std::unique_ptr<PrioTable> &table_, uint32_t start_idx, uint32_t num_threads) {
            std::vector<std::unique_ptr<GhostThread>> threads;
            threads.reserve(num_threads);

            // std::vector<std::vector<int>> xs = read_xs();

            for (int i =0; i < num_threads; i++) {
                threads.emplace_back(
                    new GhostThread(GhostThread::KernelScheduler::kGhost, [&, i] {
                        std::string thread_idx = std::to_string(i);

                        std::vector<std::vector<int>> xs = read_xs();

                        std::vector<std::vector<int>> xsTransposed(xs[0].size(), std::vector<int>(xs.size(), 0));
                        for (size_t i = 0; i < xs.size(); ++i) {
                            for (size_t j = 0; j < xs[i].size(); ++j) {
                                xsTransposed[j][i] = xs[i][j];
                            }
                        }
                        std::vector<std::vector<int>> ans = multiplyMatrices(xs, xsTransposed);

                        // std::ofstream outFile("./datatmp/" + thread_idx+"out.txt");
                        // if (outFile.is_open()) {
                        //     for (int element : ans[0]) {
                        //         outFile << element << ' ';
                        //     }
                        //     outFile.close();
                        //     // std::cout << "First row saved to out.txt\n";
                        // } 
                    }
                ));
            }

            for (int i = 0; i < num_threads; i++) {
                    auto &t = threads[i];
                    UpdateSchedItem(table_.get(), start_idx + i, t->gtid(), 1000);
            }

            for (auto &t : threads) {
                t ->Join();
            }  
        }

    void RunGenericTestWithSynch(const std::unique_ptr<PrioTable> &table_, uint32_t start_idx, uint32_t num_threads) {
            std::vector<std::unique_ptr<GhostThread>> threads;
            threads.reserve(num_threads);
            std::mutex fileMutex;

            // std::vector<std::vector<int>> xs = read_xs();

            for (int i =0; i < num_threads; i++) {
                threads.emplace_back(
                    new GhostThread(GhostThread::KernelScheduler::kGhost, [&, i] {
                        std::string thread_idx = std::to_string(i);

                        std::vector<std::vector<int>> xs = read_xs();

                        std::vector<std::vector<int>> xsTransposed(xs[0].size(), std::vector<int>(xs.size(), 0));
                        for (size_t i = 0; i < xs.size(); ++i) {
                            for (size_t j = 0; j < xs[i].size(); ++j) {
                                xsTransposed[j][i] = xs[i][j];
                            }
                        }
                        std::vector<std::vector<int>> ans = multiplyMatrices(xs, xsTransposed);
                        std::lock_guard<std::mutex> lock(fileMutex);


                        std::ofstream outFile("./out.txt", std::ios::app);
                        if (outFile.is_open()) {
                            for (int element : ans[0]) {
                                outFile << element << ' ';
                            }
                            outFile.close();
                            // std::cout << "First row saved to out.txt\n";
                        } 
                    }
                ));
            }

            for (int i = 0; i < num_threads; i++) {
                    auto &t = threads[i];
                    UpdateSchedItem(table_.get(), start_idx + i, t->gtid(), 1000);
            }

            for (auto &t : threads) {
                t ->Join();
            }  
        }         
    }
}

int main()
{
    const int kPrioMax = 51200;

    auto table_ = std::make_unique<ghost::PrioTable>(
        kPrioMax, 3,
        ghost::PrioTable::StreamCapacity::kStreamCapacity19);
    int start_idx = 0;

    // {
    //     printf("RunEmExperiment\n");
    //     ghost::ScopedTime time;
    //     ghost::RunEmExperiment(table_, start_idx);
    // }

    //    {
    //     printf("RunPremptiveEM\n");
    //     ghost::ScopedTime time;
    //     ghost::RunPremptiveEM(table_, start_idx);
    // } 

    {
        printf("RunGenericTest\n");
        ghost::ScopedTime time;
        ghost::RunGenericTest(table_, start_idx, 500);
    } 

}