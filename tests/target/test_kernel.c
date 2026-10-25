/*
 * BareMetal-RTOS - on-target kernel test suite (runs on Cortex-M3 under QEMU or hardware)
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 *
 * A high-priority runner thread executes each test in turn. Tests spawn worker threads,
 * join them, and check the observed ordering/timing. The process exit code is the number
 * of failed checks (0 = all passed).
 */
#include <string.h>

#include "board.h"
#include "rtos/ringbuf.h"
#include "rtos/rtos.h"
#include "rtos_port.h"

/* ------------------------------------------------------------- harness */

static uint32_t checks_run;
static uint32_t checks_failed;
static uint32_t test_failed;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        checks_run++;                                                                  \
        if (!(cond)) {                                                                 \
            checks_failed++;                                                           \
            test_failed++;                                                             \
            board_printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        int32_t va_ = (int32_t)(a), vb_ = (int32_t)(b);                                \
        checks_run++;                                                                  \
        if (va_ != vb_) {                                                              \
            checks_failed++;                                                           \
            test_failed++;                                                             \
            board_printf("    FAIL %s:%d: %s == %s (%d vs %d)\n", __FILE__, __LINE__, #a,  \
                         #b, va_, vb_);                                                \
        }                                                                              \
    } while (0)

void rtos_panic_hook(const char *msg)
{
    board_printf("\nPANIC: %s\n", msg);
    board_exit(100);
}

#define STACK_WORDS 256u
#define NUM_WORKERS 4u
static rtos_thread_t workers[NUM_WORKERS];
static uint32_t worker_stacks[NUM_WORKERS][STACK_WORDS] __attribute__((aligned(8)));

static void spawn(uint32_t i, const char *name, rtos_entry_t fn, void *arg, uint8_t prio)
{
    CHECK_EQ(rtos_thread_create(&workers[i], name, fn, arg, prio, worker_stacks[i], STACK_WORDS),
             RTOS_OK);
}

static void join_all(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        CHECK_EQ(rtos_thread_join(&workers[i], 5000u), RTOS_OK);
    }
}

static void busy_ticks(uint32_t ticks)
{
    uint32_t start = rtos_ticks();
    while (rtos_ticks() - start < ticks) {
    }
}

/* A small trace log shared by worker threads. */
static char trace[64];
static volatile uint32_t trace_len;

static void trace_reset(void)
{
    memset(trace, 0, sizeof(trace));
    trace_len = 0;
}

static void trace_put(char c)
{
    uint32_t s = port_irq_save();
    if (trace_len < sizeof(trace) - 1u) {
        trace[trace_len++] = c;
    }
    port_irq_restore(s);
}

/* ------------------------------------------------------------- tests */

/* Higher-priority thread created from a lower one must run immediately. */
static void preempt_high(void *arg)
{
    (void)arg;
    trace_put('b');
}
static void preempt_low(void *arg)
{
    (void)arg;
    trace_put('a');
    spawn(1, "high", preempt_high, NULL, 10);
    trace_put('c');
}
static void test_preemption(void)
{
    trace_reset();
    spawn(0, "low", preempt_low, NULL, 5);
    join_all(2);
    CHECK(strcmp(trace, "abc") == 0);
}

/* Ready threads run strictly in priority order. */
static void prio_worker(void *arg) { trace_put((char)(uintptr_t)arg); }
static void test_priority_order(void)
{
    trace_reset();
    /* Runner has the highest priority, so none of these run until it blocks in join. */
    spawn(0, "p3", prio_worker, (void *)(uintptr_t)'3', 3);
    spawn(1, "p9", prio_worker, (void *)(uintptr_t)'9', 9);
    spawn(2, "p6", prio_worker, (void *)(uintptr_t)'6', 6);
    spawn(3, "p1", prio_worker, (void *)(uintptr_t)'1', 1);
    join_all(4);
    CHECK(strcmp(trace, "9631") == 0);
}

/* Cooperative yield alternates between equal-priority threads. */
static void yield_worker(void *arg)
{
    for (int i = 0; i < 3; i++) {
        trace_put((char)(uintptr_t)arg);
        rtos_yield();
    }
}
static void test_yield(void)
{
    trace_reset();
    spawn(0, "A", yield_worker, (void *)(uintptr_t)'A', 5);
    spawn(1, "B", yield_worker, (void *)(uintptr_t)'B', 5);
    join_all(2);
    CHECK(strcmp(trace, "ABABAB") == 0);
}

