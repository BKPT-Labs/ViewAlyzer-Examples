/*
 * ViewAlyzer Zephyr demo
 *
 * One application that exercises every kernel-object type the ViewAlyzer
 * Recorder can trace, so each view in the host app has live data to show.
 * Every section below is independent — read (or delete) them one at a time.
 *
 *   Section                       What it shows in ViewAlyzer
 *   ─────────────────────────     ──────────────────────────────────────────
 *   Sine + sensor pipeline        user-value charts, message-queue depth
 *   FIFO/LIFO producer/consumer   k_fifo/k_lifo traffic and backlog
 *   Event-flag waiter             event posts, satisfied and timed-out waits
 *   Deferred-work demo            work submits, delayed work, cancels
 *   Suspend/resume (nap thread)   thread suspend windows
 *   Mutex contention trio         lock waits across three priorities
 *   Normal mutex trio             uncontended lock/unlock baseline
 *   Heap demo                     allocs/frees plus deliberate failures
 *   Workload manager              CPU-load profiles shifting between threads
 *   Heartbeat timer               k_timer start/stop/expiry
 *   Dynamically named thread      late k_thread_name_set handling
 *
 * The minimal integration (init + one user trace) is in the README; this
 * file is the everything-at-once version.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <math.h>
#include <string.h>
#include "ViewAlyzer.h"
#include "VA_Adapter_Zephyr.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define LED0_NODE DT_ALIAS(led0)
#define LED_BLINK_TIME_MS 50

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);


/* Sine wave state */
static volatile uint16_t sineVal;
static volatile uint16_t sine_index;
static volatile uint32_t tick_counter;
static uint16_t get_next_sine_value(void)
{
	float radians = (sine_index * 2.0f * (float)M_PI) / 360.0f;
	float s = sinf(radians);
	volatile uint16_t value = (uint16_t)((s + 1.0f) * 100.0f);
	sine_index = (sine_index + 1) % 360;
	return value;
}

/* Payloads carried through the message queues */
typedef struct {
	uint32_t sensorValue;
	uint32_t timestamp;
	uint8_t  taskId;
} SensorData_t;

typedef struct {
	uint8_t  profileIndex;
	uint32_t parameter;
} WorkloadCommand_t;

/* Message queues */
K_MSGQ_DEFINE(data_msgq, sizeof(SensorData_t), 10, 4);
K_MSGQ_DEFINE(command_msgq, sizeof(WorkloadCommand_t), 5, 4);

/* Mutexes */
K_MUTEX_DEFINE(shared_resource_mutex);
K_MUTEX_DEFINE(print_mutex);
K_MUTEX_DEFINE(contention_test_mutex);
K_MUTEX_DEFINE(normal_op_mutex);

/* Semaphores */
K_SEM_DEFINE(binary_sem, 0, 1);          /* binary semaphore, starts empty */
K_SEM_DEFINE(counting_sem, 0, 5);        /* counting semaphore, max 5 */
K_SEM_DEFINE(blink_sem, 1, 1);           /* blink LED guard */

/* Demo heap */
K_HEAP_DEFINE(demo_heap, 2048);

/* Event flags: default_task posts one of three bits every 500 ms; the
 * waiter needs BOTH low bits inside a short window, so the trace shows
 * posts, satisfied waits, and timed-out waits (failed ops). */
K_EVENT_DEFINE(demo_event);
#define DEMO_EVT_SENSOR_BIT   BIT(0)
#define DEMO_EVT_PROCESS_BIT  BIT(1)
#define DEMO_EVT_ALARM_BIT    BIT(2)

/* FIFO/LIFO pair: work_fifo carries filled items, free_lifo is the free
 * list they are recycled through, so both object kinds see steady traffic. */
K_FIFO_DEFINE(work_fifo);
K_LIFO_DEFINE(free_lifo);

struct work_item {
	void *fifo_reserved;   /* required first word for k_fifo/k_lifo */
	uint32_t payload;
};
static struct work_item work_pool[8];

