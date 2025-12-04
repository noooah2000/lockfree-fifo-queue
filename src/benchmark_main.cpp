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

struct Args {
  std::string impl = "hp"; // hp|ebr|none|mutex
  int producers = 4;
  int consumers = 4;
  int payload_us = 100;
  int warmup_s = 2;
  int duration_s = 5;
  std::string csv = "";
};

static void help() {
  std::cout <<
R"(Usage: bench_queue [--impl hp|ebr|none|mutex]
                     [--producers P] [--consumers C]
                     [--payload-us N] [--warmup S] [--duration S]
                     [--csv path]
)" ;
}

static Args parse(int argc, char** argv) {
  Args a;
  for (int i=1;i<argc;i++) {
    std::string k = argv[i];
    auto need = [&](int i){ if (i+1>=argc) { help(); std::exit(1);} return std::string(argv[i+1]); };
    if (k=="--help" || k=="-h") { help(); std::exit(0); }
    else if (k=="--impl") a.impl = need(i++);
    else if (k=="--producers") a.producers = std::atoi(need(i++).c_str());
    else if (k=="--consumers") a.consumers = std::atoi(need(i++).c_str());
    else if (k=="--payload-us") a.payload_us = std::atoi(need(i++).c_str());
    else if (k=="--warmup") a.warmup_s = std::atoi(need(i++).c_str());
    else if (k=="--duration") a.duration_s = std::atoi(need(i++).c_str());
    else if (k=="--csv") a.csv = need(i++);
    else { std::cerr<<"Unknown arg: "<<k<<"\n"; help(); std::exit(1); }
  }
  return a;
}

template <class Q>
static void run_bench(const Args& a, const char* impl_name) {
  Q q;
  std::atomic<bool> stop{false};
  std::atomic<long long> enq_ok{0}, deq_ok{0};
  std::atomic<int> depth{0}, max_depth{0};

  auto payload = [&](int usleep){
    if (usleep <= 0) return;
    auto t0 = Clock::now();
    while (std::chrono::duration_cast<us>(Clock::now()-t0).count() < usleep) {
      // busy-wait 少量迴圈（避免 sleep 抽風）
    }
  };

  // Producers
  std::vector<std::thread> P;
  for (int p=0;p<a.producers;p++) {
    P.emplace_back([&,p]{
      std::mt19937_64 rng(p+123);
      long long i=0;
      while (!stop.load(std::memory_order_relaxed)) {
        payload(a.payload_us);
        if (q.enqueue(std::pair<int,long long>{p,i++})) {
          enq_ok.fetch_add(1, std::memory_order_relaxed);
          int d = depth.fetch_add(1, std::memory_order_relaxed) + 1;
          int prev = max_depth.load(std::memory_order_relaxed);
          while (d > prev && !max_depth.compare_exchange_weak(prev, d)) {}
        } else {
          // closed
          break;
        }
      }
    });
  }

  // Consumers
  std::vector<std::thread> C;
  for (int c=0;c<a.consumers;c++) {
    C.emplace_back([&,c]{
      std::pair<int,long long> v;
      while (!stop.load(std::memory_order_relaxed)) {
        if (q.try_dequeue(v)) {
          deq_ok.fetch_add(1, std::memory_order_relaxed);
          depth.fetch_sub(1, std::memory_order_relaxed);
          payload(a.payload_us);
        } else {
          // 空：稍作讓步避免自旋過猛
          std::this_thread::yield();
        }
      }
    });
  }

  // warmup
  std::this_thread::sleep_for(std::chrono::seconds(a.warmup_s));
  // 正式區間
  long long deq0 = deq_ok.load();
  auto t0 = Clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(a.duration_s));
  auto t1 = Clock::now();
  stop.store(true);
  q.close();

  for (auto& t: P) t.join();
  for (auto& t: C) t.join();
  auto secs = std::chrono::duration<double>(t1-t0).count();
  long long deq1 = deq_ok.load();

  double thr = (deq1 - deq0) / secs;

  // 印出 / CSV
  if (a.csv.empty()) {
    std::cout << "impl="<<impl_name
              << " P="<<a.producers<<" C="<<a.consumers
              << " payload_us="<<a.payload_us
              << " duration_s="<<a.duration_s
              << " throughput_ops="<<thr
              << " max_depth="<<max_depth.load()
              << "\n";
  } else {
    FILE* f = std::fopen(a.csv.c_str(), "a");
    if (f) {
      std::fprintf(f, "impl,P,C,payload_us,duration_s,throughput_ops,max_depth\n");
      std::fprintf(f, "%s,%d,%d,%d,%d,%.3f,%d\n",
        impl_name, a.producers, a.consumers, a.payload_us, a.duration_s,
        thr, max_depth.load());
      std::fclose(f);
      std::cout << "Wrote CSV: " << a.csv << "\n";
    } else {
      std::perror("fopen csv");
    }
  }
}

int main(int argc, char** argv) {
  auto a = parse(argc, argv);
  if (a.impl=="hp") {
    using Q = lfq::MPMCQueue<std::pair<int,long long>, lfq::reclaimer::hazard_pointers>;
    run_bench<Q>(a, "hp");
  } else if (a.impl=="ebr") {
    using Q = lfq::MPMCQueue<std::pair<int,long long>, lfq::reclaimer::epoch_based_reclamation>;
    run_bench<Q>(a, "ebr");
  } else if (a.impl=="none") {
    using Q = lfq::MPMCQueue<std::pair<int,long long>, lfq::reclaimer::no_reclamation>;
    run_bench<Q>(a, "none");
  } else if (a.impl=="mutex") {
    using Q = lfq::MPMCQueueMutex<std::pair<int,long long>>;
    run_bench<Q>(a, "mutex");
  } else {
    help();
    return 1;
  }
  return 0;
}
