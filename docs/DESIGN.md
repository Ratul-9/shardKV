# Design Log: lldstore

Decision log and trade-offs. Each entry: what we chose, what we rejected, and why.

---

## D-001: Development environment (2026-10-24)

**Environment reported by the user**
- Host: macOS, Apple M5 chip (ARM64), 10-core CPU
- C++ compiler installed (versions to be verified in Phase 0)
- AWS account exists, no credits; user accepts the cost of the final 3-node cluster

**Consequences we must plan for**
- **No Linux on the host.** `perf`, `numactl`/NUMA, `epoll` and `sched_setaffinity` are Linux-only. macOS has `kqueue`, no `perf`, and no thread-to-core pinning API.
- **Phase 0-2 run natively on macOS.** TSan, ASan and UBSan work with Apple clang. Phase 2 uses `kqueue` only if we go below gRPC; gRPC hides this.
- **Phase 3 onward (pinning) needs Linux.** Options: a Linux VM (UTM or similar, ARM64 Linux), Docker Desktop (also a VM underneath, so NUMA and `perf` are limited), or EC2. Decide at the start of Phase 3.
- **Phase 5-6 (`perf` counters) need Linux with hardware counters.** Apple Silicon virtual machines may not expose PMU counters; EC2 instances do. Plan to measure there.
- **M5 is heterogeneous (performance and efficiency cores).** Results depend on which cores a thread lands on. Pinning to performance cores must be recorded in `docs/BENCHMARKS.md` with the core set used.
- **ARM64 memory model.** ARM is weakly ordered, unlike x86 (TSO). Bugs with missing acquire/release can hide on x86 and appear on ARM. The project must be tested on ARM (this machine) and on x86 (EC2) to be credible. This is a strong interview point.
- **Cost.** EC2 has no free credits. Estimate and record the hourly cost of the chosen instance type before the Phase 11 run, and shut instances down between runs.

**Rejected alternatives**
- Developing only on x86 EC2: slower iteration and ongoing cost during the whole project.
- Ignoring Linux-only features until the end: would leave Phases 3, 5 and 6 unmeasurable.

---

## Open questions

- Apple clang version and whether `std::hardware_destructive_interference_size` is available (depends on toolchain). To verify in Phase 0.
- Which EC2 instance type for the final cluster (ARM Graviton or x86). To decide before Phase 11, after measuring costs.