/* Time slicing shares the CPU between busy threads of equal priority. */
static volatile uint32_t rr_counts[2];
static volatile uint32_t rr_switches;
static volatile uint32_t rr_last = 0xFFu;
static void rr_worker(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;
    uint32_t start = rtos_ticks();
    while (rtos_ticks() - start < 100u) {
        rr_counts[id]++;
        uint32_t s = port_irq_save();
        if (rr_last != id) {
            rr_switches++;
            rr_last = id;
        }
        port_irq_restore(s);
    }
}
static void test_round_robin(void)
{
    rr_counts[0] = rr_counts[1] = 0;
    rr_switches = 0;
    rr_last = 0xFFu;
    spawn(0, "rr0", rr_worker, (void *)0, 4);
    spawn(1, "rr1", rr_worker, (void *)1, 4);
    join_all(2);
    CHECK(rr_counts[0] > 0u);
    CHECK(rr_counts[1] > 0u);
    /* 10-tick slices over ~100 ticks: expect several hand-overs, not just one. */
    CHECK(rr_switches >= 4u);
}

/* Sleep and periodic sleep_until keep tick-accurate timing. */
static volatile uint32_t sleep_elapsed, periodic_elapsed;
static void sleep_worker(void *arg)
{
    (void)arg;
    uint32_t t0 = rtos_ticks();
    rtos_sleep(25);
    sleep_elapsed = rtos_ticks() - t0;

    uint32_t last = rtos_ticks();
    uint32_t start = last;
    for (int i = 0; i < 10; i++) {
        rtos_sleep_until(&last, 7);
    }
    periodic_elapsed = rtos_ticks() - start;
}
static void test_sleep(void)
{
    spawn(0, "sleeper", sleep_worker, NULL, 5);
    join_all(1);
    CHECK(sleep_elapsed >= 25u && sleep_elapsed <= 26u);
    CHECK_EQ(periodic_elapsed, 70);
}

/* Semaphores: timeout, wake-up by give, saturation. */
static rtos_sem_t sem;
static volatile int sem_result;
static volatile uint32_t sem_wait_ticks;
static void sem_waiter(void *arg)
{
    (void)arg;
    uint32_t t0 = rtos_ticks();
    sem_result = rtos_sem_take(&sem, 1000);
    sem_wait_ticks = rtos_ticks() - t0;
}
static void test_semaphore(void)
{
    CHECK_EQ(rtos_sem_init(&sem, 0, 2), RTOS_OK);
    CHECK_EQ(rtos_sem_init(&sem, 3, 2), RTOS_ERR_PARAM);
    CHECK_EQ(rtos_sem_init(&sem, 0, 2), RTOS_OK);

    uint32_t t0 = rtos_ticks();
    CHECK_EQ(rtos_sem_take(&sem, 15), RTOS_ERR_TIMEOUT);
    uint32_t waited = rtos_ticks() - t0;
    CHECK(waited >= 15u && waited <= 16u);
    CHECK_EQ(rtos_sem_take(&sem, RTOS_NO_WAIT), RTOS_ERR_TIMEOUT);

    spawn(0, "semw", sem_waiter, NULL, 6);
    rtos_sleep(10); /* runner sleeps; waiter blocks on the semaphore */
    CHECK_EQ(rtos_sem_give(&sem), RTOS_OK);
    join_all(1);
    CHECK_EQ(sem_result, RTOS_OK);
    CHECK(sem_wait_ticks >= 9u && sem_wait_ticks <= 11u);
    CHECK_EQ(rtos_sem_count(&sem), 0); /* unit went straight to the waiter */

    CHECK_EQ(rtos_sem_give(&sem), RTOS_OK);
    CHECK_EQ(rtos_sem_give(&sem), RTOS_OK);
    CHECK_EQ(rtos_sem_give(&sem), RTOS_ERR_FULL);
    CHECK_EQ(rtos_sem_count(&sem), 2);
}

