#pragma once

// FSC-BT1058 receive-mode I2S bridge configuration.
// The module has been verified on the target hardware with AT+I2SCFG=1:
// enabled, module master, 48 kHz, 16-bit stereo, one-bit-delayed Philips I2S.
// Format: 0 = Philips/I2S, 1 = MSB/left-justified, 2 = PCM short.
#define BT_I2S_SAMPLE_RATE 48000
#define BT_I2S_BITS 16
#define BT_I2S_RX_MASTER false
#define BT_I2S_USE_MCLK false
#define BT_I2S_FORMAT 0
#define BT_I2S_BCLK_INV false
#define BT_I2S_WS_INV false

// Read-only startup checks compare the module replies with these values.
#define BT1058_EXPECTED_I2SCFG 1
// Hardware-verified with FSC-BT1058 V2.1.0 (20260603): module I2S slave,
// ESP32-S3 I2S master. The lab tone also accepts I2SCFG=1 for diagnostics and
// automatically selects the complementary ESP clock role.
#define BT1058_EXPECTED_SOURCE_I2SCFG 3
#define BT1058_EXPECTED_A2DPRES 1

// Explicitly started laboratory source tone. It uses the module-side I2S bus
// only and never replaces or taps the normal Edge player/DAC audio path.
#define BT_SOURCE_TONE_HZ 1000
#define BT_SOURCE_TONE_AMPLITUDE 4096

// Experimental WEB/SD/DLNA -> FSC-BT1058 source relay. The ring is a
// single-producer/single-consumer queue: Audio's post-DSP callback only copies
// samples and never waits. High-bitrate lossless decoders deliver PCM in large
// bursts, so retain enough headroom for several decoder blocks. The relay uses
// a tiny adaptive interpolation around the target fill to absorb the clock
// difference between the independent player-DAC and BT-module I2S ports.
#define BT_SOURCE_PCM_RING_FRAMES 16384
#define BT_SOURCE_PCM_PREFILL_FRAMES 8192
#define BT_SOURCE_PCM_TARGET_FRAMES 8192
#define BT_SOURCE_PCM_BLOCK_FRAMES 256
// Q16 input-frame step correction. 512 / 65536 = 0.78125%; this is far below
// a semitone but comfortably covers the measured ~0.17% clock-rate mismatch.
#define BT_SOURCE_PCM_RATE_RESPONSE_SHIFT 4
#define BT_SOURCE_PCM_MAX_STEP_CORRECTION_Q16 512
// Refill events during decoder start/reconnect are useful diagnostics but are
// not continuous-playback underruns. Count an underrun only after the player
// has produced audio steadily for this grace period.
#define BT_SOURCE_UNDERRUN_GRACE_MS 1500

// Laboratory default: if the module boots in source mode and the normal
// player is running, restore the BT output automatically. Audio-path arming is
// retried only after an A2DP connection exists.
#define BT_SOURCE_RADIO_AUTOSTART 1
#define BT_SOURCE_AUDIO_ENABLE_RETRY_MS 2000
#define BT_SOURCE_AUDIO_ENABLE_RESPONSE_TIMEOUT_MS 1500
// Keep the dedicated A2DPAUDIO transaction away from the tail of another
// automatic query so its terminal OK/ERROR can be attributed unambiguously.
#define BT_AT_COMMAND_GUARD_MS 400
#define BT_SOURCE_RADIO_AUTOSTART_RETRY_MS 5000

// The module firmware resets its classic BR/EDR name when MODCFG changes.
// Keep the ESP-side alias authoritative after the first read-only adoption,
// and restore it with a bounded set + read-back transaction.
#define BT_ALIAS_COMMAND_TIMEOUT_MS 1500
#define BT_ALIAS_VERIFY_DELAY_MS 350
#define BT_ALIAS_RETRY_DELAY_MS 1000
#define BT_ALIAS_MAX_ATTEMPTS 3

// Sink/input WebUI commands. FSC-BT1058 stores at most eight paired records
// and accepts deletion by the exact BR/EDR MAC address.
#define BT_INPUT_PAIRED_MAX_DEVICES 8
#define BT_INPUT_COMMAND_TIMEOUT_MS 3000

// WebUI -> AVRCP controls in sink/input mode. A fixed FreeRTOS queue keeps
// websocket callbacks away from UART writes and preserves quick repeated
// next/previous presses without dynamic allocation.
#define BT_SINK_CONTROL_QUEUE_LENGTH 8
#define BT_SINK_CONTROL_RESPONSE_TIMEOUT_MS 1500

// Manual source-device discovery from the WebUI. Results live in a fixed
// array so repeated scans do not fragment the heap.
#define BT_SOURCE_SCAN_SECONDS 10
#define BT_SOURCE_SCAN_MAX_DEVICES 16
#define BT_SOURCE_SCAN_TIMEOUT_MARGIN_MS 3000
#define BT_SOURCE_CONNECT_TIMEOUT_MS 20000

// Source output-route control. Local-only mode disables the module Bluetooth
// radio with BTEN but deliberately leaves AUTOCONN and the paired list intact.
#define BT_SOURCE_ROUTE_COMMAND_TIMEOUT_MS 1500
#define BT_SOURCE_ROUTE_VERIFY_DELAY_MS 350
#define BT_SOURCE_ROUTE_RETRY_DELAY_MS 1000
#define BT_SOURCE_ROUTE_MAX_ATTEMPTS 3

// Non-blocking source/sink role transition. Parameter writes are deliberately
// spaced out, then verified by the normal read-only startup probe after the
// module reboot. A failed or interrupted transition may be retried safely.
#define BT_ROLE_DISCONNECT_TIMEOUT_MS 6000
#define BT_ROLE_DISCONNECT_SETTLE_MS 350
#define BT_ROLE_SWITCH_COMMAND_SETTLE_MS 500
#define BT_ROLE_SWITCH_PROBE_DELAY_MS 1500
#define BT_ROLE_SWITCH_TIMEOUT_MS 45000
#define BT_ROLE_SWITCH_MAX_ATTEMPTS 3

// For possible later 24/32-bit RX tests: 16 = upper 16 bits,
// 8 = middle 16 bits, 0 = lower 16 bits.
#define BT_I2S_32_SHIFT 16

// Visual-only gain for the BT spectrum analyzer path. Audio output is not changed.
#define BT_SPECTRUM_GAIN 2
