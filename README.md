# BareMetal-RTOS

**A lightweight, multi-threaded real-time kernel for ARM Cortex-M, written from scratch in C, C++ and ARM assembly.**

BareMetal-RTOS runs directly on the hardware with no operating system underneath. It schedules concurrent threads under strict priorities, switches context in the `PendSV` exception, and moves sensor telemetry from interrupt handlers to processing threads through lock-free ring buffers: the kind of workload found in robots, drones and industrial controllers.

The whole project builds and runs **entirely in software**: the firmware is compiled with the real `arm-none-eabi` toolchain and executed on an emulated Cortex-M3 board (QEMU `lm3s6965evb`). No development board is needed to build, run, test or debug it.

![Language](https://img.shields.io/badge/language-C%20%7C%20C%2B%2B%20%7C%20ARM%20asm-blue)
![Target](https://img.shields.io/badge/target-ARM%20Cortex--M3-informational)
![License](https://img.shields.io/badge/license-MIT-green)

---

## Highlights

| Area | What is implemented |
|---|---|
| **Scheduler** | Preemptive, priority-based, 32 levels (0 = idle, 31 = most urgent). O(1) selection using a ready bitmap and `CLZ`. Round-robin time slicing among equal priorities. |
| **Tick & switching** | `SysTick` drives a 1 kHz kernel tick; context switches happen in a hand-written `PendSV` handler (ARM assembly) on the process stack (PSP). |
| **Threads** | Thread control blocks with separate, statically allocated stacks; `sleep`, `sleep_until` (drift-free periodic), `yield`, `join`; stack painting for high-water marks and overflow detection. |
| **Mutexes** | Ownership tracking, **transitive priority inheritance** (boosts and un-boosts ripple through chains of blocked owners), timeouts, direct hand-off to the highest-priority waiter. |
| **Semaphores** | Counting semaphores with timeouts; `give` is safe from interrupt handlers. |
| **Message queues** | Fixed-size items, blocking send/receive with timeouts, non-blocking variants usable from ISRs. |
| **Lock-free ring buffer** | Single-producer / single-consumer, wait-free, no interrupt masking; acquire/release ordering. Same header runs on Cortex-M and on the host. |
| **CPU accounting** | Exact per-thread CPU time in core cycles, charged by the context switch itself. |
| **Safety checks** | Blocking calls from ISRs return `RTOS_ERR_ISR`; mutex calls before `rtos_start()` return `RTOS_ERR_STATE`; blocking with interrupts disabled panics; recursive locking and foreign unlocks are rejected. |
| **C++ layer** | Zero-overhead wrappers (`rtos::Thread<N>`, `Mutex`, `LockGuard`, `Semaphore`, `Queue<T,N>`, `RingBuffer<T,N>`) for `-fno-exceptions -fno-rtti` builds. No heap anywhere. |
| **Tests** | 20 on-target kernel test cases (188 checks) run on the emulated MCU, plus host stress tests for the ring buffer, including ThreadSanitizer. |

---

## Demo

The demo application (`app/telemetry`, C++) is a small **vibration-telemetry and levelling controller**:

```
 TIMER0A ISR (1 kHz)                                  ┌───────────────────────────┐
  read 3-axis accelerometer ──push──► RingBuffer ──►  │ processing thread (prio 8)│
  (synthetic sensor model)   lock-free  <Sample,64>   │ IIR filter, |a|, shock    │
          │                                           │ detection, latency stats  │
          └── Semaphore::give() ─── wake-up ────────► └─────────┬─────────┬───────┘
                                                        mutex   │         │ Queue<Summary,4>
                                         SharedState ◄──────────┘         ▼
                                              ▲                 logger thread (prio 2)
                     controller thread ───────┘                 prints one line / 250 ms
                     (prio 6, 100 Hz, drives an actuator
                      that feeds back into the sensor)          2 × background threads (prio 1)
                                                                share the CPU by round-robin
```

* The sensor interrupt captures a timestamped sample every millisecond and pushes it into a lock-free ring buffer, then signals a semaphore.
* The processing thread drains the buffer, low-pass filters each axis, computes the acceleration magnitude, detects impacts (one is injected every second), and measures **interrupt-to-thread latency** in CPU cycles.
* A 100 Hz controller thread reads the filtered state under a mutex and drives an actuator that cancels a constant 80 mg tilt error. The loop closes through the sensor model, and the `ctrl` column shows it settling at about 100 mg of correction.
* A logger thread prints windowed statistics received over a message queue.
* Two CPU-bound background threads at the lowest user priority only run in idle time and share it through time slicing.
* At the end the supervisor prints a report and **verifies itself**: no lost samples, no sequence gaps, every shock detected, controller on schedule, every window logged.

### Run it

```bash
./scripts/build.sh            # build everything and run all tests
./scripts/run_qemu.sh demo    # run the telemetry demo
```

### Output (deterministic mode)

```text
BareMetal-RTOS telemetry demo  |  Cortex-M3 @ 12 MHz, tick 1000 Hz, sensor 1000 Hz
Running for 3000 ms...

  win | samples | mean |  peak | shocks | lat min/avg/max (us) | drop | ctrl
  ----+---------+------+-------+--------+----------------------+------+-----
    0 |     250 | 1027 |  1106 |      0 |    26 /    26 /    26  |    0 |  102
    1 |     250 | 1005 |  1059 |      0 |    26 /    26 /    26  |    0 |  101
    2 |     250 | 1034 |  3696 |      1 |    26 /    26 /    26  |    0 |  105
    3 |     250 | 1005 |  1059 |      1 |    26 /    26 /    26  |    0 |  105
    4 |     250 | 1006 |  1059 |      1 |    26 /    26 /    26  |    0 |  102
    5 |     250 | 1006 |  1053 |      1 |    26 /    26 /    26  |    0 |  101
    6 |     250 | 1034 |  3686 |      2 |    26 /    26 /    26  |    0 |  102
    7 |     250 | 1005 |  1059 |      2 |    26 /    26 /    26  |    0 |  104
    8 |     250 | 1006 |  1059 |      2 |    26 /    26 /    26  |    0 |  102
    9 |     250 | 1006 |  1054 |      2 |    26 /    26 /    26  |    0 |  103
   10 |     250 | 1034 |  3694 |      3 |    26 /    26 /    26  |    0 |  106

--- Run report (sensor active 3000 ms) ---
  samples produced by ISR   : 2999
  samples processed         : 2999
  ring-buffer overflows     : 0
  sequence gaps             : 0
  shocks injected/detected  : 3 / 3
  ISR->thread latency (us)  : min 26, avg 26, max 26
  controller iterations     : 300 (max wake jitter 0 ticks)
  windows logged            : 11
  background work units     : 3464321 / 3465473

  thread        prio  switches   stack used / size      cpu*
  processing       8      3001     200 /  1536 B     4.1 %
  controller       6       301     120 /  1024 B     0.1 %
  logger           2        38     208 /  1536 B     0.0 %
  background-0     1      1650      80 /   768 B    45.3 %
  background-1     1      1651      80 /   768 B    45.3 %
  idle             0         -                 -     4.7 %
  * share of CPU cycles since boot (ISR time billed to the interrupted thread)

DEMO RESULT: PASS
```

How to read it:

* **mean ≈ 1005 mg** once the controller has removed the tilt (window 0 starts at 1027 mg, before it converges). Windows 2, 6 and 10 contain an impact, hence the higher mean and the ~3.7 g peak.
* **latency** is measured from the sample timestamp taken inside the ISR to the moment the processing thread handles it. It covers the interrupt, the semaphore wake-up, the `PendSV` context switch and the ring-buffer pop. In deterministic mode QEMU advances virtual time by 64 ns per instruction, so these figures reflect instruction counts rather than a particular silicon implementation.
* the two background threads receive exactly the same share of the CPU: round-robin slicing at work. The `cpu` column comes from the kernel's cycle-accurate accounting: the processing thread needs about 4 % of the core to handle 1000 samples per second, and everything the real-time threads leave over goes to the background work.

> **Note:** QEMU prints `Timer with period zero, disabling` once at start-up. This comes from QEMU's own model of the LM3S6965 board (it appears even for an empty program) and does not affect the firmware.

---

## Quick start

### Requirements

| Tool | Tested version | Purpose |
|---|---|---|
| `arm-none-eabi-gcc` + newlib | 13.2 | cross compiler for Cortex-M |
| CMake | ≥ 3.16 (tested 3.28) | build system |
| `qemu-system-arm` | 8.2 | runs the firmware on an emulated Cortex-M3 |
| host `gcc`/`g++` | 13 | host unit tests |
| `gdb-multiarch` or `arm-none-eabi-gdb` | optional | debugging |

On Ubuntu / Debian:

```bash
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi qemu-system-arm cmake build-essential
```

On macOS (Homebrew): `brew install --cask gcc-arm-embedded && brew install qemu cmake`.

### Build and test everything

```bash
git clone https://github.com/<your-username>/BareMetal-RTOS.git
cd BareMetal-RTOS
./scripts/build.sh
```

### Or step by step

```bash
# Firmware
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build -j
(cd build && ctest --output-on-failure)       # runs both firmware images in QEMU

# Host tests
cmake -S . -B build-host
cmake --build build-host -j
(cd build-host && ctest --output-on-failure)

# Run images interactively
./scripts/run_qemu.sh demo                   # telemetry demo, deterministic timing
./scripts/run_qemu.sh tests                  # kernel test suite
./scripts/run_qemu.sh demo --realtime        # emulated clock follows the host clock
cmake --build build --target run_demo        # same thing through CMake
```

Build outputs in `build/`: `telemetry_demo.elf`, `kernel_tests.elf`, raw `.bin` images and linker `.map` files.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│ Application (C++)          app/telemetry/*.cpp                       │
├──────────────────────────────────────────────────────────────────────┤
│ C++ wrappers               rtos/rtos.hpp   Thread, Mutex, Queue, ... │
├──────────────────────────────────────────────────────────────────────┤
│ Kernel (portable C11)      kernel/src/sched.c   scheduler, threads,  │
│                                                 tick, delays         │
│                            kernel/src/sync.c    mutex, sem, queue    │
│                            rtos/ringbuf.h       lock-free SPSC       │
├──────────────────────────────────────────────────────────────────────┤
│ Port (Cortex-M3)           port/cortex-m3/port.c           SysTick,  │
│                                    initial stack frames, cycles      │
│                            port/cortex-m3/context_switch.S PendSV    │
├──────────────────────────────────────────────────────────────────────┤
│ Board (LM3S6965)           board/lm3s6965/  vectors, reset, UART,    │
│                                             timers, linker script    │
└──────────────────────────────────────────────────────────────────────┘
```

The kernel only talks to the hardware through the small port interface in `port/cortex-m3/rtos_port.h` (interrupt masking, pending a switch, building a stack frame, starting the first thread). Supporting another core means writing a new port; supporting another Cortex-M3 chip only means a new `board/` directory.

---

## Kernel API

All objects are allocated by the caller (static or global); the kernel never allocates memory. Functions return `RTOS_OK` (0) or a negative `rtos_status_t`.

### Reference

| Function | Context | Description |
|---|---|---|
| `rtos_init()` / `rtos_start()` | main | initialise the kernel / enter the scheduler (no return) |
| `rtos_thread_create(t, name, fn, arg, prio, stack, words)` | thread, main, ISR | start a thread at priority 1..31 |
| `rtos_thread_join(t, timeout)` | thread | wait for a thread's entry function to return |
| `rtos_sleep(ticks)` / `rtos_sleep_until(&last, period)` | thread | relative delay / periodic delay without drift |
| `rtos_yield()` | thread | give the CPU to the next thread of equal priority |
| `rtos_self()`, `rtos_thread_prio(t)`, `rtos_thread_stack_free(t)` | any | introspection |
| `rtos_thread_run_cycles(t)`, `rtos_idle_run_cycles()` | any | CPU time used, in core cycles |
| `rtos_mutex_lock(m, timeout)` / `rtos_mutex_unlock(m)` | thread | priority-inheriting mutex |
| `rtos_sem_take(s, timeout)` / `rtos_sem_give(s)` | thread / **any** | counting semaphore |
| `rtos_queue_send(q, item, timeout)` / `rtos_queue_recv(q, item, timeout)` | thread, **ISR with `RTOS_NO_WAIT`** | message queue |
| `rtos_rb_push` / `rtos_rb_pop` (`rtos/ringbuf.h`) | one producer / one consumer | lock-free SPSC buffer |
| `rtos_ticks()`, `rtos_idle_ticks()` | any | tick counters |

Timeouts are in ticks; `RTOS_NO_WAIT` (0) polls and `RTOS_WAIT_FOREVER` blocks indefinitely.

| Status | Meaning |
|---|---|
| `RTOS_ERR_TIMEOUT` | the wait expired, or the resource was unavailable with `RTOS_NO_WAIT` |
| `RTOS_ERR_PARAM` | invalid argument (null pointer, bad priority, stack too small, ...) |
| `RTOS_ERR_STATE` | wrong state: recursive lock, creating a running thread, joining yourself, locking a mutex before `rtos_start()` |
| `RTOS_ERR_PERM` | unlocking a mutex owned by another thread |
| `RTOS_ERR_FULL` | semaphore already at its maximum count |
| `RTOS_ERR_ISR` | a blocking call was made from an interrupt handler |

---

## How it works

### Scheduling

Each priority level has a FIFO ready list, and a 32-bit bitmap records which lists are non-empty. The next thread is always `ready_lists[31 - clz(bitmap)].head`, a constant-time operation using the Cortex-M3 `CLZ` instruction. The running thread stays at the head of its list. A thread that becomes ready goes to the back of its list, so it preempts immediately only if its priority is strictly higher.

On every tick, `rtos_tick()`:
1. releases threads whose sleep or wait deadline has arrived (the delay list is sorted by wake time and uses wrap-safe arithmetic),
2. decrements the running thread's time slice and rotates its ready list when the slice expires,
3. checks the running thread's stack guard word,
4. requests a context switch if a different thread should now run.

### Context switching

Switches are only ever *requested* (by setting `PENDSVSET`). `PendSV` runs at the lowest exception priority, so it fires only after every other interrupt has finished, which makes it the single place where threads change:

```
PendSV_Handler:                   thread stack after the switch-out
  cpsid i
  bl k_account_switch             ┌──────────┐ high address
  psp -> r0                       │ xPSR     │ ┐
  push r4-r11 onto PSP            │ PC       │ │ stacked by hardware
  rtos_cur->sp = r0               │ LR       │ │ on exception entry
  rtos_cur = rtos_next            │ R12      │ │
  r0 = rtos_cur->sp               │ R3..R0   │ ┘
  pop r4-r11 from r0              │ R11..R4  │ ← saved by PendSV
  psp = r0                        └──────────┘ ← tcb->sp
  cpsie i
  bx 0xFFFFFFFD   ; Thread mode, PSP
```

New threads get a hand-built version of this frame (`port_init_stack`), with `PC` pointing at the entry function, `R0` holding its argument, and `LR` pointing at the thread-exit routine, so a returning entry function ends the thread cleanly and wakes its joiners. Threads run on the process stack, while interrupts use the main stack, which `port_start_first` resets so that none of `main`'s stack is wasted.

### Interrupt priorities and critical sections

| Exception | Priority | Notes |
|---|---|---|
| Device IRQs (e.g. sensor timer) | `0x40` | always preempt the kernel |
| SysTick | `0xF0` | kernel tick |
| PendSV | `0xFF` | context switch, lowest |

Kernel data structures are protected by short `PRIMASK` critical sections (save, `cpsid i`, restore), so every kernel call is safe to make from both threads and interrupts. Blocking calls work by enqueueing the thread, pending `PendSV`, and restoring interrupts; the switch happens at that instant and the call resumes, with its result already filled in, when the thread is next scheduled.

### Priority inheritance

When a thread blocks on a mutex, the owner's effective priority is raised to the waiter's, and the boost is propagated along the chain if that owner is itself blocked on another mutex. On unlock, or when a waiter times out, the owner's priority is recomputed as the maximum of its base priority and the top waiter of every mutex it still holds, and the recomputation continues down the chain so that no thread keeps a boost it no longer needs. Ownership is handed directly to the highest-priority waiter, so a lower-priority thread can never barge in. The `mutex priority inheritance` test reproduces the classic Mars Pathfinder scenario (low holds, high waits, medium hogs the CPU) and checks that the high-priority thread is not starved. Two further tests cover a three-thread chain (C waits on B, which waits on A) and the case where the top waiter gives up and every boost along the chain has to be undone.

### Lock-free ring buffer

The producer only writes `head` and the consumer only writes `tail`. Indices run freely and are masked on access, so all slots are usable and fullness is simply `head - tail == capacity`. The producer copies the element, then publishes it with a **release** store to `head`; the consumer reads `head` with an **acquire** load before copying, so it can never observe a half-written element. No interrupt masking is needed, which keeps the sensor ISR short and its timing predictable. The identical header is stress-tested on a multi-core host with two OS threads and ThreadSanitizer.

### CPU accounting

`PendSV` calls a short C routine before switching, which reads the cycle counter (tick count × reload + the current SysTick value) and adds the cycles since the previous switch to the outgoing thread. That makes the figures exact rather than sampled: a thread that always finishes within a tick, like the processing thread, is still billed correctly. Time spent in interrupt handlers is billed to whichever thread they interrupted.

### Message queues

A queue is two counting semaphores (`spaces` and `items`) around a circular buffer. A sender reserves a slot by taking `spaces`, copies its item under a few-instruction critical section, and publishes it with `items`. Blocking, timeouts, priority-ordered wake-ups and ISR use all come from the semaphore implementation.

---

## Testing

| Suite | Where it runs | What it covers |
|---|---|---|
| `kernel_tests.elf` | emulated Cortex-M3 (QEMU), deterministic and real-time modes | 20 cases / 188 checks, listed below |
| `telemetry_demo.elf` | emulated Cortex-M3 | full pipeline self-check (no drops, no gaps, all shocks detected, controller on time) |
| `host_ringbuf_tests` | host, optionally under ThreadSanitizer | validation, wrap-around, 32-bit index overflow, 2 million element two-thread stress |

```text
=== BareMetal-RTOS kernel tests (20 cases) ===
[ OK ] preemption on create                 (0 ticks)
[ OK ] strict priority order                (0 ticks)
[ OK ] cooperative yield                    (0 ticks)
[ OK ] round-robin time slicing             (111 ticks)
[ OK ] sleep / sleep_until timing           (95 ticks)
[ OK ] semaphore take/give/timeout          (25 ticks)
[ OK ] mutex mutual exclusion               (17 ticks)
[ OK ] mutex priority inheritance           (42 ticks)
[ OK ] mutex timeout and errors             (11 ticks)
[ OK ] chained priority inheritance         (33 ticks)
[ OK ] inheritance unwinds on timeout       (7 ticks)
[ OK ] message queue FIFO + blocking        (15 ticks)
[ OK ] queue with multiple producers        (6 ticks)
[ OK ] ISR -> ring buffer -> thread         (150 ticks)
[ OK ] ISR -> queue -> thread               (150 ticks)
[ OK ] blocking calls rejected in ISR       (2 ticks)
[ OK ] thread join/stack/params             (30 ticks)
[ OK ] ring buffer semantics                (0 ticks)
[ OK ] guards before rtos_start             (0 ticks)
[ OK ] per-thread CPU accounting            (45 ticks)
=== 20/20 cases passed, 188/188 checks passed ===
RESULT: ALL TESTS PASSED
```

```text
=== BareMetal-RTOS host ring buffer tests ===
[ OK ] init validation
[ OK ] fill / drain / wrap
[ OK ] 32-bit index overflow
[ OK ] struct elements via C++ wrapper
  stress: 2000000 items transferred, producer hit full 52592 times
[ OK ] two-thread SPSC stress
=== 12785/12785 checks passed ===
```

The firmware's exit code is the number of failed checks, passed back to the host through ARM semihosting, so `ctest` and CI fail on any regression.

The kernel suite runs twice: once with deterministic timing and once in real-time mode, where the emulated clock follows the host clock. The real-time run is deliberately harsher. On a busy machine QEMU can deliver several ticks in a burst, which exposes tests that silently depend on threads waking in a particular order, so multi-thread scenarios are sequenced with semaphores rather than sleeps. The suite has been run repeatedly under artificial CPU load at `-O0` and `-O2` without a failure. The demo is only run in deterministic mode, because it checks the exact sensor sample rate and a starved host makes QEMU coalesce periodic timer interrupts. CI also rebuilds and reruns the firmware tests at `-O0`, `-O3` and `-Os`, and runs the ring buffer stress test under ThreadSanitizer.

```bash
(cd build && ctest -V -R kernel_tests_qemu_det)   # one suite, verbose
```

---

## Footprint

Measured with `arm-none-eabi-size` on the default `RelWithDebInfo` (`-O2`) build:

| Component | Flash | RAM |
|---|---|---|
| Kernel (scheduler, mutex, semaphore, queue, port) | **≈ 4.3 KB** | **749 B** (incl. 384 B idle stack) |
| Per thread | – | 80 B TCB + its stack |
| Telemetry demo image (kernel, board, app) | ≈ 12.2 KB | ≈ 9.8 KB (mostly thread stacks) |

---

## Debugging with GDB

QEMU exposes a GDB server, so you can single-step through the context switch without any hardware:

```bash
./scripts/run_qemu.sh demo --debug          # halts at reset, listens on :1234
```

In a second terminal:

```bash
gdb-multiarch build/telemetry_demo.elf      # or arm-none-eabi-gdb
(gdb) target remote :1234
(gdb) break PendSV_Handler
(gdb) continue
(gdb) print *rtos_cur
(gdb) print rtos_next->name
(gdb) x/16wx $psp
```

The `.map` files in `build/` show where every symbol and stack lives.

---

## Running on real hardware

The kernel and port target any Cortex-M3. Cortex-M4/M7 parts can use the same port as long as threads do not use the FPU (see [Design limits](#design-limits)). The `board/lm3s6965` directory is the only chip-specific code. For another board:

1. Copy `board/lm3s6965` and adapt the linker script memory sizes, the vector table entries, and the UART/timer register code in `board.c`.
2. Set `RTOS_CPU_HZ` to your core clock (e.g. `-DRTOS_CPU_HZ=72000000`).
3. Build with `-DBOARD_NO_SEMIHOSTING` if the program will run without a debugger attached.
4. Flash and debug with OpenOCD and GDB, for example:

```bash
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg
arm-none-eabi-gdb build/telemetry_demo.elf \
    -ex "target extended-remote :3333" -ex "monitor reset halt" -ex "load" -ex "continue"
```

With OpenOCD, `monitor arm semihosting enable` lets the test image report its exit code exactly as it does under QEMU.

---

## Configuration

Defaults live in `kernel/include/rtos/config.h`; override any of them with `-D` flags:

| Macro | Default | Meaning |
|---|---|---|
| `RTOS_MAX_PRIO` | 32 | priority levels (max 32) |
| `RTOS_TICK_HZ` | 1000 | kernel tick rate |
| `RTOS_CPU_HZ` | 12000000 | core clock used to program SysTick |
| `RTOS_TIMESLICE_TICKS` | 10 | round-robin slice |
| `RTOS_MIN_STACK_WORDS` | 64 | smallest accepted thread stack |
| `RTOS_IDLE_STACK_WORDS` | 96 | idle thread stack |

Override `rtos_panic_hook(const char *msg)` to report fatal errors (stack overflow, blocking with interrupts disabled, exiting while holding a mutex).

---

## License

Released under the [MIT License](LICENSE).

Copyright (c) 2026 **Swayam Shakya**