/* Mutex guarantees exclusion even when the owner yields inside the critical region. */
static rtos_mutex_t mtx;
static volatile uint32_t shared_counter;
static volatile uint32_t inside;
static volatile uint32_t overlap_detected;
static void mutex_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 200; i++) {
        rtos_mutex_lock(&mtx, RTOS_WAIT_FOREVER);
        if (inside++ != 0u) {
            overlap_detected++;
        }
        uint32_t v = shared_counter;
        rtos_yield(); /* invite a race */
        shared_counter = v + 1u;
        inside--;
        rtos_mutex_unlock(&mtx);
        if ((i & 7) == 0) {
            rtos_yield();
        }
    }
}
static void test_mutex_exclusion(void)
{
    CHECK_EQ(rtos_mutex_init(&mtx), RTOS_OK);
    shared_counter = 0;
    overlap_detected = 0;
    spawn(0, "m0", mutex_worker, NULL, 5);
    spawn(1, "m1", mutex_worker, NULL, 5);
    spawn(2, "m2", mutex_worker, NULL, 5);
    join_all(3);
    CHECK_EQ(shared_counter, 600);
    CHECK_EQ(overlap_detected, 0);
}

/*
 * Priority inheritance (classic inversion scenario).
 *   L (prio 3) holds the mutex, H (prio 9) blocks on it, M (prio 6) is CPU-bound.
 *   Without inheritance M starves L and therefore H. With it, L is boosted to 9,
 *   finishes its critical section, and H runs before M completes.
 */
static rtos_mutex_t pi_mtx;
static volatile uint8_t l_prio_while_contended, l_prio_after;
static void pi_low(void *arg)
{
    (void)arg;
    rtos_mutex_lock(&pi_mtx, RTOS_WAIT_FOREVER);
    rtos_sleep(5); /* H and M start meanwhile */
    busy_ticks(5);
    l_prio_while_contended = rtos_thread_prio(rtos_self());
    trace_put('L');
    rtos_mutex_unlock(&pi_mtx);
    l_prio_after = rtos_thread_prio(rtos_self());
}
static void pi_high(void *arg)
{
    (void)arg;
    rtos_sleep(1);
    rtos_mutex_lock(&pi_mtx, RTOS_WAIT_FOREVER);
    trace_put('H');
    rtos_mutex_unlock(&pi_mtx);
}
static void pi_mid(void *arg)
{
    (void)arg;
    rtos_sleep(2);
    busy_ticks(40);
    trace_put('M');
}
static void test_priority_inheritance(void)
{
    trace_reset();
    rtos_mutex_init(&pi_mtx);
    spawn(0, "L", pi_low, NULL, 3);
    spawn(1, "H", pi_high, NULL, 9);
    spawn(2, "M", pi_mid, NULL, 6);
    join_all(3);
    CHECK(strcmp(trace, "LHM") == 0);
    CHECK_EQ(l_prio_while_contended, 9);
    CHECK_EQ(l_prio_after, 3);
}

/* Lock timeout restores the owner's priority; error paths are reported. */
static rtos_mutex_t to_mtx;
static rtos_sem_t to_release;
static volatile int to_lock_result, to_unlock_result, to_recursive_result;
static volatile uint8_t owner_prio_boosted, owner_prio_restored;
static void to_owner(void *arg)
{
    (void)arg;
    rtos_mutex_lock(&to_mtx, RTOS_WAIT_FOREVER);
    to_recursive_result = rtos_mutex_lock(&to_mtx, 1);
    rtos_sleep(3);
    owner_prio_boosted = rtos_thread_prio(rtos_self());
    rtos_sem_take(&to_release, RTOS_WAIT_FOREVER);
    owner_prio_restored = rtos_thread_prio(rtos_self());
    rtos_mutex_unlock(&to_mtx);
}
static void to_contender(void *arg)
{
    (void)arg;
    rtos_sleep(1);
    to_unlock_result = rtos_mutex_unlock(&to_mtx);
    to_lock_result = rtos_mutex_lock(&to_mtx, 10);
}
static void test_mutex_timeout(void)
{
    rtos_mutex_init(&to_mtx);
    rtos_sem_init(&to_release, 0, 1);
    spawn(0, "owner", to_owner, NULL, 2);
    spawn(1, "contender", to_contender, NULL, 8);
    CHECK_EQ(rtos_thread_join(&workers[1], 1000), RTOS_OK);
    rtos_sem_give(&to_release);
    CHECK_EQ(rtos_thread_join(&workers[0], 1000), RTOS_OK);
    CHECK_EQ(to_recursive_result, RTOS_ERR_STATE);
    CHECK_EQ(to_unlock_result, RTOS_ERR_PERM);
    CHECK_EQ(to_lock_result, RTOS_ERR_TIMEOUT);
    CHECK_EQ(owner_prio_boosted, 8);
    CHECK_EQ(owner_prio_restored, 2);
    CHECK(to_mtx.owner == NULL);
}