static void register_viewalyzer_sync_objects(void)
{
	va_logQueueObjectCreateWithType(&data_msgq, "data_msgq_Queue");
	va_logQueueObjectCreateWithType(&command_msgq, "command_msgq_Queue");
	va_logQueueObjectCreateWithType(&work_fifo, "work_FifoQueue");
	va_logQueueObjectCreateWithType(&free_lifo, "free_LifoQueue");

	va_logQueueObjectCreateWithType(&shared_resource_mutex, "shared_resource_mutex_Mutex");
	va_logQueueObjectCreateWithType(&print_mutex, "print_mutex_Mutex");
	va_logQueueObjectCreateWithType(&contention_test_mutex, "contention_test_mutex_Mutex");
	va_logQueueObjectCreateWithType(&normal_op_mutex, "normal_op_mutex_Mutex");

	va_logQueueObjectCreateWithType(&binary_sem, "binary_sem_BinSem");
	va_logQueueObjectCreateWithType(&counting_sem, "counting_sem_CountSem");
	va_logQueueObjectCreateWithType(&blink_sem, "blink_sem_BinSem");

	va_logQueueObjectCreateWithType(&demo_heap, "demo_heap_Heap");
}

/* ── Demo timer ─────────────────────────────────────────────── */
static void heartbeat_timer_handler(struct k_timer *timer);
K_TIMER_DEFINE(heartbeat_timer, heartbeat_timer_handler, NULL);
static volatile uint32_t heartbeatCount;

static void heartbeat_timer_handler(struct k_timer *timer)
{
	/* The interesting part is the k_timer itself: its start/stop/expiry
	 * events appear in the timeline (the workload manager also restarts
	 * it with a new period on every profile change). */
	ARG_UNUSED(timer);
	heartbeatCount++;
}

/* Shared resources protected by mutex */
static volatile uint32_t sharedCounter;
static volatile float    sharedAccumulator;

/* Contention test variables */
static volatile uint32_t contentionCounter;
static volatile uint32_t highPrioAccess;
static volatile uint32_t medPrioAccess;
static volatile uint32_t lowPrioAccess;

/* Normal mutex shared variable */
static volatile uint32_t normalSharedValue;

/* Workload profiles cycled by the workload manager: each row assigns a
 * busy-loop length to four worker threads, so CPU load visibly shifts
 * between threads every few seconds. */
#define NUM_TASKS_TO_MANAGE 4
static volatile uint32_t task_workloads[NUM_TASKS_TO_MANAGE] = {1000, 1000, 1000, 1000};

static const uint32_t workload_profiles[][NUM_TASKS_TO_MANAGE] = {
	{4000, 1000, 1000, 6000},
	{1000, 4000, 6000, 1000},
	{6000, 6000, 1000, 1000},
	{1000, 1000, 1000, 1000},
	{2000, 5000, 3000, 4000},
	{5000, 2000, 4000, 3000},
	{3000, 4000, 2000, 5000},
	{4000, 3000, 5000, 2000},
};
#define NUM_PROFILES 8
static volatile int current_profile;


#if defined(CONFIG_SOC_STM32H503XX)
/* STM32H503RB has only 32 KB SRAM — use reduced but safe stacks. */
#define DEFAULT_STACK  512
#define LARGE_STACK    768
#else
#define DEFAULT_STACK  1024
#define LARGE_STACK    2048
#endif

/* Zephyr priorities: lower number = higher priority (opposite of FreeRTOS) */
#define PRIO_HIGHEST     2
#define PRIO_HIGH        3
#define PRIO_NORMAL      5
#define PRIO_LOW         7


static void blink_thread(void *p1, void *p2, void *p3)
{
	while (1) {
		k_sem_take(&blink_sem, K_FOREVER);
		gpio_pin_toggle_dt(&led);
		k_sem_give(&blink_sem);
		k_msleep(LED_BLINK_TIME_MS);
	}
}

