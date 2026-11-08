#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "FreeRTOS.h"
#include "task.h"

#if EOS_LAB_MODE < 1 || EOS_LAB_MODE > 5
#error "EOS_LAB_MODE must be 1 (AMP), 2 (FIFO), 3 (SMP), 4 (handoff) or 5 (capacity)"
#endif

enum { PIN_CORE0 = 16, PIN_CORE1 = 17, PIN_REQ = 18, PIN_RESP = 19 };
volatile uint32_t g_fault;

#define EOS_COREDEBUG_DEMCR (*(volatile uint32_t *)0xE000EDFCu)
#define EOS_DWT_CTRL        (*(volatile uint32_t *)0xE0001000u)
#define EOS_DWT_CYCCNT      (*(volatile uint32_t *)0xE0001004u)

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    g_fault = 1;
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

void vApplicationMallocFailedHook(void)
{
    g_fault = 2;
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

static void cycle_counter_init(void)
{
    EOS_COREDEBUG_DEMCR |= 1u << 24;
    EOS_DWT_CYCCNT = 0;
    EOS_DWT_CTRL |= 1u;
}

#if EOS_LAB_MODE == 1
static void core1_periodic(void)
{
    gpio_init(PIN_CORE1);
    gpio_set_dir(PIN_CORE1, GPIO_OUT);
    absolute_time_t deadline = delayed_by_us(get_absolute_time(), 1000);
    for (;;) {
        gpio_put(PIN_CORE1, 1);
        /* TODO AMP: add the fixed 50-NOP control computation. */
        gpio_put(PIN_CORE1, 0);
        busy_wait_until(deadline);
        deadline = delayed_by_us(deadline, 1000);
    }
}

static void load_task(void *arg)
{
    (void)arg;
    gpio_init(PIN_CORE0);
    gpio_set_dir(PIN_CORE0, GPIO_OUT);
    for (;;) {
        gpio_put(PIN_CORE0, 1);
        busy_wait_us_32(20000);
        gpio_put(PIN_CORE0, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

int main(void)
{
    stdio_init_all();
    multicore_launch_core1(core1_periodic);
#if EOS_CORE0_LOAD
    configASSERT(xTaskCreate(load_task, "load", 512, NULL, 1, NULL) == pdPASS);
#endif
    vTaskStartScheduler();
    for (;;) {}
}

#elif EOS_LAB_MODE == 2
static void core1_fifo_echo(void)
{
    cycle_counter_init();
    gpio_init(PIN_RESP);
    gpio_set_dir(PIN_RESP, GPIO_OUT);
    for (;;) {
        const uint32_t word = multicore_fifo_pop_blocking();
        gpio_put(PIN_RESP, 1);
        multicore_fifo_push_blocking(word + 1u);
        gpio_put(PIN_RESP, 0);
    }
}

static void fifo_ping_task(void *arg)
{
    (void)arg;
    gpio_init(PIN_REQ);
    gpio_set_dir(PIN_REQ, GPIO_OUT);
    for (;;) {
        /* TODO FIFO: use the DWT cycle counter to timestamp the round trip. */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

int main(void)
{
    stdio_init_all();
    cycle_counter_init();
    multicore_launch_core1(core1_fifo_echo);
    configASSERT(xTaskCreate(fifo_ping_task, "ping", 512, NULL, 1, NULL) == pdPASS);
    vTaskStartScheduler();
    for (;;) {}
}

#elif EOS_LAB_MODE == 4
/* ------------------------------------------------------------------------
 * Task 5 - hand records from core 1 to core 0.
 *
 * Core 1 (bare metal) produces a record every EOS_PRODUCE_US microseconds.
 * A FreeRTOS task on core 0 consumes the records and checks each one.
 *
 * A record has four words. The last word is a check value computed from the
 * first three, so a reader can tell whether it saw a whole record or half of
 * two different ones (a "torn" record).
 *
 *   EOS_HANDOFF_RING=OFF  one shared slot that the producer keeps rewriting
 *   EOS_HANDOFF_RING=ON   a ring buffer; you complete its memory ordering
 *
 * Only the consumer writes consumed/torn/gaps and only the producer writes
 * dropped. One writer per counter keeps the reporter's reads simple.
 * ---------------------------------------------------------------------- */
typedef struct {
    uint32_t seq;
    uint32_t stamp_us;
    uint32_t payload;
    uint32_t check;
} record_t;

static uint32_t record_check(uint32_t seq, uint32_t stamp_us, uint32_t payload)
{
    return seq ^ stamp_us ^ payload ^ 0xA5A5A5A5u;
}

static atomic_uint consumed;
static atomic_uint torn;
static atomic_uint gaps;
static atomic_uint dropped;

#if EOS_HANDOFF_RING == 0
/* One slot, no ownership rule: the producer rewrites it while the consumer
 * may be reading it. */
static volatile record_t g_slot;

static void producer_core1(void)
{
    uint32_t seq = 0;
    for (;;) {
        ++seq;
        const uint32_t stamp_us = time_us_32();  /* chip-wide 1 MHz timer */
        const uint32_t payload = seq * 2654435761u;
        g_slot.seq = seq;
        g_slot.stamp_us = stamp_us;
        g_slot.payload = payload;
        g_slot.check = record_check(seq, stamp_us, payload);
        busy_wait_us_32(EOS_PRODUCE_US);
    }
}

static void consumer_task(void *arg)
{
    (void)arg;
    for (;;) {
        record_t r;
        r.seq = g_slot.seq;
        r.stamp_us = g_slot.stamp_us;
        r.payload = g_slot.payload;
        r.check = g_slot.check;
        atomic_fetch_add_explicit(&consumed, 1, memory_order_relaxed);
        if (r.check != record_check(r.seq, r.stamp_us, r.payload)) {
            atomic_fetch_add_explicit(&torn, 1, memory_order_relaxed);
        }
        if (EOS_CONSUME_DELAY_US) {
            busy_wait_us_32(EOS_CONSUME_DELAY_US);
        }
        taskYIELD();
    }
}
#else
/* A single-producer single-consumer ring. Only the producer writes `head`
 * and only the consumer writes `tail`, so no lock is needed - but the ORDER
 * in which data and index become visible to the other core is the contract.
 *
 * TODO RING: each macro below names the memory order of one operation. They
 * start as memory_order_relaxed, which promises no ordering at all. Choose
 * the order each operation needs, then write one sentence per operation
 * saying what it protects:
 *   - the producer's load of `tail`  (is the slot it wants to reuse free?)
 *   - the producer's store of `head` (is the record complete before it is
 *                                     announced?)
 *   - the consumer's load of `head`  (is the record it will read announced?)
 *   - the consumer's store of `tail` (has the consumer finished reading
 *                                     before the slot is handed back?)
 */
#define ORDER_PRODUCER_LOAD_TAIL  memory_order_relaxed
#define ORDER_PRODUCER_STORE_HEAD memory_order_relaxed
#define ORDER_CONSUMER_LOAD_HEAD  memory_order_relaxed
#define ORDER_CONSUMER_STORE_TAIL memory_order_relaxed

enum { RING_SIZE = 16 };  /* a power of two */
static record_t ring[RING_SIZE];
static atomic_uint head;
static atomic_uint tail;

static void producer_core1(void)
{
    uint32_t seq = 0;
    for (;;) {
        ++seq;  /* the sequence advances even when the ring is full */
        const uint32_t stamp_us = time_us_32();
        const uint32_t payload = seq * 2654435761u;
        const unsigned h = atomic_load_explicit(&head, memory_order_relaxed);
        const unsigned t = atomic_load_explicit(&tail, ORDER_PRODUCER_LOAD_TAIL);
        if (h - t >= RING_SIZE) {
            atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed);
        } else {
            record_t *slot = &ring[h % RING_SIZE];
            slot->seq = seq;
            slot->stamp_us = stamp_us;
            slot->payload = payload;
            slot->check = record_check(seq, stamp_us, payload);
            atomic_store_explicit(&head, h + 1, ORDER_PRODUCER_STORE_HEAD);
        }
        busy_wait_us_32(EOS_PRODUCE_US);
    }
}

static void consumer_task(void *arg)
{
    (void)arg;
    uint32_t last_seq = 0;
    for (;;) {
        const unsigned t = atomic_load_explicit(&tail, memory_order_relaxed);
        const unsigned h = atomic_load_explicit(&head, ORDER_CONSUMER_LOAD_HEAD);
        if (t == h) {
            taskYIELD();  /* empty: let other tasks run, then look again */
            continue;
        }
        const record_t r = ring[t % RING_SIZE];
        if (r.check != record_check(r.seq, r.stamp_us, r.payload)) {
            atomic_fetch_add_explicit(&torn, 1, memory_order_relaxed);
        }
        if (r.seq != last_seq + 1u) {
            atomic_fetch_add_explicit(&gaps, r.seq - last_seq - 1u, memory_order_relaxed);
        }
        last_seq = r.seq;
        atomic_fetch_add_explicit(&consumed, 1, memory_order_relaxed);
        atomic_store_explicit(&tail, t + 1, ORDER_CONSUMER_STORE_TAIL);
        if (EOS_CONSUME_DELAY_US) {
            busy_wait_us_32(EOS_CONSUME_DELAY_US);
        }
    }
}
#endif

static void report_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        printf("handoff ring=%d: consumed=%u torn=%u gaps=%u dropped=%u (totals since boot)\n",
               EOS_HANDOFF_RING,
               atomic_load_explicit(&consumed, memory_order_relaxed),
               atomic_load_explicit(&torn, memory_order_relaxed),
               atomic_load_explicit(&gaps, memory_order_relaxed),
               atomic_load_explicit(&dropped, memory_order_relaxed));
    }
}

int main(void)
{
    stdio_init_all();
    /* Launch core 1 before the scheduler starts, as in the AMP experiment. */
    multicore_launch_core1(producer_core1);
    configASSERT(xTaskCreate(consumer_task, "consumer", 512, NULL, 1, NULL) == pdPASS);
    configASSERT(xTaskCreate(report_task, "report", 768, NULL, 2, NULL) == pdPASS);
    vTaskStartScheduler();
    for (;;) {}
}

#elif EOS_LAB_MODE == 5
/* ------------------------------------------------------------------------
 * Task 6 - does a second core pay off for this workload?
 *
 * A two-stage pipeline processes blocks of 256 samples:
 *   stage A  prepare a block   (EOS_STAGE_A_ROUNDS filter rounds)
 *   stage B  analyse the block (EOS_STAGE_B_ROUNDS filter rounds)
 * EOS_CAPACITY_PLACEMENT decides where the stages run:
 *   SINGLE      both stages in one task on core 0
 *   AMP         stage A bare metal on core 1, stage B a task on core 0
 *   SMP_PINNED  two tasks, A allowed only on core 1, B only on core 0
 *   SMP_FREE    two tasks, each allowed on either core
 *
 * Blocks live in a small pool. Each block has a state flag that says which
 * stage owns it: FREE (stage A may fill it) or FULL (stage B may read it).
 * The flag is the ownership handoff, so it is written with release and read
 * with acquire.
 *
 * The age of a block is the time from the start of stage A to the end of
 * stage B, read from the chip-wide 1 MHz timer that both cores can use.
 * ---------------------------------------------------------------------- */
enum { BLOCK_SAMPLES = 256, POOL_BLOCKS = 4, BLOCK_FREE = 0, BLOCK_FULL = 1 };

typedef struct {
    uint16_t samples[BLOCK_SAMPLES];
    uint32_t t_start_us;
    atomic_uint state;
} block_t;

static block_t pool[POOL_BLOCKS];

/* Written by one stage each; the reporter swaps them back to zero each second. */
static atomic_uint blocks_done;
static atomic_uint age_sum_us;
static atomic_uint age_max_us;
static atomic_uint a_time_sum_us;
static atomic_uint a_count;
static atomic_uint b_time_sum_us;
static atomic_uint b_count;
static atomic_uint checksum_sink;

/* A dependent chain: each sample uses the one before it, so the work cannot
 * be reordered away and its time scales with `rounds`. */
static void filter_rounds(uint16_t *s, unsigned rounds)
{
    for (unsigned r = 0; r < rounds; ++r) {
        for (unsigned i = 1; i < BLOCK_SAMPLES; ++i) {
            s[i] = (uint16_t)(s[i - 1] * 3u + s[i] + (s[i] >> 2));
        }
    }
}

static void stage_a(block_t *b)
{
    const uint32_t start = time_us_32();
    b->t_start_us = start;
    for (unsigned i = 0; i < BLOCK_SAMPLES; ++i) {
        b->samples[i] = (uint16_t)(i * 7u + start);
    }
    filter_rounds(b->samples, EOS_STAGE_A_ROUNDS);
    atomic_fetch_add_explicit(&a_time_sum_us, time_us_32() - start, memory_order_relaxed);
    atomic_fetch_add_explicit(&a_count, 1, memory_order_relaxed);
}

static void stage_b(block_t *b)
{
    const uint32_t start = time_us_32();
    filter_rounds(b->samples, EOS_STAGE_B_ROUNDS);
    uint32_t sum = 0;
    for (unsigned i = 0; i < BLOCK_SAMPLES; ++i) {
        sum += b->samples[i];
    }
    atomic_store_explicit(&checksum_sink, sum, memory_order_relaxed);  /* keep the work */
    const uint32_t end = time_us_32();
    const uint32_t age = end - b->t_start_us;
    atomic_fetch_add_explicit(&b_time_sum_us, end - start, memory_order_relaxed);
    atomic_fetch_add_explicit(&b_count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&age_sum_us, age, memory_order_relaxed);
    unsigned previous_max = atomic_load_explicit(&age_max_us, memory_order_relaxed);
    while (age > previous_max &&
           !atomic_compare_exchange_weak_explicit(&age_max_us, &previous_max, age,
                                                  memory_order_relaxed, memory_order_relaxed)) {}
    atomic_fetch_add_explicit(&blocks_done, 1, memory_order_relaxed);
}

#if EOS_PLACEMENT == 0
static void pipeline_task(void *arg)
{
    (void)arg;
    unsigned next = 0;
    for (;;) {
        block_t *b = &pool[next++ % POOL_BLOCKS];
        stage_a(b);
        stage_b(b);
    }
}
#else
/* Stage A waits for a FREE block, fills it, then hands it over as FULL. */
static void stage_a_loop(void)
{
    unsigned next = 0;
    for (;;) {
        block_t *b = &pool[next++ % POOL_BLOCKS];
        while (atomic_load_explicit(&b->state, memory_order_acquire) != BLOCK_FREE) {
            /* Waiting burns cycles on this core: a pool this small is the
             * back-pressure that keeps the stages in step. */
        }
        stage_a(b);
        atomic_store_explicit(&b->state, BLOCK_FULL, memory_order_release);
    }
}

/* Stage B waits for a FULL block, analyses it, then hands it back as FREE. */
static void stage_b_loop(void)
{
    unsigned next = 0;
    for (;;) {
        block_t *b = &pool[next++ % POOL_BLOCKS];
        while (atomic_load_explicit(&b->state, memory_order_acquire) != BLOCK_FULL) {
            taskYIELD();
        }
        stage_b(b);
        atomic_store_explicit(&b->state, BLOCK_FREE, memory_order_release);
    }
}

static void stage_a_task(void *arg) { (void)arg; stage_a_loop(); }
static void stage_b_task(void *arg) { (void)arg; stage_b_loop(); }
#endif

static unsigned take(atomic_uint *counter)
{
    return atomic_exchange_explicit(counter, 0, memory_order_relaxed);
}

static void report_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const unsigned done = take(&blocks_done);
        const unsigned age_sum = take(&age_sum_us);
        const unsigned age_max = take(&age_max_us);
        const unsigned a_sum = take(&a_time_sum_us);
        const unsigned a_n = take(&a_count);
        const unsigned b_sum = take(&b_time_sum_us);
        const unsigned b_n = take(&b_count);
        printf("capacity placement=%d: blocks/s=%u mean_age=%u us max_age=%u us "
               "T_A=%u us T_B=%u us\n",
               EOS_PLACEMENT, done, done ? age_sum / done : 0u, age_max,
               a_n ? a_sum / a_n : 0u, b_n ? b_sum / b_n : 0u);
    }
}

int main(void)
{
    stdio_init_all();
#if EOS_PLACEMENT == 0
    configASSERT(xTaskCreate(pipeline_task, "pipeline", 1024, NULL, 1, NULL) == pdPASS);
#elif EOS_PLACEMENT == 1
    multicore_launch_core1(stage_a_loop);
    configASSERT(xTaskCreate(stage_b_task, "stage_b", 1024, NULL, 1, NULL) == pdPASS);
#else
    TaskHandle_t a_handle = NULL;
    TaskHandle_t b_handle = NULL;
    configASSERT(xTaskCreate(stage_a_task, "stage_a", 1024, NULL, 1, &a_handle) == pdPASS);
    configASSERT(xTaskCreate(stage_b_task, "stage_b", 1024, NULL, 1, &b_handle) == pdPASS);
#if EOS_PLACEMENT == 2
    vTaskCoreAffinitySet(a_handle, (UBaseType_t)(1u << 1));
    vTaskCoreAffinitySet(b_handle, (UBaseType_t)(1u << 0));
#else
    vTaskCoreAffinitySet(a_handle, (UBaseType_t)3u);
    vTaskCoreAffinitySet(b_handle, (UBaseType_t)3u);
#endif
#endif
    configASSERT(xTaskCreate(report_task, "report", 768, NULL, 2, NULL) == pdPASS);
    vTaskStartScheduler();
    for (;;) {}
}

#else
static atomic_uchar seen_cores[4];
static atomic_uchar control_core_mask;
static atomic_uint worst_period_error_us;
static TaskHandle_t control_task_handle;
static TaskHandle_t load_task_handle;

static void load_task(void *arg)
{
    (void)arg;
    gpio_init(PIN_REQ);
    gpio_set_dir(PIN_REQ, GPIO_OUT);
    for (;;) {
        gpio_put(PIN_REQ, 1);
        busy_wait_us_32(20000);
        gpio_put(PIN_REQ, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void worker_task(void *arg)
{
    const unsigned id = (unsigned)(uintptr_t)arg;
    for (;;) {
        /* TODO SMP: record this task's observed core in seen_cores[id]. */
        const unsigned core = portGET_CORE_ID();
        gpio_put(core == 0 ? PIN_CORE0 : PIN_CORE1, 1);
        __asm volatile("nop");
        gpio_put(core == 0 ? PIN_CORE0 : PIN_CORE1, 0);
        taskYIELD();
    }
}

static void control_task(void *arg)
{
    (void)arg;
    gpio_init(PIN_RESP);
    gpio_set_dir(PIN_RESP, GPIO_OUT);
    uint32_t previous = time_us_32();
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1));
        const uint32_t now = time_us_32();
        const uint32_t period = now - previous;
        previous = now;
        const uint32_t error = period > 1000u ? period - 1000u : 1000u - period;
        /* TODO CONTROL: record which core ran this periodic task. */
        unsigned previous_max = atomic_load_explicit(&worst_period_error_us, memory_order_relaxed);
        while (error > previous_max &&
               !atomic_compare_exchange_weak_explicit(&worst_period_error_us, &previous_max, error,
                                                      memory_order_relaxed, memory_order_relaxed)) {}
        gpio_put(PIN_RESP, 1);
        gpio_put(PIN_RESP, 0);
    }
}

static void report_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const unsigned masks[4] = {
            atomic_load_explicit(&seen_cores[0], memory_order_relaxed),
            atomic_load_explicit(&seen_cores[1], memory_order_relaxed),
            atomic_load_explicit(&seen_cores[2], memory_order_relaxed),
            atomic_load_explicit(&seen_cores[3], memory_order_relaxed),
        };
        const unsigned control_mask = atomic_exchange_explicit(&control_core_mask, 0, memory_order_relaxed);
        const unsigned worst = atomic_exchange_explicit(&worst_period_error_us, 0, memory_order_relaxed);
        printf("worker masks: %u %u %u %u; control mask: %u; worst 1ms period error: %u us\n",
               masks[0], masks[1], masks[2], masks[3], control_mask, worst);
    }
}

