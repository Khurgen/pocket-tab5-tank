# Pocket Tank: M5Stack Tab5 Port

This branch is the hardware-port workstream for running Pocket Tank on the M5Stack Tab5.

## Porting policy

The original Pocket Tank architecture is preserved wherever possible. The portable simulation, renderer, progression logic, tokenizer, and quantized model code under `common/` should remain shared. Tab5-specific work belongs in a separate ESP-IDF target so the original Waveshare ESP32-S3 firmware remains intact.

## Hardware target

- M5Stack Tab5
- ESP32-P4 main controller
- 16 MB flash
- 32 MB PSRAM
- 1280 x 720 MIPI-DSI touchscreen
- Official Espressif M5Stack Tab5 BSP
- ESP-IDF 5.4 or newer

## Development sequence

### v0.1.x: BSP/display smoke test

Goal: prove the Tab5 toolchain and official BSP before integrating Pocket Tank.

Acceptance criteria:

1. Clean ESP-IDF configure/build for ESP32-P4.
2. Tab5 boots without panic.
3. Official BSP initializes the display.
4. 1280 x 720 LVGL screen is visible.
5. Serial log prints a clear PASS marker.
6. Device remains stable for at least ten minutes.

No Pocket Tank model, touch gestures, audio, persistence, battery logic, or original renderer is required at this stage.

### v0.2.x: Pocket Tank renderer

- Link the existing `common/` tank and renderer code.
- Preserve the original logical 448 x 368 RGB565 framebuffer initially.
- Scale or blit that framebuffer to the Tab5 display.
- Measure FPS and memory use before attempting a native 1280 x 720 renderer.

### v0.3.x: Touch

Create a Tab5 implementation of the existing `touch_port.h` contract using the BSP touch input. Preserve Pocket Tank gesture semantics instead of rewriting interaction logic.

### v0.4.x: Local LLM

- Map the existing quantized model partition.
- Use the portable scalar inference path first.
- Do not attempt to compile the ESP32-S3 PIE assembly path on ESP32-P4.
- Establish correctness and benchmark tokens/second before optimizing for RISC-V/P4.

### v0.5.x and later

Add concurrency tuning, audio, RTC, IMU, persistence, power management, battery monitoring, and finally a Tab5-native 1280 x 720 presentation.

## Non-negotiable compatibility rule

Do not refactor working shared code merely to make it look Tab5-native. First create a behaviorally equivalent port. Optimize or redesign only after a known-good physical baseline exists.

## Upstream license

Pocket Tank is MIT licensed. Preserve the upstream copyright notice and license in derived distributions.

## Current status

- [x] Fork created as `Khurgen/pocket-tab5-tank`
- [x] Port branch created: `tab5-port`
- [x] Separate `firmware-tab5/` smoke-test target started
- [ ] First clean ESP32-P4 build
- [ ] First successful physical Tab5 display boot
- [ ] Renderer integration
- [ ] Touch integration
- [ ] LLM integration