K_THREAD_DEFINE(blink_tid, DEFAULT_STACK,
		blink_thread, NULL, NULL, NULL,
		PRIO_NORMAL, 0, 0);


extern const k_tid_t nap_tid;

static void default_task(void *p1, void *p2, void *p3)
{
	/* NOTE: do NOT call VA_LogString here — it puts a ~1 KB buffer on the
	 * stack and this thread only has DEFAULT_STACK bytes. The string is
	 * logged from main() instead, which has CONFIG_MAIN_STACK_SIZE.
	 */
	uint32_t phase = 0;

	while (1) {
		/* Release binary semaphore for other tasks */
		k_sem_give(&binary_sem);

		/* Give counting semaphore resources periodically */
		k_sem_give(&counting_sem);
		k_sem_give(&counting_sem);

		/* Send periodic commands to WorkloadManager */
		WorkloadCommand_t cmd = {
			.profileIndex = (k_uptime_get_32() / 5000) % NUM_PROFILES,
			.parameter    = k_uptime_get_32(),
		};
		k_msgq_put(&command_msgq, &cmd, K_NO_WAIT);

		/* Wake the self-suspending nap thread every 2 s */
		if ((++phase % 100) == 0) {
			k_thread_resume(nap_tid);
		}

		/* Post one event bit per 500 ms, detouring to the alarm bit
		 * every third second so the waiter sometimes times out. */
		if ((phase % 25) == 0) {
			uint32_t step = (phase / 25) % 6;
			uint32_t bits = (step < 2) ? DEMO_EVT_SENSOR_BIT
				      : (step < 4) ? DEMO_EVT_PROCESS_BIT
					           : DEMO_EVT_ALARM_BIT;
			k_event_post(&demo_event, bits);
		}

		k_msleep(20);
	}
}

K_THREAD_DEFINE(default_tid, DEFAULT_STACK,
		default_task, NULL, NULL, NULL,
		PRIO_HIGH, 0, 0);


static void sensor_task(void *p1, void *p2, void *p3)
{
	while (1) {
		sineVal = get_next_sine_value();

		/* Log sine as float (normalized -1.0 to 1.0) — 500 Hz for a smooth
		 * live chart (the SWO link has ample headroom at this rate). */
		float radians = (sine_index * 2.0f * (float)M_PI) / 360.0f;
		VA_LogTraceFloat(60, sinf(radians));

		/* Feed the message queue in irregular BURSTS (2..7 messages,
		 * 150..449 ms apart), never blocking — a full queue must not
		 * throttle the chart. The consumer drains one message every
		 * 40 ms, so the queue depth ramps to the burst size and empties
		 * before the next burst: a lively backlog pattern instead of a
		 * saturated queue or a metronome. */
		static uint32_t rng = 0x12345678u;
		static int32_t next_burst_ms = 250;

		next_burst_ms -= 2; /* loop cadence is 2 ms */
		if (next_burst_ms <= 0) {
			rng ^= rng << 13;
			rng ^= rng >> 17;
			rng ^= rng << 5;
			const int burst_len = 2 + (int)(rng % 6u);
			for (int burst = 0; burst < burst_len; burst++) {
				SensorData_t sd = {
					.sensorValue = sineVal,
					.timestamp   = k_uptime_get_32(),
					.taskId      = 2,
				};
				k_msgq_put(&data_msgq, &sd, K_NO_WAIT);
			}
			next_burst_ms = 150 + (int)((rng >> 8) % 300u);
		}

		/* Dynamic workload */
		volatile uint32_t wl = task_workloads[0];
		for (volatile uint32_t i = 0; i < wl; i++) {
			__NOP();
		}

		k_msleep(2);
	}
}

K_THREAD_DEFINE(sensor_tid, DEFAULT_STACK,
		sensor_task, NULL, NULL, NULL,
		PRIO_NORMAL, 0, 0);