/* Blocking queue keeps FIFO order across a small buffer. */
static rtos_queue_t queue;
static uint32_t queue_storage[4];
static volatile uint32_t queue_errors, queue_received;
static void q_producer(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < 100u; i++) {
        if (rtos_queue_send(&queue, &i, RTOS_WAIT_FOREVER) != RTOS_OK) {
            queue_errors++;
        }
    }
}
static void q_consumer(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < 100u; i++) {
        uint32_t v = 0xFFFFFFFFu;
        if (rtos_queue_recv(&queue, &v, 1000) != RTOS_OK || v != i) {
            queue_errors++;
        }
        queue_received++;
        if ((i % 10u) == 0u) {
            rtos_sleep(1); /* let the queue fill up and the producer block */
        }
    }
}
static void test_queue(void)
{
    CHECK_EQ(rtos_queue_init(&queue, queue_storage, sizeof(uint32_t), 4), RTOS_OK);
    queue_errors = queue_received = 0;
    uint32_t v = 0;
    CHECK_EQ(rtos_queue_recv(&queue, &v, RTOS_NO_WAIT), RTOS_ERR_TIMEOUT);
    CHECK_EQ(rtos_queue_recv(&queue, &v, 5), RTOS_ERR_TIMEOUT);
    spawn(0, "prod", q_producer, NULL, 5);
    spawn(1, "cons", q_consumer, NULL, 4);
    join_all(2);
    CHECK_EQ(queue_errors, 0);
    CHECK_EQ(queue_received, 100);
    for (uint32_t i = 0; i < 4u; i++) {
        CHECK_EQ(rtos_queue_send(&queue, &i, RTOS_NO_WAIT), RTOS_OK);
    }
    CHECK_EQ(rtos_queue_send(&queue, &v, RTOS_NO_WAIT), RTOS_ERR_TIMEOUT);
    CHECK_EQ(rtos_queue_count(&queue), 4);
}

/* Interrupt -> lock-free ring buffer -> thread, no samples lost or reordered. */
#define ISR_SAMPLES 600u
static rtos_ringbuf_t isr_rb;
static uint32_t isr_rb_storage[32];
static rtos_sem_t isr_sem;
static volatile uint32_t isr_seq, isr_dropped;
static rtos_queue_t isr_queue;
static uint32_t isr_queue_storage[8];
static volatile uint32_t isr_mode; /* 0 = ring buffer, 1 = queue */

void TIMER1A_IRQHandler(void)
{
    board_timer_ack(1);
    if (isr_seq >= ISR_SAMPLES) {
        return;
    }
    uint32_t v = isr_seq++;
    if (isr_mode == 0u) {
        if (!rtos_rb_push(&isr_rb, &v)) {
            isr_dropped++;
        }
        rtos_sem_give(&isr_sem);
    } else if (rtos_queue_send(&isr_queue, &v, RTOS_NO_WAIT) != RTOS_OK) {
        isr_dropped++;
    }
}

