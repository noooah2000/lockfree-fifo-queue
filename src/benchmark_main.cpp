#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <iostream>
#include <random>

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
    int payload_us = 100;
    int warmup_s = 2;
    int duration_s = 5;
    std::string csv_path = "";
};

static void print_help() 
{
    std::cout <<
R"(Usage: bench_queue [--impl hp|ebr|none|mutex]
                     [--producers P] [--consumers C]
                     [--payload-us N] [--warmup S] [--duration S]
                     [--csv path]
)" ;
}

static BenchmarkArgs parse_args(int argc, char** argv) 
{
    BenchmarkArgs args;
    for (int i = 1; i < argc; i++) 
    {
        std::string key = argv[i];
        auto get_value = [&](int idx){ 
            if (idx + 1 >= argc) { print_help(); std::exit(1); } 
            return std::string(argv[idx + 1]); 
        };

        if (key == "--help" || key == "-h") { print_help(); std::exit(0); }
        else if (key == "--impl") args.impl = get_value(i++);
        else if (key == "--producers") args.num_producers = std::atoi(get_value(i++).c_str());
        else if (key == "--consumers") args.num_consumers = std::atoi(get_value(i++).c_str());
        else if (key == "--payload-us") args.payload_us = std::atoi(get_value(i++).c_str());
        else if (key == "--warmup") args.warmup_s = std::atoi(get_value(i++).c_str());
        else if (key == "--duration") args.duration_s = std::atoi(get_value(i++).c_str());
        else if (key == "--csv") args.csv_path = get_value(i++);
        else { std::cerr << "Unknown arg: " << key << "\n"; print_help(); std::exit(1); }
    }
    return args;
}

template <class QueueType>
static void run_benchmark(const BenchmarkArgs& args, const char* impl_name) 
{
    QueueType queue;
    std::atomic<bool> stop_signal{false};
    std::atomic<long long> enqueue_count{0}, dequeue_count{0};
    std::atomic<int> current_depth{0}, max_depth{0};

    // 模擬負載
    auto simulate_work = [&](int usleep) {
        if (usleep <= 0) return;
        auto start = Clock::now();
        while (std::chrono::duration_cast<us>(Clock::now() - start).count() < usleep) {
            // busy-wait to avoid context switch overhead in payload simulation
            cpu_relax(); 
        }
    };

    // Producers
    std::vector<std::thread> producer_threads;
    for (int p = 0; p < args.num_producers; p++) 
    {
        producer_threads.emplace_back([&, p] {
            std::mt19937_64 rng(p + 123); // 簡單隨機源
            (void)rng;
            long long seq = 0;
            int ops_since_quiescent = 0;

            while (!stop_signal.load(std::memory_order_relaxed)) 
            {
                simulate_work(args.payload_us);
                
                if (queue.enqueue(std::pair<int, long long>{p, seq++})) 
                {
                    enqueue_count.fetch_add(1, std::memory_order_relaxed);
                    int depth = current_depth.fetch_add(1, std::memory_order_relaxed) + 1;
                    int prev_max = max_depth.load(std::memory_order_relaxed);
                    while (depth > prev_max && !max_depth.compare_exchange_weak(prev_max, depth)) {}
                } 
                else 
                {
                    break; // Queue closed
                }

                // [重要] 定期呼叫 quiescent 以驅動 SMR (EBR 需要這個)
                if (++ops_since_quiescent >= 64) 
                {
                    QueueType::quiescent();
                    ops_since_quiescent = 0;
                }
            }
        });
    }

    // Consumers
    std::vector<std::thread> consumer_threads;
    for (int c = 0; c < args.num_consumers; c++) 
    {
        consumer_threads.emplace_back([&, c] {
            std::pair<int, long long> value;
            int ops_since_quiescent = 0;

            while (!stop_signal.load(std::memory_order_relaxed)) 
            {
                if (queue.try_dequeue(value)) 
                {
                    dequeue_count.fetch_add(1, std::memory_order_relaxed);
                    current_depth.fetch_sub(1, std::memory_order_relaxed);
                    simulate_work(args.payload_us);
                } 
                else 
                {
                    // 佇列空：稍微讓步
                    std::this_thread::yield();
                }

                // [重要] 定期呼叫 quiescent
                if (++ops_since_quiescent >= 64) 
                {
                    QueueType::quiescent();
                    ops_since_quiescent = 0;
                }
            }
        });
    }

    // Warmup
    std::cout << "Warming up for " << args.warmup_s << "s..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(args.warmup_s));

    // 正式量測區間
    long long dequeue_start = dequeue_count.load();
    auto time_start = Clock::now();
    
    std::cout << "Running benchmark for " << args.duration_s << "s..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(args.duration_s));
    
    auto time_end = Clock::now();
    stop_signal.store(true);
    queue.close();

    for (auto& t : producer_threads) t.join();
    for (auto& t : consumer_threads) t.join();

    long long dequeue_end = dequeue_count.load();
    double duration_sec = std::chrono::duration<double>(time_end - time_start).count();
    double throughput = (dequeue_end - dequeue_start) / duration_sec;

    // 輸出結果
    if (args.csv_path.empty()) 
    {
        std::cout << "Impl=" << impl_name
                  << " Producers=" << args.num_producers 
                  << " Consumers=" << args.num_consumers
                  << " Payload(us)=" << args.payload_us
                  << " Duration(s)=" << args.duration_s
                  << " Throughput(ops/sec)=" << throughput
                  << " MaxDepth=" << max_depth.load()
                  << "\n";
    } 
    else 
    {
        FILE* f = std::fopen(args.csv_path.c_str(), "a");
        if (f) 
        {
            // 如果檔案是空的，寫入 Header (這只是一個簡單檢查，實務上可能需要更嚴謹的判斷)
            fseek(f, 0, SEEK_END);
            if (ftell(f) == 0) {
                std::fprintf(f,"impl,P,C,payload_us,duration_s,throughput_ops,max_depth\n");
            }

            std::fprintf(f, "%s,%d,%d,%d,%d,%.3f,%d\n",
                            impl_name,
                            args.num_producers,  // P
                            args.num_consumers,  // C
                            args.payload_us,
                            args.duration_s,
                            throughput,          // throughput_ops
                            max_depth.load());
            std::fclose(f);
            std::cout << "Wrote CSV: " << args.csv_path << "\n";
        } 
        else 
        {
            std::perror("fopen csv");
        }
    }
}

int main(int argc, char** argv) 
{
    auto args = parse_args(argc, argv);
    
    // 這裡假設你的 Reclaimer 類別命名是 CamelCase
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