static void processor_task(void *p1, void *p2, void *p3)
{
	SensorData_t rx;

	while (1) {
		if (k_msgq_get(&data_msgq, &rx, K_MSEC(100)) == 0) {
			/* Process received sensor data under mutex */
			if (k_mutex_lock(&shared_resource_mutex, K_MSEC(10)) == 0) {
				sharedCounter++;
				sharedAccumulator += rx.sensorValue;
				k_mutex_unlock(&shared_resource_mutex);
			}
		}

		/* Steady drain, slower than the producer's burst rate: the
		 * queue depth ramps up during each burst and empties between
		 * them */
		k_msleep(40);
	}
}

K_THREAD_DEFINE(processor_tid, DEFAULT_STACK,
		processor_task, NULL, NULL, NULL,
		PRIO_NORMAL, 0, 0);


/* ── FIFO/LIFO producer + consumer ──────────────────────────── */
/* Items circulate work_fifo -> free_lifo -> work_fifo; the producer runs
 * slightly faster than the consumer so the free list occasionally runs
 * dry and the fifo builds a small backlog. */
static void fifo_producer_task(void *p1, void *p2, void *p3)
{
	/* Seed the free list once. */
	for (size_t i = 0; i < ARRAY_SIZE(work_pool); i++) {
		k_lifo_put(&free_lifo, &work_pool[i]);
	}

	while (1) {
		struct work_item *item = k_lifo_get(&free_lifo, K_NO_WAIT);
		if (item != NULL) {
			item->payload = k_uptime_get_32();
			k_fifo_put(&work_fifo, item);
		}
		k_msleep(30);
	}
}

K_THREAD_DEFINE(fifo_producer_tid, DEFAULT_STACK,
		fifo_producer_task, NULL, NULL, NULL,
		PRIO_NORMAL, 0, 0);

static void fifo_consumer_task(void *p1, void *p2, void *p3)
{
	while (1) {
		struct work_item *item = k_fifo_get(&work_fifo, K_MSEC(200));
		if (item != NULL) {
			k_lifo_put(&free_lifo, item);
		}
		k_msleep(45);
	}
}

K_THREAD_DEFINE(fifo_consumer_tid, DEFAULT_STACK,
		fifo_consumer_task, NULL, NULL, NULL,
		PRIO_NORMAL, 0, 0);

/* ── Event-flag waiter ──────────────────────────────────────── */
/* Needs BOTH low bits inside 1.2 s (clear on match via the wait-all +
 * clear option), so some waits complete and some time out. */
static volatile uint32_t evt_waits_ok;
static volatile uint32_t evt_wait_timeouts;

static void evt_wait_task(void *p1, void *p2, void *p3)
{
	while (1) {
		uint32_t got = k_event_wait_all(&demo_event,
						DEMO_EVT_SENSOR_BIT | DEMO_EVT_PROCESS_BIT,
						false, K_MSEC(1200));
		if (got != 0) {
			k_event_clear(&demo_event,
				      DEMO_EVT_SENSOR_BIT | DEMO_EVT_PROCESS_BIT);
			evt_waits_ok++;
		} else {
			evt_wait_timeouts++;
		}
		k_msleep(50);
	}
}

K_THREAD_DEFINE(evt_wait_tid, DEFAULT_STACK,
		evt_wait_task, NULL, NULL, NULL,
		PRIO_NORMAL, 0, 0);

/* ── Deferred-work demo ─────────────────────────────────────── */
/* One immediate submit and one 250 ms schedule per cycle, plus a 400 ms
 * schedule that is canceled 100 ms in - so the trace shows submits,
 * armed windows that fire, and armed windows cut short by a cancel.
 * Handlers run on the system workqueue. */
static volatile uint32_t quick_work_runs;
static volatile uint32_t deferred_work_runs;

static void quick_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	quick_work_runs++;
}
K_WORK_DEFINE(quick_work, quick_work_handler);

