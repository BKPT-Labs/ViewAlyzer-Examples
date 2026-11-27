// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
#![no_std]
#![no_main]

use cortex_m_rt::entry;
use embassy_executor::{Executor, Spawner};
use embassy_stm32::gpio::{Level, Output, Speed};
use embassy_sync::{
    blocking_mutex::raw::CriticalSectionRawMutex,
    channel::Channel,
    mutex::Mutex,
    semaphore::{GreedySemaphore, Semaphore},
    signal::Signal,
};
use embassy_time::{Duration, Instant, Timer};
use panic_halt as _;

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
        spawner.spawn(producer(spawner).unwrap());
        spawner.spawn(consumer(led).unwrap());
        spawner.spawn(refresh().unwrap());
        spawner.spawn(calibrate().unwrap());
        spawner.spawn(sensor().unwrap());
        spawner.spawn(filter().unwrap());
        spawner.spawn(setpoint().unwrap());
        spawner.spawn(controller().unwrap());
    })
}

#[embassy_executor::task(name = "Producer")]
async fn producer(spawner: Spawner) {
    let mut sequence = 0u32;
    for burst in (0..4).cycle() {
        // Burst past channel capacity, then leave the consumer waiting for input.
        for index in 0..10 {
            // The ninth sample of every fourth burst is expendable.
            if burst == 0 && index == QUEUE_SLOTS {
                let _ = SAMPLES.try_send(sequence);
            } else {
                SAMPLES.send(sequence).await;
            }
            sequence = sequence.wrapping_add(1);
            if sequence == 25 || sequence == 50 {
                spawner.spawn(heartbeat().unwrap());
            }
        }
        Timer::after_millis(330).await;
    }
}

#[embassy_executor::task(name = "Heartbeat")]
async fn heartbeat() {
    let metadata = embassy_executor::MetadataRef::for_current_task().await;
    metadata.set_priority(1);
    metadata.set_deadline((Instant::now() + Duration::from_millis(25)).as_ticks());
    embassy_futures::yield_now().await;
    Timer::after_millis(10).await;
}

#[embassy_executor::task(name = "Consumer")]
async fn consumer(mut led: Output<'static>) {
    loop {
        let sample = SAMPLES.receive().await;
        let mut guard = DISPLAY.lock().await;
        *guard = sample;
        led.toggle();
        Timer::after_millis(18).await;
    }
}

#[embassy_executor::task(name = "Refresh")]
async fn refresh() {
    loop {
        {
            let _permit = SLOT.acquire(1).await.unwrap();
            let _display = DISPLAY.lock().await;
            Timer::after_millis(25).await;
        }
        Timer::after_millis(30).await;
    }
}

#[embassy_executor::task(name = "Calibration")]
async fn calibrate() {
    loop {
        {
            let _permit = SLOT.acquire(1).await.unwrap();
            Timer::after_millis(15).await;
        }
        Timer::after_millis(28).await;
    }
}

fn sine_sample(period_ms: u64, amplitude: f32) -> i32 {
    let phase_ms = Instant::now().as_millis() % period_ms;
    let angle = core::f32::consts::TAU * phase_ms as f32 / period_ms as f32;
    SIGNAL_MIDPOINT + libm::roundf(amplitude * libm::sinf(angle)) as i32
}

#[embassy_executor::task(name = "Sensor")]
async fn sensor() {
    loop {
        let reading = sine_sample(3200, 512.0);
        demo_log_raw(reading);
        RAW_READINGS.send(reading).await;
        Timer::after_millis(16).await;
    }
}

#[embassy_executor::task(name = "Filter")]
async fn filter() {
    let mut estimate = SIGNAL_MIDPOINT;
    // Move the backlog between the two queues while plotting each real result.
    for interval_ms in [21, 11].into_iter().cycle() {
        for _ in 0..32 {
            let reading = RAW_READINGS.receive().await;
            estimate += (reading - estimate) / 4;
            demo_log_filtered(estimate);
            FILTERED_READINGS.send(estimate).await;
            Timer::after_millis(interval_ms).await;
        }
    }
}

#[embassy_executor::task(name = "Setpoint")]
async fn setpoint() {
    loop {
        SETPOINT.signal(sine_sample(4800, 256.0));
        Timer::after_millis(16).await;
    }
}

#[embassy_executor::task(name = "Controller")]
async fn controller() {
    let mut target = SIGNAL_MIDPOINT;
    loop {
        let estimate = FILTERED_READINGS.receive().await;
        if let Some(latest) = SETPOINT.try_take() {
            target = latest;
        }
        demo_log_control(target, target - estimate);
        Timer::after_millis(16).await;
    }
}
