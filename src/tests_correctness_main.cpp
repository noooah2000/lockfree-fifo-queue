#include <atomic>
#include <thread>
#include <vector>
#include <set>
#include <map>
#include <iostream>
#include <cassert>
#include "queue/lockfree_queue.hpp"
#include "queue/mutex_queue.hpp"
#include "reclaimer/hazard_pointers.hpp"
#include "reclaimer/epoch_based_reclamation.hpp"
#include "reclaimer/no_reclamation.hpp"

template <class Q>
static bool test_linearization(int P=4, int C=4, int K=10000) {
  Q q;
  std::atomic<int> done{0};
  std::vector<std::thread> prod, cons;

  // 每個 producer 產生 K 筆 (producer_id, seq)
  for (int p=0;p<P;p++) {
    prod.emplace_back([&,p]{
      for (int i=0;i<K;i++) { while (!q.enqueue(std::pair<int,int>{p,i})) {} }
      if (done.fetch_add(1)+1==P) q.close();
    });
  }

  std::vector<std::map<int,int>> seen(C); // consumer c: last seq for producer p
  std::atomic<int> total{0};

  for (int c=0;c<C;c++) {
    cons.emplace_back([&,c]{
      std::pair<int,int> v;
      while (true) {
        if (q.try_dequeue(v)) {
          total.fetch_add(1);
          auto& rec = seen[c];
          auto it = rec.find(v.first);
          int last = (it==rec.end()? -1 : it->second);
          // 每個 producer 的序列不可逆序
          if (v.second < last) {
            std::cerr<<"Order violation: p="<<v.first<<" got="<<v.second<<" last="<<last<<"\n";
            std::abort();
          }
          rec[v.first] = v.second;
        } else if (q.is_closed()) {
          break;
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  for (auto& t: prod) t.join();
  for (auto& t: cons) t.join();

  bool ok = (total.load() == P*K);
  if (!ok) std::cerr<<"Loss/dup mismatch total="<<total.load()<<" expect="<<(P*K)<<"\n";
  return ok;
}

template <class Q>
static bool test_shutdown_semantics() {
  Q q;
  int x=42;
  assert(q.enqueue(x)==true);
  q.close();
  // 關閉後 enqueue 應失敗
  if (q.enqueue(x) != false) return false;
  // 消費者仍可把剩餘元素取完
  int y=0;
  bool ok = q.try_dequeue(y);
  return ok && y==42;
}

static void run_all(const char* name, auto makeQ) {
  std::cout<<"[RUN] "<<name<<"\n";
  bool ok1 = test_linearization<decltype(makeQ())>();
  bool ok2 = test_shutdown_semantics<decltype(makeQ())>();
  std::cout<<" linearization: "<<(ok1?"PASS":"FAIL")<<"\n";
  std::cout<<" shutdown_semantics: "<<(ok2?"PASS":"FAIL")<<"\n";
  std::cout<<"\n";
}

int main() {
  // Lock-free: HP / EBR / None（目前皆以直接 delete 作為 retire）
  run_all("lockfree(HP)", [](){ return lfq::MPMCQueue<int, lfq::reclaimer::hazard_pointers>{}; });
  run_all("lockfree(EBR)", [](){ return lfq::MPMCQueue<int, lfq::reclaimer::epoch_based_reclamation>{}; });
  run_all("lockfree(None)", [](){ return lfq::MPMCQueue<int, lfq::reclaimer::no_reclamation>{}; });

  // Mutex baseline
  run_all("mutex", [](){ return lfq::MPMCQueueMutex<int>{}; });

  // ABA guard：骨架版（未實作 SMR）暫時無法有效測，留待你補完後新增專門測試。
  std::cout<<"[NOTE] ABA guard test is N/A in the skeleton (SMR not implemented yet).\n";
  return 0;
}