static void deferred_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	deferred_work_runs++;
}
K_WORK_DELAYABLE_DEFINE(deferred_work, deferred_work_handler);

static void work_demo_task(void *p1, void *p2, void *p3)
{
	while (1) {
		k_work_submit(&quick_work);
		k_work_schedule(&deferred_work, K_MSEC(250));
		k_msleep(700);

		k_work_schedule(&deferred_work, K_MSEC(400));
		k_msleep(100);
		k_work_cancel_delayable(&deferred_work);
		k_msleep(600);
	}
}

K_THREAD_DEFINE(work_demo_tid, DEFAULT_STACK,
		work_demo_task, NULL, NULL, NULL,
		PRIO_NORMAL, 0, 0);

/* ── Suspend/resume demo ────────────────────────────────────── */
/* A burst of work, then self-suspend; default_task resumes it every 2 s.
 * Each suspend-to-resume window shows as one sleep period. */
static volatile uint32_t nap_wakeups;

static void nap_task(void *p1, void *p2, void *p3)
{
	while (1) {
		nap_wakeups++;
		k_msleep(30);
		k_thread_suspend(k_current_get());
	}
}

K_THREAD_DEFINE(nap_tid, DEFAULT_STACK,
		nap_task, NULL, NULL, NULL,
		PRIO_NORMAL, 0, 0);

/* Forward-declare thread IDs for notification targets */
extern const k_tid_t calculator_tid;
extern const k_tid_t stack_test_tid;
extern const k_tid_t consumer_tid;

static void notifier_task(void *p1, void *p2, void *p3)
{
	while (1) {
		k_msleep(100);
	}
}

K_THREAD_DEFINE(notifier_tid, DEFAULT_STACK,
		notifier_task, NULL, NULL, NULL,
		PRIO_NORMAL, 0, 0);


static void stack_test_task(void *p1, void *p2, void *p3)
{
	static uint32_t seed = 12345;

	while (1) {
		seed = seed * 1103515245 + 12345;
		uint32_t random_val = (seed >> 16) & 0x7FFF;

		/* Use array on stack to consume varying amounts */
#if defined(CONFIG_SOC_STM32H503XX)
		volatile uint32_t stack_consumer[20];
		uint32_t stack_words = 5 + (random_val % 16);
#else
		volatile uint32_t stack_consumer[80];
		uint32_t stack_words = 10 + (random_val % 71);
#endif

		for (uint32_t i = 0; i < stack_words; i++) {
			stack_consumer[i] = i + k_uptime_get_32();
		}

		volatile uint32_t sum = 0;
		for (uint32_t i = 0; i < stack_words; i++) {
			sum += stack_consumer[i];
		}

		if (k_mutex_lock(&shared_resource_mutex, K_MSEC(100)) == 0) {
			sharedCounter += stack_words;
			sharedAccumulator += sum;
			VA_LogTraceFloat(61, sharedAccumulator);
			k_mutex_unlock(&shared_resource_mutex);
		}

		/* Dynamic workload */
		volatile uint32_t wl = task_workloads[1];
		for (volatile uint32_t i = 0; i < wl; i++) {
			__NOP();
		}

		VA_LogTrace(43, k_uptime_get_32());
		k_msleep(5);
	}
}

K_THREAD_DEFINE(stack_test_tid, LARGE_STACK,
		stack_test_task, NULL, NULL, NULL,
		PRIO_NORMAL, 0, 0);


static void consumer_task(void *p1, void *p2, void *p3)
{
	while (1) {
		/* Wait for binary semaphore from DefaultTask */
		if (k_sem_take(&binary_sem, K_MSEC(1000)) == 0) {
			volatile uint32_t work = 0;
			for (int i = 0; i < 1000; i++) {
				work += i;
			}

			/* Try counting semaphore resource */
			if (k_sem_take(&counting_sem, K_MSEC(10)) == 0) {
				k_msleep(5);
				k_sem_give(&counting_sem);
			}
		}

		k_msleep(10);
	}
}