static volatile uint32_t isr_got, isr_order_errors;
static void isr_consumer(void *arg)
{
    (void)arg;
    uint32_t expect = 0;
    while (expect < ISR_SAMPLES) {
        uint32_t v;
        if (isr_mode == 0u) {
            if (rtos_sem_take(&isr_sem, 500) != RTOS_OK) {
                break;
            }
            while (rtos_rb_pop(&isr_rb, &v)) {
                if (v != expect) {
                    isr_order_errors++;
                }
                expect++;
            }
        } else {
            if (rtos_queue_recv(&isr_queue, &v, 500) != RTOS_OK) {
                break;
            }
            if (v != expect) {
                isr_order_errors++;
            }
            expect++;
        }
    }
    isr_got = expect;
}
static void run_isr_test(uint32_t mode)
{
    isr_mode = mode;
    isr_seq = isr_dropped = isr_got = isr_order_errors = 0;
    CHECK(rtos_rb_init(&isr_rb, isr_rb_storage, sizeof(uint32_t), 32));
    rtos_sem_init(&isr_sem, 0, 0xFFFFu);
    rtos_queue_init(&isr_queue, isr_queue_storage, sizeof(uint32_t), 8);
    spawn(0, "isr-consumer", isr_consumer, NULL, 7);
    board_timer_start(1, 4000, 0x40);
    join_all(1);
    board_timer_stop(1);
    CHECK_EQ(isr_got, ISR_SAMPLES);
    CHECK_EQ(isr_dropped, 0);
    CHECK_EQ(isr_order_errors, 0);
}
static void test_isr_ringbuffer(void) { run_isr_test(0); }
static void test_isr_queue(void) { run_isr_test(1); }

/* Blocking calls from an ISR are rejected instead of corrupting the scheduler. */
static volatile int isr_block_result = 1, isr_lock_result = 1;
static volatile uint32_t isr_block_done;
void TIMER0A_IRQHandler(void)
{
    board_timer_ack(0);
    if (!isr_block_done) {
        isr_block_result = rtos_sem_take(&sem, 10);
        isr_lock_result = rtos_mutex_lock(&mtx, 10);
        isr_block_done = 1;
    }
}
static void test_isr_misuse(void)
{
    rtos_sem_init(&sem, 0, 1);
    rtos_mutex_init(&mtx);
    isr_block_done = 0;
    board_timer_start(0, 1000, 0x40);
    while (!isr_block_done) {
        rtos_sleep(1);
    }
    board_timer_stop(0);
    CHECK_EQ(isr_block_result, RTOS_ERR_ISR);
    CHECK_EQ(isr_lock_result, RTOS_ERR_ISR);
}

/* Join timeout, stack watermark, argument validation, thread restart. */
static void long_worker(void *arg)
{
    (void)arg;
    rtos_sleep(30);
}
static void test_threads_misc(void)
{
    spawn(0, "long", long_worker, NULL, 5);
    CHECK_EQ(rtos_thread_join(&workers[0], 5), RTOS_ERR_TIMEOUT);
    CHECK_EQ(rtos_thread_join(&workers[0], RTOS_NO_WAIT), RTOS_ERR_TIMEOUT);
    CHECK_EQ(rtos_thread_create(&workers[0], "dup", long_worker, NULL, 5, worker_stacks[0],
                                STACK_WORDS),
             RTOS_ERR_STATE);
    CHECK_EQ(rtos_thread_join(&workers[0], 1000), RTOS_OK);
    CHECK_EQ(workers[0].state, RTOS_THREAD_DEAD);

    uint32_t free_words = rtos_thread_stack_free(&workers[0]);
    CHECK(free_words > 0u && free_words < STACK_WORDS);
    CHECK(workers[0].switches >= 1u);

    CHECK_EQ(rtos_thread_create(&workers[1], "p0", long_worker, NULL, 0, worker_stacks[1],
                                STACK_WORDS),
             RTOS_ERR_PARAM);
    CHECK_EQ(rtos_thread_create(&workers[1], "p32", long_worker, NULL, 32, worker_stacks[1],
                                STACK_WORDS),
             RTOS_ERR_PARAM);
    CHECK_EQ(rtos_thread_create(&workers[1], "tiny", long_worker, NULL, 5, worker_stacks[1], 8),
             RTOS_ERR_PARAM);
    CHECK_EQ(rtos_thread_create(NULL, "null", long_worker, NULL, 5, worker_stacks[1],
                                STACK_WORDS),
             RTOS_ERR_PARAM);
    CHECK_EQ(rtos_thread_join(rtos_self(), 10), RTOS_ERR_STATE);
    CHECK_EQ(rtos_mutex_lock(NULL, 1), RTOS_ERR_PARAM);
    CHECK_EQ(rtos_sem_give(NULL), RTOS_ERR_PARAM);
    CHECK_EQ(rtos_queue_init(&queue, queue_storage, 0, 4), RTOS_ERR_PARAM);
}

