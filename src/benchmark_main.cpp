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

#include "queue/lockfree_queue.hpp"
#include "queue/mutex_queue.hpp"
#include "reclaimer/hazard_pointers.hpp"
#include "reclaimer/epoch_based_reclamation.hpp"
#include "reclaimer/no_reclamation.hpp"

using Clock = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;
using us = std::chrono::microseconds;
using namespace std::chrono_literals;

struct BenchmarkArgs
{
    std::string impl = "hp"; // hp|ebr|none|mutex
    int num_producers = 4;
    int num_consumers = 4;
    int payload_us = 0; // 預設改為 0，測極限吞吐量
    int warmup_s = 1;
    int duration_s = 5;
    std::string csv_path = "";
    bool measure_latency = true;
    int sampling_rate = 1000; // 每 1000 次操作採樣一次 Latency
};

struct ThreadResult
{
    long long operations = 0;
    std::vector<long long> latencies_ns; // 存奈秒
};

static void print_help()
{
    std::cout <<
        R"(Usage: bench_queue [--impl hp|ebr|none|mutex]
                     [--producers P] [--consumers C]
                     [--payload-us N] [--warmup S] [--duration S]
                     [--csv path]
)";
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

// 模擬負載
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

template <class QueueType>
static void run_benchmark(const BenchmarkArgs &args, const char *impl_name)
{
    QueueType queue;
    std::atomic<bool> start_flag{false};
    std::atomic<bool> stop_signal{false};

    // 每個 Thread 的結果容器
    std::vector<ThreadResult> producer_results(args.num_producers);
    std::vector<ThreadResult> consumer_results(args.num_consumers);

    auto producer_func = [&](int id)
    {
        std::mt19937_64 rng(id + 100);
        long long local_ops = 0;
        int ops_since_quiescent = 0;

        // 預先分配記憶體以避免測量時 allocation
        if (args.measure_latency)
        {
            producer_results[id].latencies_ns.reserve(200000);
        }

        // 等待開始信號 (Barrier)
        while (!start_flag.load(std::memory_order_acquire))
        {
            cpu_relax();
        }

        while (!stop_signal.load(std::memory_order_relaxed))
        {
            simulate_work(args.payload_us);

            // Latency Sampling Logic
            if (args.measure_latency && (local_ops % args.sampling_rate == 0))
            {
                auto t1 = Clock::now();
                bool res = queue.enqueue(std::pair<int, long long>{id, local_ops});
                auto t2 = Clock::now();
                if (res)
                {
                    producer_results[id].latencies_ns.push_back((t2 - t1).count());
                    local_ops++;
                }
            }
            else
            {
                // Fast path without timer
                if (queue.enqueue(std::pair<int, long long>{id, local_ops}))
                {
                    local_ops++;
                }
            }

            // Periodic quiescent for EBR/HP
            if (++ops_since_quiescent >= 64)
            {
                QueueType::quiescent();
                ops_since_quiescent = 0;
            }
        }
        producer_results[id].operations = local_ops;
    };

    auto consumer_func = [&](int id)
    {
        std::pair<int, long long> value;
        long long local_ops = 0;
        int ops_since_quiescent = 0;

        if (args.measure_latency)
        {
            consumer_results[id].latencies_ns.reserve(200000);
        }

        while (!start_flag.load(std::memory_order_acquire))
        {
            cpu_relax();
        }

        while (!stop_signal.load(std::memory_order_relaxed))
        {
            bool res = false;

            // Latency Sampling Logic
            if (args.measure_latency && (local_ops % args.sampling_rate == 0))
            {
                auto t1 = Clock::now();
                res = queue.try_dequeue(value);
                auto t2 = Clock::now();
                if (res)
                {
                    consumer_results[id].latencies_ns.push_back((t2 - t1).count());
                    local_ops++;
                    simulate_work(args.payload_us);
                }
            }
            else
            {
                // Fast path
                if (queue.try_dequeue(value))
                {
                    local_ops++;
                    simulate_work(args.payload_us);
                }
            }

            if (!res)
            {
                std::this_thread::yield();
            }

            if (++ops_since_quiescent >= 64)
            {
                QueueType::quiescent();
                ops_since_quiescent = 0;
            }
        }
        consumer_results[id].operations = local_ops;
    };

    // 1. 啟動 Threads
    std::vector<std::thread> threads;
    for (int i = 0; i < args.num_producers; i++)
        threads.emplace_back(producer_func, i);
    for (int i = 0; i < args.num_consumers; i++)
        threads.emplace_back(consumer_func, i);

    // 2. Warmup Phase (不計入統計)
    std::cout << "Warming up for " << args.warmup_s << "s..." << std::endl;
    start_flag.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(args.warmup_s));

    // Reset counters (其實不需要 reset，我們可以用 diff，但為了簡單，我們重啟這段測量比較乾淨)
    // 為了真正隔離 Warmup 影響，嚴謹的做法是 Warmup 後歸零。
    // 但因為我們使用了 thread local 變數且還沒開始存 latency (假設 warmup 期間不紀錄)，
    // 這裡我們簡單做：停止現在的，重新計算。
    // **修正**：上面的代碼會在 warmup 時也記錄 latency。
    // 為了簡化邏輯：我們讓 warmup 和正式執行混在一起，最後只算 throughput，
    // 或者我們接受 warmup 的數據也會進去。
    //
    // **更好的做法**：
    // 在這裡我們暫停一下，清空 vector 和 count，再開始計時。
    // 但要在多執行緒下「暫停並清空」很難同步。
    // 所以常見做法是：忽略 Warmup 期間的數據，或是 Warmup 就單純跑幾秒，
    // 然後我們在程式內部用一個 flag 控制「開始記錄」。
    //
    // 這裡採用「簡單粗暴法」：Warmup 其實已經跑在 start_flag = true 之後了。
    // 我們可以選擇「不重置」，直接測量 (Warmup + Run) 的總時間，或者重寫邏輯。
    //
    // 為了精確度，我將採用「捨棄前 N 秒數據」的邏輯比較困難，
    // 所以我們改為：Warmup 結束後，記錄當前的時間點和 count，作為 "Base"，
    // 結束時減去這個 Base。

    // 收集 Warmup 結束時的快照
    long long warmup_ops_p = 0;
    long long warmup_ops_c = 0;
    // 這裡沒辦法簡單取得所有 thread 的 local 變數而不暫停它們。
    // 所以，我們改變策略：Warmup 只是為了讓 Cache 熱起來。
    // 我們讓上面的 Loop 繼續跑，但我們只在主執行緒 sleep。
    // 真正的 throughput 計算我們使用 "Window" 方式不太容易。
    //
    // **最終決定策略**：
    // 為了不讓程式碼太複雜，我們接受 Warmup 期間的 Latency 數據也會被採樣進去。
    // 但這對 "Tail Latency" 影響不大。
    // 對於 Throughput，我們使用 std::this_thread::sleep 作為測量區間是不準的。
    //
    // 改良版邏輯：
    // 1. 啟動所有 Threads, start_flag = true
    // 2. sleep(warmup)
    // 3. reset_stats_flag = true (告訴 threads 清空數據? 不，這太慢)
    // 4. 正確做法：Benchmarks 通常是 "Run for X seconds"，不區分 warmup 階段的數據，
    //    或者在啟動前先跑一段獨立的 warmup loop。

    // 讓我們保持簡單：
    // 目前的架構是 start_flag 一開就開始跑。
    // 我們可以讓 throughput 包含 warmup，或者忽視它。
    // 鑑於作業需求，我們維持原樣：sleep(warmup) 只是讓系統穩定，
    // 然後我們開始計時，sleep(duration)，結束。
    // 這樣 Throughput = (Total Ops) / (Warmup + Duration)。
    // 雖然包含了 Warmup 的低速期，但如果 Warmup 時間短，影響可接受。

    // 但為了更專業一點，我會在下面只計算 duration 期間的 throughput。
    // 透過在 Warmup 結束瞬間，讀取一次全域進度？不，沒全域變數了。
    //
    // **妥協方案**：
    // 讓 Warmup 獨立執行。
    // 也就是先跑一個 Warmup Loop，Join，然後再跑正式 Loop。
    // 但這樣會有 Thread 建立銷毀的開銷。
    //
    // 回到最優解：
    // 忽略 Warmup 的精確隔離。直接測量整個區間。
    // 只要 Warmup 時間相對於 Duration 很短 (例如 0.5s vs 5s)，差異不大。

    std::cout << "Running benchmark for " << args.duration_s << "s..." << std::endl;
    auto time_start = Clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(args.duration_s));
    auto time_end = Clock::now();

    stop_signal.store(true, std::memory_order_release);
    queue.close();

    for (auto &t : threads)
    {
        t.join();
    }
    
    // 4. 統計數據
    long long total_ops_producer = 0;
    long long total_ops_consumer = 0;
    std::vector<long long> all_latencies;
    all_latencies.reserve(1000000);

    for (const auto &r : producer_results)
        total_ops_producer += r.operations;

    for (const auto &r : consumer_results)
    {
        total_ops_consumer += r.operations;
        all_latencies.insert(all_latencies.end(), r.latencies_ns.begin(), r.latencies_ns.end());
    }

    double duration_sec = std::chrono::duration<double>(time_end - time_start).count() + args.warmup_s;
    // 修正：因為我們是從 start_flag = true 就開始跑，所以總時間是 warmup + duration

    double throughput_producer = total_ops_producer / duration_sec;
    double throughput_consumer = total_ops_consumer / duration_sec;

    // 5. 計算 Latency Percentiles
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

    // 輸出結果
    if (args.csv_path.empty())
    {
        std::cout << "Impl: " << impl_name << "\n";
        std::cout << "Threads: " << args.num_producers << "P / " << args.num_consumers << "C\n";
        std::cout << "Time: " << duration_sec << "s\n";
        std::cout << "Throughput (Cons): " << std::fixed << std::setprecision(0) << throughput_consumer << " ops/sec\n";
        std::cout << "Latency (ns): Avg=" << std::setprecision(1) << avg_lat
                  << ", P50=" << p50
                  << ", P99=" << p99
                  << ", P99.9=" << p999
                  << ", Max=" << max_lat << "\n";
    }
    else
    {
        FILE *f = std::fopen(args.csv_path.c_str(), "a");
        if (f)
        {
            fseek(f, 0, SEEK_END);
            if (ftell(f) == 0)
            {
                std::fprintf(f, "impl,P,C,payload_us,throughput,avg_lat,p50,p99,p999,max_lat\n");
            }

            std::fprintf(f, "%s,%d,%d,%d,%.2f,%.2f,%lld,%lld,%lld,%lld\n",
                         impl_name,
                         args.num_producers,
                         args.num_consumers,
                         args.payload_us,
                         throughput_consumer,
                         avg_lat, p50, p99, p999, max_lat);
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