K_THREAD_DEFINE(consumer_tid, DEFAULT_STACK,
		consumer_task, NULL, NULL, NULL,
		PRIO_NORMAL, 0, 0);


static void worker_task(void *p1, void *p2, void *p3)
{
	while (1) {
		if (k_mutex_lock(&print_mutex, K_MSEC(100)) == 0) {
			volatile uint32_t protected_op = k_uptime_get_32() * 2;
			(void)protected_op;
			k_mutex_unlock(&print_mutex);
		}

		/* Dynamic workload */
		volatile uint32_t wl = task_workloads[2];
		for (volatile uint32_t i = 0; i < wl; i++) {
			__NOP();
		}

		k_msleep(100);
	}
}

K_THREAD_DEFINE(worker_tid, DEFAULT_STACK,
		worker_task, NULL, NULL, NULL,
		PRIO_NORMAL, 0, 0);


static void calculator_task(void *p1, void *p2, void *p3)
{
	while (1) {
		VA_LogEvent(45, USER_EVENT_START);

		if (k_mutex_lock(&shared_resource_mutex, K_MSEC(50)) == 0) {
			volatile uint32_t localCounter = sharedCounter;
			(void)localCounter;
			k_mutex_unlock(&shared_resource_mutex);
		}

		volatile uint32_t wastecycles = 0;
		for (volatile uint32_t i = 0; i < 1000; i++) {
			wastecycles += i;
		}

		/* Dynamic workload */
		volatile uint32_t wl = task_workloads[3];
		for (volatile uint32_t i = 0; i < wl; i++) {
			__NOP();
		}

		VA_LogEvent(45, USER_EVENT_END);

		k_msleep(10);
	}
}

K_THREAD_DEFINE(calculator_tid, DEFAULT_STACK,
		calculator_task, NULL, NULL, NULL,
		PRIO_NORMAL, 0, 0);


static void workload_manager_task(void *p1, void *p2, void *p3)
{
	WorkloadCommand_t rxcmd;
	int64_t lastChange = k_uptime_get();

	while (1) {
		if (k_msgq_get(&command_msgq, &rxcmd, K_MSEC(100)) == 0) {
			if (rxcmd.profileIndex < NUM_PROFILES
			    && rxcmd.profileIndex != current_profile) {
				current_profile = rxcmd.profileIndex;
				for (int i = 0; i < NUM_TASKS_TO_MANAGE; i++) {
					task_workloads[i] = workload_profiles[current_profile][i];
				}
				lastChange = k_uptime_get();

				/* Cycle the heartbeat timer with new profile period */
				k_timer_stop(&heartbeat_timer);
				k_timer_start(&heartbeat_timer,
					      K_MSEC(250 + current_profile * 100),
					      K_MSEC(250 + current_profile * 100));
			}
		}

		/* Auto profile change every 2.5 s */
		if ((k_uptime_get() - lastChange) > 2500) {
			current_profile = (current_profile + 1) % NUM_PROFILES;
			for (int i = 0; i < NUM_TASKS_TO_MANAGE; i++) {
				task_workloads[i] = workload_profiles[current_profile][i];
			}
			lastChange = k_uptime_get();

			/* Cycle the heartbeat timer: stop then restart with varied period */
			k_timer_stop(&heartbeat_timer);
			k_timer_start(&heartbeat_timer,
				      K_MSEC(250 + current_profile * 100),
				      K_MSEC(250 + current_profile * 100));
		}

		k_msleep(100);
	}
}

K_THREAD_DEFINE(wlmgr_tid, DEFAULT_STACK,
		workload_manager_task, NULL, NULL, NULL,
		PRIO_HIGHEST, 0, 0);


