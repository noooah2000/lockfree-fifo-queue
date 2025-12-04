# Lock-free MPMC FIFO Queue (HP/EBR/None) + Mutex Baseline

極簡專題版：提供一個可編譯、可跑 benchmark / correctness 的骨架。
- Lock-free 佇列（Michael & Scott MPMC，**先行版本未實作真正 SMR**）
- Reclaimer 策略型別：`hazard_pointers` / `epoch_based_reclamation` / `no_reclamation`（目前皆為 no-op，待完善）
- Mutex baseline：`mutex_queue.hpp`

## Build & Run
```bash
make -j
make run-bench     # 顯示幫助
make run-tests     # 跑簡單正確性測試
