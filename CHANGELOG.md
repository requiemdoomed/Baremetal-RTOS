# Changelog

All notable changes to this project are documented here.

## [1.1.0] - 2026-10

### Added
- Exact per-thread CPU accounting in core cycles, charged at every context switch
  (`rtos_thread_run_cycles`, `rtos_idle_run_cycles`). The demo report shows each thread's share.
- Kernel tests for chained priority inheritance, inheritance unwinding on timeout,
  multi-producer queues, pre-start guards and CPU accounting (20 cases, 188 checks).
- Kernel test suite now also runs in QEMU real-time mode as part of `ctest`.
- `-Wstrict-prototypes -Wmissing-prototypes` for all C sources; source paths in assertion
  messages are relative to the repository.

### Fixed
- Priority un-boosting now walks the whole chain of blocked owners. Previously, if the top
  waiter of a chain timed out, owners further down kept their boosted priority until they
  unlocked.
- `rtos_mutex_lock` and `rtos_mutex_unlock` return `RTOS_ERR_STATE` when called before
  `rtos_start()` instead of dereferencing a null current thread.
- Multi-thread tests are sequenced with semaphores instead of sleeps, so they hold when the
  emulator delivers ticks in bursts on a heavily loaded host.

## [1.0.0] - 2026-10

### Added
- Preemptive priority scheduler (32 levels, O(1) bitmap selection, round-robin time slicing).
- SysTick tick and PendSV context switch for Cortex-M3.
- Threads with sleep, periodic sleep, yield, join, stack painting and overflow detection.
- Mutexes with priority inheritance, counting semaphores, message queues.
- Lock-free single-producer / single-consumer ring buffer.
- C++ wrappers, LM3S6965 board support, telemetry demo, on-target and host test suites, CI.