static void contention_low_task(void *p1, void *p2, void *p3)
{
	while (1) {
		/* Low priority holds mutex for a long time → causes contention */
		if (k_mutex_lock(&contention_test_mutex, K_FOREVER) == 0) {
			lowPrioAccess++;
			contentionCounter++;

			volatile uint32_t work = 0;
			for (int i = 0; i < 50000; i++) {
				work += i;
			}

			/* Hold mutex for 20 ms to ensure contention */
			k_msleep(20);

			k_mutex_unlock(&contention_test_mutex);
		}

		k_msleep(80);
	}
}

static void contention_med_task(void *p1, void *p2, void *p3)
{
	k_msleep(25);  /* offset start */

	while (1) {
		if (k_mutex_lock(&contention_test_mutex, K_MSEC(100)) == 0) {
			medPrioAccess++;
			contentionCounter++;

			volatile uint32_t work = 0;
			for (int i = 0; i < 10000; i++) {
				work += i;
			}
			k_msleep(5);

			k_mutex_unlock(&contention_test_mutex);
		}

		k_msleep(60);
	}
}

static void contention_high_task(void *p1, void *p2, void *p3)
{
	k_msleep(40);  /* offset start */

	while (1) {
		int64_t start = k_uptime_get();

		if (k_mutex_lock(&contention_test_mutex, K_MSEC(100)) == 0) {
			int32_t waitTime = (int32_t)(k_uptime_get() - start);
			highPrioAccess++;
			contentionCounter++;
			VA_LogTraceFloat(62, (float)waitTime);

			volatile uint32_t work = 0;
			for (int i = 0; i < 5000; i++) {
				work += i;
			}
			k_msleep(3);

			k_mutex_unlock(&contention_test_mutex);
		}

		k_msleep(50);
	}
}

K_THREAD_DEFINE(cont_low_tid, DEFAULT_STACK,
		contention_low_task, NULL, NULL, NULL,
		PRIO_LOW, 0, 0);

K_THREAD_DEFINE(cont_med_tid, DEFAULT_STACK,
		contention_med_task, NULL, NULL, NULL,
		PRIO_HIGH, 0, 0);

K_THREAD_DEFINE(cont_high_tid, DEFAULT_STACK,
		contention_high_task, NULL, NULL, NULL,
		PRIO_HIGHEST, 0, 0);


static void normal_low_task(void *p1, void *p2, void *p3)
{
	while (1) {
		if (k_mutex_lock(&normal_op_mutex, K_MSEC(50)) == 0) {
			normalSharedValue += 1;
			k_mutex_unlock(&normal_op_mutex);
		}

		volatile uint32_t work = 0;
		for (int i = 0; i < 20000; i++) {
			work += i;
		}

		k_msleep(200);
	}
}

static void normal_med_task(void *p1, void *p2, void *p3)
{
	k_msleep(25);

	while (1) {
		if (k_mutex_lock(&normal_op_mutex, K_MSEC(50)) == 0) {
			normalSharedValue += 10;
			k_mutex_unlock(&normal_op_mutex);
		}

		volatile uint32_t work = 0;
		for (int i = 0; i < 10000; i++) {
			work += i;
		}

		k_msleep(150);
	}
}

static void normal_high_task(void *p1, void *p2, void *p3)
{
	k_msleep(40);

	while (1) {
		if (k_mutex_lock(&normal_op_mutex, K_MSEC(50)) == 0) {
			normalSharedValue += 100;
			k_mutex_unlock(&normal_op_mutex);
		}

		volatile uint32_t work = 0;
		for (int i = 0; i < 5000; i++) {
			work += i;
		}
		tick_counter++;
		k_msleep(120);
	}
}

K_THREAD_DEFINE(norm_low_tid, DEFAULT_STACK,
		normal_low_task, NULL, NULL, NULL,
		PRIO_LOW, 0, 0);

K_THREAD_DEFINE(norm_med_tid, DEFAULT_STACK,
		normal_med_task, NULL, NULL, NULL,
		PRIO_HIGH, 0, 0);

