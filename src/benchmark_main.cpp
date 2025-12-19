/**
 * @file benchmark_main.cpp
 * @brief Entry point for the Lock-Free Queue Benchmark Suite.
 * * This program measures the throughput, latency, and memory characteristics 
 * of various concurrent queue implementations under different contention scenarios.
 * It supports customizable producer/consumer ratios, payload simulation, and 
 * safe memory reclamation strategies (HP, EBR, None).
 */

#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <iostream>
#include <random>
#include <algorithm> // for sort
#include <numeric>   // for accumulate
#include <iomanip>   // for setprecision
#include <memory>
#include <sys/resource.h>

#include "queue/lockfree_queue.hpp"
#include "queue/mutex_queue.hpp"
#include "reclaimer/hazard_pointers.hpp"
#include "reclaimer/epoch_based_reclamation.hpp"
#include "reclaimer/no_reclamation.hpp"

using Clock = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;
using us = std::chrono::microseconds;
using namespace std::chrono_literals;

/**
 * @brief Configuration parameters for the benchmark run.
 * * Holds command-line arguments that control the test environment,
 * including thread counts, duration, and the specific implementation to test.
 */
struct BenchmarkArgs
{
    std::string impl = "hp";    ///< Implementation type: "hp", "ebr", "none", or "mutex"
    int num_producers = 4;      ///< Number of producer threads
    int num_consumers = 4;      ///< Number of consumer threads
    int payload_us = 0;         ///< Simulated workload per operation in microseconds (0 = max throughput)
    int warmup_s = 1;           ///< Warmup duration in seconds (data not recorded)
    int duration_s = 5;         ///< Measurement duration in seconds
    std::string csv_path = "";  ///< Optional path to append results in CSV format
    int sampling_rate = 1 << 10;///< Latency sampling interval (power of 2 optimized)
};

/**
 * @brief Thread-local statistics container.
 * * Aggregates the number of operations performed and captures latency samples
 * for a single worker thread.
 */
struct ThreadResult
{
    long long operations = 0;            ///< Total successful operations
    std::vector<long long> latencies_ns; ///< Sampled latencies in nanoseconds
};

static void print_help()
{
    std::cout <<
        R"(Usage: bench_queue [--impl hp|ebr|none|mutex] [--producers P] [--consumers C]
                              [--payload-us N] [--warmup S] [--duration S] [--csv path])";
}

static BenchmarkArgs parse_args(int argc, char **argv)
{
    BenchmarkArgs args;
    for (int i = 1; i < argc; i++)
    {
        std::string key = argv[i];
        auto get_value = [&](int idx)
        {
            if (idx + 1 >= argc)
            {
                print_help();
                std::exit(1);
            }
            return std::string(argv[idx + 1]);
        };

        if (key == "--help" || key == "-h")
        {
            print_help();
            std::exit(0);
        }
        else if (key == "--impl")
            args.impl = get_value(i++);
        else if (key == "--producers")
            args.num_producers = std::atoi(get_value(i++).c_str());
        else if (key == "--consumers")
            args.num_consumers = std::atoi(get_value(i++).c_str());
        else if (key == "--payload-us")
            args.payload_us = std::atoi(get_value(i++).c_str());
        else if (key == "--warmup")
            args.warmup_s = std::atoi(get_value(i++).c_str());
        else if (key == "--duration")
            args.duration_s = std::atoi(get_value(i++).c_str());
        else if (key == "--csv")
            args.csv_path = get_value(i++);
    }
    return args;
}

// Simulate CPU-bound workload
inline void simulate_work(int usleep)
{
    if (usleep <= 0)
        return;
    auto start = Clock::now();
    while (std::chrono::duration_cast<us>(Clock::now() - start).count() < usleep)
    {
        cpu_relax();
    }
}

/**
 * @brief Executes the benchmark for a specific queue type.
 * * Manages the lifecycle of producer/consumer threads, handles warmup/measurement 
 * phases, and computes final statistics (throughput, latency percentiles, memory usage).
 * * @tparam QueueType The specific queue instantiation to test (e.g., LockFreeQueue<..., HazardPointer>)
 * @param args Configuration arguments
 * @param impl_name Display name for the implementation
 */
