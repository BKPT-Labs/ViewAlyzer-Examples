// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
use std::{
    path::{Path, PathBuf},
    process::Command,
};

fn main() {
    let recorder = Path::new("../../../ViewAlyzer/ViewAlyzerRecorder");
    let cmsis = Path::new("../../freertos/Nucleo_G474_VA/Drivers/CMSIS");
    let core = recorder.join("core");
    let adapter = recorder.join("embassy/VA_Adapter_Embassy.c");
    let graphs = Path::new("src/graphs.c");
    let mut build = cc::Build::new();
    build
        .compiler("arm-none-eabi-gcc")
        .archiver("arm-none-eabi-ar")
        .flag("-mcpu=cortex-m4")
        .flag("-mfpu=fpv4-sp-d16")
        .flag("-mfloat-abi=hard")
        .include(".")
        .include(&core)
        .include(recorder.join("embassy"))
        .include(cmsis.join("Include"))
        .include(cmsis.join("Device/ST/STM32G4xx/Include"))
        .define("VA_CONFIG_HEADER", "va_config.h")
        .define("VA_DEVICE_HEADER", "stm32g474xx.h")
        .file(core.join("ViewAlyzer.c"))
        .file(&adapter)
        .file(graphs)
        .compile("viewalyzer_recorder");

    // Link the same Cortex-M4 hard-float newlib that supplied the C headers.
    let libc = Command::new(build.get_compiler().path())
        .args([
            "-mcpu=cortex-m4",
            "-mthumb",
            "-mfpu=fpv4-sp-d16",
            "-mfloat-abi=hard",
            "-print-file-name=libc_nano.a",
        ])
        .output()
        .expect("locate Arm GNU newlib");
    let path = PathBuf::from(String::from_utf8(libc.stdout).unwrap().trim());
    assert!(
        libc.status.success() && path.is_file(),
        "Arm GNU newlib-nano required"
    );
    println!(
        "cargo:rustc-link-search=native={}",
        path.parent().unwrap().display()
    );
    println!("cargo:rustc-link-lib=static=c_nano");
    let profile = recorder.join("embassy/VA_EmbassyConfig.h");
    for path in [
        Path::new("va_config.h"),
        graphs,
        &core,
        &adapter,
        &profile,
        cmsis,
    ] {
        println!("cargo:rerun-if-changed={}", path.display());
    }
}
