// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
#![no_std]
#![no_main]

use core::{future::poll_fn, ptr, task::Poll};
use cortex_m_rt::entry;
use embassy_executor::{Executor, Spawner};
use embassy_stm32::gpio::{Level, Output, Speed};
use embassy_sync::{
    blocking_mutex::raw::CriticalSectionRawMutex, channel::Channel, mutex::Mutex,
    semaphore::GreedySemaphore, signal::Signal,
};
use embassy_time::{Duration, Instant};
use panic_halt as _;
use viewalyzer_embassy::{Traced, sleep, spawn_named};

type Raw = CriticalSectionRawMutex;
const QUEUE_SLOTS: usize = 8;
type Samples = Channel<Raw, u32, QUEUE_SLOTS>;
type Display = Mutex<Raw, u32>;
type Slot = GreedySemaphore<Raw>;

static SAMPLES: Samples = Channel::new();
static DISPLAY: Display = Mutex::new(0);
static SLOT: Slot = GreedySemaphore::new(1);

type Readings = Channel<Raw, i32, QUEUE_SLOTS>;
static RAW_READINGS: Readings = Channel::new();
static FILTERED_READINGS: Readings = Channel::new();
static SETPOINT: Signal<Raw, i32> = Signal::new();
const SIGNAL_MIDPOINT: i32 = 512;

// The C shim uses the recorder's public event types and logging macros.
unsafe extern "C" {
    safe fn demo_register_graphs();
    safe fn demo_log_raw(value: i32);
    safe fn demo_log_filtered(value: i32);
    safe fn demo_log_control(target: i32, error: i32);
    safe fn demo_log_sample_try_send(queue: usize, task: u32, used: u32, sent: bool);
}

#[entry]
fn main() -> ! {
    let p = embassy_stm32::init(Default::default());
    let hz = embassy_stm32::rcc::clocks(&p.RCC)
        .hclk1
        .to_hertz()
        .unwrap()
        .0;
    let led = Output::new(p.PA5, Level::Low, Speed::Low);
    let executor = cortex_m::singleton!(: Executor = Executor::new()).unwrap();
    executor.run(|spawner| {
        viewalyzer_embassy::init(spawner, hz, 1);
        demo_register_graphs();
        let samples = Traced::channel(&SAMPLES, c"Samples");
        let display = Traced::mutex(&DISPLAY, c"Display");
        let slot = Traced::semaphore(&SLOT, c"ADC permit");
        spawn_named(spawner, producer(samples, spawner).unwrap(), c"Producer");
        spawn_named(
            spawner,
            consumer(samples, display, led).unwrap(),
            c"Consumer",
        );
        spawn_named(spawner, refresh(display, slot).unwrap(), c"Refresh");
        spawn_named(spawner, calibrate(slot).unwrap(), c"Calibration");
        let raw = Traced::channel(&RAW_READINGS, c"Raw readings");
        let filtered = Traced::channel(&FILTERED_READINGS, c"Filtered data");
        spawn_named(spawner, sensor(raw).unwrap(), c"Sensor");
        spawn_named(spawner, filter(raw, filtered).unwrap(), c"Filter");
        spawn_named(spawner, setpoint().unwrap(), c"Setpoint");
        spawn_named(spawner, controller(filtered).unwrap(), c"Controller");
    })
}

#[embassy_executor::task]
async fn producer(samples: Traced<Samples>, spawner: Spawner) {
    let task =
        poll_fn(|cx| Poll::Ready(embassy_executor::raw::task_from_waker(cx.waker()).id())).await;
    let mut sequence = 0u32;
    for burst in (0..4).cycle() {
        // Burst past channel capacity, then leave the consumer waiting for input.
        for index in 0..10 {
            // The ninth sample of every fourth burst is expendable.
            if burst == 0 && index == QUEUE_SLOTS {
                let sent = SAMPLES.try_send(sequence).is_ok();
                demo_log_sample_try_send(
                    ptr::from_ref(&SAMPLES) as usize,
                    task,
                    SAMPLES.len() as u32,
                    sent,
                );
            } else {
                samples.send(sequence).await;
            }
            sequence = sequence.wrapping_add(1);
            if sequence == 25 || sequence == 50 {
                spawn_named(spawner, heartbeat().unwrap(), c"Heartbeat");
            }
        }
        sleep(Duration::from_millis(330)).await;
    }
}

#[embassy_executor::task]
async fn heartbeat() {
    embassy_futures::yield_now().await;
    sleep(Duration::from_millis(10)).await;
}

#[embassy_executor::task]
async fn consumer(samples: Traced<Samples>, display: Traced<Display>, mut led: Output<'static>) {
    loop {
        let sample = samples.receive().await;
        let mut guard = display.lock().await;
        *guard = sample;
        led.toggle();
        sleep(Duration::from_millis(18)).await;
    }
}

#[embassy_executor::task]
async fn refresh(display: Traced<Display>, slot: Traced<Slot>) {
    loop {
        {
            let _permit = slot.acquire().await;
            let _display = display.lock().await;
            sleep(Duration::from_millis(25)).await;
        }
        sleep(Duration::from_millis(30)).await;
    }
}

#[embassy_executor::task]
async fn calibrate(slot: Traced<Slot>) {
    loop {
        {
            let _permit = slot.acquire().await;
            sleep(Duration::from_millis(15)).await;
        }
        sleep(Duration::from_millis(28)).await;
    }
}

fn sine_sample(period_ms: u64, amplitude: f32) -> i32 {
    let phase_ms = Instant::now().as_millis() % period_ms;
    let angle = core::f32::consts::TAU * phase_ms as f32 / period_ms as f32;
    SIGNAL_MIDPOINT + libm::roundf(amplitude * libm::sinf(angle)) as i32
}

#[embassy_executor::task]
async fn sensor(raw: Traced<Readings>) {
    loop {
        let reading = sine_sample(3200, 512.0);
        demo_log_raw(reading);
        raw.send(reading).await;
        sleep(Duration::from_millis(16)).await;
    }
}

#[embassy_executor::task]
async fn filter(raw: Traced<Readings>, filtered: Traced<Readings>) {
    let mut estimate = SIGNAL_MIDPOINT;
    // Move the backlog between the two queues while plotting each real result.
    for interval_ms in [21, 11].into_iter().cycle() {
        for _ in 0..32 {
            let reading = raw.receive().await;
            estimate += (reading - estimate) / 4;
            demo_log_filtered(estimate);
            filtered.send(estimate).await;
            sleep(Duration::from_millis(interval_ms)).await;
        }
    }
}

#[embassy_executor::task]
async fn setpoint() {
    loop {
        SETPOINT.signal(sine_sample(4800, 256.0));
        sleep(Duration::from_millis(16)).await;
    }
}

#[embassy_executor::task]
async fn controller(filtered: Traced<Readings>) {
    let mut target = SIGNAL_MIDPOINT;
    loop {
        let estimate = filtered.receive().await;
        if let Some(latest) = SETPOINT.try_take() {
            target = latest;
        }
        demo_log_control(target, target - estimate);
        sleep(Duration::from_millis(16)).await;
    }
}