K_THREAD_DEFINE(norm_high_tid, DEFAULT_STACK,
		normal_high_task, NULL, NULL, NULL,
		PRIO_HIGHEST, 0, 0);


/* ── Heap demo thread ───────────────────────────────────────── */
static void heap_demo_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	/* Cycle through a few allocation sizes so the event table
	   shows varying alloc amounts alongside free events. */
	static const size_t sizes[] = { 64, 128, 256, 32, 512 };
	int idx = 0;

	while (1) {
		size_t sz = sizes[idx % ARRAY_SIZE(sizes)];
		void *ptr = k_heap_alloc(&demo_heap, sz, K_NO_WAIT);
		if (ptr != NULL) {
			/* Simulate brief use of the buffer */
			memset(ptr, 0xAA, sz);
			k_msleep(80);
			k_heap_free(&demo_heap, ptr);
		}
		idx++;

		/* Every ~9 s: deliberately exhaust the heap so failed
		   allocations appear (red ticks on the host's heap gutter). */
		if ((idx % 24) == 0) {
			void *held[16];
			int n = 0;
			while (n < (int)ARRAY_SIZE(held)) {
				held[n] = k_heap_alloc(&demo_heap, 256, K_NO_WAIT);
				if (held[n] == NULL)
					break;   /* emits a heap-fail event */
				n++;
			}
			k_msleep(50);
			while (n-- > 0)
				k_heap_free(&demo_heap, held[n]);
		}
		k_msleep(300);
	}
}

K_THREAD_DEFINE(heap_demo_tid, DEFAULT_STACK,
		heap_demo_thread, NULL, NULL, NULL,
		PRIO_NORMAL, 0, 0);


/* Dynamically created thread, named AFTER creation via k_thread_name_set:
   must appear in the viewer as "DynNamed", not as the "thread" placeholder. */
static struct k_thread dyn_thread;
static K_THREAD_STACK_DEFINE(dyn_stack, 1024);

static void dyn_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		k_msleep(75);
	}
}

int main(void)
{
	int ret;

	if (!gpio_is_ready_dt(&led)) {
		printk("LED device not ready\n");
		return -1;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		printk("Failed to configure LED pin\n");
		return -1;
	}


	k_msleep(1000);                     /* give debugger time to attach */

	VA_Init(SystemCoreClock);

	/* Start heartbeat timer — 500 ms period */
	k_timer_start(&heartbeat_timer, K_MSEC(500), K_MSEC(500));

	/* Threads register themselves with the recorder as they run, so no
	 * explicit thread registration is needed here.  Sync objects get a
	 * one-time registration so they show up with readable names. */
	register_viewalyzer_sync_objects();

	/* User traces: pick a free id and a display name; the values arrive
	 * via VA_LogTrace / VA_LogTraceFloat calls in the threads above. */
	VA_RegisterUserTrace(43, "Tick Counter", VA_USER_TYPE_GRAPH);
	VA_RegisterUserTrace(60, "Sine Float", VA_USER_TYPE_GRAPH);
	VA_RegisterUserTrace(61, "Shared Accum", VA_USER_TYPE_GRAPH);
	VA_RegisterUserTrace(62, "HighPrio WaitF", VA_USER_TYPE_GRAPH);

	VA_RegisterUserEvent(45, "Calc Event");

	/* Logged from main (2 KB stack) — VA_LogString uses a ~1 KB stack buf. */
	VA_LogString(1, "System started");

	/* Cooperative priority (negative): must reach the viewer signed. */
	k_tid_t dyn_tid = k_thread_create(&dyn_thread, dyn_stack,
					  K_THREAD_STACK_SIZEOF(dyn_stack),
					  dyn_thread_fn, NULL, NULL, NULL,
					  K_PRIO_COOP(2), 0, K_NO_WAIT);
	k_msleep(200);                  /* let it register as unnamed first */
	k_thread_name_set(dyn_tid, "DynNamed");

	return 0;
}