int main(void)
{
    stdio_init_all();
    gpio_init(PIN_CORE0);
    gpio_init(PIN_CORE1);
    gpio_init(PIN_REQ);
    gpio_set_dir(PIN_CORE0, GPIO_OUT);
    gpio_set_dir(PIN_CORE1, GPIO_OUT);
    gpio_set_dir(PIN_REQ, GPIO_OUT);
    for (unsigned i = 0; i < 4; ++i) {
        configASSERT(xTaskCreate(worker_task, "worker", 512,
                                 (void *)(uintptr_t)i, 1, NULL) == pdPASS);
    }
    configASSERT(xTaskCreate(load_task, "load", 512, NULL, 1,
                             &load_task_handle) == pdPASS);
    configASSERT(xTaskCreate(control_task, "control", 512, NULL, 2,
                             &control_task_handle) == pdPASS);
    configASSERT(xTaskCreate(report_task, "report", 768, NULL, 2, NULL) == pdPASS);
    vTaskCoreAffinitySet(load_task_handle, (UBaseType_t)(1u << 0));
#if defined(EOS_PIN_CONTROL)
    vTaskCoreAffinitySet(control_task_handle, (UBaseType_t)(1u << 1));
#endif
    vTaskStartScheduler();
    for (;;) {}
}
#endif