/* Lock-free ring buffer semantics (single context). */
static void test_ringbuf_basic(void)
{
    rtos_ringbuf_t rb;
    uint16_t storage[8];
    CHECK(!rtos_rb_init(&rb, storage, sizeof(uint16_t), 6)); /* not a power of two */
    CHECK(rtos_rb_init(&rb, storage, sizeof(uint16_t), 8));
    uint16_t v = 0;
    CHECK(!rtos_rb_pop(&rb, &v));
    for (uint16_t i = 0; i < 8u; i++) {
        CHECK(rtos_rb_push(&rb, &i));
    }
    CHECK(!rtos_rb_push(&rb, &v));
    CHECK_EQ(rtos_rb_count(&rb), 8);
    for (uint16_t i = 0; i < 8u; i++) {
        CHECK(rtos_rb_pop(&rb, &v));
        CHECK_EQ(v, i);
    }
    CHECK_EQ(rtos_rb_count(&rb), 0);
}

/*
 * Transitive priority inheritance: C (8) waits on m2 held by B (4), which waits on m1
 * held by A (2). A must run at 8 so that M (6), a CPU hog, cannot starve C.
 * The set-up is sequenced with semaphores, not sleeps, so it holds even if ticks arrive
 * in bursts.
 */
static rtos_mutex_t chain_m1, chain_m2;
static rtos_sem_t chain_a_locked, chain_b_locked, chain_c_ready;
static volatile uint8_t chain_a_prio, chain_a_after;

static bool wait_for_prio(uint8_t want, uint32_t max_ticks)
{
    for (uint32_t i = 0; i < max_ticks; i++) {
        if (rtos_thread_prio(rtos_self()) == want) {
            return true;
        }
        rtos_sleep(1);
    }
    return rtos_thread_prio(rtos_self()) == want;
}
static void chain_setup(void)
{
    rtos_mutex_init(&chain_m1);
    rtos_mutex_init(&chain_m2);
    rtos_sem_init(&chain_a_locked, 0, 1);
    rtos_sem_init(&chain_b_locked, 0, 1);
    rtos_sem_init(&chain_c_ready, 0, 1);
}
static void chain_a(void *arg)
{
    (void)arg;
    rtos_mutex_lock(&chain_m1, RTOS_WAIT_FOREVER);
    rtos_sem_give(&chain_a_locked);
    wait_for_prio(8, 200); /* boosted once C blocks behind B */
    chain_a_prio = rtos_thread_prio(rtos_self());
    busy_ticks(3);
    rtos_mutex_unlock(&chain_m1);
    chain_a_after = rtos_thread_prio(rtos_self());
}
static void chain_b(void *arg)
{
    (void)arg;
    rtos_sem_take(&chain_a_locked, RTOS_WAIT_FOREVER);
    rtos_mutex_lock(&chain_m2, RTOS_WAIT_FOREVER);
    rtos_sem_give(&chain_b_locked);
    rtos_mutex_lock(&chain_m1, RTOS_WAIT_FOREVER);
    trace_put('B');
    rtos_mutex_unlock(&chain_m1);
    rtos_mutex_unlock(&chain_m2);
}
static void chain_c(void *arg)
{
    (void)arg;
    rtos_sem_take(&chain_b_locked, RTOS_WAIT_FOREVER);
    rtos_sem_give(&chain_c_ready); /* wakes M, which runs only once C blocks */
    rtos_mutex_lock(&chain_m2, RTOS_WAIT_FOREVER);
    trace_put('C');
    rtos_mutex_unlock(&chain_m2);
}
static void chain_m(void *arg)
{
    (void)arg;
    rtos_sem_take(&chain_c_ready, RTOS_WAIT_FOREVER);
    busy_ticks(30);
    trace_put('M');
}
static void test_chained_inheritance(void)
{
    trace_reset();
    chain_setup();
    spawn(0, "A", chain_a, NULL, 2);
    spawn(1, "B", chain_b, NULL, 4);
    spawn(2, "C", chain_c, NULL, 8);
    spawn(3, "M", chain_m, NULL, 6);
    join_all(4);
    CHECK(strcmp(trace, "BCM") == 0);
    CHECK_EQ(chain_a_prio, 8);
    CHECK_EQ(chain_a_after, 2);
}