template <class QueueType>
static void run_benchmark(const BenchmarkArgs &args, const char *impl_name)
{
    QueueType queue;
    std::atomic<bool> start_flag{false};
    std::atomic<bool> stop_signal{false};
    
    // Depth tracking metrics
    // current_depth: Approximate current depth (updated in batches)
    // max_depth: Historical maximum depth observed
    std::atomic<long long> current_depth{0};
    std::atomic<long long> max_depth{0};

    // Real-time progress indicators
    // Using vector of unique_ptr to store atomic counters (atomics are not copyable/movable)
    std::vector<std::unique_ptr<std::atomic<long long>>> consumer_progress;
    std::vector<std::unique_ptr<std::atomic<long long>>> producer_progress;
    for(int i=0; i < args.num_producers; ++i) 
        producer_progress.push_back(std::make_unique<std::atomic<long long>>(0));
    for(int i=0; i < args.num_consumers; ++i) 
        consumer_progress.push_back(std::make_unique<std::atomic<long long>>(0));

    // Results container for each thread
    std::vector<ThreadResult> producer_results(args.num_producers);
    std::vector<ThreadResult> consumer_results(args.num_consumers);

    auto producer_func = [&](int id)
    {
        std::mt19937_64 rng(id + 100);
        long long local_ops = 0;
        std::atomic<long long>* my_progress = producer_progress[id].get();
        int sampling_rate_mask = args.sampling_rate - 1;

        // Note: Pre-allocation of latency vectors is skipped for producers 
        // as we only track throughput to minimize observer effect.

        // Wait for start signal (Barrier)
        while (!start_flag.load(std::memory_order_acquire))
        {
            cpu_relax();
        }

        while (!stop_signal.load(std::memory_order_relaxed))
        {
            simulate_work(args.payload_us);

            // Enqueue Logic
            // Branchless optimization: Producers push data at full speed.
            if (queue.enqueue(std::pair<int, long long>{id, local_ops}))
            {
                local_ops++;
            }

            // Batch processing: Execute auxiliary tasks every 1024 operations.
            // Using bitwise AND for low-overhead sampling.
            if ((local_ops & sampling_rate_mask) == 0)
            {
                my_progress->store(local_ops, std::memory_order_relaxed);

                // Batch update queue depth.
                // Note: This introduces a slight temporal inaccuracy, but significantly reduces contention.
                long long old_depth = current_depth.fetch_add(args.sampling_rate, std::memory_order_relaxed);
                long long new_depth = old_depth + args.sampling_rate;

                // Update max_depth using CAS loop
                long long prev_max = max_depth.load(std::memory_order_relaxed);
                while (new_depth > prev_max && !max_depth.compare_exchange_weak(prev_max, 
                                                                                new_depth, 
                                                                                std::memory_order_relaxed)) 
                {
                    // Retry if another thread updated max_depth concurrently
                }   

                QueueType::quiescent();
            }
        }
        producer_results[id].operations = local_ops;
    };

    auto consumer_func = [&](int id)
    {
        std::pair<int, long long> value;
        long long local_ops = 0;
        std::atomic<long long>* my_progress = consumer_progress[id].get();
        int sampling_rate_mask = args.sampling_rate - 1;
        
        // Reserve memory to avoid reallocation spikes during measurement
        consumer_results[id].latencies_ns.reserve(200000);
        
        while (!start_flag.load(std::memory_order_acquire))
        {
            cpu_relax();
        }

        while (!stop_signal.load(std::memory_order_relaxed))
        {
            bool res = false;

            // Latency Sampling Logic
            if ((local_ops & sampling_rate_mask) == 0)
            {
                auto t1 = Clock::now();
                res = queue.try_dequeue(value);
                auto t2 = Clock::now();
                if (res)
                {
                    consumer_results[id].latencies_ns.push_back((t2 - t1).count());
                    local_ops++;
                    my_progress->store(local_ops, std::memory_order_relaxed);
                    
                    // Batch decrement of queue depth
                    current_depth.fetch_sub(args.sampling_rate, std::memory_order_relaxed);

                    simulate_work(args.payload_us);
                    QueueType::quiescent();
                }
            }
            else
            {
                // Fast path: No latency measurement or depth updates
                if (queue.try_dequeue(value))
                {
                    local_ops++;
                    simulate_work(args.payload_us);
                }
            }

            if (!res) std::this_thread::yield();
        }
        consumer_results[id].operations = local_ops;
    };

    // 1. Launch Threads
    std::vector<std::thread> threads;
    for (int i = 0; i < args.num_producers; i++)
        threads.emplace_back(producer_func, i);
    for (int i = 0; i < args.num_consumers; i++)
        threads.emplace_back(consumer_func, i);

    // 2. Warmup Phase
    std::cout << "Warming up for " << args.warmup_s << "s..." << std::endl;
    start_flag.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(args.warmup_s));

    // Capture snapshot after warmup to exclude warmup ops from final stats
    long long warmup_ops_producer = 0;
    long long warmup_ops_consumer = 0;
    for(const auto& prog : producer_progress) 
        warmup_ops_producer += prog->load(std::memory_order_relaxed);
    for(const auto& prog : consumer_progress) 
        warmup_ops_consumer += prog->load(std::memory_order_relaxed);
    
    // 3. Measurement Phase
    std::cout << "Running benchmark for " << args.duration_s << "s..." << std::endl;
    auto time_start = Clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(args.duration_s));
    auto time_end = Clock::now();

    stop_signal.store(true, std::memory_order_release);
    queue.close();

    for (auto &t : threads) t.join();

    // --------------------------------------------------------
    // Measure Peak Memory Usage (RSS)
    // --------------------------------------------------------
    long peak_mem_kb = 0;
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) 
    {
        peak_mem_kb = usage.ru_maxrss; 
        // Normalize units: Linux uses KB, macOS uses Bytes
        #ifdef __APPLE__
            peak_mem_kb /= 1024; 
        #endif
    }
    
    // 4. Aggregate Statistics
    long long total_ops_producer = 0;
    long long total_ops_consumer = 0;
    std::vector<long long> all_latencies;
    all_latencies.reserve(1000000);

    for (const auto &r : producer_results)
    {
        total_ops_producer += r.operations;
    }    
    for (const auto &r : consumer_results)
    {
        total_ops_consumer += r.operations;
        all_latencies.insert(all_latencies.end(), r.latencies_ns.begin(), r.latencies_ns.end());
    }

    double duration_sec = std::chrono::duration<double>(time_end - time_start).count();

    double throughput_consumer = (total_ops_consumer - warmup_ops_consumer) / duration_sec;
    double throughput_producer = (total_ops_producer - warmup_ops_producer) / duration_sec;

    // 5. Calculate Latency Percentiles
    std::sort(all_latencies.begin(), all_latencies.end());

    auto get_percentile = [&](double p) -> long long
    {
        if (all_latencies.empty())
            return 0;
        size_t idx = static_cast<size_t>(all_latencies.size() * p / 100.0);
        if (idx >= all_latencies.size())
            idx = all_latencies.size() - 1;
        return all_latencies[idx];
    };

    long long p50 = get_percentile(50.0);
    long long p99 = get_percentile(99.0);
    long long p999 = get_percentile(99.9);
    long long max_lat = all_latencies.empty() ? 0 : all_latencies.back();
    double avg_lat = 0;
    if (!all_latencies.empty())
    {
        unsigned __int128 sum = 0;
        for (auto x : all_latencies)
            sum += x;
        avg_lat = (double)sum / all_latencies.size();
    }

    // Output Results
    if (args.csv_path.empty())
    {
        std::cout << "Impl: " << impl_name << "\n";
        std::cout << "Threads: " << args.num_producers << "P / " << args.num_consumers << "C\n";
        std::cout << "Time: " << duration_sec << "s\n";
        std::cout << "Throughput (Prod): " << std::fixed << std::setprecision(0) << throughput_producer << " ops/sec\n";
        std::cout << "Throughput (Cons): " << std::fixed << std::setprecision(0) << throughput_consumer << " ops/sec\n";
        std::cout << "Latency (ns): Avg=" << std::setprecision(1) << avg_lat
                  << ", P50=" << p50
                  << ", p99=" << p99
                  << ", p99.9=" << p999
                  << ", Max=" << max_lat << "\n";
        std::cout << "Max Depth (Approx): " << max_depth.load() << "\n";
        std::cout << "Peak Memory: " << peak_mem_kb / 1024.0 << " MB\n";
        std::cout << "Producer Total: "<< total_ops_producer 
                  << ", Consumer Total: " << total_ops_consumer << "\n";
    }
    else
    {
        FILE *f = std::fopen(args.csv_path.c_str(), "a");
        if (f)
        {
            fseek(f, 0, SEEK_END);
            if (ftell(f) == 0)
            {
                std::fprintf(f, "impl,P,C,payload_us,throughput_prod,throughput_cons,avg_lat,p50,p99,p999,max_lat,max_depth,peak_mem_kb\n");
            }

            std::fprintf(f, "%s,%d,%d,%d,%.2f,%.2f,%.2f,%lld,%lld,%lld,%lld,%lld,%ld\n",
                         impl_name,
                         args.num_producers,
                         args.num_consumers,
                         args.payload_us,
                         throughput_producer,
                         throughput_consumer,
                         avg_lat, p50, p99, p999, max_lat,
                         max_depth.load(), peak_mem_kb);
            std::fclose(f);
            std::cout << "Wrote CSV: " << args.csv_path << "\n";
        }
    }
}

int main(int argc, char **argv)
{
    auto args = parse_args(argc, argv);

    if (args.impl == "hp")
    {
        using Q = mpmcq::LockFreeQueue<std::pair<int, long long>, mpmcq::reclaimer::hazard_pointers>;
        run_benchmark<Q>(args, "HazardPointer");
    }
    else if (args.impl == "ebr")
    {
        using Q = mpmcq::LockFreeQueue<std::pair<int, long long>, mpmcq::reclaimer::epoch_based_reclamation>;
        run_benchmark<Q>(args, "EBR");
    }
    else if (args.impl == "none")
    {
        using Q = mpmcq::LockFreeQueue<std::pair<int, long long>, mpmcq::reclaimer::no_reclamation>;
        run_benchmark<Q>(args, "NoReclamation");
    }
    else if (args.impl == "mutex")
    {
        using Q = mpmcq::MutexQueue<std::pair<int, long long>>;
        run_benchmark<Q>(args, "MutexQueue");
    }
    else
    {
        print_help();
        return 1;
    }
    return 0;
}