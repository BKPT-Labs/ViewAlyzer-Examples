/**
 * @file va_config.h
 * @brief Project-owned ViewAlyzer configuration (VA_CONFIG_HEADER pattern).
 *
 * Started from ViewAlyzerRecorder/core/va_config_template.h. Built with
 * -DVA_CONFIG_HEADER=va_config.h (a bare token, no quotes), so this file is
 * included before every default in ViewAlyzerConfig.h and anything set here
 * wins. It lives in the project, not in the vendored recorder tree, so
 * package updates leave it alone.
 *
 * This set answers "which task is blocking on which mutex, and how deep are
 * the stacks?" without paying for the ~700 mutex give/take events a second
 * this demo produces. Mutex CONTENTION still works with mutex events off:
 * the mutex and both tasks stay registered so the contention event can name
 * them, but no give/take events reach the wire (measured on a Nucleo-G474RE:
 * mutex 678 -> 0, contention 18 -> 19, everything else intact).
 *
 * Everything not named below follows VA_TRACE_DEFAULT=0 and is compiled out
 * entirely: no events, no packet builders, no registry storage, no kernel
 * trace hooks.
 */

#ifndef VA_CONFIG_H
#define VA_CONFIG_H

/* Nothing is traced unless it is named below. */
#define VA_TRACE_DEFAULT 0

#define VA_TRACE_TASKS              1
#define VA_TRACE_STACK_USAGE        1
#define VA_TRACE_ISRS               1
#define VA_TRACE_MUTEX_CONTENTION   1
#define VA_TRACE_USER_VALUES        1

/* Communication tracing: the app's Communication Map and Event Table draw
   queue, semaphore, mutex, and task-notification traffic from these. */
#define VA_TRACE_TASK_NOTIFICATIONS 1
#define VA_TRACE_MUTEXES            1
#define VA_TRACE_SEMAPHORES         1
#define VA_TRACE_QUEUES             1

/* Timer tracing: the app's Timers view draws arm/fire/lateness from these.
   Software timers are a handful of events per second in this demo. */
#define VA_TRACE_TIMERS             1
#define VA_TRACE_EVENT_FLAGS        1

/* RAM buffer transport (built with -DVA_RAMBUF=ON)
   The ring size lives in CMakeLists.txt (-DVA_RAMBUF_SIZE=16384u); do
   not also define it here or the compiler flags a redefinition.
   Placement is a knob too: the G4 has no data cache, so the default
   .bss spot streams fine - define this only to pin the ring to a
   specific RAM region (e.g. CCM-SRAM, keeping it off the SRAM your app
   uses) via a section from your linker script. The host finds the ring
   wherever it lands (ELF symbol or magic-tag scan): */
/* #define VA_RAMBUF_ATTRIBUTES __attribute__((section(".va_rambuf"))) */

#endif /* VA_CONFIG_H */