/* When the top waiter of a chain gives up, every boosted owner drops back. */
static rtos_sem_t unwind_release;
static volatile uint8_t unwind_a_boosted, unwind_a_after_timeout, unwind_b_after_timeout;
static volatile int unwind_c_result;
static void unwind_a(void *arg)
{
    (void)arg;
    rtos_mutex_lock(&chain_m1, RTOS_WAIT_FOREVER);
    rtos_sem_give(&chain_a_locked);
    wait_for_prio(8, 200);
    unwind_a_boosted = rtos_thread_prio(rtos_self());
    while (workers[2].state != RTOS_THREAD_DEAD) { /* until C has timed out */
        rtos_sleep(1);
    }
    unwind_a_after_timeout = rtos_thread_prio(rtos_self());
    unwind_b_after_timeout = rtos_thread_prio(&workers[1]);
    rtos_sem_take(&unwind_release, RTOS_WAIT_FOREVER);
    rtos_mutex_unlock(&chain_m1);
}
static void unwind_c(void *arg)
{
    (void)arg;
    rtos_sem_take(&chain_b_locked, RTOS_WAIT_FOREVER);
    unwind_c_result = rtos_mutex_lock(&chain_m2, 6);
}
static void test_inheritance_unwind(void)
{
    chain_setup();
    rtos_sem_init(&unwind_release, 0, 1);
    spawn(0, "A", unwind_a, NULL, 2);
    spawn(1, "B", chain_b, NULL, 4); /* locks m2, then blocks on m1 held by A */
    spawn(2, "C", unwind_c, NULL, 8);
    CHECK_EQ(rtos_thread_join(&workers[2], 1000), RTOS_OK);
    while (unwind_a_after_timeout == 0u) {
        rtos_sleep(1);
    }
    rtos_sem_give(&unwind_release);
    join_all(2);
    CHECK_EQ(unwind_c_result, RTOS_ERR_TIMEOUT);
    CHECK_EQ(unwind_a_boosted, 8);
    CHECK_EQ(unwind_a_after_timeout, 4); /* still boosted by B, no longer by C */
    CHECK_EQ(unwind_b_after_timeout, 4);
}

/* Several producers on one queue: nothing lost, per-producer order preserved. */
#define MP_PRODUCERS 3u
#define MP_ITEMS 60u
static volatile uint32_t mp_errors, mp_total;
static void mp_producer(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;
    for (uint32_t n = 0; n < MP_ITEMS; n++) {
        uint32_t v = (id << 16) | n;
        if (rtos_queue_send(&queue, &v, RTOS_WAIT_FOREVER) != RTOS_OK) {
            mp_errors++;
        }
        if ((n % 7u) == id) {
            rtos_yield();
        }
    }
}
static void mp_consumer(void *arg)
{
    (void)arg;
    uint32_t next[MP_PRODUCERS] = {0};
    for (uint32_t i = 0; i < MP_PRODUCERS * MP_ITEMS; i++) {
        uint32_t v;
        if (rtos_queue_recv(&queue, &v, 1000) != RTOS_OK) {
            mp_errors++;
            break;
        }
        uint32_t id = v >> 16, n = v & 0xFFFFu;
        if (id >= MP_PRODUCERS || n != next[id]) {
            mp_errors++;
        } else {
            next[id]++;
        }
        mp_total++;
    }
}
static void test_queue_multi_producer(void)
{
    rtos_queue_init(&queue, queue_storage, sizeof(uint32_t), 4);
    mp_errors = mp_total = 0;
    spawn(0, "mp0", mp_producer, (void *)0, 5);
    spawn(1, "mp1", mp_producer, (void *)1, 5);
    spawn(2, "mp2", mp_producer, (void *)2, 5);
    spawn(3, "mpc", mp_consumer, NULL, 3);
    join_all(4);
    CHECK_EQ(mp_errors, 0);
    CHECK_EQ(mp_total, MP_PRODUCERS * MP_ITEMS);
    CHECK_EQ(rtos_queue_count(&queue), 0);
}

/* Calls made before rtos_start() fail cleanly instead of crashing. */
static int prestart_lock_result = 1, prestart_unlock_result = 1, prestart_sem_result = 1;
static rtos_mutex_t prestart_mtx;
static rtos_sem_t prestart_sem;
static void test_prestart_guards(void)
{
    CHECK_EQ(prestart_lock_result, RTOS_ERR_STATE);
    CHECK_EQ(prestart_unlock_result, RTOS_ERR_STATE);
    CHECK_EQ(prestart_sem_result, RTOS_ERR_STATE);
}

/* CPU-time accounting charges cycles to the thread that actually ran. */
static void spin_worker(void *arg)
{
    busy_ticks((uint32_t)(uintptr_t)arg);
}
static void test_cpu_accounting(void)
{
    spawn(0, "spin", spin_worker, (void *)40, 5);
    join_all(1);
    const uint64_t per_tick = RTOS_CPU_HZ / RTOS_TICK_HZ;
    uint64_t c = rtos_thread_run_cycles(&workers[0]);
    CHECK(c >= 39u * per_tick && c <= 42u * per_tick);
    uint64_t self0 = rtos_thread_run_cycles(rtos_self());
    busy_ticks(5);
    uint64_t self1 = rtos_thread_run_cycles(rtos_self());
    CHECK(self1 - self0 >= 4u * per_tick); /* includes the slice in progress */
}

/* ------------------------------------------------------------- runner */

typedef struct {
    const char *name;
    void (*fn)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"preemption on create", test_preemption},
    {"strict priority order", test_priority_order},
    {"cooperative yield", test_yield},
    {"round-robin time slicing", test_round_robin},
    {"sleep / sleep_until timing", test_sleep},
    {"semaphore take/give/timeout", test_semaphore},
    {"mutex mutual exclusion", test_mutex_exclusion},
    {"mutex priority inheritance", test_priority_inheritance},
    {"mutex timeout and errors", test_mutex_timeout},
    {"chained priority inheritance", test_chained_inheritance},
    {"inheritance unwinds on timeout", test_inheritance_unwind},
    {"message queue FIFO + blocking", test_queue},
    {"queue with multiple producers", test_queue_multi_producer},
    {"ISR -> ring buffer -> thread", test_isr_ringbuffer},
    {"ISR -> queue -> thread", test_isr_queue},
    {"blocking calls rejected in ISR", test_isr_misuse},
    {"thread join/stack/params", test_threads_misc},
    {"ring buffer semantics", test_ringbuf_basic},
    {"guards before rtos_start", test_prestart_guards},
    {"per-thread CPU accounting", test_cpu_accounting},
};

static rtos_thread_t runner;
static uint32_t runner_stack[512] __attribute__((aligned(8)));

static void runner_entry(void *arg)
{
    (void)arg;
    uint32_t passed = 0;
    const uint32_t n = sizeof(tests) / sizeof(tests[0]);
    board_printf("\n=== BareMetal-RTOS kernel tests (%u cases) ===\n", n);
    for (uint32_t i = 0; i < n; i++) {
        test_failed = 0;
        uint32_t t0 = rtos_ticks();
        tests[i].fn();
        board_printf("[%s] %-36s (%u ticks)\n", test_failed ? "FAIL" : " OK ", tests[i].name,
                     rtos_ticks() - t0);
        if (!test_failed) {
            passed++;
        }
    }
    board_printf("=== %u/%u cases passed, %u/%u checks passed ===\n", passed, n,
                 checks_run - checks_failed, checks_run);
    board_printf(checks_failed ? "RESULT: FAILED\n" : "RESULT: ALL TESTS PASSED\n");
    board_exit((int)checks_failed);
}

int main(void)
{
    board_init();
    rtos_init();
    rtos_mutex_init(&prestart_mtx);
    rtos_sem_init(&prestart_sem, 0, 1);
    prestart_lock_result = rtos_mutex_lock(&prestart_mtx, 10);
    prestart_unlock_result = rtos_mutex_unlock(&prestart_mtx);
    prestart_sem_result = rtos_sem_take(&prestart_sem, 10);
    rtos_thread_create(&runner, "runner", runner_entry, NULL, 20, runner_stack, 512);
    rtos_start();
}
