// Mofei simulator — headless screenshot harness.
//
// Spawns QEMU with the same args as the Tauri lib::start_sim path, reads
// framebuffer frames over the chardev Unix socket, and writes a PNG of the
// last frame received during a fixed observation window. Designed for CI
// regression: boot-into-known-pixels checks.
//
// Usage:
//   mofei-sim-headless --firmware path/to/firmware.elf \
//                      --duration 10 \
//                      --out boot.png
//
// Implementation notes:
//   * No Tauri / no webview. The whole pipeline is the QEMU process plus
//     a single Tokio task that reads the chardev socket.
//   * IPC frame format mirrors apps/simulator/src-tauri/src/ipc.rs (8-byte
//     header, channel byte, length-prefixed payload). Only channel 0x01
//     framebuffer is consumed; other channels are drained and discarded.
//   * On exit (after `--duration` seconds), QEMU is killed and the most
//     recent framebuffer is written to `--out`. If no framebuffer was ever
//     received, exits with code 2 and writes nothing.

mod debug_transport;
#[path = "../../src-tauri/src/simulator_resolver.rs"]
mod simulator_resolver;

use std::collections::{HashMap, HashSet};
use std::os::unix::ffi::OsStrExt;
use std::path::{Component, Path, PathBuf};
use std::process::ExitStatus;
use std::process::Stdio;
use std::sync::Arc;
use std::time::Duration;
use std::time::Instant as StdInstant;
use std::{fs, io};

use anyhow::{anyhow, Context, Result};
use clap::{Parser, ValueEnum};
use image::{GrayImage, ImageBuffer, Luma};
use serde::{Deserialize, Serialize};
use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio::net::{UnixDatagram, UnixStream};
use tokio::process::Command;
use tokio::sync::mpsc;
use tokio::time;

use mofei_sim_headless::sd_image;

const FIXTURE_TTF_CANDIDATES: &[&str] = &[
    "apps/panda-os/host/tests/fixtures/tiny.ttf",
    "lib/EpdFont/builtinFonts/source/Ubuntu/Ubuntu-Regular.ttf",
    "build/notosans_cjk_tc_fallback.ttf",
    "apps/simulator/sdcard/.mofei/fonts/MiSans-Regular.ttf",
];
const FIXTURE_EPUB_PATH: &str = "test/epubs/test_display_none.epub";
const FIXTURE_RENDERABLE_EPUB_PATH: &str = "test/epubs/test_kerning_ligature.epub";
const FIXTURE_LARGE_EPUB_PATH: &str = "test/epubs/test_tables.epub";
const FIXTURE_IMAGE_HEAVY_EPUB_PATH: &str = "test/epubs/test_jpeg_images.epub";
const SEEDED_EPUB_FILENAME: &str = "aaa_test_display_none.epub";
const SEEDED_RENDERABLE_EPUB_FILENAME: &str = "aaa_test_kerning_ligature.epub";
const SEEDED_LARGE_EPUB_FILENAME: &str = "aaa_test_tables.epub";
const SEEDED_IMAGE_HEAVY_EPUB_FILENAME: &str = "aaa_test_jpeg_images.epub";
const FIXTURE_TC_EPUB_PATH: &str = "test/epubs/test_tc.epub";
const SEEDED_TC_EPUB_FILENAME: &str = "aaa_test_tc.epub";
const SEEDED_EMPTY_TXT_FILENAME: &str = "aaa_empty.txt";
const SEEDED_MFP_FALLBACK_FILENAME: &str = "㐀.txt";
// The runner retains the complete broad Reader family and seeds the active
// 26px source-complete UI fallback used by the default Mofei UI ladder. The full 14-file payload is validated by
// host/golden-SD tests; duplicating every complete MFP into both simulator
// system roots would exceed the bounded 128 MiB temporary FAT image.
const PANDA_DEFAULT_FONT_PACKS: [&str; 8] = [
    "notosans_tc_13_japanese_common.mfp",
    "notosans_tc_16_japanese_common.mfp",
    "notosans_tc_20_japanese_common.mfp",
    "notosans_tc_24_japanese_common.mfp",
    "notosans_tc_26_japanese_common.mfp",
    "notosans_tc_32_japanese_common.mfp",
    "notosans_tc_40_japanese_common.mfp",
    "notosans_tc_26_source_complete.mfp",
];
const PANDA_MFP_FALLBACK_CASE_PACKS: [&str; 1] = ["notosans_tc_26_source_complete.mfp"];
const TXT_TOUCH_PAGE_TURN_CASE_ID: &str =
    "dashboard-file-browser-txt-reader-touch-page-turn-refresh-trace-back-to-dashboard-smoke";
const STORAGE_DIAGNOSTIC_CASE_PREFIX: &str = "lilygo-storage-";
const SEEDED_RENDERABLE_EPUB_CACHE_PATH: &str = "epub_3316698266/book.bin";
const BOOK_METADATA_CACHE_VERSION: u8 = 5;
const SEEDED_READER_SETTINGS_JSON: &str = concat!(
    "{\"fontFamily\":0,\"ttfFontName\":\"\",\"frontButtonBack\":0,",
    "\"frontButtonConfirm\":1,\"frontButtonLeft\":2,\"frontButtonRight\":3}\n"
);
const SEEDED_READER_GRID_SETTINGS_JSON: &str = concat!(
    "{\"fontFamily\":0,\"ttfFontName\":\"\",\"frontButtonBack\":0,",
    "\"frontButtonConfirm\":1,\"frontButtonLeft\":2,\"frontButtonRight\":3,",
    "\"fileBrowserLayoutMode\":1}\n"
);
const SEEDED_BROWSER_GRID_PREFS: &[u8] = b"MBP1\x01\x01\x00\x00\x00";
const SEEDED_BROWSER_LIST_PREFS: &[u8] = b"MBP1\x01\x00\x00\x00\x00";
const SEEDED_TTF_PICKER_SETTINGS_JSON: &str = concat!(
    "{\"fontFamily\":0,\"ttfFontName\":\"reader.ttf\",\"frontButtonBack\":0,",
    "\"frontButtonConfirm\":1,\"frontButtonLeft\":2,\"frontButtonRight\":3}\n"
);
const SEEDED_OPDS_SERVERS_JSON: &str = concat!(
    "{\"servers\":[{\"name\":\"Simulator OPDS\",\"url\":\"http://127.0.0.1:9/opds\",",
    "\"username\":\"\",\"password\":\"\"}]}\n"
);
const SEEDED_OPDS_EMPTY_SERVERS_JSON: &str = concat!(
    "{\"servers\":[{\"name\":\"Simulator OPDS Empty\",\"url\":\"http://127.0.0.1:9/opds-empty\",",
    "\"username\":\"\",\"password\":\"\"}]}\n"
);
const MAX_SERIAL_COMMAND_BYTES: usize = 255;
const SEEDED_OPDS_MALFORMED_SERVERS_JSON: &str = concat!(
    "{\"servers\":[{\"name\":\"Simulator OPDS Malformed\",\"url\":\"http://127.0.0.1:9/opds-malformed\",",
    "\"username\":\"\",\"password\":\"\"}]}\n"
);
const SEEDED_OPDS_FETCH_FAILED_SERVERS_JSON: &str = concat!(
    "{\"servers\":[{\"name\":\"Simulator OPDS Fetch Failed\",\"url\":\"http://127.0.0.1:9/opds-fetch-failed\",",
    "\"username\":\"\",\"password\":\"\"}]}\n"
);
const SEEDED_OPDS_DASHBOARD_SHORTCUTS_JSON: &str = concat!(
    "{\"shortcuts\":[\"recent_reading\",\"reading\",\"study\",\"file_browser\",",
    "\"import_sync\",\"opds_browser\",\"settings\",\"weather\",\"arcade\"]}\n"
);

fn seeded_study_review_queue_msq() -> Vec<u8> {
    let queues: [&[(&str, &str, &str)]; 3] = [
        &[
            ("alpha", "first letter", "Seeded Deck"),
            ("beta", "second letter", "Seeded Deck"),
        ],
        &[("gamma", "third letter", "Seeded Deck")],
        &[("delta", "fourth letter", "Seeded Deck")],
    ];
    let mut bytes = Vec::with_capacity(256);
    bytes.extend_from_slice(&0x3151534Du32.to_le_bytes());
    bytes.extend_from_slice(&[2, 0, 0, 0]);
    for queue in queues {
        bytes.extend_from_slice(&(queue.len() as u16).to_le_bytes());
        for (front, back, deck_name) in queue {
            let front = front.as_bytes();
            let back = back.as_bytes();
            let deck_name = deck_name.as_bytes();
            bytes.extend_from_slice(&1u16.to_le_bytes());
            bytes.extend_from_slice(&0u16.to_le_bytes());
            bytes.extend_from_slice(&0u32.to_le_bytes());
            bytes.extend_from_slice(&0u16.to_le_bytes());
            bytes.extend_from_slice(&(front.len() as u16).to_le_bytes());
            bytes.extend_from_slice(&(back.len() as u16).to_le_bytes());
            bytes.extend_from_slice(&(deck_name.len() as u16).to_le_bytes());
            bytes.extend_from_slice(front);
            bytes.extend_from_slice(back);
            bytes.extend_from_slice(deck_name);
        }
    }
    bytes
}

fn seeded_study_state_mst() -> Vec<u8> {
    let history = [
        (20260601u32, 2u16, 2u16, 2u16, 0u16, 1u16),
        (20260602u32, 2u16, 1u16, 1u16, 0u16, 2u16),
        (20260603u32, 3u16, 1u16, 1u16, 0u16, 2u16),
    ];
    let mut bytes = Vec::with_capacity(8 + 20 + history.len() * 14);
    bytes.extend_from_slice(&0x3154534Du32.to_le_bytes());
    bytes.extend_from_slice(&[1, 0]);
    bytes.extend_from_slice(&(history.len() as u16).to_le_bytes());
    bytes.extend_from_slice(&20260603u32.to_le_bytes());
    bytes.extend_from_slice(&20260602u32.to_le_bytes());
    bytes.extend_from_slice(&3u16.to_le_bytes());
    bytes.extend_from_slice(&1u16.to_le_bytes());
    bytes.extend_from_slice(&1u16.to_le_bytes());
    bytes.extend_from_slice(&0u16.to_le_bytes());
    bytes.extend_from_slice(&2u16.to_le_bytes());
    bytes.extend_from_slice(&0u16.to_le_bytes());
    for (date_key, due, completed, correct, wrong, streak) in history {
        bytes.extend_from_slice(&date_key.to_le_bytes());
        bytes.extend_from_slice(&due.to_le_bytes());
        bytes.extend_from_slice(&completed.to_le_bytes());
        bytes.extend_from_slice(&correct.to_le_bytes());
        bytes.extend_from_slice(&wrong.to_le_bytes());
        bytes.extend_from_slice(&streak.to_le_bytes());
    }
    bytes
}

const SEEDED_STUDY_DECK_JSON: &str = concat!(
    "{\"title\":\"Seeded Deck\",\"cards\":[",
    "{\"front\":\"alpha\",\"back\":\"first letter\",\"example\":\"alpha example\"},",
    "{\"front\":\"beta\",\"back\":\"second letter\",\"example\":\"beta example\"},",
    "{\"front\":\"gamma\",\"back\":\"third letter\",\"example\":\"gamma example\"}",
    "]}\n"
);
const HEADER_LEN: usize = 8;
const MAX_PAYLOAD: u32 = 1 << 20;

const CHANNEL_FRAMEBUFFER: u8 = 0x01;
const CHANNEL_CONTROL: u8 = 0x06;
const CHANNEL_TOUCH_EVENT: u8 = 0x04;
const CHANNEL_BUTTON_EVENT: u8 = 0x05;
const CHANNEL_SYNTHETIC_TOUCH_EVENT: u8 = 0x07;
const CHANNEL_DEBUG: u8 = debug_transport::CHANNEL_DEBUG;
const CHANNEL_SERIAL_COMMAND: u8 = 0x09;
const PERIPHERAL_CONTROL_VERSION: u8 = 1;
const PERIPHERAL_CONTROL_ACK: u8 = 0x80;
const PERIPHERAL_CONTROL_TRACE: u8 = 0x81;
const PERIPHERAL_CONTROL_STORAGE_DEVICE: u8 = 3;
const PERIPHERAL_CONTROL_BQ27220_DEVICE: u8 = 4;
const PERIPHERAL_CONTROL_SX1262_DEVICE: u8 = 5;
const PERIPHERAL_CONTROL_BQ25896_DEVICE: u8 = 6;
const PERIPHERAL_CONTROL_PCF8563_DEVICE: u8 = 7;
const PERIPHERAL_CONTROL_GNSS_DEVICE: u8 = 8;
const PERIPHERAL_CONTROL_TPS651851_DEVICE: u8 = 9;
const PERIPHERAL_CONTROL_RADIO_TX: u8 = 0x84;
const PERIPHERAL_CONTROL_MAX_PAYLOAD_BYTES: usize = 64;
const PERIPHERAL_CONTROL_TRACE_HEADER_BYTES: usize = 22;
const PERIPHERAL_CONTROL_TRACE_DATA_MAX_BYTES: usize = 16;
const PERIPHERAL_CONTROL_ONE_SHOT: u8 = 1;
const PERIPHERAL_CONTROL_SET_STATE: u8 = 1;
const PERIPHERAL_CONTROL_SET_FAULT: u8 = 2;
const PERIPHERAL_CONTROL_RESET: u8 = 3;
const PERIPHERAL_CONTROL_QUERY: u8 = 4;
const SX1262_CONTROL_SET_ENDPOINT: u8 = 1;
const SX1262_CONTROL_SCHEDULE_RECEIVE: u8 = 2;
const SX1262_CONTROL_DELIVER_BROKER_PACKET: u8 = 3;
const SX1262_BROKER_MAX_PACKET_BYTES: usize = 27;
const SX1262_SCRIPTED_MAX_PACKET_BYTES: usize = 45;
const SX1262_BROKER_DELIVERY_DELAY_US: u32 = 1_000;
const SX1262_BROKER_RSSI_DBM: i16 = -70;
const SX1262_BROKER_SNR_DB: i8 = 7;
const STORAGE_CONTROL_STATE_PRESENT: u8 = 1;
const STORAGE_CONTROL_STATE_WRITABLE: u8 = 2;
const STORAGE_CONTROL_FAULT_READ: u8 = 1;
const STORAGE_CONTROL_FAULT_WRITE: u8 = 2;
const STORAGE_CONTROL_FAULT_CORRUPT: u8 = 3;
const MIN_MEANINGFUL_CONTENT_BYTES: usize = 64;
const FLASH_SIZE_BYTES: usize = 16 * 1024 * 1024;
const BOOTLOADER_OFFSET: usize = 0x0000;
const PARTITION_TABLE_OFFSET: usize = 0x8000;
const PARTITION_ENTRY_SIZE: usize = 32;
const PARTITION_MAGIC: [u8; 2] = [0xAA, 0x50];
const PARTITION_END_MAGIC: [u8; 2] = [0xEB, 0xEB];
const FIRST_TAP_INDEX: usize = 0;
const NO_WAIT_FRAMES: u64 = 0;
const NO_INPUT_FRAME_GAP: u64 = 0;
const FIRST_TAP_FRAME_GAP: u64 = 1;
const MIN_STARTUP_FRAMES_BEFORE_INPUT: u64 = 1;
const MAX_STARTUP_FRAMES_BEFORE_INPUT: u64 = 60;
const DEFAULT_STEP_TIMEOUT_S: u64 = 30;
const ASSERT_ACTIVITY_STABLE_MS: u64 = 150;
const E2E_REFERENCE_OUTPUT_WIDTH: u32 = 480;
const E2E_REFERENCE_OUTPUT_HEIGHT: u32 = 800;
const E2E_SD_IMAGE_BYTES: u64 = sd_image::SD_IMAGE_BYTES;
const E2E_BOOT_COPY_MAX_BYTES: u64 = 64 * 1024 * 1024;
const UNIX_SOCKET_PATH_SAFE_BYTES: usize = 100;
const SOCKET_SUFFIX_HASH_BYTES: usize = 8;

// ── E2E case model ──────────────────────────────────────────────────────────

#[derive(Clone, Debug, Deserialize, Serialize)]
struct E2ECase {
    id: String,
    description: String,
    #[serde(default, rename = "skipReason")]
    skip_reason: Option<String>,
    #[serde(default, rename = "requiresCapabilities")]
    required_capabilities: Vec<String>,
    #[serde(default, rename = "referenceViewport")]
    reference_viewport: ReferenceViewport,
    #[serde(default)]
    boot: E2EBoot,
    #[serde(default, rename = "beforeBoot")]
    before_boot: Vec<CaseStep>,
    steps: Vec<CaseStep>,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct E2EBoot {
    #[serde(default)]
    copy_files: Vec<E2EBootCopyFile>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct E2EBootCopyFile {
    path: String,
    source: String,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
struct ReferenceViewport {
    width: u32,
    height: u32,
}

impl Default for ReferenceViewport {
    fn default() -> Self {
        Self {
            width: E2E_REFERENCE_OUTPUT_WIDTH,
            height: E2E_REFERENCE_OUTPUT_HEIGHT,
        }
    }
}

impl ReferenceViewport {
    fn validate(self) -> Result<Self> {
        if self.width == 0 || self.height == 0 {
            return Err(anyhow!(
                "referenceViewport width={} height={} must have positive dimensions",
                self.width,
                self.height
            ));
        }
        Ok(self)
    }
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct BoardRegistry {
    default_board: String,
    boards: Vec<BoardProfile>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct BoardProfile {
    id: String,
    raw_width: u32,
    raw_height: u32,
    framebuffer_width: u32,
    framebuffer_height: u32,
    framebuffer_format: FramebufferFormat,
    output_width: u32,
    output_height: u32,
    #[serde(default)]
    capabilities: Vec<String>,
    key_map: Vec<BoardKey>,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq)]
#[serde(rename_all = "lowercase")]
enum FramebufferFormat {
    Mono1,
    Gray16,
}

#[derive(Clone, Debug, Deserialize)]
struct BoardKey {
    id: u8,
    label: String,
    #[serde(default)]
    aliases: Vec<String>,
}

#[derive(Clone, Debug)]
struct BoardGeometry {
    raw_width: u32,
    raw_height: u32,
    framebuffer_width: u32,
    framebuffer_height: u32,
    framebuffer_format: FramebufferFormat,
    output_width: u32,
    output_height: u32,
}

impl BoardGeometry {
    fn fb_bytes(&self) -> usize {
        let pixels = self.framebuffer_width as usize * self.framebuffer_height as usize;
        match self.framebuffer_format {
            FramebufferFormat::Mono1 => pixels.div_ceil(8),
            FramebufferFormat::Gray16 => pixels.div_ceil(2),
        }
    }

    fn map_reference_touch(&self, viewport: ReferenceViewport, tap: TouchTap) -> TouchTap {
        TouchTap {
            x: scale_reference_touch_coord(u32::from(tap.x), viewport.width, self.output_width),
            y: scale_reference_touch_coord(u32::from(tap.y), viewport.height, self.output_height),
        }
    }

    fn map_reference_screenshot_region(
        &self,
        viewport: ReferenceViewport,
        x: u32,
        y: u32,
        width: u32,
        height: u32,
    ) -> Result<ScreenshotRegion> {
        if width == 0 || height == 0 {
            return Err(anyhow!(
                "reference screenshot region x={} y={} width={} height={} must have positive dimensions",
                x,
                y,
                width,
                height
            ));
        }
        let x_end = x.checked_add(width).ok_or_else(|| {
            anyhow!(
                "reference screenshot region x={} width={} overflows",
                x,
                width
            )
        })?;
        let y_end = y.checked_add(height).ok_or_else(|| {
            anyhow!(
                "reference screenshot region y={} height={} overflows",
                y,
                height
            )
        })?;
        if x_end > viewport.width || y_end > viewport.height {
            return Err(anyhow!(
                "reference screenshot region x={} y={} width={} height={} exceeds {}x{}",
                x,
                y,
                width,
                height,
                viewport.width,
                viewport.height
            ));
        }

        let mapped_x = scale_reference_region_start(x, viewport.width, self.output_width);
        let mapped_y = scale_reference_region_start(y, viewport.height, self.output_height);
        let mapped_x_end =
            scale_reference_region_end(x_end, viewport.width, self.output_width, mapped_x);
        let mapped_y_end =
            scale_reference_region_end(y_end, viewport.height, self.output_height, mapped_y);

        Ok(ScreenshotRegion {
            x: mapped_x,
            y: mapped_y,
            width: mapped_x_end.saturating_sub(mapped_x).max(1),
            height: mapped_y_end.saturating_sub(mapped_y).max(1),
        })
    }

    fn source_pixel_for_output(&self, x: u32, y: u32) -> Option<(u32, u32)> {
        if x >= self.output_width || y >= self.output_height {
            return None;
        }
        if self.output_width == self.framebuffer_width
            && self.output_height == self.framebuffer_height
        {
            return Some((x, y));
        }
        if self.output_width == self.framebuffer_height
            && self.output_height == self.framebuffer_width
        {
            return Some((y, self.raw_height - 1 - x));
        }
        None
    }
}

fn scale_reference_touch_coord(value: u32, from: u32, to: u32) -> u16 {
    if from == 0 || to == 0 {
        return 0;
    }
    let rounded = ((value as u64 * to as u64) + u64::from(from / 2)) / u64::from(from);
    rounded.min(u64::from(to.saturating_sub(1))) as u16
}

fn scale_reference_region_bound(value: u32, from: u32, to: u32) -> u32 {
    if from == 0 || to == 0 {
        return 0;
    }
    let rounded = ((value as u64 * to as u64) + u64::from(from / 2)) / u64::from(from);
    rounded.min(u64::from(to)) as u32
}

fn scale_reference_region_start(value: u32, from: u32, to: u32) -> u32 {
    scale_reference_region_bound(value, from, to).min(to.saturating_sub(1))
}

fn scale_reference_region_end(value: u32, from: u32, to: u32, start: u32) -> u32 {
    scale_reference_region_bound(value, from, to)
        .max(start.saturating_add(1))
        .min(to)
}

fn scale_reference_pixel_threshold(
    threshold: u32,
    reference_pixels: u32,
    mapped_pixels: u32,
) -> u32 {
    if threshold == 0 || reference_pixels == 0 || mapped_pixels == 0 {
        return 0;
    }
    let scaled = (threshold as u64 * mapped_pixels as u64).div_ceil(reference_pixels as u64);
    scaled.max(1).min(u64::from(mapped_pixels)) as u32
}

#[derive(Clone, Debug)]
struct BoardRuntime {
    id: String,
    geometry: BoardGeometry,
    capabilities: std::collections::HashSet<String>,
    button_ids_by_name: HashMap<String, u8>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct CaseStep {
    #[serde(rename = "type")]
    step_type: String,
    #[serde(default)]
    label: Option<String>,
    #[serde(default)]
    event: String,
    #[serde(default)]
    x: u16,
    #[serde(default)]
    y: u16,
    #[serde(default)]
    width: u16,
    #[serde(default)]
    height: u16,
    #[serde(default)]
    button: String,
    #[serde(default)]
    direction: String,
    #[serde(default)]
    activity: String,
    #[serde(default)]
    text: Option<String>,
    #[serde(default)]
    path: Option<String>,
    #[serde(default)]
    device: String,
    #[serde(default)]
    operation: String,
    #[serde(default)]
    field: String,
    #[serde(default)]
    value: Option<bool>,
    #[serde(default, rename = "voltageMv")]
    voltage_mv: Option<u16>,
    #[serde(default, rename = "stateOfCharge")]
    state_of_charge: Option<u8>,
    #[serde(default)]
    status: Option<u8>,
    #[serde(default, rename = "latchedFault")]
    latched_fault: Option<u8>,
    #[serde(default, rename = "currentFault")]
    current_fault: Option<u8>,
    #[serde(default)]
    year: Option<u8>,
    #[serde(default)]
    month: Option<u8>,
    #[serde(default)]
    day: Option<u8>,
    #[serde(default)]
    weekday: Option<u8>,
    #[serde(default)]
    hour: Option<u8>,
    #[serde(default)]
    minute: Option<u8>,
    #[serde(default)]
    second: Option<u8>,
    #[serde(default, rename = "oscillatorStopped")]
    oscillator_stopped: Option<bool>,
    #[serde(default)]
    variant: String,
    #[serde(default)]
    fixture: String,
    #[serde(default)]
    powered: Option<bool>,
    #[serde(default, rename = "endpointId")]
    endpoint_id: Option<u64>,
    #[serde(default, rename = "payloadHex")]
    payload_hex: Option<String>,
    #[serde(default, rename = "deliveryDelayUs")]
    delivery_delay_us: Option<u32>,
    #[serde(default, rename = "rssiDbm")]
    rssi_dbm: Option<i16>,
    #[serde(default, rename = "snrDb")]
    snr_db: Option<i8>,
    #[serde(default, rename = "oneShot")]
    one_shot: bool,
    #[serde(default, rename = "afterInput")]
    after_input: bool,
    #[serde(default, rename = "waitForFrame")]
    wait_for_frame: bool,
    #[serde(default)]
    #[serde(rename = "delayMs")]
    delay_ms: u64,
    #[serde(default)]
    #[serde(rename = "holdMs")]
    hold_ms: Option<u64>,
    #[serde(default)]
    #[serde(rename = "minDarkRatio")]
    min_dark_ratio: Option<f64>,
    #[serde(default)]
    #[serde(rename = "maxDarkRatio")]
    max_dark_ratio: Option<f64>,
    #[serde(default)]
    #[serde(rename = "minWhiteRatio")]
    min_white_ratio: Option<f64>,
    #[serde(default)]
    #[serde(rename = "maxWhiteRatio")]
    max_white_ratio: Option<f64>,
    #[serde(default)]
    #[serde(rename = "minDarkPixels")]
    min_dark_pixels: Option<u32>,
    #[serde(default)]
    #[serde(rename = "maxDarkPixels")]
    max_dark_pixels: Option<u32>,
    #[serde(default)]
    #[serde(rename = "minWhitePixels")]
    min_white_pixels: Option<u32>,
    #[serde(default)]
    #[serde(rename = "maxWhitePixels")]
    max_white_pixels: Option<u32>,
}

#[derive(Clone, Debug, Serialize)]
struct StepResult {
    #[serde(rename = "type")]
    step_type: String,
    status: String,
    #[serde(rename = "elapsedMs")]
    elapsed_ms: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
struct CaseResult {
    #[serde(rename = "caseId")]
    case_id: String,
    status: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
    steps: Vec<StepResult>,
    #[serde(rename = "totalElapsedMs")]
    total_elapsed_ms: u64,
    screenshot: String,
    transcript: String,
    firmware: String,
}

#[derive(Debug)]
struct CaseRunOutcome {
    case_id: String,
    status: String,
    artifacts_dir: PathBuf,
    error: Option<String>,
}

#[derive(Debug)]
struct DeferredSdFileAssertion {
    result_index: usize,
    step_type: String,
    relative_path: PathBuf,
    display_path: String,
    contains: Option<String>,
    step_start: StdInstant,
}

#[derive(Clone, Debug)]
struct E2EContext {
    transcript: Vec<String>,
    current_activity: Option<String>,
    activity_events: Vec<(StdInstant, String)>,
    pending_input_activity_after: Option<usize>,
    pending_input_log_after: Option<usize>,
}

#[derive(Debug)]
struct SimulatorLocation {
    name: String,
    timezone: String,
    latitude: f64,
    longitude: f64,
}

impl Default for SimulatorLocation {
    fn default() -> Self {
        Self {
            name: "Taipei".to_string(),
            timezone: "Asia%2FTaipei".to_string(),
            latitude: 25.0330,
            longitude: 121.5654,
        }
    }
}

#[derive(Parser, Clone, Debug)]
#[command(version, about = "Mofei simulator headless screenshot harness")]
struct Args {
    /// Board registry id. Defaults to this checkout's boards.json defaultBoard.
    #[arg(long)]
    board: Option<String>,

    /// Path to the firmware ELF or BIN to boot in QEMU.
    #[arg(long)]
    firmware: PathBuf,

    /// Path to the espressif/qemu fork's `qemu-system-xtensa` binary.
    /// Defaults to PANDA_SIMULATOR_QEMU or this checkout's .qemu-cache path.
    #[arg(long, default_value = ".qemu-cache/qemu/build/qemu-system-xtensa")]
    qemu: PathBuf,

    /// Override MOFEI_SIM_HEAP_MODE for the QEMU child. Missing value inherits the environment and defaults to device.
    #[arg(long, value_enum)]
    heap_mode: Option<HeapMode>,

    /// Loopback TCP port for the Panda simulator debug protocol. Defaults to PANDA_SIM_DEBUG_PORT.
    #[arg(long)]
    debug_port: Option<u16>,

    /// IPC chardev socket path. Will be removed on start if it exists.
    #[arg(long, default_value = "/tmp/mofei-sim-headless.sock")]
    socket: PathBuf,

    /// Number of seconds to run before screenshotting and exiting. The harness
    /// exits early after the first meaningful framebuffer unless touch injection
    /// is requested.
    #[arg(long, default_value_t = 10)]
    duration: u64,

    /// Output PNG path. Required unless --case writes screenshot.png under --artifacts.
    #[arg(long)]
    out: Option<PathBuf>,

    /// Host directory mounted as the simulator SD card.
    #[arg(long)]
    sd_root: Option<PathBuf>,

    /// Echo firmware UART output to stderr (off by default — keep CI quiet).
    #[arg(long)]
    serial: bool,

    /// Inject a center tap through the simulator IPC path and require firmware touch proof in stderr logs.
    #[arg(long)]
    inject_touch_tap: bool,

    /// Portrait logical X coordinate for --inject-touch-tap.
    #[arg(long, default_value_t = 240)]
    tap_x: u16,

    /// Portrait logical Y coordinate for --inject-touch-tap.
    #[arg(long, default_value_t = 400)]
    tap_y: u16,

    /// Additional portrait logical taps to inject after the first tap, encoded as x:y.
    #[arg(long, value_parser = parse_touch_tap)]
    extra_touch_tap: Vec<TouchTap>,

    /// Comma-separated hardware button ids to click after the first framebuffer (0=Back, 1=Confirm, 2=Left, 3=Right, 4=Up, 5=Down).
    #[arg(long, value_delimiter = ',')]
    inject_buttons: Vec<u8>,

    /// Hold duration for injected front-button clicks.
    #[arg(long, default_value_t = 250)]
    button_hold_ms: u64,

    /// Delay after --inject-touch-tap before any --inject-buttons clicks.
    #[arg(long, default_value_t = 250)]
    post_tap_delay_ms: u64,

    /// Number of post-tap framebuffer frames to observe before clicking buttons.
    #[arg(long, default_value_t = 1)]
    post_tap_frames_before_buttons: u64,

    /// Number of meaningful framebuffer frames to observe before injecting the first input.
    #[arg(long, default_value_t = 1)]
    startup_frames_before_input: u64,

    /// Path to JSON E2E case file (enables structured case execution mode).
    #[arg(long)]
    case: Option<PathBuf>,

    /// Directory of JSON E2E case files to run sequentially.
    #[arg(long)]
    case_dir: Option<PathBuf>,

    /// Directory for E2E artifacts: transcript, result JSON, screenshot.
    #[arg(long, default_value = ".simulator-e2e")]
    artifacts: PathBuf,

    /// Write the framebuffer for each successful screenshot assertion below
    /// <artifacts>/checkpoints. Disabled by default to keep ordinary E2E runs small.
    #[arg(long)]
    capture_assertion_screenshots: bool,

    /// Maximum seconds to wait for a single step's evidence before failing.
    #[arg(long, default_value_t = DEFAULT_STEP_TIMEOUT_S)]
    step_timeout: u64,

    /// Execute cases even when they declare skipReason, for validating whether
    /// a known simulator blocker has been fixed.
    #[arg(long)]
    run_skipped: bool,

    /// Run-scoped Unix datagram socket that receives SX1262 packets from a peer simulator.
    #[arg(long, requires = "radio_broker_peer")]
    radio_broker_bind: Option<PathBuf>,

    /// Peer simulator's run-scoped SX1262 broker socket.
    #[arg(long, requires = "radio_broker_bind")]
    radio_broker_peer: Option<PathBuf>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, ValueEnum)]
enum HeapMode {
    Device,
    Test,
}

impl HeapMode {
    fn as_env_value(self) -> &'static str {
        match self {
            Self::Device => "device",
            Self::Test => "test",
        }
    }
}

#[derive(Debug)]
struct InjectionPlan {
    taps: Vec<TouchTap>,
    inject_buttons: Vec<u8>,
    button_hold_ms: u64,
    post_tap_delay_ms: u64,
    post_tap_frames_before_buttons: u64,
    startup_frames_before_input: u64,
}

struct QemuChildGuard {
    child: Option<tokio::process::Child>,
    socket_path: PathBuf,
    monitor_socket_path: PathBuf,
}

#[derive(Clone)]
struct RadioPeerSender {
    socket: Arc<UnixDatagram>,
    peer_path: PathBuf,
}

struct RadioPeerHandle {
    task: tokio::task::JoinHandle<()>,
    bind_path: PathBuf,
}

impl Drop for RadioPeerHandle {
    fn drop(&mut self) {
        self.task.abort();
        cleanup_stale_socket(&self.bind_path);
    }
}

#[derive(Debug)]
struct SdDrive {
    qemu_arg: String,
    image_path: Option<PathBuf>,
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
struct TouchTap {
    x: u16,
    y: u16,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum TouchGestureDirection {
    Up,
    Down,
    Left,
    Right,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum SyntheticTouchEventType {
    Tap,
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
    LongPress,
}

impl SyntheticTouchEventType {
    fn code(self) -> u8 {
        match self {
            Self::Tap => 1,
            Self::SwipeLeft => 2,
            Self::SwipeRight => 3,
            Self::SwipeUp => 4,
            Self::SwipeDown => 5,
            Self::LongPress => 6,
        }
    }

    fn label(self) -> &'static str {
        match self {
            Self::Tap => "tap",
            Self::SwipeLeft => "swipe_left",
            Self::SwipeRight => "swipe_right",
            Self::SwipeUp => "swipe_up",
            Self::SwipeDown => "swipe_down",
            Self::LongPress => "long_press",
        }
    }
}

#[derive(Debug)]
enum HeadlessEvent {
    Frame(Vec<u8>),
    PeripheralControl(Vec<u8>),
    Injected,
    TouchRead(TouchTap),
    ButtonRead { button_id: u8, pressed: bool },
    SerialLine,
}

#[derive(Debug)]
enum InjectionCommand {
    TouchTap {
        x: u16,
        y: u16,
    },
    TouchSwipe {
        x: u16,
        y: u16,
        direction: TouchGestureDirection,
    },
    ButtonClick {
        button_id: u8,
        hold_ms: Option<u64>,
    },
    SyntheticTouchEvent {
        touch_type: SyntheticTouchEventType,
        x: u16,
        y: u16,
    },
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ButtonInjection {
    ButtonClick { button_id: u8 },
    SyntheticTouch { touch_type: SyntheticTouchEventType },
}

impl InjectionPlan {
    fn has_injections(&self) -> bool {
        !self.taps.is_empty() || !self.inject_buttons.is_empty()
    }
}

impl QemuChildGuard {
    fn new(
        child: tokio::process::Child,
        socket_path: PathBuf,
        monitor_socket_path: PathBuf,
    ) -> Self {
        Self {
            child: Some(child),
            socket_path,
            monitor_socket_path,
        }
    }

    async fn wait(&mut self) -> Result<ExitStatus> {
        let Some(child) = self.child.as_mut() else {
            return Err(anyhow!("QEMU child already cleaned up"));
        };
        let status = child.wait().await?;
        self.child = None;
        Ok(status)
    }

    async fn cleanup(&mut self) {
        if let Some(mut child) = self.child.take() {
            let _ = child.kill().await;
            let _ = child.wait().await;
        }
        cleanup_stale_socket(&self.socket_path);
        cleanup_stale_socket(&self.monitor_socket_path);
    }
}

impl Drop for QemuChildGuard {
    fn drop(&mut self) {
        if let Some(child) = self.child.as_mut() {
            // Drop cannot await; start_kill prevents orphaned QEMU on early-return paths.
            let _ = child.start_kill();
        }
        cleanup_stale_socket(&self.socket_path);
        cleanup_stale_socket(&self.monitor_socket_path);
    }
}

fn parse_touch_tap(value: &str) -> std::result::Result<TouchTap, String> {
    let (x, y) = value
        .split_once(':')
        .ok_or_else(|| "expected x:y touch coordinate".to_string())?;
    let x = x
        .parse::<u16>()
        .map_err(|err| format!("invalid touch x coordinate: {err}"))?;
    let y = y
        .parse::<u16>()
        .map_err(|err| format!("invalid touch y coordinate: {err}"))?;
    Ok(TouchTap { x, y })
}

fn load_board_runtime(board_id: Option<&str>) -> Result<BoardRuntime> {
    let registry: BoardRegistry = serde_json::from_str(include_str!("../../boards.json"))
        .context("parsing standalone boards.json")?;
    let selected_id = board_id
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .unwrap_or(&registry.default_board);
    let profile = registry
        .boards
        .into_iter()
        .find(|board| board.id == selected_id)
        .ok_or_else(|| anyhow!("unsupported simulator board '{selected_id}'"))?;
    let mut button_ids_by_name = HashMap::new();
    for key in &profile.key_map {
        button_ids_by_name.insert(key.label.trim().to_ascii_lowercase(), key.id);
        for alias in &key.aliases {
            button_ids_by_name.insert(alias.trim().to_ascii_lowercase(), key.id);
        }
    }

    if profile.id == "mofei"
        || profile.id == "s3r8"
    {
        button_ids_by_name.insert("back".to_string(), 0);
        button_ids_by_name.insert("lock".to_string(), 0);
        button_ids_by_name.insert("confirm".to_string(), 1);
        button_ids_by_name.insert("key1".to_string(), 1);
        button_ids_by_name.insert("left".to_string(), 2);
        button_ids_by_name.insert("key2".to_string(), 2);
        button_ids_by_name.insert("right".to_string(), 3);
        button_ids_by_name.insert("up".to_string(), 4);
        button_ids_by_name.insert("down".to_string(), 5);
    } else if profile.id == "s37uc" {
        button_ids_by_name.insert("back".to_string(), 0);
        button_ids_by_name.insert("lock".to_string(), 0);
        button_ids_by_name.insert("confirm".to_string(), 1);
        button_ids_by_name.insert("key1".to_string(), 1);
    }
    let mut capabilities = profile
        .capabilities
        .into_iter()
        .map(|value| value.trim().to_ascii_lowercase())
        .filter(|value| !value.is_empty())
        .collect::<std::collections::HashSet<_>>();
    capabilities.extend(
        button_ids_by_name
            .keys()
            .map(|name| format!("button.{name}")),
    );

    Ok(BoardRuntime {
        id: profile.id,
        geometry: BoardGeometry {
            raw_width: profile.raw_width,
            raw_height: profile.raw_height,
            framebuffer_width: profile.framebuffer_width,
            framebuffer_height: profile.framebuffer_height,
            framebuffer_format: profile.framebuffer_format,
            output_width: profile.output_width,
            output_height: profile.output_height,
        },
        capabilities,
        button_ids_by_name,
    })
}

fn cleanup_stale_socket(path: &Path) {
    if path.exists() {
        let _ = std::fs::remove_file(path);
    }
}

#[derive(Clone, Copy)]
struct FlashLayout {
    boot_app0_offset: usize,
    app_offset: usize,
}

fn absolute_path(path: &Path) -> Result<PathBuf> {
    if path.is_absolute() {
        Ok(path.to_path_buf())
    } else {
        Ok(std::env::current_dir()?.join(path))
    }
}

fn canonical_existing_path(path: &Path, description: &str) -> Result<PathBuf> {
    let path = absolute_path(path)?;
    if !path.exists() {
        return Err(anyhow!("{description} not found at {}", path.display()));
    }
    path.canonicalize()
        .with_context(|| format!("canonicalizing {description} {}", path.display()))
}

fn normalize_args_paths(args: &Args) -> Result<Args> {
    let mut normalized = args.clone();
    let default_qemu = Path::new(simulator_resolver::QEMU_RELATIVE_PATH);
    let qemu = if args.qemu == default_qemu {
        simulator_resolver::qemu_path(None, &standalone_root_path()).map_err(anyhow::Error::msg)?
    } else {
        simulator_resolver::qemu_path(Some(&args.qemu), &standalone_root_path()).map_err(anyhow::Error::msg)?
    };
    normalized.qemu = canonical_existing_path(&qemu, "QEMU binary")?;
    normalized.firmware = canonical_existing_path(&args.firmware, "Firmware")?;
    normalized.socket = absolute_path(&args.socket)?;
    normalized.artifacts = absolute_path(&args.artifacts)?;
    normalized.out = args
        .out
        .as_ref()
        .map(|path| absolute_path(path))
        .transpose()?;
    normalized.sd_root = args
        .sd_root
        .as_ref()
        .map(|path| absolute_path(path))
        .transpose()?;
    normalized.case = args
        .case
        .as_ref()
        .map(|path| canonical_existing_path(path, "E2E case file"))
        .transpose()?;
    normalized.case_dir = args
        .case_dir
        .as_ref()
        .map(|path| canonical_existing_path(path, "E2E case directory"))
        .transpose()?;
    normalized.radio_broker_bind = args
        .radio_broker_bind
        .as_ref()
        .map(|path| absolute_path(path))
        .transpose()?;
    normalized.radio_broker_peer = args
        .radio_broker_peer
        .as_ref()
        .map(|path| absolute_path(path))
        .transpose()?;
    Ok(normalized)
}

fn default_sd_root() -> Result<PathBuf> {
    Ok(standalone_root_path().join("sdcard"))
}

fn standalone_root_path() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap_or_else(|| Path::new("."))
        .to_path_buf()
}

fn simulator_sd_root(arg_root: Option<PathBuf>) -> Result<PathBuf> {
    let root = if let Some(path) = arg_root {
        path
    } else if let Some(path) = std::env::var_os("MOFEI_SIM_SD_ROOT") {
        PathBuf::from(path)
    } else {
        default_sd_root()?
    };
    let root = absolute_path(&root)?;
    std::fs::create_dir_all(&root)
        .with_context(|| format!("creating simulator SD root {}", root.display()))?;
    Ok(root)
}

fn normalize_timezone_for_firmware(timezone: &str) -> String {
    timezone.trim().replace('/', "%2F")
}

fn is_valid_location(location: &SimulatorLocation) -> bool {
    !location.name.trim().is_empty()
        && !location.timezone.trim().is_empty()
        && location.latitude.is_finite()
        && location.longitude.is_finite()
        && (-90.0..=90.0).contains(&location.latitude)
        && (-180.0..=180.0).contains(&location.longitude)
}

fn env_simulator_location() -> Option<SimulatorLocation> {
    let lat = std::env::var("MOFEI_SIM_LOCATION_LAT")
        .ok()?
        .trim()
        .parse::<f64>()
        .ok()?;
    let lon = std::env::var("MOFEI_SIM_LOCATION_LON")
        .ok()?
        .trim()
        .parse::<f64>()
        .ok()?;
    let location = SimulatorLocation {
        name: std::env::var("MOFEI_SIM_LOCATION_NAME")
            .ok()
            .filter(|value| !value.trim().is_empty())
            .unwrap_or_else(|| "Host Location".to_string()),
        timezone: normalize_timezone_for_firmware(
            &std::env::var("MOFEI_SIM_LOCATION_TIMEZONE")
                .ok()
                .filter(|value| !value.trim().is_empty())
                .unwrap_or_else(|| "Asia%2FTaipei".to_string()),
        ),
        latitude: lat,
        longitude: lon,
    };
    is_valid_location(&location).then_some(location)
}

fn json_escape(value: &str) -> String {
    let mut escaped = String::new();
    for ch in value.chars() {
        match ch {
            '"' => escaped.push_str("\\\""),
            '\\' => escaped.push_str("\\\\"),
            '\n' => escaped.push_str("\\n"),
            '\r' => escaped.push_str("\\r"),
            '\t' => escaped.push_str("\\t"),
            c if c.is_control() => escaped.push_str(&format!("\\u{:04x}", c as u32)),
            c => escaped.push(c),
        }
    }
    escaped
}

fn write_simulator_location(sd_root: &Path) -> Result<SimulatorLocation> {
    let location = env_simulator_location().unwrap_or_default();
    let system_dir = sd_root.join(".mofei");
    std::fs::create_dir_all(&system_dir).with_context(|| {
        format!(
            "creating simulator system directory {}",
            system_dir.display()
        )
    })?;
    let path = system_dir.join("simulator_location.json");
    let payload = format!(
        "{{\"name\":\"{}\",\"timezone\":\"{}\",\"latitude\":{:.6},\"longitude\":{:.6}}}\n",
        json_escape(&location.name),
        json_escape(&location.timezone),
        location.latitude,
        location.longitude
    );
    std::fs::write(&path, payload)
        .with_context(|| format!("writing simulator location {}", path.display()))?;
    Ok(location)
}

fn copy_file_if_newer(source: &Path, dest: &Path) -> io::Result<()> {
    let should_copy = match (fs::metadata(source), fs::metadata(dest)) {
        (Ok(source_meta), Ok(dest_meta)) => match (source_meta.modified(), dest_meta.modified()) {
            (Ok(source_mtime), Ok(dest_mtime)) => source_mtime > dest_mtime,
            _ => true,
        },
        (Ok(_), Err(_)) => true,
        _ => false,
    };
    if !should_copy {
        return Ok(());
    }
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::copy(source, dest)?;
    Ok(())
}

fn sync_newer_dir_recursive(from: &Path, to: &Path) -> io::Result<()> {
    fs::create_dir_all(to)?;
    if !from.exists() {
        return Ok(());
    }
    for entry in fs::read_dir(from)? {
        let entry = entry?;
        let source = entry.path();
        let dest = to.join(entry.file_name());
        let file_type = entry.file_type()?;
        if file_type.is_dir() {
            sync_newer_dir_recursive(&source, &dest)?;
        } else if file_type.is_file() {
            copy_file_if_newer(&source, &dest)?;
        }
    }
    Ok(())
}

fn sync_visible_system_dir(sd_root: &Path) -> Result<()> {
    let hidden = sd_root.join(".mofei");
    let visible = sd_root.join("mofei");
    fs::create_dir_all(&hidden)
        .with_context(|| format!("creating simulator hidden system dir {}", hidden.display()))?;
    fs::create_dir_all(&visible).with_context(|| {
        format!(
            "creating simulator visible system dir {}",
            visible.display()
        )
    })?;
    sync_newer_dir_recursive(&visible, &hidden).with_context(|| {
        format!(
            "syncing newer simulator system files {} -> {}",
            visible.display(),
            hidden.display()
        )
    })?;
    sync_newer_dir_recursive(&hidden, &visible).with_context(|| {
        format!(
            "syncing newer simulator system files {} -> {}",
            hidden.display(),
            visible.display()
        )
    })
}

fn sd_directory_drive(sd_root: &Path) -> SdDrive {
    SdDrive {
        qemu_arg: format!("file=fat:rw:{},if=sd", sd_root.display()),
        image_path: None,
    }
}

fn e2e_sd_image_drive(sd_root: &Path, artifacts_dir: &Path) -> Result<SdDrive> {
    let image_path = artifacts_dir.join("sdcard.img");
    let _ = fs::remove_file(&image_path);
    sd_image::create_sd_image(sd_root, &image_path, E2E_SD_IMAGE_BYTES)?;

    eprintln!(
        "[e2e] simulator SD image: {} from {}",
        image_path.display(),
        sd_root.display()
    );
    Ok(SdDrive {
        qemu_arg: format!("file={},if=sd,format=raw", image_path.display()),
        image_path: Some(image_path),
    })
}

fn e2e_case_sd_root(args: &Args, artifacts_dir: &Path) -> Result<PathBuf> {
    let sd_root = args
        .sd_root
        .clone()
        .unwrap_or_else(|| artifacts_dir.join(".sd"));
    if args.sd_root.is_none() && sd_root.exists() {
        fs::remove_dir_all(&sd_root).with_context(|| {
            format!("clearing stale simulator E2E SD root {}", sd_root.display())
        })?;
    }
    simulator_sd_root(Some(sd_root))
}

fn partition_label(entry: &[u8]) -> String {
    let label = &entry[12..28];
    let end = label.iter().position(|b| *b == 0).unwrap_or(label.len());
    String::from_utf8_lossy(&label[..end]).to_string()
}

fn partition_offset(entry: &[u8]) -> usize {
    u32::from_le_bytes([entry[4], entry[5], entry[6], entry[7]]) as usize
}

fn parse_flash_layout(partition_table: &[u8]) -> Result<FlashLayout> {
    let mut boot_app0_offset = None;
    let mut app_offset = None;
    let mut first_app_offset = None;

    for entry in partition_table.chunks_exact(PARTITION_ENTRY_SIZE) {
        let magic = [entry[0], entry[1]];
        if magic == PARTITION_END_MAGIC {
            break;
        }
        if magic != PARTITION_MAGIC {
            return Err(anyhow!(
                "invalid partition table magic {:02x}{:02x}",
                entry[0],
                entry[1]
            ));
        }

        let entry_type = entry[2];
        let entry_subtype = entry[3];
        let offset = partition_offset(entry);
        let label = partition_label(entry);

        if entry_type == 0x00 {
            first_app_offset.get_or_insert(offset);
            if label == "app0" || entry_subtype == 0x10 {
                app_offset = Some(offset);
            }
        } else if label == "otadata" || (entry_type == 0x01 && entry_subtype == 0x00) {
            boot_app0_offset = Some(offset);
        }
    }

    Ok(FlashLayout {
        boot_app0_offset: boot_app0_offset
            .ok_or_else(|| anyhow!("partition table has no otadata entry for boot_app0.bin"))?,
        app_offset: app_offset
            .or(first_app_offset)
            .ok_or_else(|| anyhow!("partition table has no app entry for firmware.bin"))?,
    })
}

fn copy_flash_segment(
    img: &mut [u8],
    occupied: &mut Vec<(usize, usize, &'static str)>,
    offset: usize,
    data: &[u8],
    name: &'static str,
) -> Result<()> {
    let end = offset
        .checked_add(data.len())
        .ok_or_else(|| anyhow!("{name} offset overflows flash image"))?;
    if end > img.len() {
        return Err(anyhow!("{name} too large for flash offset 0x{offset:x}"));
    }
    for &(existing_offset, existing_end, existing_name) in occupied.iter() {
        if offset < existing_end && end > existing_offset {
            return Err(anyhow!(
                "{name} at 0x{offset:x}..0x{end:x} overlaps {existing_name} at 0x{existing_offset:x}..0x{existing_end:x}"
            ));
        }
    }
    img[offset..end].copy_from_slice(data);
    occupied.push((offset, end, name));
    Ok(())
}

/// Patch adc_hal_self_calibration in the firmware binary so it returns
/// immediately (movi a2, 0; retw.n). The ADC calibration hangs in QEMU
/// because the internal REGI2C bus is not emulated.
fn patch_adc_calibration(fw: &mut [u8]) {
    if fw.len() < 24 || fw[0] != 0xE9 {
        eprintln!("[headless] patch_adc: not an ESP image");
        return;
    }
    let segments = fw[1] as usize;
    let mut pos = 24usize;
    for seg_idx in 0..segments {
        if pos + 8 > fw.len() {
            break;
        }
        let load_addr = u32::from_le_bytes([fw[pos], fw[pos + 1], fw[pos + 2], fw[pos + 3]]);
        let seg_size =
            u32::from_le_bytes([fw[pos + 4], fw[pos + 5], fw[pos + 6], fw[pos + 7]]) as usize;
        eprintln!(
            "[headless] seg {seg_idx}: load=0x{load_addr:08x} size=0x{seg_size:x} data_at=0x{:x}",
            pos + 8
        );
        pos += 8;
        if load_addr == 0x42000020 && pos.checked_add(seg_size).map_or(false, |e| e <= fw.len()) {
            let needle: [u8; 4] = [0x36, 0xc1, 0x00, 0x7d];
            for i in (0..seg_size.saturating_sub(4)).step_by(2) {
                if fw[pos + i..pos + i + 4] == needle {
                    fw[pos + i] = 0x02;
                    fw[pos + i + 1] = 0x0c;
                    fw[pos + i + 2] = 0x1d;
                    fw[pos + i + 3] = 0xf0;
                    eprintln!("[headless] Patched adc_hal_self_calibration at seg offset 0x{i:x}");
                    return;
                }
            }
            eprintln!("[headless] adc calibration needle not found in .flash.text");
        }
        pos += seg_size;
    }
}

/// Patch the bootloader's software-reset infinite loop so the function
/// returns instead of spinning forever.  On real hardware the bootloader
/// triggers SW_PROCPU_RESET via RTC_CNTL and the chip resets immediately.
/// In QEMU the reset may not fully restart the bootloader, leaving the CPU
/// stuck in a `j self` loop.  Replacing the loop with `retw.n` lets the
/// function return and execution continues past the reset attempt.
fn patch_bootloader_reset(bl: &mut [u8]) {
    if bl.len() < 24 || bl[0] != 0xE9 {
        eprintln!("[headless] patch_bootloader_reset: not an ESP image");
        return;
    }
    let segments = bl[1] as usize;
    let mut pos = 24usize;
    for _seg_idx in 0..segments {
        if pos + 8 > bl.len() {
            break;
        }
        let load_addr = u32::from_le_bytes([bl[pos], bl[pos + 1], bl[pos + 2], bl[pos + 3]]);
        let seg_size =
            u32::from_le_bytes([bl[pos + 4], bl[pos + 5], bl[pos + 6], bl[pos + 7]]) as usize;
        pos += 8;

        if load_addr == 0x403cb700 && pos.checked_add(seg_size).map_or(false, |e| e <= bl.len()) {
            // At seg2 offset 0x19e9 (0x403cd0e9 - 0x403cb700):
            //   06 ff ff  =  j self  (infinite loop waiting for reset)
            // Replace with:
            //   1d f0    =  retw.n  (return from function)
            //   00       =  NOP padding
            const RESET_LOOP_SEG_OFFSET: usize = 0x19e9;
            if RESET_LOOP_SEG_OFFSET + 3 <= seg_size {
                let patch_offset = pos + RESET_LOOP_SEG_OFFSET;
                if bl[patch_offset] == 0x06
                    && bl[patch_offset + 1] == 0xff
                    && bl[patch_offset + 2] == 0xff
                {
                    bl[patch_offset] = 0x1D;
                    bl[patch_offset + 1] = 0xF0;
                    bl[patch_offset + 2] = 0x00;
                    eprintln!(
                        "[headless] Patched bootloader reset loop at seg2 offset 0x{:x} → retw.n",
                        RESET_LOOP_SEG_OFFSET
                    );
                } else {
                    eprintln!(
                        "[headless] Bootloader reset loop bytes don't match: {:02x} {:02x} {:02x}",
                        bl[patch_offset],
                        bl[patch_offset + 1],
                        bl[patch_offset + 2]
                    );
                }
            } else {
                eprintln!(
                    "[headless] Bootloader seg2 too small for reset loop patch (size=0x{:x})",
                    seg_size
                );
            }
        }
        pos += seg_size;
    }
}

/// Patch the ESP32-S3 ROM binary to allow booting with incomplete SPI flash
/// emulation. Two patches:
///
/// 1. At offset 0x45b27: NOP the `j 0x45890` (error handler jump) that fires
///    when the bootloader image checksum verification fails at 0x45b1f.
///    The ROM reads the bootloader from SPI flash and verifies a checksum on
///    each segment. QEMU's SPI flash emulation returns slightly incorrect
///    data in certain modes, causing the checksum to mismatch. NOP'ing this
///    jump makes the code fall through to the success path at 0x45b2a even
///    when the checksum fails, so the bootloader still gets loaded into RAM.
///
/// 2. At offset 0x43ac8: NOP out the `j self` panic loop as a safety net.
fn patch_rom_spi_panic(rom: &mut [u8]) {
    // Patch 1: NOP the error jump after checksum failure
    const ROM_CHECKSUM_ERROR_JUMP: usize = 0x45b27;
    if ROM_CHECKSUM_ERROR_JUMP + 3 <= rom.len() {
        // Original: 46 59 ff = j 0x45890
        // Replace:  00 00 20 = or a0, a0, a0 (3-byte NOP)
        if rom[ROM_CHECKSUM_ERROR_JUMP] == 0x46
            && rom[ROM_CHECKSUM_ERROR_JUMP + 1] == 0x59
            && rom[ROM_CHECKSUM_ERROR_JUMP + 2] == 0xff
        {
            rom[ROM_CHECKSUM_ERROR_JUMP] = 0x00;
            rom[ROM_CHECKSUM_ERROR_JUMP + 1] = 0x00;
            rom[ROM_CHECKSUM_ERROR_JUMP + 2] = 0x20;
            eprintln!(
                "[headless] Patched ROM checksum error jump at offset 0x{:x} → NOP",
                ROM_CHECKSUM_ERROR_JUMP
            );
        } else {
            eprintln!(
                "[headless] ROM checksum error jump bytes don't match: {:02x} {:02x} {:02x}",
                rom[ROM_CHECKSUM_ERROR_JUMP],
                rom[ROM_CHECKSUM_ERROR_JUMP + 1],
                rom[ROM_CHECKSUM_ERROR_JUMP + 2]
            );
        }
    }

    // Patch 2: NOP out the panic loop at 0x43ac8
    const ROM_SPI_PANIC_OFFSET: usize = 0x43ac8;
    if ROM_SPI_PANIC_OFFSET + 3 <= rom.len() {
        if rom[ROM_SPI_PANIC_OFFSET] == 0x06
            && rom[ROM_SPI_PANIC_OFFSET + 1] == 0xff
            && rom[ROM_SPI_PANIC_OFFSET + 2] == 0xff
        {
            rom[ROM_SPI_PANIC_OFFSET] = 0x00;
            rom[ROM_SPI_PANIC_OFFSET + 1] = 0x00;
            rom[ROM_SPI_PANIC_OFFSET + 2] = 0x20;
            eprintln!(
                "[headless] Patched ROM SPI panic loop at offset 0x{:x} → NOP",
                ROM_SPI_PANIC_OFFSET
            );
        }
    }

    // Patch 3: NOP out SHA_BUSY polling loop at 0x45109.
    // The ROM function at 0x450e4 outputs firmware bytes one at a time,
    // polling SHA_BUSY (0x6003b018) after each byte. QEMU's SHA emulation
    // returns 0 (not busy) but the per-byte MMIO read is extremely slow.
    // Original: 26 18 f7 = beqi a8, 1, -6  (loop back if busy)
    // Replace:  00 20 20 = or a0, a0, a0   (3-byte NOP — fall through)
    const ROM_SHA_BUSY_POLL: usize = 0x45109;
    if ROM_SHA_BUSY_POLL + 3 <= rom.len() {
        if rom[ROM_SHA_BUSY_POLL] == 0x26
            && rom[ROM_SHA_BUSY_POLL + 1] == 0x18
            && rom[ROM_SHA_BUSY_POLL + 2] == 0xf7
        {
            rom[ROM_SHA_BUSY_POLL] = 0x00;
            rom[ROM_SHA_BUSY_POLL + 1] = 0x20;
            rom[ROM_SHA_BUSY_POLL + 2] = 0x20;
            eprintln!(
                "[headless] Patched ROM SHA BUSY poll at offset 0x{:x} → NOP",
                ROM_SHA_BUSY_POLL
            );
        }
    }

    // Patch 4: Keep ROM strcmp's ENTRY and replace the body with immediate
    // "not equal" return. ENTRY must run so Xtensa window state remains valid.
    // The ROM strcmp function does byte-by-byte comparison with alignment
    // optimizations. When called on long strings from zero-filled RAM (not
    // real flash), it loops for an extremely long time in QEMU TCG.
    // Original: 36 21 00 = entry a1, 16
    // Patch +3: 0c 12    = movi.n a2, 1   (return "not equal")
    //           1d f0    = retw.n
    const ROM_STRCMP: usize = 0x55448;
    if ROM_STRCMP + 7 <= rom.len() {
        if rom[ROM_STRCMP] == 0x36 && rom[ROM_STRCMP + 1] == 0x21 && rom[ROM_STRCMP + 2] == 0x00 {
            rom[ROM_STRCMP + 3] = 0x0c; // movi.n a2, 1
            rom[ROM_STRCMP + 4] = 0x12;
            rom[ROM_STRCMP + 5] = 0x1d; // retw.n
            rom[ROM_STRCMP + 6] = 0xf0;
            eprintln!(
                "[headless] Patched ROM strcmp at offset 0x{:x}+3 → return 1 (not equal)",
                ROM_STRCMP
            );
        }
    }

    // Patch 5: Keep ROM strlen's ENTRY and replace the body with immediate
    // "length 0" return.
    // Same issue — scans for null terminator in large zero-filled regions.
    // Original: 36 21 00 = entry a1, 16
    // Patch +3: 0c 02    = movi.n a2, 0   (return length 0)
    //           1d f0    = retw.n
    const ROM_STRLEN: usize = 0x55698;
    if ROM_STRLEN + 7 <= rom.len() {
        if rom[ROM_STRLEN] == 0x36 && rom[ROM_STRLEN + 1] == 0x21 && rom[ROM_STRLEN + 2] == 0x00 {
            rom[ROM_STRLEN + 3] = 0x0c; // movi.n a2, 0
            rom[ROM_STRLEN + 4] = 0x02;
            rom[ROM_STRLEN + 5] = 0x1d; // retw.n
            rom[ROM_STRLEN + 6] = 0xf0;
            eprintln!(
                "[headless] Patched ROM strlen at offset 0x{:x}+3 → return 0",
                ROM_STRLEN
            );
        }
    }
}

fn build_flash_image(firmware_bin: &Path, output_path: Option<&Path>) -> Result<PathBuf> {
    let build_dir = firmware_bin.parent().unwrap_or(Path::new("."));
    let bootloader =
        existing_sibling_or_nested(build_dir, "bootloader.bin", "bootloader/bootloader.bin");
    let boot_app0 = existing_sibling_or_nested(build_dir, "boot_app0.bin", "ota_data_initial.bin");
    let partitions = existing_sibling_or_nested(
        build_dir,
        "partitions.bin",
        "partition_table/partition-table.bin",
    );
    // 并行 E2E 必须为每个 case 保留独立 flash，避免 QEMU 的 NVS/启动状态互相污染。
    let flash_img = output_path
        .map(Path::to_path_buf)
        .unwrap_or_else(|| build_dir.join("flash.img"));

    if !bootloader.exists() || !boot_app0.exists() || !partitions.exists() {
        return Err(anyhow!(
            "Missing Panda AI OS ESP-IDF image files in {}. Expected bootloader/bootloader.bin, partition_table/partition-table.bin, ota_data_initial.bin, and panda_os.bin.",
            build_dir.display()
        ));
    }

    let mut img = vec![0xFFu8; FLASH_SIZE_BYTES];

    let mut bl_data = std::fs::read(&bootloader)?;
    let boot_app0_data = std::fs::read(&boot_app0)?;
    let pt_data = std::fs::read(&partitions)?;
    let mut fw_data = std::fs::read(firmware_bin)?;
    let layout = parse_flash_layout(&pt_data)?;

    patch_bootloader_reset(&mut bl_data);
    patch_adc_calibration(&mut fw_data);
    let mut occupied = Vec::new();
    copy_flash_segment(
        &mut img,
        &mut occupied,
        BOOTLOADER_OFFSET,
        &bl_data,
        "bootloader",
    )?;
    copy_flash_segment(
        &mut img,
        &mut occupied,
        PARTITION_TABLE_OFFSET,
        &pt_data,
        "partitions",
    )?;
    copy_flash_segment(
        &mut img,
        &mut occupied,
        layout.boot_app0_offset,
        &boot_app0_data,
        "boot_app0",
    )?;
    copy_flash_segment(
        &mut img,
        &mut occupied,
        layout.app_offset,
        &fw_data,
        "firmware",
    )?;

    std::fs::write(&flash_img, img)?;

    Ok(flash_img)
}

fn qemu_symbol_elf_path(firmware_path: &Path) -> Result<PathBuf> {
    match firmware_path.extension().and_then(|s| s.to_str()) {
        Some("elf") => Ok(firmware_path.to_path_buf()),
        Some("bin") => {
            let elf_path = firmware_path.with_extension("elf");
            if elf_path.exists() {
                Ok(elf_path)
            } else {
                Err(anyhow!(
                    "Matching firmware ELF not found for {} at {}; build Panda AI OS device firmware before launching .bin so QEMU symbol intercepts match the firmware image",
                    firmware_path.display(),
                    elf_path.display()
                ))
            }
        }
        _ => Err(anyhow!("Expected firmware.bin (preferred) or firmware.elf")),
    }
}

fn existing_sibling_or_nested(build_dir: &Path, sibling: &str, nested: &str) -> PathBuf {
    let direct = build_dir.join(sibling);
    if direct.exists() {
        return direct;
    }
    build_dir.join(nested)
}

fn qemu_pc_bios_dir(qemu_path: &Path) -> Option<PathBuf> {
    let qemu_dir = qemu_path.parent()?;
    let bios_dir = qemu_dir.join("../pc-bios");
    bios_dir
        .join("esp32s3_rev0_rom.bin")
        .exists()
        .then_some(bios_dir)
}

fn add_qemu_pc_bios_dir(qemu_cmd: &mut Command, qemu_path: &Path) {
    if let Some(bios_dir) = qemu_pc_bios_dir(qemu_path) {
        qemu_cmd.arg("-L").arg(bios_dir);
    }
}

fn qemu_rom_path(qemu_path: &Path) -> Option<PathBuf> {
    let rom_path = qemu_path.parent()?.join("../pc-bios/esp32s3_rev0_rom.bin");
    rom_path.exists().then_some(rom_path)
}

fn add_qemu_rom_env(qemu_cmd: &mut Command, qemu_path: &Path) {
    if let Some(rom_path) = qemu_rom_path(qemu_path) {
        qemu_cmd.env("MOFEI_ROM_BINARY", rom_path);
    }
}

fn add_qemu_heap_mode_env(qemu_cmd: &mut Command, heap_mode: Option<HeapMode>) {
    if let Some(heap_mode) = heap_mode {
        qemu_cmd.env("MOFEI_SIM_HEAP_MODE", heap_mode.as_env_value());
    }
}

#[tokio::main]
async fn main() -> Result<()> {
    let args = normalize_args_paths(&Args::parse())?;
    let board = load_board_runtime(args.board.as_deref())?;
    eprintln!(
        "[headless] board: {} raw={}x{} output={}x{}",
        board.id,
        board.geometry.raw_width,
        board.geometry.raw_height,
        board.geometry.output_width,
        board.geometry.output_height
    );
    let debug_writer = debug_transport::HeadlessDebugWriter::new();
    let debug_broker = debug_transport::DebugTransportBroker::new();
    let _debug_server = debug_transport::spawn_from_config(
        args.debug_port,
        debug_writer.clone(),
        debug_broker.clone(),
    )?;

    match (&args.case, &args.case_dir) {
        (Some(_), Some(_)) => {
            return Err(anyhow!("use only one of --case or --case-dir"));
        }
        (Some(case_path), None) => {
            let case = load_e2e_case(case_path)?;
            let outcome =
                run_e2e_case(&args, case, debug_writer.clone(), debug_broker.clone()).await?;
            if outcome.status != "FAIL" {
                return Ok(());
            }
            eprintln!(
                "[e2e] CASE FAILED: {} — {}",
                outcome.case_id,
                outcome.error.unwrap_or_default()
            );
            std::process::exit(1);
        }
        (None, Some(case_dir)) => {
            return run_e2e_case_dir(&args, case_dir, debug_writer.clone(), debug_broker.clone())
                .await;
        }
        (None, None) => {}
    }

    let out = args
        .out
        .as_ref()
        .ok_or_else(|| anyhow!("--out is required unless --case or --case-dir is provided"))?;

    let firmware_path = args.firmware.clone();

    let monitor_socket = args.socket.with_extension("monitor.sock");
    if args.socket.exists() {
        std::fs::remove_file(&args.socket)
            .with_context(|| format!("removing stale socket {}", args.socket.display()))?;
    }
    if monitor_socket.exists() {
        std::fs::remove_file(&monitor_socket).with_context(|| {
            format!("removing stale monitor socket {}", monitor_socket.display())
        })?;
    }

    let socket_arg = format!(
        "socket,id=mofei,path={},server=on,wait=off",
        args.socket.display()
    );
    let monitor_arg = format!("unix:{},server=on,wait=off", monitor_socket.display());
    let sd_root = simulator_sd_root(args.sd_root.clone())?;
    eprintln!("[headless] simulator SD root: {}", sd_root.display());
    let simulator_location = write_simulator_location(&sd_root)?;
    sync_visible_system_dir(&sd_root)?;
    eprintln!(
        "[headless] simulator location: {} {} {:.4},{:.4}",
        simulator_location.name,
        simulator_location.timezone,
        simulator_location.latitude,
        simulator_location.longitude
    );

    let mut qemu_cmd = Command::new(&args.qemu);
    add_qemu_pc_bios_dir(&mut qemu_cmd, &args.qemu);
    add_qemu_rom_env(&mut qemu_cmd, &args.qemu);
    add_qemu_heap_mode_env(&mut qemu_cmd, args.heap_mode);
    qemu_cmd.env("MOFEI_SIM_BOARD", &board.id);
    qemu_cmd.arg("-machine").arg("esp32s3");
    qemu_cmd.arg("-smp").arg("1");

    let symbol_elf_path = qemu_symbol_elf_path(&firmware_path)?;
    eprintln!("[headless] firmware image: {}", firmware_path.display());
    eprintln!("[headless] QEMU symbol ELF: {}", symbol_elf_path.display());
    qemu_cmd.env("MOFEI_FIRMWARE_ELF", &symbol_elf_path);

    if firmware_path.extension().and_then(|s| s.to_str()) == Some("bin") {
        let flash_img = build_flash_image(&firmware_path, None)?;
        qemu_cmd
            .arg("-drive")
            .arg(format!("file={},if=mtd,format=raw", flash_img.display()));

        // Match the interactive Tauri launcher: keep the assembled flash image
        // available for partition/flash-backed data, but enter through the ELF
        // trampoline when the matching ELF exists. The pure ROM flash-boot path
        // repeatedly trips the bootloader software-reset sequence under QEMU,
        // while the ELF path is the simulator-supported route that redirects
        // FreeRTOS startup into app_main/loopTask.
        qemu_cmd.arg("-kernel").arg(&symbol_elf_path);

        // Patch the ROM binary to skip SPI flash validation panics.
        // The ROM at 0x40043ac8 has a panic loop that fires when SPI flash
        // validation fails in QEMU's incomplete SPI controller emulation.
        // We write a patched copy and pass it via MOFEI_ROM_BINARY env var
        // so QEMU's flash boot path loads it instead of the default ROM.
        let qemu_dir = args.qemu.parent().unwrap_or(Path::new("."));
        let rom_src = qemu_dir.join("../pc-bios/esp32s3_rev0_rom.bin");
        if rom_src.exists() {
            let mut rom_data = std::fs::read(&rom_src)
                .with_context(|| format!("reading ROM from {}", rom_src.display()))?;
            patch_rom_spi_panic(&mut rom_data);
            let patched_rom = firmware_path
                .parent()
                .unwrap_or(Path::new("."))
                .join("esp32s3_rev0_rom.patched.bin");
            std::fs::write(&patched_rom, rom_data)?;
            qemu_cmd.env("MOFEI_ROM_BINARY", &patched_rom);
        } else {
            eprintln!(
                "[headless] WARNING: ROM binary not found at {}, skipping ROM patch",
                rom_src.display()
            );
        }
    } else if firmware_path.extension().and_then(|s| s.to_str()) == Some("elf") {
        qemu_cmd.arg("-kernel").arg(&symbol_elf_path);
        // Do not pass -drive when booting ELF, to ensure trampoline is used
    } else {
        return Err(anyhow!("Expected firmware.bin (preferred) or firmware.elf"));
    }

    qemu_cmd
        .arg("-drive")
        .arg(sd_directory_drive(&sd_root).qemu_arg);

    let child = qemu_cmd
        .arg("-display")
        .arg("none")
        .arg("-serial")
        .arg(if args.serial { "stdio" } else { "null" })
        .arg("-S")
        .arg("-monitor")
        .arg(&monitor_arg)
        .arg("-semihosting-config")
        .arg("enable=on,target=native")
        .arg("-chardev")
        .arg(&socket_arg)
        .stdin(Stdio::null())
        .stdout(if args.serial {
            Stdio::inherit()
        } else {
            Stdio::null()
        })
        .stderr(Stdio::piped())
        .spawn()
        .with_context(|| format!("spawning {}", args.qemu.display()))?;
    let mut qemu = QemuChildGuard::new(child, args.socket.clone(), monitor_socket.clone());

    let (event_tx, mut event_rx) = mpsc::unbounded_channel::<HeadlessEvent>();
    let stderr = qemu
        .child
        .as_mut()
        .ok_or_else(|| anyhow!("QEMU child missing immediately after spawn"))?
        .stderr
        .take()
        .ok_or_else(|| anyhow!("QEMU stderr pipe unavailable"))?;
    let mut taps = Vec::new();
    if args.inject_touch_tap {
        taps.push(TouchTap {
            x: args.tap_x,
            y: args.tap_y,
        });
    }
    taps.extend(args.extra_touch_tap.iter().copied());
    let mapped_taps = taps
        .iter()
        .map(|tap| {
            board
                .geometry
                .map_reference_touch(ReferenceViewport::default(), *tap)
        })
        .collect::<Vec<TouchTap>>();
    for (reference, mapped) in taps.iter().zip(mapped_taps.iter()) {
        if reference != mapped {
            eprintln!(
                "[headless] mapped reference tap x={} y={} to board tap x={} y={}",
                reference.x, reference.y, mapped.x, mapped.y
            );
        }
    }

    let stderr_handle = tokio::spawn(read_qemu_stderr(
        stderr,
        event_tx.clone(),
        mapped_taps.clone(),
        None,
    ));
    let has_injections = !mapped_taps.is_empty() || !args.inject_buttons.is_empty();
    let injection_plan = InjectionPlan {
        taps: mapped_taps.clone(),
        inject_buttons: args.inject_buttons.clone(),
        button_hold_ms: args.button_hold_ms,
        post_tap_delay_ms: args.post_tap_delay_ms,
        post_tap_frames_before_buttons: args.post_tap_frames_before_buttons,
        startup_frames_before_input: args.startup_frames_before_input,
    };
    let stream = connect_unix_socket(&args.socket, 120, Duration::from_millis(100))
        .await
        .with_context(|| format!("connecting ipc socket {}", args.socket.display()))?;
    let (read_half, write_half) = stream.into_split();
    let (outbound_tx, outbound_rx) = mpsc::unbounded_channel::<debug_transport::OutFrame>();
    debug_writer.bind(outbound_tx).await;
    let (radio_peer, _radio_peer_handle) =
        start_radio_peer_bridge(&args, debug_writer.clone()).await?;
    let writer_handle = tokio::spawn(async move {
        if let Err(err) = write_loop(write_half, outbound_rx).await {
            eprintln!("[headless] IPC write loop stopped: {err}");
        }
    });
    let (_injection_tx, injection_rx) = mpsc::unbounded_channel::<InjectionCommand>();
    let reader_handle = tokio::spawn(read_loop(
        read_half,
        event_tx,
        injection_plan,
        injection_rx,
        board.geometry.clone(),
        debug_writer.clone(),
        debug_broker.clone(),
        radio_peer,
    ));
    continue_qemu(&monitor_socket)
        .await
        .with_context(|| format!("continuing QEMU via monitor {}", monitor_socket.display()))?;

    // Capture framebuffers until the duration elapses or QEMU exits.
    let deadline = time::Instant::now() + Duration::from_secs(args.duration);
    let mut last_fb: Option<Vec<u8>> = None;
    let mut frames_seen: u64 = 0;
    let mut frames_at_injection: Option<u64> = None;
    let mut touch_reads_seen = std::collections::HashSet::new();
    let require_lilygo_confirm_proof =
        board.id == "lilygo-t5s3-pro" && args.inject_buttons.contains(&1);
    let mut lilygo_confirm_down_seen = false;
    let mut lilygo_confirm_up_seen = false;
    let mut termination_requested = false;
    let shutdown = wait_for_shutdown_signal();
    tokio::pin!(shutdown);
    loop {
        tokio::select! {
            _ = time::sleep_until(deadline) => break,
            _ = &mut shutdown => {
                eprintln!("[headless] received shutdown signal");
                termination_requested = true;
                break;
            }
            maybe_event = event_rx.recv() => {
                match maybe_event {
                    Some(HeadlessEvent::Frame(fb)) => {
                        let has_meaningful_content = framebuffer_has_meaningful_content(&fb, &board.geometry);
                        last_fb = Some(fb);
                        frames_seen += 1;
                        let post_injection_frames = frames_at_injection
                            .map(|frame_count| frames_seen.saturating_sub(frame_count))
                            .unwrap_or(0);
                        let input_proof_complete = touch_reads_seen.len() >= mapped_taps.len()
                            && (!require_lilygo_confirm_proof
                                || (lilygo_confirm_down_seen && lilygo_confirm_up_seen));
                        if has_meaningful_content
                            && (!has_injections || (input_proof_complete && post_injection_frames > 0))
                        {
                            if mapped_taps.is_empty() || args.inject_buttons.is_empty() {
                                break;
                            }
                        }
                    }
                    Some(HeadlessEvent::Injected) => {
                        frames_at_injection = Some(frames_seen);
                        eprintln!("[headless] injection completed after {frames_seen} framebuffer(s)");
                    }
                    Some(HeadlessEvent::TouchRead(tap)) => {
                        touch_reads_seen.insert(tap);
                        eprintln!("[headless] touch read observed by firmware x={} y={}", tap.x, tap.y);
                    }
                    Some(HeadlessEvent::ButtonRead { button_id, pressed }) => {
                        if button_id == 1 {
                            if pressed {
                                lilygo_confirm_down_seen = true;
                            } else if lilygo_confirm_down_seen {
                                lilygo_confirm_up_seen = true;
                            }
                            eprintln!(
                                "[headless] button read observed by firmware button_id={button_id} pressed={pressed}"
                            );
                        }
                    }
                    Some(HeadlessEvent::PeripheralControl(payload)) => {
                        log_peripheral_control_event(&payload);
                    }
                    Some(HeadlessEvent::SerialLine) => {}
                    None => break,
                }
            }
            status = qemu.wait() => {
                eprintln!("[headless] QEMU exited with {:?}", status);
                break;
            }
        }
    }

    // Stop QEMU and reader.
    qemu.cleanup().await;
    debug_writer.unbind().await;
    reader_handle.abort();
    writer_handle.abort();
    stderr_handle.abort();

    if termination_requested {
        return Err(anyhow!("received shutdown signal"));
    }

    eprintln!("[headless] frames received: {frames_seen}");
    if let Some(frame_count) = frames_at_injection {
        eprintln!(
            "[headless] post-injection frames received: {}",
            frames_seen.saturating_sub(frame_count)
        );
    }
    let fb = last_fb.ok_or_else(|| anyhow!("no framebuffer received before deadline"))?;
    if has_injections {
        let frames_at_injection = frames_at_injection.ok_or_else(|| {
            anyhow!("no injection performed before deadline; received {frames_seen} framebuffer(s)")
        })?;
        if !mapped_taps.is_empty() {
            for tap in &mapped_taps {
                if !touch_reads_seen.contains(tap) {
                    return Err(anyhow!(
                        "no firmware touch_read observed after injected tap x={} y={}",
                        tap.x,
                        tap.y
                    ));
                }
            }
        }
        if frames_seen <= frames_at_injection {
            return Err(anyhow!(
                "no framebuffer received after injection; startup frame count was {frames_at_injection}"
            ));
        }
        if require_lilygo_confirm_proof && !(lilygo_confirm_down_seen && lilygo_confirm_up_seen) {
            return Err(anyhow!(
                "LilyGo Confirm injection was not observed as ordered PCA9535 firmware reads"
            ));
        }
    }
    write_png(out, &fb, &board.geometry).with_context(|| format!("writing {}", out.display()))?;
    eprintln!("[headless] screenshot written: {}", out.display());
    Ok(())
}

async fn read_qemu_stderr(
    stderr: tokio::process::ChildStderr,
    event_tx: mpsc::UnboundedSender<HeadlessEvent>,
    taps: Vec<TouchTap>,
    ctx: Option<std::sync::Arc<tokio::sync::Mutex<E2EContext>>>,
) -> Result<()> {
    let mut lines = BufReader::new(stderr).lines();
    while let Some(line) = lines.next_line().await? {
        eprintln!("{line}");
        if let Some(ctx) = &ctx {
            let mut ctx = ctx.lock().await;
            ctx.push_line(&line);
            ctx.parse_activity_transition(&line);
        }
        for tap in &taps {
            let touch_read_marker = format!("[MOFEI-SIM] touch_read raw={},{}", tap.x, tap.y);
            let gt911_coordinates = format!("x={} y={}", tap.x, tap.y);
            if line.contains(&touch_read_marker)
                || (line.contains("[LILYGO-GT911-TRACE]") && line.contains(&gt911_coordinates))
            {
                let _ = event_tx.send(HeadlessEvent::TouchRead(*tap));
            }
        }
        if line.contains("[LILYGO-PCA9535-FUNCTION-TRACE]") && line.contains("source=firmware-read")
        {
            if line.contains("pressed=1") {
                let _ = event_tx.send(HeadlessEvent::ButtonRead {
                    button_id: 1,
                    pressed: true,
                });
            } else if line.contains("pressed=0") {
                let _ = event_tx.send(HeadlessEvent::ButtonRead {
                    button_id: 1,
                    pressed: false,
                });
            }
        }
    }
    Ok(())
}

async fn connect_unix_socket(path: &Path, attempts: u32, delay: Duration) -> Result<UnixStream> {
    let mut last_error = None;
    for _ in 0..attempts {
        match UnixStream::connect(path).await {
            Ok(stream) => return Ok(stream),
            Err(err) => {
                last_error = Some(err);
                time::sleep(delay).await;
            }
        }
    }
    Err(last_error
        .map(anyhow::Error::from)
        .unwrap_or_else(|| anyhow!("no connection attempts performed")))
}

async fn continue_qemu(monitor_socket: &Path) -> Result<()> {
    let mut monitor = connect_unix_socket(monitor_socket, 120, Duration::from_millis(100)).await?;
    monitor.write_all(b"cont\n").await?;
    Ok(())
}

#[cfg(unix)]
async fn wait_for_shutdown_signal() {
    use tokio::signal::unix::{signal, SignalKind};

    let mut terminate =
        signal(SignalKind::terminate()).expect("failed to register SIGTERM handler");
    tokio::select! {
        _ = tokio::signal::ctrl_c() => {}
        _ = terminate.recv() => {}
    }
}

#[cfg(not(unix))]
async fn wait_for_shutdown_signal() {
    let _ = tokio::signal::ctrl_c().await;
}

async fn read_loop(
    mut read_half: tokio::net::unix::OwnedReadHalf,
    event_tx: mpsc::UnboundedSender<HeadlessEvent>,
    injection_plan: InjectionPlan,
    mut injection_rx: mpsc::UnboundedReceiver<InjectionCommand>,
    geometry: BoardGeometry,
    writer: debug_transport::HeadlessDebugWriter,
    debug_broker: debug_transport::DebugTransportBroker,
    radio_peer: Option<RadioPeerSender>,
) -> Result<()> {
    let mut header = [0u8; HEADER_LEN];
    let mut injection_started = false;
    let mut injection_completed = false;
    let mut next_tap_index = 0usize;
    let mut frames_since_last_injection = 0u64;
    let mut meaningful_frames_before_input = 0u64;
    loop {
        tokio::select! {
            header_result = read_half.read_exact(&mut header) => {
                match header_result {
                    Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof
                        || e.kind() == std::io::ErrorKind::BrokenPipe => return Ok(()),
                    Err(e) => return Err(e.into()),
                    Ok(_) => {}
                }
                let channel = header[0];
                let _flags = header[1];
                let payload_len = u32::from_le_bytes([header[4], header[5], header[6], header[7]]);
                if payload_len > MAX_PAYLOAD {
                    return Err(anyhow!("payload_len {payload_len} exceeds cap"));
                }
                let mut payload = vec![0u8; payload_len as usize];
                if payload_len > 0 {
                    read_half.read_exact(&mut payload).await?;
                }
                match channel {
                    CHANNEL_FRAMEBUFFER => {
                        if payload.len() == geometry.fb_bytes() {
                            let has_content = framebuffer_has_content(&payload, &geometry);
                            if has_content && !injection_started {
                                meaningful_frames_before_input =
                                    meaningful_frames_before_input.saturating_add(1);
                            }
                            let can_start_injection = injection_plan.has_injections()
                                && !injection_started
                                && !injection_completed
                                && has_content
                                && meaningful_frames_before_input
                                    >= injection_plan.startup_frames_before_input.clamp(
                                        MIN_STARTUP_FRAMES_BEFORE_INPUT,
                                        MAX_STARTUP_FRAMES_BEFORE_INPUT,
                                    );
                            if event_tx.send(HeadlessEvent::Frame(payload)).is_err() {
                                return Ok(());
                            }

                            if !injection_plan.has_injections() || injection_completed {
                                continue;
                            }

                            if can_start_injection {
                                injection_started = true;
                            } else if injection_started {
                                frames_since_last_injection += 1;
                            } else {
                                continue;
                            }

                            if next_tap_index < injection_plan.taps.len() {
                                let required_frames = if next_tap_index == FIRST_TAP_INDEX {
                                    if injection_plan.startup_frames_before_input
                                        <= MIN_STARTUP_FRAMES_BEFORE_INPUT
                                    {
                                        NO_INPUT_FRAME_GAP
                                    } else {
                                        FIRST_TAP_FRAME_GAP
                                    }
                                } else {
                                    injection_plan.post_tap_frames_before_buttons
                                };
                                if frames_since_last_injection < required_frames {
                                    continue;
                                }

                                let tap = injection_plan.taps[next_tap_index];
                                write_touch_tap(&writer, tap.x, tap.y).await?;
                                if injection_plan.post_tap_delay_ms > 0 {
                                    time::sleep(Duration::from_millis(injection_plan.post_tap_delay_ms))
                                        .await;
                                }
                                next_tap_index += 1;
                                frames_since_last_injection = 0;
                                if next_tap_index == injection_plan.taps.len()
                                    && injection_plan.inject_buttons.is_empty()
                                {
                                    injection_completed = true;
                                    if event_tx.send(HeadlessEvent::Injected).is_err() {
                                        return Ok(());
                                    }
                                }
                                continue;
                            }

                            let required_button_frames = if injection_plan.taps.is_empty() {
                                NO_WAIT_FRAMES
                            } else {
                                injection_plan.post_tap_frames_before_buttons
                            };
                            if !injection_plan.inject_buttons.is_empty()
                                && frames_since_last_injection >= required_button_frames
                            {
                                for button_id in &injection_plan.inject_buttons {
                                    write_button_click(
                                        &writer,
                                        *button_id,
                                        injection_plan.button_hold_ms,
                                    )
                                    .await?;
                                }
                                injection_completed = true;
                                if event_tx.send(HeadlessEvent::Injected).is_err() {
                                    return Ok(());
                                }
                            }
                        }
                    }
                    CHANNEL_DEBUG => {
                        debug_broker.publish(payload);
                    }
                    CHANNEL_CONTROL => {
                        if is_sx1262_tx_event(&payload) {
                            if let Some(peer) = &radio_peer {
                                match peer.forward(&payload).await {
                                    Ok(()) => eprintln!(
                                        "[radio-broker] forwarded local SX1262 transmit event"
                                    ),
                                    Err(error) => eprintln!("[radio-broker] forward failed: {error}"),
                                }
                            }
                        }
                        let _ = event_tx.send(HeadlessEvent::PeripheralControl(payload));
                    }
                    _ => { /* drain other channels */ }
                }
            }
            maybe_cmd = injection_rx.recv() => {
                match maybe_cmd {
                    Some(InjectionCommand::TouchTap { x, y }) => {
                        write_touch_tap(&writer, x, y).await?;
                        let _ = event_tx.send(HeadlessEvent::Injected);
                    }
                    Some(InjectionCommand::TouchSwipe { x, y, direction }) => {
                        write_touch_swipe(&writer, x, y, direction, &geometry).await?;
                        let _ = event_tx.send(HeadlessEvent::Injected);
                    }
                    Some(InjectionCommand::ButtonClick { button_id, hold_ms }) => {
                        write_button_click(
                            &writer,
                            button_id,
                            hold_ms.unwrap_or(injection_plan.button_hold_ms),
                        )
                        .await?;
                        let _ = event_tx.send(HeadlessEvent::Injected);
                    }
                    Some(InjectionCommand::SyntheticTouchEvent { touch_type, x, y }) => {
                        write_synthetic_touch_event(&writer, touch_type, x, y).await?;
                        let _ = event_tx.send(HeadlessEvent::Injected);
                    }
                    None => { /* channel closed */ }
                }
            }
        }
    }
}

async fn write_loop(
    mut write_half: tokio::net::unix::OwnedWriteHalf,
    mut rx: mpsc::UnboundedReceiver<debug_transport::OutFrame>,
) -> Result<()> {
    while let Some(frame) = rx.recv().await {
        let len = frame.payload.len() as u32;
        let header = [
            frame.channel,
            frame.flags,
            0,
            0,
            (len & 0xFF) as u8,
            ((len >> 8) & 0xFF) as u8,
            ((len >> 16) & 0xFF) as u8,
            ((len >> 24) & 0xFF) as u8,
        ];
        write_half.write_all(&header).await?;
        if !frame.payload.is_empty() {
            write_half.write_all(&frame.payload).await?;
        }
        if frame.channel == CHANNEL_SERIAL_COMMAND {
            eprintln!("[headless] serial command frame written bytes={len}");
        }
    }
    Ok(())
}

fn framebuffer_white_byte(geometry: &BoardGeometry) -> u8 {
    match geometry.framebuffer_format {
        FramebufferFormat::Mono1 => 0xFF,
        FramebufferFormat::Gray16 => 0x00,
    }
}

fn framebuffer_has_content(payload: &[u8], geometry: &BoardGeometry) -> bool {
    let white = framebuffer_white_byte(geometry);
    payload.iter().any(|byte| *byte != white)
}

fn framebuffer_has_meaningful_content(payload: &[u8], geometry: &BoardGeometry) -> bool {
    let white = framebuffer_white_byte(geometry);
    payload.iter().filter(|byte| **byte != white).count() >= MIN_MEANINGFUL_CONTENT_BYTES
}

async fn enqueue_encoded_frame(
    writer: &debug_transport::HeadlessDebugWriter,
    frame: &[u8],
) -> Result<()> {
    if frame.len() < HEADER_LEN {
        return Err(anyhow!("encoded IPC frame shorter than header"));
    }
    let payload_len = u32::from_le_bytes([frame[4], frame[5], frame[6], frame[7]]) as usize;
    if frame.len() != HEADER_LEN + payload_len {
        return Err(anyhow!(
            "encoded IPC frame len {} does not match payload_len {}",
            frame.len(),
            payload_len
        ));
    }
    let sent = writer
        .send(debug_transport::OutFrame {
            channel: frame[0],
            flags: frame[1],
            payload: frame[HEADER_LEN..].to_vec(),
        })
        .await;
    if sent {
        Ok(())
    } else {
        Err(anyhow!("headless IPC writer is not connected"))
    }
}

// TapGestureRecognizer 的默认上限为 500ms；保留明显余量，避免 QEMU 调度抖动
// 将固定的 down/up 注入误判为超时长按。
const TOUCH_CONTACT_HOLD_MS: u64 = 250;

async fn write_touch_tap(
    writer: &debug_transport::HeadlessDebugWriter,
    x: u16,
    y: u16,
) -> Result<()> {
    // Match apps/simulator/src-tauri/src/ipc.rs::encode_touch_event and inject a
    // tap near the portrait logical center after the first framebuffer proves
    // the IPC link is live. Coordinates are firmware logical 480x800 UI
    // coordinates, not the native 800x480 framebuffer coordinates.
    let down = encode_touch_frame(0x01, x, y, 0);
    let up = encode_touch_frame(0x03, x, y, 0);
    enqueue_encoded_frame(writer, &down).await?;
    // LilyGo 的 GT911 由同步 UI 循环轮询；保持按下足够久，确保一次忙碌的
    // 首帧渲染不会吞掉完整的按下/抬起对。
    time::sleep(Duration::from_millis(TOUCH_CONTACT_HOLD_MS)).await;
    enqueue_encoded_frame(writer, &up).await?;
    eprintln!("[headless] injected touch tap action=down/up x={x} y={y}");
    Ok(())
}

async fn write_touch_swipe(
    writer: &debug_transport::HeadlessDebugWriter,
    x: u16,
    y: u16,
    direction: TouchGestureDirection,
    geometry: &BoardGeometry,
) -> Result<()> {
    const SWIPE_STEPS: i32 = 3;
    let ui_width = geometry.output_width as i32;
    let ui_height = geometry.output_height as i32;
    let swipe_distance = (ui_width.min(ui_height) as f32 * 0.45).round() as i32;

    let (dx, dy, label) = match direction {
        TouchGestureDirection::Up => (0, -swipe_distance, "up"),
        TouchGestureDirection::Down => (0, swipe_distance, "down"),
        TouchGestureDirection::Left => (-swipe_distance, 0, "left"),
        TouchGestureDirection::Right => (swipe_distance, 0, "right"),
    };

    let clamp_coord =
        |value: i32, max_exclusive: i32| -> u16 { value.clamp(0, max_exclusive - 1) as u16 };

    let start_x = i32::from(x);
    let start_y = i32::from(y);
    let end_x = clamp_coord(start_x + dx, ui_width);
    let end_y = clamp_coord(start_y + dy, ui_height);

    let down = encode_touch_frame(0x01, x, y, 0);
    enqueue_encoded_frame(writer, &down).await?;
    // 先让轮询式触控驱动观察到起点；否则 GT911 可能只看见第一帧移动，
    // 导致手势被误判为列表项目点击。
    time::sleep(Duration::from_millis(TOUCH_CONTACT_HOLD_MS)).await;

    for step in 1..=SWIPE_STEPS {
        let progress = step as f32 / SWIPE_STEPS as f32;
        let move_x = clamp_coord(
            (start_x as f32 + dx as f32 * progress).round() as i32,
            ui_width,
        );
        let move_y = clamp_coord(
            (start_y as f32 + dy as f32 * progress).round() as i32,
            ui_height,
        );
        let move_frame = encode_touch_frame(0x02, move_x, move_y, 0);
        time::sleep(Duration::from_millis(50)).await;
        enqueue_encoded_frame(writer, &move_frame).await?;
    }

    let up = encode_touch_frame(0x03, end_x, end_y, 0);
    time::sleep(Duration::from_millis(50)).await;
    enqueue_encoded_frame(writer, &up).await?;
    eprintln!("[headless] injected touch swipe {label} start=({x},{y}) end=({end_x},{end_y})");
    Ok(())
}

fn encode_touch_frame(action: u8, x: u16, y: u16, finger_id: u8) -> [u8; HEADER_LEN + 6] {
    let mut frame = [0u8; HEADER_LEN + 6];
    frame[0] = CHANNEL_TOUCH_EVENT;
    frame[4..8].copy_from_slice(&(6u32).to_le_bytes());
    frame[8] = action;
    frame[9..11].copy_from_slice(&x.to_le_bytes());
    frame[11..13].copy_from_slice(&y.to_le_bytes());
    frame[13] = finger_id;
    frame
}

async fn write_synthetic_touch_event(
    writer: &debug_transport::HeadlessDebugWriter,
    touch_type: SyntheticTouchEventType,
    x: u16,
    y: u16,
) -> Result<()> {
    let frame = encode_synthetic_touch_event_frame(touch_type, x, y);
    enqueue_encoded_frame(writer, &frame).await?;
    eprintln!(
        "[headless] injected synthetic touch event {} x={x} y={y}",
        touch_type.label()
    );
    Ok(())
}

fn encode_synthetic_touch_event_frame(
    touch_type: SyntheticTouchEventType,
    x: u16,
    y: u16,
) -> [u8; HEADER_LEN + 5] {
    let mut frame = [0u8; HEADER_LEN + 5];
    frame[0] = CHANNEL_SYNTHETIC_TOUCH_EVENT;
    frame[4..8].copy_from_slice(&(5u32).to_le_bytes());
    frame[8] = touch_type.code();
    frame[9..11].copy_from_slice(&x.to_le_bytes());
    frame[11..13].copy_from_slice(&y.to_le_bytes());
    frame
}

async fn write_button_click(
    writer: &debug_transport::HeadlessDebugWriter,
    button_id: u8,
    hold_ms: u64,
) -> Result<()> {
    let down = encode_button_frame(button_id, true);
    let up = encode_button_frame(button_id, false);
    if hold_ms == 0 {
        enqueue_encoded_frame(writer, &down).await?;
        enqueue_encoded_frame(writer, &up).await?;
        eprintln!("[headless] injected button tap button_id={button_id}");
        return Ok(());
    }
    enqueue_encoded_frame(writer, &down).await?;
    time::sleep(Duration::from_millis(hold_ms)).await;
    enqueue_encoded_frame(writer, &up).await?;
    eprintln!("[headless] injected button click button_id={button_id} hold_ms={hold_ms}");
    Ok(())
}

fn encode_button_frame(button_id: u8, pressed: bool) -> [u8; HEADER_LEN + 2] {
    let mut frame = [0u8; HEADER_LEN + 2];
    frame[0] = CHANNEL_BUTTON_EVENT;
    frame[4..8].copy_from_slice(&(2u32).to_le_bytes());
    frame[8] = button_id;
    frame[9] = if pressed { 1 } else { 0 };
    frame
}

fn framebuffer_gray_value(fb: &[u8], geometry: &BoardGeometry, source_x: u32, source_y: u32) -> u8 {
    let pixel = source_y * geometry.framebuffer_width + source_x;
    match geometry.framebuffer_format {
        FramebufferFormat::Mono1 => {
            let byte_idx = (pixel / 8) as usize;
            let bit = pixel % 8;
            if byte_idx < fb.len() && (fb[byte_idx] >> (7 - bit)) & 1 != 0 {
                255
            } else {
                0
            }
        }
        FramebufferFormat::Gray16 => {
            let byte_idx = (pixel / 2) as usize;
            if byte_idx >= fb.len() {
                return 255;
            }
            let level = if pixel % 2 == 0 {
                fb[byte_idx] >> 4
            } else {
                fb[byte_idx] & 0x0F
            };
            255 - level * 17
        }
    }
}

/// Write the native framebuffer as the board's logical output PNG.
fn write_png(path: &Path, fb: &[u8], geometry: &BoardGeometry) -> Result<()> {
    let mut img: GrayImage = ImageBuffer::new(geometry.output_width, geometry.output_height);
    for output_y in 0..geometry.output_height {
        for output_x in 0..geometry.output_width {
            let (source_x, source_y) = geometry
                .source_pixel_for_output(output_x, output_y)
                .ok_or_else(|| {
                    anyhow!(
                        "unsupported board geometry raw={}x{} output={}x{}",
                        geometry.raw_width,
                        geometry.raw_height,
                        geometry.output_width,
                        geometry.output_height
                    )
                })?;
            let value = framebuffer_gray_value(fb, geometry, source_x, source_y);
            img.put_pixel(output_x, output_y, Luma([value]));
        }
    }
    img.save(path)?;
    Ok(())
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct ScreenshotRegion {
    x: u32,
    y: u32,
    width: u32,
    height: u32,
}

impl ScreenshotRegion {
    fn pixels(self) -> u32 {
        self.width * self.height
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct ScreenshotRegionStats {
    total_pixels: u32,
    dark_pixels: u32,
    white_pixels: u32,
}

impl ScreenshotRegionStats {
    fn dark_ratio(self) -> f64 {
        self.dark_pixels as f64 / self.total_pixels as f64
    }

    fn white_ratio(self) -> f64 {
        self.white_pixels as f64 / self.total_pixels as f64
    }
}

fn framebuffer_pixel_is_dark(fb: &[u8], geometry: &BoardGeometry, x: u32, y: u32) -> Result<bool> {
    if fb.len() != geometry.fb_bytes() {
        return Err(anyhow!(
            "framebuffer length {} does not match expected {}",
            fb.len(),
            geometry.fb_bytes()
        ));
    }
    if x >= geometry.output_width || y >= geometry.output_height {
        return Err(anyhow!(
            "screenshot pixel x={} y={} is outside {}x{}",
            x,
            y,
            geometry.output_width,
            geometry.output_height
        ));
    }

    let (source_x, source_y) = geometry.source_pixel_for_output(x, y).ok_or_else(|| {
        anyhow!(
            "unsupported board geometry raw={}x{} output={}x{}",
            geometry.raw_width,
            geometry.raw_height,
            geometry.output_width,
            geometry.output_height
        )
    })?;
    Ok(framebuffer_gray_value(fb, geometry, source_x, source_y) < 128)
}

fn framebuffer_region_stats(
    fb: &[u8],
    geometry: &BoardGeometry,
    x: u32,
    y: u32,
    width: u32,
    height: u32,
) -> Result<ScreenshotRegionStats> {
    if width == 0 || height == 0 {
        return Err(anyhow!(
            "screenshot region x={} y={} width={} height={} must have positive dimensions",
            x,
            y,
            width,
            height
        ));
    }
    let x_end = x
        .checked_add(width)
        .ok_or_else(|| anyhow!("screenshot region x={} width={} overflows", x, width))?;
    let y_end = y
        .checked_add(height)
        .ok_or_else(|| anyhow!("screenshot region y={} height={} overflows", y, height))?;
    if x_end > geometry.output_width || y_end > geometry.output_height {
        return Err(anyhow!(
            "screenshot region x={} y={} width={} height={} exceeds {}x{}",
            x,
            y,
            width,
            height,
            geometry.output_width,
            geometry.output_height
        ));
    }

    let mut dark_pixels = 0u32;
    for sample_y in y..y_end {
        for sample_x in x..x_end {
            if framebuffer_pixel_is_dark(fb, geometry, sample_x, sample_y)? {
                dark_pixels += 1;
            }
        }
    }

    let total_pixels = width * height;
    Ok(ScreenshotRegionStats {
        total_pixels,
        dark_pixels,
        white_pixels: total_pixels - dark_pixels,
    })
}

fn validate_ratio_threshold(label: &str, name: &str, value: f64) -> Result<()> {
    if !value.is_finite() || !(0.0..=1.0).contains(&value) {
        return Err(anyhow!(
            "assert_screenshot_region '{}' has invalid {}={} (expected finite 0.0..=1.0)",
            label,
            name,
            value
        ));
    }
    Ok(())
}

fn assert_screenshot_region(
    step: &CaseStep,
    fb: &[u8],
    geometry: &BoardGeometry,
    reference_viewport: ReferenceViewport,
) -> Result<String> {
    let label = step
        .label
        .as_deref()
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .unwrap_or("screenshot_region");
    let x = step.x as u32;
    let y = step.y as u32;
    let width = step.width as u32;
    let height = step.height as u32;
    let region =
        geometry.map_reference_screenshot_region(reference_viewport, x, y, width, height)?;
    let reference_region = ScreenshotRegion {
        x,
        y,
        width,
        height,
    };

    let has_threshold = step.min_dark_ratio.is_some()
        || step.max_dark_ratio.is_some()
        || step.min_white_ratio.is_some()
        || step.max_white_ratio.is_some()
        || step.min_dark_pixels.is_some()
        || step.max_dark_pixels.is_some()
        || step.min_white_pixels.is_some()
        || step.max_white_pixels.is_some();
    if !has_threshold {
        return Err(anyhow!(
            "assert_screenshot_region '{}' requires at least one ratio or pixel threshold",
            label
        ));
    }

    let stats = framebuffer_region_stats(
        fb,
        geometry,
        region.x,
        region.y,
        region.width,
        region.height,
    )?;
    let dark_ratio = stats.dark_ratio();
    let white_ratio = stats.white_ratio();
    let region_summary = if region == reference_region {
        format!(
            "x={} y={} width={} height={}",
            region.x, region.y, region.width, region.height
        )
    } else {
        format!(
            "ref=({},{} {}x{}) mapped=({},{} {}x{})",
            reference_region.x,
            reference_region.y,
            reference_region.width,
            reference_region.height,
            region.x,
            region.y,
            region.width,
            region.height
        )
    };
    let summary = format!(
        "'{}' {} dark={}/{} ({:.4}) white={}/{} ({:.4})",
        label,
        region_summary,
        stats.dark_pixels,
        stats.total_pixels,
        dark_ratio,
        stats.white_pixels,
        stats.total_pixels,
        white_ratio
    );

    if let Some(min) = step.min_dark_ratio {
        validate_ratio_threshold(label, "minDarkRatio", min)?;
        if dark_ratio < min {
            return Err(anyhow!(
                "{} dark ratio {:.4} below min {:.4}",
                summary,
                dark_ratio,
                min
            ));
        }
    }
    if let Some(max) = step.max_dark_ratio {
        validate_ratio_threshold(label, "maxDarkRatio", max)?;
        if dark_ratio > max {
            return Err(anyhow!(
                "{} dark ratio {:.4} above max {:.4}",
                summary,
                dark_ratio,
                max
            ));
        }
    }
    if let Some(min) = step.min_white_ratio {
        validate_ratio_threshold(label, "minWhiteRatio", min)?;
        if white_ratio < min {
            return Err(anyhow!(
                "{} white ratio {:.4} below min {:.4}",
                summary,
                white_ratio,
                min
            ));
        }
    }
    if let Some(max) = step.max_white_ratio {
        validate_ratio_threshold(label, "maxWhiteRatio", max)?;
        if white_ratio > max {
            return Err(anyhow!(
                "{} white ratio {:.4} above max {:.4}",
                summary,
                white_ratio,
                max
            ));
        }
    }
    if let Some(min) = step.min_dark_pixels {
        let min = scale_reference_pixel_threshold(min, reference_region.pixels(), region.pixels());
        if stats.dark_pixels < min {
            return Err(anyhow!(
                "{} dark pixels {} below min {}",
                summary,
                stats.dark_pixels,
                min
            ));
        }
    }
    if let Some(max) = step.max_dark_pixels {
        let max = scale_reference_pixel_threshold(max, reference_region.pixels(), region.pixels());
        if stats.dark_pixels > max {
            return Err(anyhow!(
                "{} dark pixels {} above max {}",
                summary,
                stats.dark_pixels,
                max
            ));
        }
    }
    if let Some(min) = step.min_white_pixels {
        let min = scale_reference_pixel_threshold(min, reference_region.pixels(), region.pixels());
        if stats.white_pixels < min {
            return Err(anyhow!(
                "{} white pixels {} below min {}",
                summary,
                stats.white_pixels,
                min
            ));
        }
    }
    if let Some(max) = step.max_white_pixels {
        let max = scale_reference_pixel_threshold(max, reference_region.pixels(), region.pixels());
        if stats.white_pixels > max {
            return Err(anyhow!(
                "{} white pixels {} above max {}",
                summary,
                stats.white_pixels,
                max
            ));
        }
    }

    Ok(summary)
}

fn safe_checkpoint_label(label: Option<&str>) -> String {
    let raw = label
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .unwrap_or("screenshot_region");
    let safe = safe_artifact_name(raw);
    let trimmed = safe.trim_matches(|ch| ch == '.' || ch == '-' || ch == '_');
    if trimmed.is_empty() {
        "screenshot_region".to_string()
    } else {
        trimmed.to_string()
    }
}

fn assertion_checkpoint_path(
    artifacts_dir: &Path,
    step_index: usize,
    label: Option<&str>,
) -> PathBuf {
    artifacts_dir.join("checkpoints").join(format!(
        "{:03}-{}.png",
        step_index + 1,
        safe_checkpoint_label(label)
    ))
}

fn assert_screenshot_region_with_checkpoint(
    step: &CaseStep,
    fb: &[u8],
    geometry: &BoardGeometry,
    reference_viewport: ReferenceViewport,
    capture_enabled: bool,
    artifacts_dir: &Path,
    step_index: usize,
) -> Result<String> {
    let summary = assert_screenshot_region(step, fb, geometry, reference_viewport)?;
    if capture_enabled {
        let path = assertion_checkpoint_path(artifacts_dir, step_index, step.label.as_deref());
        let parent = path
            .parent()
            .ok_or_else(|| anyhow!("assertion checkpoint has no parent: {}", path.display()))?;
        fs::create_dir_all(parent)
            .with_context(|| format!("creating assertion checkpoints dir {}", parent.display()))?;
        write_png(&path, fb, geometry)
            .with_context(|| format!("writing assertion checkpoint {}", path.display()))?;
        eprintln!("[e2e] assertion checkpoint written: {}", path.display());
    }
    Ok(summary)
}

fn safe_repo_copy_source(raw_path: &str) -> Result<PathBuf> {
    let trimmed = raw_path.trim();
    if trimmed.is_empty() {
        return Err(anyhow!("source must name a repository file"));
    }

    let path = Path::new(trimmed);
    if path.is_absolute() {
        return Err(anyhow!("source '{}' must be repository-relative", trimmed));
    }

    let mut relative = PathBuf::new();
    for component in path.components() {
        match component {
            Component::Normal(part) => relative.push(part),
            Component::CurDir
            | Component::ParentDir
            | Component::RootDir
            | Component::Prefix(_) => {
                return Err(anyhow!(
                    "source '{}' must stay inside the repository root",
                    trimmed
                ));
            }
        }
    }
    if relative.as_os_str().is_empty() {
        return Err(anyhow!("source must name a repository file"));
    }
    Ok(relative)
}

fn safe_sd_boot_destination(raw_path: &str) -> Result<PathBuf> {
    let trimmed = raw_path.trim();
    let relative = trimmed
        .strip_prefix('/')
        .ok_or_else(|| anyhow!("SD destination '{}' must be rooted at '/'", trimmed))?;
    safe_sd_relative_path(relative)
        .with_context(|| format!("SD destination '{}' must stay inside the SD root", trimmed))
}

fn safe_sd_relative_path(raw_path: &str) -> Result<PathBuf> {
    let trimmed = raw_path.trim();
    if trimmed.is_empty() {
        return Err(anyhow!("SD file assertion requires non-empty path"));
    }

    let path = Path::new(trimmed);
    if path.is_absolute() {
        return Err(anyhow!(
            "SD file assertion path '{}' must be relative",
            trimmed
        ));
    }

    let mut relative = PathBuf::new();
    for component in path.components() {
        match component {
            Component::Normal(part) => relative.push(part),
            Component::CurDir => {}
            Component::ParentDir | Component::RootDir | Component::Prefix(_) => {
                return Err(anyhow!(
                    "SD file assertion path '{}' must stay inside the SD root",
                    trimmed
                ));
            }
        }
    }

    if relative.as_os_str().is_empty() {
        return Err(anyhow!("SD file assertion requires non-empty path"));
    }
    Ok(relative)
}

fn assert_sd_file_exists(
    sd_root: &Path,
    relative_path: &Path,
    display_path: &str,
) -> Result<String> {
    let full_path = sd_root.join(relative_path);
    if !full_path.is_file() {
        return Err(anyhow!(
            "assert_sd_file_exists '{}' failed: {} is not a file",
            display_path,
            full_path.display()
        ));
    }
    Ok(format!(
        "'{}' exists at {}",
        display_path,
        full_path.display()
    ))
}

fn assert_sd_file_contains(
    sd_root: &Path,
    relative_path: &Path,
    display_path: &str,
    expected_text: &str,
) -> Result<String> {
    let full_path = sd_root.join(relative_path);
    let content = fs::read_to_string(&full_path).map_err(|e| {
        anyhow!(
            "assert_sd_file_contains '{}' failed: {} could not be read as UTF-8 text: {}",
            display_path,
            full_path.display(),
            e
        )
    })?;
    if !content.contains(expected_text) {
        return Err(anyhow!(
            "assert_sd_file_contains '{}' failed: {} did not contain {:?}",
            display_path,
            full_path.display(),
            expected_text
        ));
    }
    Ok(format!(
        "'{}' contains {:?} at {}",
        display_path,
        expected_text,
        full_path.display()
    ))
}

// ── E2E case execution ──────────────────────────────────────────────────────

impl E2EContext {
    fn new() -> Self {
        Self {
            transcript: Vec::new(),
            current_activity: None,
            activity_events: Vec::new(),
            pending_input_activity_after: None,
            pending_input_log_after: None,
        }
    }

    fn push_line(&mut self, line: &str) {
        self.transcript.push(line.to_string());
    }

    fn record_activity_event(&mut self, name: impl Into<String>) {
        let name = name.into();
        self.activity_events.push((StdInstant::now(), name.clone()));
        self.current_activity = Some(name);
    }

    fn mark_input_activity_boundary(&mut self) {
        self.pending_input_activity_after = Some(self.activity_events.len());
        self.pending_input_log_after = Some(self.transcript.len());
    }

    fn take_pending_input_activity_boundary(&mut self) -> Option<usize> {
        self.pending_input_activity_after.take()
    }

    fn take_pending_input_log_boundary(&mut self) -> Option<usize> {
        self.pending_input_log_after.take()
    }

    fn parse_activity_transition(&mut self, line: &str) {
        if let Some(start) = line.find("Entering activity: ") {
            let rest = &line[start + "Entering activity: ".len()..];
            if let Some(end) = rest.find(" (lang=") {
                let name = rest[..end].to_string();
                self.record_activity_event(name);
            }
        } else if let Some(start) = line.find("Exiting activity: ") {
            let rest = &line[start + "Exiting activity: ".len()..];
            if let Some(end) = rest.find(" (lang=") {
                self.activity_events
                    .push((StdInstant::now(), format!("Exit:{}", &rest[..end])));
            }
        } else if line.contains("[MOFEI-TRACE]") && line.contains("Dashboard::render") {
            self.record_activity_event("Dashboard");
        } else if line.contains("[MOFEI-TRACE]") && line.contains("Settings::render") {
            self.record_activity_event("Settings");
        } else if let Some(activity) = ui_refresh_activity_name(line) {
            self.record_activity_event(activity);
        } else if line.contains("[MOFEI-TRACE]") {
            for (trace_label, activity_name) in [
                ("CalendarActivity::render", "Calendar"),
                ("WeatherClockActivity::render", "WeatherClock"),
                ("ArcadeHubActivity::render", "ArcadeHub"),
                ("Game2048Activity::render", "Game2048"),
                ("RecentBooksActivity::render", "RecentBooks"),
                ("ButtonRemapActivity::render", "ButtonRemap"),
                ("DeviceDiagnosticsActivity::render", "DeviceDiagnostics"),
                ("StatusBarSettingsActivity::render", "StatusBarSettings"),
                ("ReadingHubActivity::render", "Reading"),
                ("StudyHubActivity::render", "Study"),
                ("StudyCardsTodayActivity::render", "StudyCardsToday"),
                ("StudyLaterActivity::render", "StudyLater"),
                ("StudyQuizActivity::render", "StudyQuiz"),
                ("StudyRecoveryActivity::render", "StudyRecovery"),
                ("SavedCardsActivity::render", "SavedCards"),
                ("LearningReportActivity::render", "LearningReport"),
                ("ReviewQueueActivity::render", "ReviewQueue"),
                ("DeckImportStatusActivity::render", "DeckImportStatus"),
                ("FileBrowserActivity::render", "FileBrowser"),
                ("AppletsActivity::render", "Applets"),
                ("LuaAppActivity::render", "LuaApp"),
                ("OpdsServerListActivity::render", "OpdsServerList"),
                ("OpdsBookBrowserActivity::render", "OpdsBookBrowser"),
                ("OpdsSettingsActivity::render", "OpdsSettings"),
                ("DictionaryActivity::render", "Dictionary"),
                ("TtfFontSelectActivity::render", "TtfFontSelect"),
                ("TimeZoneSelectActivity::render", "TimeZoneSelect"),
                (
                    "TraditionalChineseFontsActivity::render",
                    "TraditionalChineseFonts",
                ),
                ("LanguageSelectActivity::render", "LanguageSelect"),
                ("SleepWallpaperActivity::render", "SleepWallpaper"),
                ("KeyboardEntryActivity::render", "KeyboardEntry"),
                (
                    "ReaderFrontlightSelectionActivity::render",
                    "ReaderFrontlightSelection",
                ),
                (
                    "EpubReaderChapterSelectionActivity::render",
                    "EpubReaderChapterSelection",
                ),
                (
                    "EpubReaderPercentSelectionActivity::render",
                    "EpubReaderPercentSelection",
                ),
                ("EpubSearchResultsActivity::render", "EpubSearchResults"),
                ("TxtSearchResultsActivity::render", "TxtSearchResults"),
                ("TxtBookmarksActivity::render", "TxtBookmarks"),
                ("EpubBookmarksActivity::render", "EpubBookmarks"),
                ("EpubReaderFootnotesActivity::render", "EpubReaderFootnotes"),
                ("EpubReaderActivity::render", "EpubReader"),
                ("TxtReaderActivity::render", "TxtReader"),
                ("XtcReaderActivity::render", "XtcReader"),
                ("ReaderActivity::render", "Reader"),
            ] {
                if line.contains(trace_label) {
                    self.record_activity_event(activity_name);
                    break;
                }
            }
        }
    }
}

fn ui_refresh_activity_name(line: &str) -> Option<String> {
    let marker = "UIREFRESH ";
    let activity_marker = "activity=";
    let start = line.find(marker)?;
    let rest = &line[start + marker.len()..];
    let activity_start = rest.find(activity_marker)? + activity_marker.len();
    let activity_rest = &rest[activity_start..];
    let activity_end = activity_rest
        .find(|ch: char| ch.is_whitespace() || ch == ':')
        .unwrap_or(activity_rest.len());
    let activity = activity_rest[..activity_end].trim();
    if activity.is_empty() {
        return None;
    }
    Some(activity.to_string())
}

fn load_e2e_case(case_path: &Path) -> Result<E2ECase> {
    let raw = std::fs::read_to_string(case_path)
        .with_context(|| format!("reading case file {}", case_path.display()))?;
    let case: E2ECase = serde_json::from_str(&raw)
        .with_context(|| format!("parsing case file {}", case_path.display()))?;
    if case.steps.is_empty() {
        return Err(anyhow!("case file has no steps"));
    }
    validate_boot_copy_files(&case)?;
    validate_before_boot_steps(&case)?;
    Ok(case)
}

fn validate_boot_copy_files(case: &E2ECase) -> Result<()> {
    let mut destinations = HashSet::new();
    for (index, file) in case.boot.copy_files.iter().enumerate() {
        let destination = safe_sd_boot_destination(&file.path)
            .with_context(|| format!("boot.copyFiles entry {} has invalid path", index + 1))?;
        safe_repo_copy_source(&file.source)
            .with_context(|| format!("boot.copyFiles entry {} has invalid source", index + 1))?;
        if !destinations.insert(destination) {
            return Err(anyhow!(
                "boot.copyFiles contains duplicate destination '{}'",
                file.path
            ));
        }
    }
    Ok(())
}

fn validate_before_boot_steps(case: &E2ECase) -> Result<()> {
    for (index, step) in case.before_boot.iter().enumerate() {
        if step.step_type != "peripheral_control" {
            return Err(anyhow!(
                "beforeBoot step {} must use type peripheral_control, got '{}'",
                index + 1,
                step.step_type
            ));
        }
    }
    Ok(())
}

fn load_e2e_case_dir(case_dir: &Path) -> Result<Vec<(PathBuf, E2ECase)>> {
    if !case_dir.exists() {
        return Err(anyhow!(
            "E2E case directory not found: {}",
            case_dir.display()
        ));
    }
    if !case_dir.is_dir() {
        return Err(anyhow!(
            "E2E case path is not a directory: {}",
            case_dir.display()
        ));
    }

    let mut paths = Vec::new();
    collect_e2e_case_paths(case_dir, &mut paths)?;
    paths.sort();

    if paths.is_empty() {
        return Err(anyhow!(
            "E2E case directory has no .json files: {}",
            case_dir.display()
        ));
    }

    paths
        .into_iter()
        .map(|path| {
            let case = load_e2e_case(&path)?;
            Ok((path, case))
        })
        .collect()
}

fn collect_e2e_case_paths(dir: &Path, paths: &mut Vec<PathBuf>) -> Result<()> {
    for entry in
        fs::read_dir(dir).with_context(|| format!("reading case directory {}", dir.display()))?
    {
        let entry = entry?;
        let file_type = entry
            .file_type()
            .with_context(|| format!("reading file type for {}", entry.path().display()))?;
        let path = entry.path();
        if file_type.is_dir() {
            collect_e2e_case_paths(&path, paths)?;
        } else if file_type.is_file()
            && path.extension().and_then(|value| value.to_str()) == Some("json")
        {
            paths.push(path);
        }
    }
    Ok(())
}

fn safe_artifact_name(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    for ch in value.chars() {
        if ch.is_ascii_alphanumeric() || ch == '-' || ch == '_' || ch == '.' {
            out.push(ch);
        } else {
            out.push('-');
        }
    }
    let out = out.trim_matches('-');
    if out.is_empty() {
        "case".to_string()
    } else {
        out.to_string()
    }
}

fn case_artifact_name(case_path: &Path, case: &E2ECase) -> String {
    if !case.id.trim().is_empty() {
        safe_artifact_name(&case.id)
    } else {
        let stem = case_path
            .file_stem()
            .and_then(|value| value.to_str())
            .unwrap_or("case");
        safe_artifact_name(stem)
    }
}

fn path_with_suffix(path: &Path, suffix: &str) -> PathBuf {
    let parent = path.parent().unwrap_or_else(|| Path::new(""));
    let stem = path
        .file_stem()
        .and_then(|value| value.to_str())
        .unwrap_or("mofei-sim-headless");
    let extension = path.extension().and_then(|value| value.to_str());
    let file_name = socket_file_name(stem, suffix, extension);
    let candidate = parent.join(&file_name);
    if socket_path_fits_unix_limit(&candidate)
        && socket_path_fits_unix_limit(&candidate.with_extension("monitor.sock"))
    {
        return candidate;
    }

    let safe_stem = safe_artifact_name(stem);
    let safe_suffix = safe_artifact_name(suffix);
    let hash = stable_socket_suffix_hash(&safe_suffix);
    for suffix_len in (0..=safe_suffix.len()).rev() {
        let suffix_prefix = safe_suffix[..suffix_len].trim_matches('-');
        let compact_suffix = if suffix_prefix.is_empty() {
            hash.clone()
        } else {
            format!("{suffix_prefix}-{hash}")
        };
        let compact = parent.join(socket_file_name(&safe_stem, &compact_suffix, extension));
        if socket_path_fits_unix_limit(&compact)
            && socket_path_fits_unix_limit(&compact.with_extension("monitor.sock"))
        {
            return compact;
        }
    }

    parent.join(socket_file_name(&safe_stem, &hash, extension))
}

fn socket_file_name(stem: &str, suffix: &str, extension: Option<&str>) -> String {
    if let Some(extension) = extension {
        format!("{stem}-{suffix}.{extension}")
    } else {
        format!("{stem}-{suffix}")
    }
}

fn socket_path_fits_unix_limit(path: &Path) -> bool {
    path.as_os_str().as_bytes().len() < UNIX_SOCKET_PATH_SAFE_BYTES
}

fn stable_socket_suffix_hash(value: &str) -> String {
    let mut hash = 0xcbf29ce484222325u64;
    for byte in value.as_bytes() {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    format!("{hash:016x}")[..SOCKET_SUFFIX_HASH_BYTES].to_string()
}

fn simulator_button_id(board: &BoardRuntime, name: &str) -> Result<u8> {
    let normalized = name.trim().to_ascii_lowercase();
    board
        .button_ids_by_name
        .get(&normalized)
        .copied()
        .ok_or_else(|| {
            let mut supported = board
                .button_ids_by_name
                .keys()
                .cloned()
                .collect::<Vec<String>>();
            supported.sort();
            anyhow!(
                "unsupported simulator button '{}' for board '{}' (supported: {})",
                name,
                board.id,
                supported.join(", ")
            )
        })
}

fn synthetic_touch_for_direction_button(name: &str) -> Option<SyntheticTouchEventType> {
    match name.trim().to_ascii_lowercase().as_str() {
        "up" => Some(SyntheticTouchEventType::SwipeDown),
        "down" => Some(SyntheticTouchEventType::SwipeUp),
        "left" => Some(SyntheticTouchEventType::SwipeRight),
        "right" => Some(SyntheticTouchEventType::SwipeLeft),
        _ => None,
    }
}

fn simulator_button_injection(board: &BoardRuntime, name: &str) -> Result<ButtonInjection> {
    simulator_button_id(board, name).map(|button_id| ButtonInjection::ButtonClick { button_id })
}

fn simulator_nav_injection(board: &BoardRuntime, direction: &str) -> Result<ButtonInjection> {
    match simulator_button_id(board, direction) {
        Ok(button_id) => Ok(ButtonInjection::ButtonClick { button_id }),
        Err(button_error) => synthetic_touch_for_direction_button(direction)
            .map(|touch_type| ButtonInjection::SyntheticTouch { touch_type })
            .ok_or(button_error),
    }
}

fn missing_case_capabilities(case: &E2ECase, board: &BoardRuntime) -> Vec<String> {
    let mut required = case
        .required_capabilities
        .iter()
        .map(|value| value.trim().to_ascii_lowercase())
        .filter(|value| !value.is_empty())
        .collect::<std::collections::HashSet<_>>();
    for step in &case.steps {
        match step.step_type.as_str() {
            "button" if !step.button.trim().is_empty() => {
                required.insert(format!(
                    "button.{}",
                    step.button.trim().to_ascii_lowercase()
                ));
            }
            "touch" | "touch_event" | "nav" => {
                required.insert("input.touch".to_string());
            }
            _ => {}
        }
    }
    let mut missing = required
        .difference(&board.capabilities)
        .cloned()
        .collect::<Vec<_>>();
    missing.sort();
    missing
}

/// True when the environment declares the firmware under test was built without
/// the serial Debug Console. Production release artifacts append
/// `sdkconfig.release`, which omits `CONFIG_PANDA_DEBUG_CONSOLE`, so the
/// `serial_command` step can never receive a reply there.
///
/// `PANDA_` is the current name exported by `release-beta.sh`. The `MURPHY_`
/// spelling is still accepted so an older release script or an already-exported
/// shell environment keeps working; without that fallback a stale caller would
/// silently stop skipping and the cases would time out as if Study were broken.
fn debug_console_unavailable() -> bool {
    [
        "PANDA_SIMULATOR_E2E_DEBUG_CONSOLE",
        "MURPHY_SIMULATOR_E2E_DEBUG_CONSOLE",
    ]
    .iter()
    .any(|key| matches!(std::env::var(key).ok().as_deref().map(str::trim), Some("0")))
}

fn case_skip_reason(case: &E2ECase, board: &BoardRuntime) -> Option<String> {
    if let Some(reason) = case
        .skip_reason
        .as_deref()
        .map(str::trim)
        .filter(|value| !value.is_empty())
    {
        return Some(reason.to_string());
    }
    // A case that drives the firmware over the Debug Console cannot run against a
    // build that omits it. Skip explicitly rather than waiting for a reply that
    // can never arrive (the symptom is an assert_log_contains timeout, which reads
    // as a product defect).
    if debug_console_unavailable()
        && case
            .steps
            .iter()
            .chain(case.before_boot.iter())
            .any(|step| step.step_type == "serial_command")
    {
        return Some(
            "requires the serial Debug Console, which production release builds omit \
             (sdkconfig.release drops CONFIG_PANDA_DEBUG_CONSOLE)"
                .to_string(),
        );
    }
    let missing = missing_case_capabilities(case, board);
    (!missing.is_empty()).then(|| format!("missing board capabilities: {}", missing.join(", ")))
}

fn board_center_touch(geometry: &BoardGeometry) -> TouchTap {
    TouchTap {
        x: (geometry.output_width / 2).min(u32::from(u16::MAX)) as u16,
        y: (geometry.output_height / 2).min(u32::from(u16::MAX)) as u16,
    }
}

fn synthetic_touch_event_type(event: &str) -> Result<SyntheticTouchEventType> {
    match event.trim().to_ascii_lowercase().as_str() {
        "" | "tap" => Ok(SyntheticTouchEventType::Tap),
        "swipe_up" => Ok(SyntheticTouchEventType::SwipeUp),
        "swipe_down" => Ok(SyntheticTouchEventType::SwipeDown),
        "swipe_left" => Ok(SyntheticTouchEventType::SwipeLeft),
        "swipe_right" => Ok(SyntheticTouchEventType::SwipeRight),
        "long_press" => Ok(SyntheticTouchEventType::LongPress),
        other => Err(anyhow!(
            "unsupported touch_event '{other}' (supported: tap, swipe_up, swipe_down, swipe_left, swipe_right, long_press)"
        )),
    }
}

fn serial_command_line(text: &str) -> Result<Vec<u8>> {
    let command = text.trim();
    if command.is_empty() {
        return Err(anyhow!("serial_command step requires non-empty text"));
    }
    if command.contains(['\r', '\n']) {
        return Err(anyhow!("serial_command text must contain exactly one line"));
    }
    if command.len() > MAX_SERIAL_COMMAND_BYTES {
        return Err(anyhow!(
            "serial_command text exceeds {MAX_SERIAL_COMMAND_BYTES} bytes"
        ));
    }
    let mut line = Vec::with_capacity(command.len() + 1);
    line.extend_from_slice(command.as_bytes());
    line.push(b'\n');
    Ok(line)
}

fn decode_bounded_hex(value: &str, max_bytes: usize, label: &str) -> Result<Vec<u8>> {
    let value = value.trim();
    if value.is_empty() || value.len() % 2 != 0 {
        return Err(anyhow!(
            "{label} must contain a non-empty even number of hex digits"
        ));
    }
    if value.len() / 2 > max_bytes {
        return Err(anyhow!(
            "{label} exceeds the {max_bytes}-byte simulator limit"
        ));
    }
    let mut bytes = Vec::with_capacity(value.len() / 2);
    for offset in (0..value.len()).step_by(2) {
        let byte = u8::from_str_radix(&value[offset..offset + 2], 16)
            .with_context(|| format!("{label} contains invalid hex at byte {}", offset / 2))?;
        bytes.push(byte);
    }
    Ok(bytes)
}

fn encoded_nack_fault(step: &CaseStep, label: &str) -> Result<Vec<u8>> {
    if step.value.is_some()
        || step.voltage_mv.is_some()
        || step.state_of_charge.is_some()
        || step.status.is_some()
        || step.latched_fault.is_some()
        || step.current_fault.is_some()
        || step.year.is_some()
        || step.month.is_some()
        || step.day.is_some()
        || step.weekday.is_some()
        || step.hour.is_some()
        || step.minute.is_some()
        || step.second.is_some()
        || step.oscillator_stopped.is_some()
        || !step.variant.trim().is_empty()
        || !step.fixture.trim().is_empty()
        || step.powered.is_some()
        || step.endpoint_id.is_some()
        || step.payload_hex.is_some()
        || step.delivery_delay_us.is_some()
        || step.rssi_dbm.is_some()
        || step.snr_db.is_some()
    {
        return Err(anyhow!(
            "{label} set_fault accepts only field 'nack'/'none' and optional oneShot"
        ));
    }
    match step.field.trim().to_ascii_lowercase().as_str() {
        "none" | "clear" if !step.one_shot => Ok(vec![0]),
        "nack" => Ok(vec![if step.one_shot { 1 } else { 2 }]),
        "none" | "clear" => Err(anyhow!("{label} clear fault does not support oneShot")),
        field => Err(anyhow!("unsupported {label} fault '{field}'")),
    }
}

fn gnss_variant_id(value: &str) -> Result<u8> {
    match value.trim().to_ascii_lowercase().as_str() {
        "l76k" => Ok(0),
        "ublox_m10" | "ublox-m10" | "mia_m10q" | "mia-m10q" => Ok(1),
        variant => Err(anyhow!("unsupported GNSS variant '{variant}'")),
    }
}

fn gnss_fixture_id(value: &str) -> Result<u8> {
    match value.trim().to_ascii_lowercase().as_str() {
        "fixed" => Ok(0),
        "no_fix" | "no-fix" => Ok(1),
        "silent" | "absent" => Ok(2),
        "malformed" | "invalid" => Ok(3),
        "stale" => Ok(4),
        fixture => Err(anyhow!("unsupported GNSS fixture '{fixture}'")),
    }
}

fn ensure_empty_control_fields(step: &CaseStep, label: &str, operation: &str) -> Result<()> {
    if step.one_shot
        || !step.field.trim().is_empty()
        || step.value.is_some()
        || step.voltage_mv.is_some()
        || step.state_of_charge.is_some()
        || step.status.is_some()
        || step.latched_fault.is_some()
        || step.current_fault.is_some()
        || step.year.is_some()
        || step.month.is_some()
        || step.day.is_some()
        || step.weekday.is_some()
        || step.hour.is_some()
        || step.minute.is_some()
        || step.second.is_some()
        || step.oscillator_stopped.is_some()
        || !step.variant.trim().is_empty()
        || !step.fixture.trim().is_empty()
        || step.powered.is_some()
        || step.endpoint_id.is_some()
        || step.payload_hex.is_some()
        || step.delivery_delay_us.is_some()
        || step.rssi_dbm.is_some()
        || step.snr_db.is_some()
    {
        return Err(anyhow!(
            "{label} {operation} does not accept state or fault fields"
        ));
    }
    Ok(())
}

fn peripheral_control_request(step: &CaseStep, request_id: u32) -> Result<(u8, Vec<u8>)> {
    let device = match step.device.trim().to_ascii_lowercase().as_str() {
        "storage" | "sd" => PERIPHERAL_CONTROL_STORAGE_DEVICE,
        "bq27220" | "battery" => PERIPHERAL_CONTROL_BQ27220_DEVICE,
        "sx1262" | "radio" | "lora" => PERIPHERAL_CONTROL_SX1262_DEVICE,
        "bq25896" | "charger" => PERIPHERAL_CONTROL_BQ25896_DEVICE,
        "pcf8563" | "rtc" => PERIPHERAL_CONTROL_PCF8563_DEVICE,
        "gnss" | "gps" => PERIPHERAL_CONTROL_GNSS_DEVICE,
        "tps651851" | "tps65185" | "epd_power" | "epd-power" => PERIPHERAL_CONTROL_TPS651851_DEVICE,
        value => return Err(anyhow!("unsupported peripheral_control device '{value}'")),
    };
    let operation = step.operation.trim().to_ascii_lowercase();
    let field = step.field.trim().to_ascii_lowercase();
    let mut flags = 0u8;
    let (operation_id, body) = if device == PERIPHERAL_CONTROL_BQ27220_DEVICE {
        match operation.as_str() {
            "set_state" => {
                if step.one_shot || !field.is_empty() || step.value.is_some() {
                    return Err(anyhow!(
                        "BQ27220 set_state does not accept field, value, or oneShot"
                    ));
                }
                let voltage_mv = step
                    .voltage_mv
                    .ok_or_else(|| anyhow!("BQ27220 set_state requires voltageMv"))?;
                let state_of_charge = step
                    .state_of_charge
                    .ok_or_else(|| anyhow!("BQ27220 set_state requires stateOfCharge"))?;
                if state_of_charge > 100 {
                    return Err(anyhow!("BQ27220 stateOfCharge must be in 0..100"));
                }
                let [voltage_low, voltage_high] = voltage_mv.to_le_bytes();
                (
                    PERIPHERAL_CONTROL_SET_STATE,
                    vec![voltage_low, voltage_high, state_of_charge],
                )
            }
            "set_fault" => {
                if (!field.is_empty() && field != "nack")
                    || step.value.is_some()
                    || step.voltage_mv.is_some()
                    || step.state_of_charge.is_some()
                {
                    return Err(anyhow!(
                        "BQ27220 set_fault accepts only optional field 'nack' and oneShot"
                    ));
                }
                if step.one_shot {
                    flags = PERIPHERAL_CONTROL_ONE_SHOT;
                }
                (PERIPHERAL_CONTROL_SET_FAULT, vec![1])
            }
            "reset" => {
                if step.one_shot
                    || !field.is_empty()
                    || step.value.is_some()
                    || step.voltage_mv.is_some()
                    || step.state_of_charge.is_some()
                {
                    return Err(anyhow!(
                        "BQ27220 reset does not accept state, field, value, or oneShot"
                    ));
                }
                (PERIPHERAL_CONTROL_RESET, Vec::new())
            }
            _ => {
                return Err(anyhow!(
                    "unsupported BQ27220 peripheral_control operation '{operation}'"
                ));
            }
        }
    } else if device == PERIPHERAL_CONTROL_BQ25896_DEVICE {
        match operation.as_str() {
            "set_state" => {
                if step.one_shot
                    || !field.is_empty()
                    || step.value.is_some()
                    || step.voltage_mv.is_some()
                    || step.state_of_charge.is_some()
                    || step.year.is_some()
                    || step.month.is_some()
                    || step.day.is_some()
                    || step.weekday.is_some()
                    || step.hour.is_some()
                    || step.minute.is_some()
                    || step.second.is_some()
                    || step.oscillator_stopped.is_some()
                    || !step.variant.trim().is_empty()
                    || !step.fixture.trim().is_empty()
                    || step.powered.is_some()
                    || step.endpoint_id.is_some()
                    || step.payload_hex.is_some()
                    || step.delivery_delay_us.is_some()
                    || step.rssi_dbm.is_some()
                    || step.snr_db.is_some()
                {
                    return Err(anyhow!(
                        "BQ25896 set_state accepts only status, latchedFault, and currentFault"
                    ));
                }
                (
                    PERIPHERAL_CONTROL_SET_STATE,
                    vec![
                        step.status
                            .ok_or_else(|| anyhow!("BQ25896 set_state requires status"))?,
                        step.latched_fault
                            .ok_or_else(|| anyhow!("BQ25896 set_state requires latchedFault"))?,
                        step.current_fault
                            .ok_or_else(|| anyhow!("BQ25896 set_state requires currentFault"))?,
                    ],
                )
            }
            "set_fault" => (
                PERIPHERAL_CONTROL_SET_FAULT,
                encoded_nack_fault(step, "BQ25896")?,
            ),
            "reset" | "query" => {
                ensure_empty_control_fields(step, "BQ25896", &operation)?;
                (
                    if operation == "reset" {
                        PERIPHERAL_CONTROL_RESET
                    } else {
                        PERIPHERAL_CONTROL_QUERY
                    },
                    Vec::new(),
                )
            }
            _ => {
                return Err(anyhow!(
                    "unsupported BQ25896 peripheral_control operation '{operation}'"
                ));
            }
        }
    } else if device == PERIPHERAL_CONTROL_PCF8563_DEVICE {
        match operation.as_str() {
            "set_state" => {
                if step.one_shot
                    || !field.is_empty()
                    || step.value.is_some()
                    || step.voltage_mv.is_some()
                    || step.state_of_charge.is_some()
                    || step.status.is_some()
                    || step.latched_fault.is_some()
                    || step.current_fault.is_some()
                    || !step.variant.trim().is_empty()
                    || !step.fixture.trim().is_empty()
                    || step.powered.is_some()
                    || step.endpoint_id.is_some()
                    || step.payload_hex.is_some()
                    || step.delivery_delay_us.is_some()
                    || step.rssi_dbm.is_some()
                    || step.snr_db.is_some()
                {
                    return Err(anyhow!(
                        "PCF8563 set_state accepts only date/time fields and oscillatorStopped"
                    ));
                }
                (
                    PERIPHERAL_CONTROL_SET_STATE,
                    vec![
                        step.year
                            .ok_or_else(|| anyhow!("PCF8563 set_state requires year"))?,
                        step.month
                            .ok_or_else(|| anyhow!("PCF8563 set_state requires month"))?,
                        step.day
                            .ok_or_else(|| anyhow!("PCF8563 set_state requires day"))?,
                        step.weekday
                            .ok_or_else(|| anyhow!("PCF8563 set_state requires weekday"))?,
                        step.hour
                            .ok_or_else(|| anyhow!("PCF8563 set_state requires hour"))?,
                        step.minute
                            .ok_or_else(|| anyhow!("PCF8563 set_state requires minute"))?,
                        step.second
                            .ok_or_else(|| anyhow!("PCF8563 set_state requires second"))?,
                        u8::from(step.oscillator_stopped.unwrap_or(false)),
                    ],
                )
            }
            "set_fault" if field == "invalid" => {
                if step.one_shot
                    || step.value.is_some()
                    || step.voltage_mv.is_some()
                    || step.state_of_charge.is_some()
                    || step.status.is_some()
                    || step.latched_fault.is_some()
                    || step.current_fault.is_some()
                    || step.year.is_some()
                    || step.month.is_some()
                    || step.day.is_some()
                    || step.weekday.is_some()
                    || step.hour.is_some()
                    || step.minute.is_some()
                    || step.second.is_some()
                    || step.oscillator_stopped.is_some()
                    || !step.variant.trim().is_empty()
                    || !step.fixture.trim().is_empty()
                    || step.powered.is_some()
                    || step.endpoint_id.is_some()
                    || step.delivery_delay_us.is_some()
                    || step.rssi_dbm.is_some()
                    || step.snr_db.is_some()
                {
                    return Err(anyhow!(
                        "PCF8563 invalid fixture accepts only field 'invalid' and payloadHex"
                    ));
                }
                let registers = decode_bounded_hex(
                    step.payload_hex
                        .as_deref()
                        .ok_or_else(|| anyhow!("PCF8563 invalid fixture requires payloadHex"))?,
                    8,
                    "PCF8563 payloadHex",
                )?;
                if registers.len() != 8 {
                    return Err(anyhow!(
                        "PCF8563 invalid fixture payloadHex must encode exactly 8 registers"
                    ));
                }
                let mut body = Vec::with_capacity(9);
                body.push(3);
                body.extend_from_slice(&registers);
                (PERIPHERAL_CONTROL_SET_FAULT, body)
            }
            "set_fault" => (
                PERIPHERAL_CONTROL_SET_FAULT,
                encoded_nack_fault(step, "PCF8563")?,
            ),
            "reset" | "query" => {
                ensure_empty_control_fields(step, "PCF8563", &operation)?;
                (
                    if operation == "reset" {
                        PERIPHERAL_CONTROL_RESET
                    } else {
                        PERIPHERAL_CONTROL_QUERY
                    },
                    Vec::new(),
                )
            }
            _ => {
                return Err(anyhow!(
                    "unsupported PCF8563 peripheral_control operation '{operation}'"
                ));
            }
        }
    } else if device == PERIPHERAL_CONTROL_GNSS_DEVICE {
        match operation.as_str() {
            "set_state" => {
                if step.one_shot
                    || !field.is_empty()
                    || step.value.is_some()
                    || step.voltage_mv.is_some()
                    || step.state_of_charge.is_some()
                    || step.status.is_some()
                    || step.latched_fault.is_some()
                    || step.current_fault.is_some()
                    || step.year.is_some()
                    || step.month.is_some()
                    || step.day.is_some()
                    || step.weekday.is_some()
                    || step.hour.is_some()
                    || step.minute.is_some()
                    || step.second.is_some()
                    || step.oscillator_stopped.is_some()
                    || step.payload_hex.is_some()
                    || step.endpoint_id.is_some()
                    || step.delivery_delay_us.is_some()
                    || step.rssi_dbm.is_some()
                    || step.snr_db.is_some()
                {
                    return Err(anyhow!(
                        "GNSS set_state accepts only variant, fixture, and powered"
                    ));
                }
                (
                    PERIPHERAL_CONTROL_SET_STATE,
                    vec![
                        gnss_variant_id(&step.variant)?,
                        gnss_fixture_id(&step.fixture)?,
                        u8::from(
                            step.powered
                                .ok_or_else(|| anyhow!("GNSS set_state requires powered"))?,
                        ),
                    ],
                )
            }
            "set_fault" => {
                if step.one_shot
                    || step.value.is_some()
                    || step.voltage_mv.is_some()
                    || step.state_of_charge.is_some()
                    || step.status.is_some()
                    || step.latched_fault.is_some()
                    || step.current_fault.is_some()
                    || step.year.is_some()
                    || step.month.is_some()
                    || step.day.is_some()
                    || step.weekday.is_some()
                    || step.hour.is_some()
                    || step.minute.is_some()
                    || step.second.is_some()
                    || step.oscillator_stopped.is_some()
                    || !step.variant.trim().is_empty()
                    || !step.fixture.trim().is_empty()
                    || step.powered.is_some()
                    || step.endpoint_id.is_some()
                    || step.payload_hex.is_some()
                    || step.delivery_delay_us.is_some()
                    || step.rssi_dbm.is_some()
                    || step.snr_db.is_some()
                {
                    return Err(anyhow!(
                        "GNSS set_fault accepts only a fixture name in field"
                    ));
                }
                (PERIPHERAL_CONTROL_SET_FAULT, vec![gnss_fixture_id(&field)?])
            }
            "reset" | "query" => {
                ensure_empty_control_fields(step, "GNSS", &operation)?;
                (
                    if operation == "reset" {
                        PERIPHERAL_CONTROL_RESET
                    } else {
                        PERIPHERAL_CONTROL_QUERY
                    },
                    Vec::new(),
                )
            }
            _ => {
                return Err(anyhow!(
                    "unsupported GNSS peripheral_control operation '{operation}'"
                ));
            }
        }
    } else if device == PERIPHERAL_CONTROL_TPS651851_DEVICE {
        match operation.as_str() {
            "set_state" => {
                if step.one_shot
                    || !matches!(field.as_str(), "hold_not_ready" | "hold-not-ready")
                    || step.voltage_mv.is_some()
                    || step.state_of_charge.is_some()
                    || step.status.is_some()
                    || step.latched_fault.is_some()
                    || step.current_fault.is_some()
                    || step.year.is_some()
                    || step.month.is_some()
                    || step.day.is_some()
                    || step.weekday.is_some()
                    || step.hour.is_some()
                    || step.minute.is_some()
                    || step.second.is_some()
                    || step.oscillator_stopped.is_some()
                    || !step.variant.trim().is_empty()
                    || !step.fixture.trim().is_empty()
                    || step.powered.is_some()
                    || step.endpoint_id.is_some()
                    || step.payload_hex.is_some()
                    || step.delivery_delay_us.is_some()
                    || step.rssi_dbm.is_some()
                    || step.snr_db.is_some()
                {
                    return Err(anyhow!(
                        "TPS651851 set_state requires field 'hold_not_ready' and value"
                    ));
                }
                (
                    PERIPHERAL_CONTROL_SET_STATE,
                    vec![u8::from(step.value.ok_or_else(|| {
                        anyhow!("TPS651851 set_state requires value")
                    })?)],
                )
            }
            "set_fault" => (
                PERIPHERAL_CONTROL_SET_FAULT,
                encoded_nack_fault(step, "TPS651851")?,
            ),
            "reset" | "query" => {
                ensure_empty_control_fields(step, "TPS651851", &operation)?;
                (
                    if operation == "reset" {
                        PERIPHERAL_CONTROL_RESET
                    } else {
                        PERIPHERAL_CONTROL_QUERY
                    },
                    Vec::new(),
                )
            }
            _ => {
                return Err(anyhow!(
                    "unsupported TPS651851 peripheral_control operation '{operation}'"
                ));
            }
        }
    } else if device == PERIPHERAL_CONTROL_SX1262_DEVICE {
        match operation.as_str() {
            "set_endpoint" => {
                if step.one_shot
                    || !field.is_empty()
                    || step.value.is_some()
                    || step.voltage_mv.is_some()
                    || step.state_of_charge.is_some()
                    || step.payload_hex.is_some()
                    || step.delivery_delay_us.is_some()
                    || step.rssi_dbm.is_some()
                    || step.snr_db.is_some()
                {
                    return Err(anyhow!(
                        "SX1262 set_endpoint accepts only a non-zero endpointId"
                    ));
                }
                let endpoint_id = step
                    .endpoint_id
                    .filter(|value| *value != 0)
                    .ok_or_else(|| anyhow!("SX1262 set_endpoint requires a non-zero endpointId"))?;
                let mut body = vec![SX1262_CONTROL_SET_ENDPOINT];
                body.extend_from_slice(&endpoint_id.to_le_bytes());
                (PERIPHERAL_CONTROL_SET_STATE, body)
            }
            "schedule_receive" => {
                if step.one_shot
                    || !field.is_empty()
                    || step.value.is_some()
                    || step.voltage_mv.is_some()
                    || step.state_of_charge.is_some()
                    || step.endpoint_id.is_some()
                {
                    return Err(anyhow!(
                        "SX1262 schedule_receive accepts payloadHex and optional deliveryDelayUs/rssiDbm/snrDb"
                    ));
                }
                let packet = decode_bounded_hex(
                    step.payload_hex
                        .as_deref()
                        .ok_or_else(|| anyhow!("SX1262 schedule_receive requires payloadHex"))?,
                    SX1262_SCRIPTED_MAX_PACKET_BYTES,
                    "SX1262 payloadHex",
                )?;
                let delay_us = step
                    .delivery_delay_us
                    .unwrap_or(SX1262_BROKER_DELIVERY_DELAY_US);
                let rssi_dbm = step.rssi_dbm.unwrap_or(SX1262_BROKER_RSSI_DBM);
                let snr_db = step.snr_db.unwrap_or(SX1262_BROKER_SNR_DB);
                if !(-127..=0).contains(&rssi_dbm) || !(-32..=31).contains(&snr_db) {
                    return Err(anyhow!(
                        "SX1262 metrics require rssiDbm in -127..0 and snrDb in -32..31"
                    ));
                }
                let mut body = vec![SX1262_CONTROL_SCHEDULE_RECEIVE];
                body.extend_from_slice(&delay_us.to_le_bytes());
                body.extend_from_slice(&rssi_dbm.to_le_bytes());
                body.push(snr_db as u8);
                body.push(packet.len() as u8);
                body.extend_from_slice(&packet);
                (PERIPHERAL_CONTROL_SET_STATE, body)
            }
            "set_fault" => {
                if step.one_shot
                    || step.value.is_some()
                    || step.voltage_mv.is_some()
                    || step.state_of_charge.is_some()
                    || step.endpoint_id.is_some()
                    || step.payload_hex.is_some()
                    || step.delivery_delay_us.is_some()
                    || step.rssi_dbm.is_some()
                    || step.snr_db.is_some()
                {
                    return Err(anyhow!(
                        "SX1262 set_fault accepts only a fault name in field"
                    ));
                }
                let fault = match field.as_str() {
                    "none" => 0,
                    "missing_device" => 1,
                    "stuck_busy" => 2,
                    "drop_packet" => 3,
                    "crc_error" => 4,
                    "header_error" => 5,
                    "rx_timeout" => 6,
                    "air_disconnected" => 7,
                    _ => return Err(anyhow!("unsupported SX1262 fault '{field}'")),
                };
                (PERIPHERAL_CONTROL_SET_FAULT, vec![fault])
            }
            "reset" => {
                if step.one_shot
                    || !field.is_empty()
                    || step.value.is_some()
                    || step.voltage_mv.is_some()
                    || step.state_of_charge.is_some()
                    || step.endpoint_id.is_some()
                    || step.payload_hex.is_some()
                    || step.delivery_delay_us.is_some()
                    || step.rssi_dbm.is_some()
                    || step.snr_db.is_some()
                {
                    return Err(anyhow!(
                        "SX1262 reset does not accept state or fault fields"
                    ));
                }
                (PERIPHERAL_CONTROL_RESET, Vec::new())
            }
            _ => {
                return Err(anyhow!(
                    "unsupported SX1262 peripheral_control operation '{operation}'"
                ));
            }
        }
    } else {
        match operation.as_str() {
            "set_state" => {
                if step.one_shot {
                    return Err(anyhow!(
                        "peripheral_control set_state does not support oneShot"
                    ));
                }
                let field_id = match field.as_str() {
                    "present" => STORAGE_CONTROL_STATE_PRESENT,
                    "writable" => STORAGE_CONTROL_STATE_WRITABLE,
                    _ => return Err(anyhow!("unsupported storage state field '{field}'")),
                };
                let value = step
                    .value
                    .ok_or_else(|| anyhow!("peripheral_control set_state requires value"))?;
                (
                    PERIPHERAL_CONTROL_SET_STATE,
                    vec![field_id, u8::from(value)],
                )
            }
            "set_fault" => {
                let field_id = match field.as_str() {
                    "read" => STORAGE_CONTROL_FAULT_READ,
                    "write" | "read_only" => STORAGE_CONTROL_FAULT_WRITE,
                    "corrupt" => STORAGE_CONTROL_FAULT_CORRUPT,
                    _ => return Err(anyhow!("unsupported storage fault field '{field}'")),
                };
                if step.one_shot {
                    if field_id == STORAGE_CONTROL_FAULT_CORRUPT {
                        return Err(anyhow!("storage corrupt fault does not support oneShot"));
                    }
                    flags = PERIPHERAL_CONTROL_ONE_SHOT;
                }
                let value = step
                    .value
                    .ok_or_else(|| anyhow!("peripheral_control set_fault requires value"))?;
                (
                    PERIPHERAL_CONTROL_SET_FAULT,
                    vec![field_id, u8::from(value)],
                )
            }
            "reset" | "query" => {
                if step.one_shot
                    || !field.is_empty()
                    || step.value.is_some()
                    || step.voltage_mv.is_some()
                    || step.state_of_charge.is_some()
                {
                    return Err(anyhow!(
                        "peripheral_control {operation} does not accept field, value, or oneShot"
                    ));
                }
                let operation_id = if operation == "reset" {
                    PERIPHERAL_CONTROL_RESET
                } else {
                    PERIPHERAL_CONTROL_QUERY
                };
                (operation_id, Vec::new())
            }
            _ => {
                return Err(anyhow!(
                    "unsupported peripheral_control operation '{operation}'"
                ));
            }
        }
    };

    let mut payload = Vec::with_capacity(10 + body.len());
    payload.extend_from_slice(&[PERIPHERAL_CONTROL_VERSION, operation_id, device, flags]);
    payload.extend_from_slice(&request_id.to_le_bytes());
    payload.extend_from_slice(&[body.len() as u8, 0]);
    payload.extend_from_slice(&body);
    if payload.len() > PERIPHERAL_CONTROL_MAX_PAYLOAD_BYTES {
        return Err(anyhow!(
            "peripheral_control payload exceeds the {PERIPHERAL_CONTROL_MAX_PAYLOAD_BYTES}-byte wire limit"
        ));
    }
    Ok((device, payload))
}

fn peripheral_control_ack_result(
    payload: &[u8],
    expected_device: u8,
    expected_request_id: u32,
) -> Option<Result<()>> {
    if payload.len() < 8
        || payload[0] != PERIPHERAL_CONTROL_VERSION
        || payload[2] != expected_device
        || u32::from_le_bytes(payload[4..8].try_into().expect("checked ACK request bytes"))
            != expected_request_id
    {
        return None;
    }
    if payload[1] != PERIPHERAL_CONTROL_ACK {
        return None;
    }
    if payload.len() != 12 {
        return Some(Err(anyhow!(
            "malformed peripheral_control ACK for device {expected_device} request {expected_request_id}: {} bytes",
            payload.len()
        )));
    }

    let accepted = payload[3] != 0;
    let error = payload[8];
    if accepted && error == 0 {
        Some(Ok(()))
    } else {
        Some(Err(anyhow!(
            "peripheral_control rejected device {expected_device} request {expected_request_id}: accepted={} error={error}",
            u8::from(accepted)
        )))
    }
}

fn peripheral_control_trace_summary(payload: &[u8]) -> Option<Result<String>> {
    if payload.len() < 2
        || payload[0] != PERIPHERAL_CONTROL_VERSION
        || payload[1] != PERIPHERAL_CONTROL_TRACE
    {
        return None;
    }
    if payload.len() < PERIPHERAL_CONTROL_TRACE_HEADER_BYTES {
        return Some(Err(anyhow!(
            "malformed peripheral_control TRACE: {} bytes",
            payload.len()
        )));
    }
    let data_length = payload[20] as usize;
    if data_length > PERIPHERAL_CONTROL_TRACE_DATA_MAX_BYTES
        || payload.len() != PERIPHERAL_CONTROL_TRACE_HEADER_BYTES + data_length
    {
        return Some(Err(anyhow!(
            "malformed peripheral_control TRACE data length: header={} actual={}",
            data_length,
            payload
                .len()
                .saturating_sub(PERIPHERAL_CONTROL_TRACE_HEADER_BYTES)
        )));
    }
    let device = payload[2];
    let sequence = u32::from_le_bytes(payload[4..8].try_into().expect("checked trace sequence"));
    let virtual_time_ns =
        u64::from_le_bytes(payload[8..16].try_into().expect("checked trace time"));
    let direction = match payload[18] {
        0 => "write",
        1 => "read",
        _ => "unknown",
    };
    let data = payload[PERIPHERAL_CONTROL_TRACE_HEADER_BYTES..]
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect::<String>();
    Some(Ok(format!(
        "peripheral_trace device={device} sequence={sequence} virtual_time_ns={virtual_time_ns} result={} bus={} address=0x{:02x} direction={direction} register=0x{:02x} bytes={data_length} data={data}",
        payload[3], payload[16], payload[17], payload[19]
    )))
}

fn log_peripheral_control_event(payload: &[u8]) {
    match peripheral_control_trace_summary(payload) {
        Some(Ok(summary)) => eprintln!("[headless] {summary}"),
        Some(Err(error)) => eprintln!("[headless] peripheral control trace rejected: {error}"),
        None => eprintln!(
            "[headless] peripheral control response bytes={}",
            payload.len()
        ),
    }
}

fn is_sx1262_tx_event(payload: &[u8]) -> bool {
    payload.len() >= 35
        && payload[0] == PERIPHERAL_CONTROL_VERSION
        && payload[1] == PERIPHERAL_CONTROL_RADIO_TX
        && payload[2] == PERIPHERAL_CONTROL_SX1262_DEVICE
}

fn radio_tx_event_to_broker_control(
    payload: &[u8],
    request_id: u32,
) -> Result<(u32, u64, Vec<u8>)> {
    if !is_sx1262_tx_event(payload) {
        return Err(anyhow!("not an SX1262 transmit event"));
    }
    if payload[3] != 0 {
        return Err(anyhow!(
            "SX1262 transmit exceeds the {SX1262_BROKER_MAX_PACKET_BYTES}-byte broker limit"
        ));
    }
    let packet_length = payload[34] as usize;
    if packet_length == 0
        || packet_length > SX1262_BROKER_MAX_PACKET_BYTES
        || payload.len() != 35 + packet_length
    {
        return Err(anyhow!("malformed SX1262 transmit event length"));
    }
    let sequence = u32::from_le_bytes(payload[4..8].try_into().expect("checked sequence"));
    let endpoint_id = u64::from_le_bytes(payload[8..16].try_into().expect("checked endpoint"));
    if endpoint_id == 0 {
        return Err(anyhow!("SX1262 transmit event has no run-scoped endpoint"));
    }

    let mut body = Vec::with_capacity(27 + packet_length);
    body.push(SX1262_CONTROL_DELIVER_BROKER_PACKET);
    body.extend_from_slice(&payload[8..16]);
    body.extend_from_slice(&payload[24..34]);
    body.extend_from_slice(&SX1262_BROKER_DELIVERY_DELAY_US.to_le_bytes());
    body.extend_from_slice(&SX1262_BROKER_RSSI_DBM.to_le_bytes());
    body.push(SX1262_BROKER_SNR_DB as u8);
    body.push(packet_length as u8);
    body.extend_from_slice(&payload[35..]);

    let mut control = Vec::with_capacity(10 + body.len());
    control.extend_from_slice(&[
        PERIPHERAL_CONTROL_VERSION,
        PERIPHERAL_CONTROL_SET_STATE,
        PERIPHERAL_CONTROL_SX1262_DEVICE,
        0,
    ]);
    control.extend_from_slice(&request_id.to_le_bytes());
    control.extend_from_slice(&[body.len() as u8, 0]);
    control.extend_from_slice(&body);
    Ok((sequence, endpoint_id, control))
}

impl RadioPeerSender {
    async fn forward(&self, payload: &[u8]) -> Result<()> {
        self.socket
            .send_to(payload, &self.peer_path)
            .await
            .with_context(|| {
                format!(
                    "forwarding SX1262 transmit event to {}",
                    self.peer_path.display()
                )
            })?;
        Ok(())
    }
}

async fn start_radio_peer_bridge(
    args: &Args,
    writer: debug_transport::HeadlessDebugWriter,
) -> Result<(Option<RadioPeerSender>, Option<RadioPeerHandle>)> {
    let (Some(bind_path), Some(peer_path)) = (
        args.radio_broker_bind.as_ref(),
        args.radio_broker_peer.as_ref(),
    ) else {
        return Ok((None, None));
    };
    cleanup_stale_socket(bind_path);
    let socket = Arc::new(
        UnixDatagram::bind(bind_path)
            .with_context(|| format!("binding SX1262 broker socket {}", bind_path.display()))?,
    );
    let sender = RadioPeerSender {
        socket: socket.clone(),
        peer_path: peer_path.clone(),
    };
    let bind_path_for_task = bind_path.clone();
    let task = tokio::spawn(async move {
        let mut buffer = [0u8; 128];
        let mut last_sequence = 0u32;
        let mut request_id = 0x8000_0000u32;
        loop {
            let (length, _) = match socket.recv_from(&mut buffer).await {
                Ok(received) => received,
                Err(error) => {
                    eprintln!("[radio-broker] receive failed: {error}");
                    return;
                }
            };
            let (sequence, endpoint_id, control) =
                match radio_tx_event_to_broker_control(&buffer[..length], request_id) {
                    Ok(delivery) => delivery,
                    Err(error) => {
                        eprintln!("[radio-broker] rejected peer event: {error}");
                        continue;
                    }
                };
            if sequence <= last_sequence {
                eprintln!(
                    "[radio-broker] ignored non-monotonic peer sequence={sequence} last={last_sequence}"
                );
                continue;
            }
            last_sequence = sequence;
            if !writer
                .send_within(
                    debug_transport::OutFrame {
                        channel: CHANNEL_CONTROL,
                        flags: 0,
                        payload: control,
                    },
                    Duration::from_millis(500),
                )
                .await
            {
                eprintln!(
                    "[radio-broker] delivery transport unavailable endpoint={endpoint_id} sequence={sequence}"
                );
                continue;
            }
            eprintln!(
                "[radio-broker] delivered endpoint={endpoint_id} sequence={sequence} request={request_id}"
            );
            request_id = request_id.wrapping_add(1).max(0x8000_0000);
        }
    });
    eprintln!(
        "[radio-broker] bound={} peer={}",
        bind_path.display(),
        peer_path.display()
    );
    Ok((
        Some(sender),
        Some(RadioPeerHandle {
            task,
            bind_path: bind_path_for_task,
        }),
    ))
}

async fn read_qemu_stdout(
    stdout: tokio::process::ChildStdout,
    ctx: std::sync::Arc<tokio::sync::Mutex<E2EContext>>,
    event_tx: mpsc::UnboundedSender<HeadlessEvent>,
) -> Result<()> {
    let mut lines = BufReader::new(stdout).lines();
    while let Some(line) = lines.next_line().await? {
        {
            let mut ctx = ctx.lock().await;
            ctx.push_line(&line);
            ctx.parse_activity_transition(&line);
        }
        let _ = event_tx.send(HeadlessEvent::SerialLine);
    }
    Ok(())
}

async fn wait_for_activity(
    ctx: &std::sync::Arc<tokio::sync::Mutex<E2EContext>>,
    expected: &str,
    timeout: Duration,
    after_event_count: Option<usize>,
) -> Result<bool> {
    let deadline = tokio::time::Instant::now() + timeout;
    let expected_normalized = expected.trim().to_ascii_lowercase();
    let stable_duration = Duration::from_millis(ASSERT_ACTIVITY_STABLE_MS);
    let mut matched_after_input_at: Option<tokio::time::Instant> = None;
    loop {
        let now = tokio::time::Instant::now();
        {
            let ctx = ctx.lock().await;
            if let Some(after_event_count) = after_event_count {
                let saw_matching_event = ctx
                    .activity_events
                    .iter()
                    .skip(after_event_count)
                    .any(|(_, activity)| activity_matches(&expected_normalized, activity));
                let current_matches = ctx
                    .current_activity
                    .as_ref()
                    .is_some_and(|activity| activity_matches(&expected_normalized, activity));
                if current_matches
                    && (saw_matching_event || after_event_count >= ctx.activity_events.len())
                {
                    match matched_after_input_at {
                        Some(first_seen) if now.duration_since(first_seen) >= stable_duration => {
                            return Ok(true);
                        }
                        Some(_) => {}
                        None => matched_after_input_at = Some(now),
                    }
                } else {
                    matched_after_input_at = None;
                }
            } else if let Some(ref current) = ctx.current_activity {
                if activity_matches(&expected_normalized, current) {
                    return Ok(true);
                }
            }
        }
        if now >= deadline {
            let ctx = ctx.lock().await;
            let current = ctx
                .current_activity
                .clone()
                .unwrap_or_else(|| "none".to_string());
            if let Some(after_event_count) = after_event_count {
                return Err(anyhow!(
                    "timed out waiting for activity '{}' after input event boundary {} (current: '{}', activity events: {})",
                    expected,
                    after_event_count,
                    current,
                    ctx.activity_events.len()
                ));
            }
            return Err(anyhow!(
                "timed out waiting for activity '{}' (current: '{}')",
                expected,
                current
            ));
        }
        time::sleep(Duration::from_millis(50)).await;
    }
}

fn activity_matches(expected_normalized: &str, activity: &str) -> bool {
    let activity_normalized = activity.trim().to_ascii_lowercase();
    if expected_normalized == "other" {
        !activity_normalized.starts_with("exit:") && activity_normalized != "dashboard"
    } else {
        activity_normalized == expected_normalized
    }
}

async fn wait_for_log_contains(
    ctx: &std::sync::Arc<tokio::sync::Mutex<E2EContext>>,
    expected: &str,
    timeout: Duration,
    after_line_count: Option<usize>,
) -> Result<bool> {
    let deadline = tokio::time::Instant::now() + timeout;
    loop {
        {
            let ctx = ctx.lock().await;
            let line_offset = after_line_count.unwrap_or(0);
            if ctx
                .transcript
                .iter()
                .skip(line_offset)
                .any(|line| line.contains(expected))
            {
                return Ok(true);
            }
        }
        if tokio::time::Instant::now() >= deadline {
            if let Some(after_line_count) = after_line_count {
                let ctx = ctx.lock().await;
                return Err(anyhow!(
                    "timed out waiting for log line containing '{}' after input log boundary {} (transcript lines: {})",
                    expected,
                    after_line_count,
                    ctx.transcript.len()
                ));
            }
            return Err(anyhow!(
                "timed out waiting for log line containing '{}'",
                expected
            ));
        }
        time::sleep(Duration::from_millis(50)).await;
    }
}

async fn wait_for_peripheral_control_ack(
    event_rx: &mut mpsc::UnboundedReceiver<HeadlessEvent>,
    last_fb: &mut Option<Vec<u8>>,
    expected_device: u8,
    expected_request_id: u32,
    timeout: Duration,
) -> Result<()> {
    let deadline = tokio::time::Instant::now() + timeout;
    loop {
        match time::timeout_at(deadline, event_rx.recv()).await {
            Ok(Some(HeadlessEvent::Frame(fb))) => *last_fb = Some(fb),
            Ok(Some(HeadlessEvent::PeripheralControl(payload))) => {
                log_peripheral_control_event(&payload);
                if let Some(result) =
                    peripheral_control_ack_result(&payload, expected_device, expected_request_id)
                {
                    return result;
                }
            }
            Ok(Some(HeadlessEvent::Injected))
            | Ok(Some(HeadlessEvent::TouchRead(_)))
            | Ok(Some(HeadlessEvent::ButtonRead { .. }))
            | Ok(Some(HeadlessEvent::SerialLine)) => {}
            Ok(None) => {
                return Err(anyhow!(
                    "simulator event stream closed while waiting for peripheral_control ACK for device {expected_device} request {expected_request_id}"
                ));
            }
            Err(_) => {
                return Err(anyhow!(
                    "timed out waiting for peripheral_control ACK for device {expected_device} request {expected_request_id}"
                ));
            }
        }
    }
}

async fn execute_peripheral_control(
    step: &CaseStep,
    request_id: u32,
    phase: &str,
    debug_writer: &debug_transport::HeadlessDebugWriter,
    event_rx: &mut mpsc::UnboundedReceiver<HeadlessEvent>,
    last_fb: &mut Option<Vec<u8>>,
    timeout: Duration,
) -> Result<()> {
    let (device, payload) = peripheral_control_request(step, request_id)?;
    if phase.is_empty() {
        eprintln!(
            "[e2e] peripheral_control device={} operation={} request={request_id}",
            step.device, step.operation
        );
    } else {
        eprintln!(
            "[e2e] {phase} peripheral_control device={} operation={} request={request_id}",
            step.device, step.operation
        );
    }
    if !debug_writer
        .send(debug_transport::OutFrame {
            channel: CHANNEL_CONTROL,
            flags: 0,
            payload,
        })
        .await
    {
        return Err(anyhow!(
            "simulator peripheral control transport unavailable"
        ));
    }
    wait_for_peripheral_control_ack(event_rx, last_fb, device, request_id, timeout).await
}

fn drain_pending_e2e_events(
    event_rx: &mut mpsc::UnboundedReceiver<HeadlessEvent>,
    last_fb: &mut Option<Vec<u8>>,
) -> u64 {
    let mut frames_drained = 0u64;
    loop {
        match event_rx.try_recv() {
            Ok(HeadlessEvent::Frame(fb)) => {
                *last_fb = Some(fb);
                frames_drained += 1;
            }
            Ok(HeadlessEvent::PeripheralControl(payload)) => {
                log_peripheral_control_event(&payload);
            }
            Ok(HeadlessEvent::Injected)
            | Ok(HeadlessEvent::TouchRead(_))
            | Ok(HeadlessEvent::ButtonRead { .. })
            | Ok(HeadlessEvent::SerialLine) => {}
            Err(tokio::sync::mpsc::error::TryRecvError::Empty)
            | Err(tokio::sync::mpsc::error::TryRecvError::Disconnected) => break,
        }
    }
    frames_drained
}

async fn wait_for_injection(
    event_rx: &mut mpsc::UnboundedReceiver<HeadlessEvent>,
    last_fb: &mut Option<Vec<u8>>,
    timeout: Duration,
    wait_for_frame: bool,
) -> Result<u64> {
    let deadline = tokio::time::Instant::now() + timeout;
    let mut frames_drained = 0u64;
    let mut injection_seen = false;
    loop {
        let now = tokio::time::Instant::now();
        if now >= deadline {
            return Err(anyhow!(if injection_seen && wait_for_frame {
                "timed out waiting for simulator framebuffer after injection"
            } else {
                "timed out waiting for simulator injection"
            }));
        }
        match time::timeout_at(deadline, event_rx.recv()).await {
            Ok(Some(HeadlessEvent::Frame(fb))) => {
                *last_fb = Some(fb);
                frames_drained += 1;
                if injection_seen && wait_for_frame {
                    return Ok(frames_drained);
                }
            }
            Ok(Some(HeadlessEvent::Injected)) => {
                if !wait_for_frame {
                    return Ok(frames_drained);
                }
                injection_seen = true;
            }
            Ok(Some(HeadlessEvent::PeripheralControl(payload))) => {
                log_peripheral_control_event(&payload);
            }
            Ok(Some(HeadlessEvent::TouchRead(_)))
            | Ok(Some(HeadlessEvent::ButtonRead { .. }))
            | Ok(Some(HeadlessEvent::SerialLine)) => {}
            Ok(None) => {
                return Err(anyhow!(if injection_seen && wait_for_frame {
                    "simulator event stream closed while waiting for framebuffer after injection"
                } else {
                    "simulator event stream closed while waiting for injection"
                }))
            }
            Err(_) => {
                return Err(anyhow!(if injection_seen && wait_for_frame {
                    "timed out waiting for simulator framebuffer after injection"
                } else {
                    "timed out waiting for simulator injection"
                }))
            }
        }
    }
}

fn persist_e2e_artifacts(
    artifacts_dir: &Path,
    case_result: &CaseResult,
    transcript_lines: &[String],
    fb: &[u8],
    geometry: &BoardGeometry,
) -> Result<()> {
    fs::create_dir_all(artifacts_dir)
        .with_context(|| format!("creating artifacts dir {}", artifacts_dir.display()))?;

    let transcript_path = artifacts_dir.join("transcript.log");
    fs::write(&transcript_path, transcript_lines.join("\n") + "\n")
        .with_context(|| format!("writing transcript {}", transcript_path.display()))?;
    eprintln!("[e2e] transcript written: {}", transcript_path.display());

    let result_path = artifacts_dir.join("result.json");
    let result_json = serde_json::to_string_pretty(case_result)
        .with_context(|| "serializing case result to JSON")?;
    fs::write(&result_path, result_json + "\n")
        .with_context(|| format!("writing result {}", result_path.display()))?;
    eprintln!("[e2e] result written: {}", result_path.display());

    let screenshot_path = artifacts_dir.join("screenshot.png");
    write_png(&screenshot_path, fb, geometry)?;
    eprintln!("[e2e] screenshot written: {}", screenshot_path.display());

    Ok(())
}

fn write_simulator_system_fixture(
    sd_root: &Path,
    relative_path: &Path,
    contents: impl AsRef<[u8]>,
) -> Result<()> {
    let contents = contents.as_ref();
    for system_dir_name in [".mofei", "mofei"] {
        let fixture_path = sd_root.join(system_dir_name).join(relative_path);
        let parent = fixture_path.parent().ok_or_else(|| {
            anyhow!(
                "simulator fixture path has no parent: {}",
                fixture_path.display()
            )
        })?;
        fs::create_dir_all(parent)
            .with_context(|| format!("creating simulator fixture dir {}", parent.display()))?;
        fs::write(&fixture_path, contents)
            .with_context(|| format!("writing simulator fixture {}", fixture_path.display()))?;
    }
    Ok(())
}

fn write_murphy_system_fixture(
    sd_root: &Path,
    relative_path: &Path,
    contents: impl AsRef<[u8]>,
) -> Result<()> {
    let contents = contents.as_ref();
    for system_dir_name in [".murphy", "murphy"] {
        let fixture_path = sd_root.join(system_dir_name).join(relative_path);
        let parent = fixture_path.parent().ok_or_else(|| {
            anyhow!(
                "Panda AI OS fixture path has no parent: {}",
                fixture_path.display()
            )
        })?;
        fs::create_dir_all(parent)
            .with_context(|| format!("creating Panda AI OS fixture dir {}", parent.display()))?;
        fs::write(&fixture_path, contents)
            .with_context(|| format!("writing Panda AI OS fixture {}", fixture_path.display()))?;
    }
    Ok(())
}

fn simulator_system_fixture_exists(sd_root: &Path, relative_path: &Path) -> bool {
    [".mofei", "mofei"]
        .iter()
        .any(|system_dir_name| sd_root.join(system_dir_name).join(relative_path).exists())
}

fn repo_relative_fixture_path(relative_path: &str) -> PathBuf {
    let path = Path::new(relative_path);
    if path.is_absolute() {
        return path.to_path_buf();
    }

    standalone_root_path().join(path)
}

fn repo_root_path() -> PathBuf {
    standalone_root_path()
}

fn seed_boot_copy_files(case: &E2ECase, sd_root: &Path) -> Result<()> {
    if case.boot.copy_files.is_empty() {
        return Ok(());
    }

    let repo_root = fs::canonicalize(repo_root_path()).context("resolving repository root")?;
    let canonical_sd_root = fs::canonicalize(sd_root)
        .with_context(|| format!("resolving simulator SD root {}", sd_root.display()))?;

    for (index, file) in case.boot.copy_files.iter().enumerate() {
        let source_relative = safe_repo_copy_source(&file.source)
            .with_context(|| format!("boot.copyFiles entry {} has invalid source", index + 1))?;
        let source = fs::canonicalize(repo_root.join(&source_relative))
            .with_context(|| format!("boot.copyFiles source '{}' does not exist", file.source))?;
        if !source.starts_with(&repo_root) {
            return Err(anyhow!(
                "boot.copyFiles source '{}' resolves outside the repository root",
                file.source
            ));
        }
        let source_metadata = fs::metadata(&source)
            .with_context(|| format!("reading boot.copyFiles source {}", source.display()))?;
        if !source_metadata.is_file()
            || source_metadata.len() == 0
            || source_metadata.len() > E2E_BOOT_COPY_MAX_BYTES
        {
            return Err(anyhow!(
                "boot.copyFiles source '{}' must be a non-empty file no larger than {} bytes",
                file.source,
                E2E_BOOT_COPY_MAX_BYTES
            ));
        }

        let destination_relative = safe_sd_boot_destination(&file.path)
            .with_context(|| format!("boot.copyFiles entry {} has invalid path", index + 1))?;
        let destination = canonical_sd_root.join(&destination_relative);
        let parent = destination
            .parent()
            .ok_or_else(|| anyhow!("boot.copyFiles destination '{}' has no parent", file.path))?;
        fs::create_dir_all(parent).with_context(|| {
            format!(
                "creating boot.copyFiles destination dir {}",
                parent.display()
            )
        })?;
        let canonical_parent = fs::canonicalize(parent).with_context(|| {
            format!(
                "resolving boot.copyFiles destination dir {}",
                parent.display()
            )
        })?;
        if !canonical_parent.starts_with(&canonical_sd_root) {
            return Err(anyhow!(
                "boot.copyFiles destination '{}' resolves outside the SD root",
                file.path
            ));
        }
        if destination
            .symlink_metadata()
            .is_ok_and(|metadata| metadata.file_type().is_symlink())
        {
            return Err(anyhow!(
                "boot.copyFiles destination '{}' must not be a symlink",
                file.path
            ));
        }

        let copied = fs::copy(&source, &destination).with_context(|| {
            format!(
                "copying boot fixture {} to {}",
                source.display(),
                destination.display()
            )
        })?;
        if copied != source_metadata.len() {
            return Err(anyhow!(
                "boot.copyFiles copied {} of {} bytes from '{}'",
                copied,
                source_metadata.len(),
                file.source
            ));
        }
    }
    Ok(())
}

/// True when a case navigates pages inside the TXT reader, so the small seeded
/// TXT would leave it on a single page and starve the navigation steps.
fn case_id_needs_multipage_txt(case_id: &str) -> bool {
    if case_id == TXT_TOUCH_PAGE_TURN_CASE_ID {
        return true;
    }
    const TXT_PAGING_MARKERS: &[&str] = &[
        "txt-reader-quick-settings",
        "frontlight-quick-settings",
        "txt-reader-touch-page-turn",
        "txt-swipe-page",
    ];
    TXT_PAGING_MARKERS
        .iter()
        .any(|marker| case_id.contains(marker))
}

fn seeded_recent_books_store() -> Vec<u8> {
    const MAGIC_MRCB: u32 = 0x4243_524d;
    const VERSION: u8 = 2;
    const PATH: &[u8] = b"/Books/sshpub.txt";
    const NAME: &[u8] = b"sshpub.txt";
    const PROGRESS_PERCENT: u32 = 0;
    const LAST_OPEN_MS: u64 = 1_700_000_000_000;

    let mut store = Vec::with_capacity(8 + 16 + PATH.len() + NAME.len());
    store.extend_from_slice(&MAGIC_MRCB.to_le_bytes());
    store.push(VERSION);
    store.push(0);
    store.extend_from_slice(&1u16.to_le_bytes());
    store.extend_from_slice(&(PATH.len() as u16).to_le_bytes());
    store.extend_from_slice(&(NAME.len() as u16).to_le_bytes());
    store.extend_from_slice(&PROGRESS_PERCENT.to_le_bytes());
    store.extend_from_slice(&LAST_OPEN_MS.to_le_bytes());
    store.extend_from_slice(PATH);
    store.extend_from_slice(NAME);
    store
}

fn seeded_library_root_epub_reading_index() -> Vec<u8> {
    // Mirrors LibraryCatalog v2: one EPUB with a valid page position but no
    // total-page count, as produced before EPUB layout is complete.
    const MAGIC: u32 = 0x4249_4c4d;
    const FOOTER_MAGIC: u32 = 0x3246_4c4d;
    const PATH: &[u8] = b"/Books/root-reading.epub";
    const TITLE: &[u8] = b"root-reading";
    const AUTHOR: &[u8] = b"";
    const TOTAL_PAGES: u32 = 0;
    const CURRENT_PAGE: u32 = 7;
    const LAST_READ_TIME: u64 = 1_700_000_000;
    const SOURCE_FINGERPRINT: u32 = 0;

    let record_bytes = 48 + PATH.len() + TITLE.len() + AUTHOR.len();
    let records_end = 16 + record_bytes;
    let mut catalog = Vec::with_capacity(records_end + 24);
    catalog.extend_from_slice(&MAGIC.to_le_bytes());
    catalog.push(2);
    catalog.push(0);
    catalog.extend_from_slice(&16u16.to_le_bytes());
    catalog.extend_from_slice(&0u64.to_le_bytes());
    catalog.extend_from_slice(&(record_bytes as u32).to_le_bytes());
    catalog.extend_from_slice(&(PATH.len() as u16).to_le_bytes());
    catalog.extend_from_slice(&(TITLE.len() as u16).to_le_bytes());
    catalog.extend_from_slice(&(AUTHOR.len() as u16).to_le_bytes());
    catalog.extend_from_slice(&0u16.to_le_bytes());
    catalog.extend_from_slice(&0u64.to_le_bytes());
    catalog.extend_from_slice(&TOTAL_PAGES.to_le_bytes());
    catalog.extend_from_slice(&CURRENT_PAGE.to_le_bytes());
    catalog.extend_from_slice(&LAST_READ_TIME.to_le_bytes());
    catalog.extend_from_slice(&SOURCE_FINGERPRINT.to_le_bytes());
    let mut path_hash = 1_469_598_103_934_665_603u64;
    for byte in PATH {
        path_hash ^= *byte as u64;
        path_hash = path_hash.wrapping_mul(1_099_511_628_211);
    }
    catalog.extend_from_slice(&path_hash.to_le_bytes());
    catalog.extend_from_slice(PATH);
    catalog.extend_from_slice(TITLE);
    catalog.extend_from_slice(AUTHOR);
    catalog.extend_from_slice(&0u32.to_le_bytes());
    catalog.extend_from_slice(&FOOTER_MAGIC.to_le_bytes());
    catalog.extend_from_slice(&1u32.to_le_bytes());
    catalog.extend_from_slice(&(records_end as u64).to_le_bytes());
    catalog.extend_from_slice(&(records_end as u64).to_le_bytes());
    catalog
}

fn seeded_reader_progress_epub() -> Vec<u8> {
    // ReaderProgress: MPRG v1, EPUB, page hint 7.
    let mut out = vec![0u8; 28];
    out[0..4].copy_from_slice(&0x4750_524Du32.to_le_bytes());
    out[4] = 1;
    out[5] = 1;
    out[16..20].copy_from_slice(&0u32.to_le_bytes());
    out[20..24].copy_from_slice(&6u32.to_le_bytes());
    out
}

fn reader_progress_path(book_path: &str) -> String {
    let mut hash = 2_166_136_261u32;
    for byte in book_path.bytes() {
        hash ^= byte as u32;
        hash = hash.wrapping_mul(16_777_619);
    }
    format!("progress/{hash:08x}.mprg")
}

fn provision_murphy_font_packs(sd_root: &Path, directory: &str, font_names: &[&str]) -> Result<()> {
    let source_dir = repo_root_path().join("apps/panda-os/host/tests/fixtures");
    let target_dir = sd_root.join(directory);
    fs::create_dir_all(&target_dir).with_context(|| {
        format!(
            "creating Panda AI OS font fixture dir {}",
            target_dir.display()
        )
    })?;
    for font_name in font_names {
        let source = source_dir.join(font_name);
        let target = target_dir.join(font_name);
        fs::copy(&source, &target).with_context(|| {
            format!(
                "copying Panda AI OS font fixture {} -> {}",
                source.display(),
                target.display()
            )
        })?;
    }
    Ok(())
}

fn provision_murphy_default_font_packs(sd_root: &Path) -> Result<()> {
    // FontStore discovers this canonical hidden directory first. Mirroring the
    // broad packs into /murphy/fonts doubles the payload and exceeds the
    // isolated 128 MiB E2E FAT image on font-picker cases.
    provision_murphy_font_packs(sd_root, ".murphy/fonts", &PANDA_DEFAULT_FONT_PACKS)
}

fn provision_mfp_fallback_case_font_packs(sd_root: &Path) -> Result<()> {
    // The case asserts the simulator's legacy fallback branch at its native
    // 26px role. It needs only this source-complete file, not a mirror of the
    // broad pack family that would exhaust the isolated FAT image.
    provision_murphy_font_packs(sd_root, "murphy/fonts", &PANDA_MFP_FALLBACK_CASE_PACKS)
}

fn seeded_epub_fixture_for_case(case_id: &str) -> (&'static str, &'static str) {
    if case_id.contains("image-heavy-epub") {
        return (
            FIXTURE_IMAGE_HEAVY_EPUB_PATH,
            SEEDED_IMAGE_HEAVY_EPUB_FILENAME,
        );
    }
    if case_id.contains("large-epub") {
        return (FIXTURE_LARGE_EPUB_PATH, SEEDED_LARGE_EPUB_FILENAME);
    }
    if case_id.contains("epub-touch-lock")
        || case_id.contains("layout-grid-epub-books")
        || case_id.contains("cover-card")
    {
        return (
            FIXTURE_RENDERABLE_EPUB_PATH,
            SEEDED_RENDERABLE_EPUB_FILENAME,
        );
    }

    (FIXTURE_EPUB_PATH, SEEDED_EPUB_FILENAME)
}

fn generated_epub_chapter_for_case(case_id: &str) -> (&'static str, &'static str, bool) {
    if case_id.contains("image-heavy-epub") {
        return (
            "JPEG Image Tests",
            r#"<h1>JPEG Image Tests</h1>
<p>This generated simulator EPUB contains an embedded JPEG fixture.</p>
<p><img src="images/sample.jpg" alt="JPEG sample"/></p>
<p>The reader should render this page without crashing.</p>"#,
            true,
        );
    }
    if case_id.contains("large-epub") {
        return (
            "Table Fixture",
            r#"<h1>Table Fixture</h1>
<p>This generated simulator EPUB includes a small table-heavy section.</p>
<table>
<tr><th>Column A</th><th>Column B</th><th>Column C</th></tr>
<tr><td>Alpha</td><td>Beta</td><td>Gamma</td></tr>
<tr><td>Delta</td><td>Epsilon</td><td>Zeta</td></tr>
<tr><td>Eta</td><td>Theta</td><td>Iota</td></tr>
</table>
<p>Additional body text keeps the page visually non-empty after parsing.</p>"#,
            false,
        );
    }
    (
        "Display None Fixture",
        r#"<h1>Display None Fixture</h1>
<p style="display:none">This hidden paragraph must not dominate rendering.</p>
<p>Visible EPUB content for simulator FileBrowser reader flows.</p>
<p>The fixture is generated when ignored test EPUB artifacts are absent.</p>"#,
        false,
    )
}

fn crc32(bytes: &[u8]) -> u32 {
    let mut crc = 0xffff_ffffu32;
    for byte in bytes {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            let mask = 0u32.wrapping_sub(crc & 1);
            crc = (crc >> 1) ^ (0xedb8_8320 & mask);
        }
    }
    !crc
}

fn push_le16(out: &mut Vec<u8>, value: u16) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_le32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_zip_entry(
    out: &mut Vec<u8>,
    central_directory: &mut Vec<(String, u32, u32, u32, u32)>,
    name: &str,
    data: &[u8],
) -> Result<()> {
    let offset = u32::try_from(out.len()).context("generated EPUB offset exceeds ZIP32")?;
    let name_bytes = name.as_bytes();
    let name_len = u16::try_from(name_bytes.len()).context("generated EPUB ZIP name too long")?;
    let data_len = u32::try_from(data.len()).context("generated EPUB entry too large")?;
    let crc = crc32(data);

    push_le32(out, 0x0403_4b50);
    push_le16(out, 20);
    push_le16(out, 0);
    push_le16(out, 0);
    push_le16(out, 0);
    push_le16(out, 33);
    push_le32(out, crc);
    push_le32(out, data_len);
    push_le32(out, data_len);
    push_le16(out, name_len);
    push_le16(out, 0);
    out.extend_from_slice(name_bytes);
    out.extend_from_slice(data);

    central_directory.push((name.to_string(), crc, data_len, data_len, offset));
    Ok(())
}

fn generated_epub_bytes(case_id: &str) -> Result<Vec<u8>> {
    let (title, chapter_body, include_jpeg) = generated_epub_chapter_for_case(case_id);
    let escaped_title = title;
    let chapter = format!(
        r#"<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>{escaped_title}</title></head>
<body>{chapter_body}</body>
</html>"#
    );
    let opf = format!(
        r#"<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="book-id" version="2.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="book-id">urn:uuid:simulator-generated-{}</dc:identifier>
    <dc:title>{escaped_title}</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    {}
  </manifest>
  <spine toc="ncx"><itemref idref="chapter"/></spine>
</package>"#,
        case_id.replace(|c: char| !c.is_ascii_alphanumeric(), "-"),
        if include_jpeg {
            r#"<item id="sample-jpeg" href="images/sample.jpg" media-type="image/jpeg"/>"#
        } else {
            ""
        }
    );
    let ncx = format!(
        r#"<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head><meta name="dtb:uid" content="urn:uuid:simulator-generated"/></head>
  <docTitle><text>{escaped_title}</text></docTitle>
  <navMap>
    <navPoint id="chapter" playOrder="1">
      <navLabel><text>{escaped_title}</text></navLabel>
      <content src="chapter.xhtml"/>
    </navPoint>
  </navMap>
</ncx>"#
    );

    let mut out = Vec::new();
    let mut central_directory = Vec::new();
    push_zip_entry(
        &mut out,
        &mut central_directory,
        "mimetype",
        b"application/epub+zip",
    )?;
    push_zip_entry(
        &mut out,
        &mut central_directory,
        "META-INF/container.xml",
        br#"<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>"#,
    )?;
    push_zip_entry(
        &mut out,
        &mut central_directory,
        "OEBPS/content.opf",
        opf.as_bytes(),
    )?;
    push_zip_entry(
        &mut out,
        &mut central_directory,
        "OEBPS/toc.ncx",
        ncx.as_bytes(),
    )?;
    push_zip_entry(
        &mut out,
        &mut central_directory,
        "OEBPS/chapter.xhtml",
        chapter.as_bytes(),
    )?;
    if include_jpeg {
        push_zip_entry(
            &mut out,
            &mut central_directory,
            "OEBPS/images/sample.jpg",
            &[
                0xff, 0xd8, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x08, 0x06, 0x06, 0x07, 0x06, 0x05, 0x08,
                0x07, 0x07, 0x07, 0x09, 0x09, 0x08, 0x0a, 0x0c, 0x14, 0x0d, 0x0c, 0x0b, 0x0b, 0x0c,
                0x19, 0x12, 0x13, 0x0f, 0x14, 0x1d, 0x1a, 0x1f, 0x1e, 0x1d, 0x1a, 0x1c, 0x1c, 0x20,
                0x24, 0x2e, 0x27, 0x20, 0x22, 0x2c, 0x23, 0x1c, 0x1c, 0x28, 0x37, 0x29, 0x2c, 0x30,
                0x31, 0x34, 0x34, 0x34, 0x1f, 0x27, 0x39, 0x3d, 0x38, 0x32, 0x3c, 0x2e, 0x33, 0x34,
                0x32, 0xff, 0xc0, 0x00, 0x0b, 0x08, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x11, 0x00,
                0xff, 0xc4, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3f, 0x00, 0x37, 0xff,
                0xd9,
            ],
        )?;
    }

    let central_start = u32::try_from(out.len())
        .context("generated EPUB central directory offset exceeds ZIP32")?;
    for (name, crc, compressed_size, uncompressed_size, local_offset) in &central_directory {
        let name_bytes = name.as_bytes();
        let name_len =
            u16::try_from(name_bytes.len()).context("generated EPUB ZIP name too long")?;
        push_le32(&mut out, 0x0201_4b50);
        push_le16(&mut out, 20);
        push_le16(&mut out, 20);
        push_le16(&mut out, 0);
        push_le16(&mut out, 0);
        push_le16(&mut out, 0);
        push_le16(&mut out, 33);
        push_le32(&mut out, *crc);
        push_le32(&mut out, *compressed_size);
        push_le32(&mut out, *uncompressed_size);
        push_le16(&mut out, name_len);
        push_le16(&mut out, 0);
        push_le16(&mut out, 0);
        push_le16(&mut out, 0);
        push_le16(&mut out, 0);
        push_le32(&mut out, 0);
        push_le32(&mut out, *local_offset);
        out.extend_from_slice(name_bytes);
    }
    let central_size = u32::try_from(out.len())
        .context("generated EPUB central directory size exceeds ZIP32")?
        - central_start;
    let entry_count = u16::try_from(central_directory.len())
        .context("generated EPUB has too many ZIP entries")?;
    push_le32(&mut out, 0x0605_4b50);
    push_le16(&mut out, 0);
    push_le16(&mut out, 0);
    push_le16(&mut out, entry_count);
    push_le16(&mut out, entry_count);
    push_le32(&mut out, central_size);
    push_le32(&mut out, central_start);
    push_le16(&mut out, 0);
    Ok(out)
}

fn copy_or_generate_epub_fixture(source: &Path, target: &Path, case_id: &str) -> Result<()> {
    if source.exists() {
        fs::copy(source, target).with_context(|| {
            format!(
                "copying simulator EPUB fixture {} -> {}",
                source.display(),
                target.display()
            )
        })?;
        return Ok(());
    }

    // The generated fallback is a single short chapter, i.e. a ONE-page book.
    // Cases that page forward, cross spines, or assert multi-page navigation
    // cannot reach their asserted state on it and fail with misleading
    // symptoms (they look like Reader/back-navigation defects). Fail loudly so a
    // missing gitignored fixture is never mistaken for a product regression.
    // Provision test/epubs/ the same way as .mofei-font-source/ and
    // docs/design/gb/; the blobs are recoverable from git history.
    if case_requires_multipage_epub(case_id) {
        anyhow::bail!(
            "simulator EPUB fixture missing: {}\n  case {} asserts multi-page navigation, and the \
             generated fallback is a single-page book.\n  Restore the gitignored fixtures under \
             test/epubs/ before running this case.",
            source.display(),
            case_id
        );
    }

    let bytes = generated_epub_bytes(case_id)?;
    fs::write(target, bytes).with_context(|| {
        format!(
            "writing generated simulator EPUB fixture {} for missing source {}",
            target.display(),
            source.display()
        )
    })?;
    Ok(())
}

/// True when a case drives page-forward / spine-crossing navigation, so the
/// single-page generated fallback cannot satisfy it.
fn case_requires_multipage_epub(case_id: &str) -> bool {
    const MULTIPAGE_MARKERS: &[&str] = &[
        "page-forward",
        "page-turn",
        "overflow-second-page",
        "overlay-button-nav",
        "large-epub",
        "percent",
        "chapter",
        "search-results",
    ];
    MULTIPAGE_MARKERS
        .iter()
        .any(|marker| case_id.contains(marker))
}

fn seeded_opds_servers_json_for_case(case_id: &str) -> &'static str {
    if case_id.contains("opds-browser-empty-feed") {
        return SEEDED_OPDS_EMPTY_SERVERS_JSON;
    }
    if case_id.contains("opds-browser-malformed-feed") {
        return SEEDED_OPDS_MALFORMED_SERVERS_JSON;
    }
    if case_id.contains("opds-browser-fetch-failed") {
        return SEEDED_OPDS_FETCH_FAILED_SERVERS_JSON;
    }
    SEEDED_OPDS_SERVERS_JSON
}

fn append_book_cache_string(out: &mut Vec<u8>, value: &str) {
    out.extend_from_slice(&(value.len() as u32).to_le_bytes());
    out.extend_from_slice(value.as_bytes());
}

fn seeded_renderable_epub_book_cache() -> Vec<u8> {
    const TITLE: &str = "Kerning & Ligature Edge Cases";
    const AUTHOR: &str = "Panda Test Fixtures";
    const LANGUAGE: &str = "en";
    const COVER_HREF: &str = "OEBPS/cover.jpg";
    const TEXT_HREF: &str = "OEBPS/cover.xhtml";
    const TOC_TITLE: &str = "Cover";

    let spine_count: u16 = 1;
    let toc_count: u16 = 1;
    let metadata_size = [TITLE, AUTHOR, LANGUAGE, COVER_HREF, TEXT_HREF]
        .iter()
        .map(|value| 4 + value.len() as u32)
        .sum::<u32>();
    let header_size = 1 + 4 + 2 + 2;
    let lut_offset = header_size + metadata_size;
    let lut_size = 4 * (spine_count as u32 + toc_count as u32);
    let spine_pos = lut_offset + lut_size;
    let spine_data_size = 4 + TEXT_HREF.len() as u32 + 4 + 2;
    let toc_pos = spine_pos + spine_data_size;

    let mut out = Vec::new();
    out.push(BOOK_METADATA_CACHE_VERSION);
    out.extend_from_slice(&lut_offset.to_le_bytes());
    out.extend_from_slice(&spine_count.to_le_bytes());
    out.extend_from_slice(&toc_count.to_le_bytes());
    append_book_cache_string(&mut out, TITLE);
    append_book_cache_string(&mut out, AUTHOR);
    append_book_cache_string(&mut out, LANGUAGE);
    append_book_cache_string(&mut out, COVER_HREF);
    append_book_cache_string(&mut out, TEXT_HREF);
    out.extend_from_slice(&spine_pos.to_le_bytes());
    out.extend_from_slice(&toc_pos.to_le_bytes());
    append_book_cache_string(&mut out, TEXT_HREF);
    out.extend_from_slice(&356u32.to_le_bytes());
    out.extend_from_slice(&0i16.to_le_bytes());
    append_book_cache_string(&mut out, TOC_TITLE);
    append_book_cache_string(&mut out, TEXT_HREF);
    append_book_cache_string(&mut out, "");
    out.push(1u8);
    out.extend_from_slice(&0i16.to_le_bytes());
    out
}

fn run_toolchain_fixture_command(
    toolchain_dir: &Path,
    args: &[&str],
    environment: &[(&str, &str)],
) -> Result<()> {
    let mut command = std::process::Command::new("bun");
    command
        .current_dir(toolchain_dir)
        .arg("src/cli.ts")
        .args(args)
        .envs(environment.iter().copied());
    let output = command.output().with_context(|| {
        format!(
            "running Panda toolchain fixture command in {}: bun src/cli.ts {}",
            toolchain_dir.display(),
            args.join(" ")
        )
    })?;
    if !output.status.success() {
        return Err(anyhow!(
            "Panda toolchain fixture command failed: bun src/cli.ts {}\nstatus: {}\nstdout:\n{}\nstderr:\n{}",
            args.join(" "),
            output.status,
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        ));
    }
    Ok(())
}

#[derive(Deserialize)]
struct ToolchainFixtureKey {
    #[serde(rename = "privateKeyPem")]
    private_key_pem: String,
    #[serde(rename = "publicKeyPem")]
    public_key_pem: String,
}

fn read_toolchain_fixture_key(path: &Path) -> Result<ToolchainFixtureKey> {
    let text = fs::read_to_string(path)
        .with_context(|| format!("reading Panda fixture key {}", path.display()))?;
    serde_json::from_str(&text)
        .with_context(|| format!("parsing Panda fixture key {}", path.display()))
}

#[derive(Clone, Copy)]
struct PandaLuaFixtureApp {
    template_name: Option<&'static str>,
    example_dir: Option<&'static str>,
    app_dir_name: &'static str,
    signed_package_name: &'static str,
}

const PANDA_HELLO_FIXTURE_APP: PandaLuaFixtureApp = PandaLuaFixtureApp {
    template_name: Some("hello"),
    example_dir: None,
    app_dir_name: "hello",
    signed_package_name: "hello.signed.pap",
};

const PANDA_COUNTER_FIXTURE_APP: PandaLuaFixtureApp = PandaLuaFixtureApp {
    template_name: None,
    example_dir: Some("apps/examples/counter"),
    app_dir_name: "counter",
    signed_package_name: "counter.signed.pap",
};

fn panda_lua_fixture_app_for_case(case_id: &str) -> Option<PandaLuaFixtureApp> {
    if !case_id.contains("panda-apps-lua-") {
        return None;
    }
    if case_id.contains("counter") {
        Some(PANDA_COUNTER_FIXTURE_APP)
    } else {
        Some(PANDA_HELLO_FIXTURE_APP)
    }
}

fn copy_dir_recursive(source: &Path, target: &Path) -> Result<()> {
    fs::create_dir_all(target)
        .with_context(|| format!("creating recursive copy target {}", target.display()))?;
    for entry in fs::read_dir(source)
        .with_context(|| format!("reading recursive copy source {}", source.display()))?
    {
        let entry = entry
            .with_context(|| format!("reading recursive copy entry under {}", source.display()))?;
        let file_type = entry.file_type().with_context(|| {
            format!(
                "reading recursive copy entry type {}",
                entry.path().display()
            )
        })?;
        let target_path = target.join(entry.file_name());
        if file_type.is_dir() {
            copy_dir_recursive(&entry.path(), &target_path)?;
        } else if file_type.is_file() {
            fs::copy(entry.path(), &target_path).with_context(|| {
                format!(
                    "copying recursive fixture source {} -> {}",
                    entry.path().display(),
                    target_path.display()
                )
            })?;
        }
    }
    Ok(())
}

fn seed_panda_lua_app_package(sd_root: &Path, fixture: PandaLuaFixtureApp) -> Result<()> {
    let repo_root = repo_root_path();
    let toolchain_dir = repo_root.join("apps/toolchain");
    let fixture_root = std::env::temp_dir().join(format!(
        "mofei-panda-e2e-fixture-{}-{}-{}",
        std::process::id(),
        fixture.app_dir_name,
        stable_socket_suffix_hash(&sd_root.to_string_lossy())
    ));
    let app_dir = fixture_root.join(fixture.app_dir_name);
    let root_key_path = fixture_root.join("root-key.json");
    let key_path = fixture_root.join("key.json");
    let package_path = fixture_root.join(format!("{}.pap", fixture.app_dir_name));
    let signed_package_path = fixture_root.join(fixture.signed_package_name);

    if fixture_root.exists() {
        fs::remove_dir_all(&fixture_root).with_context(|| {
            format!("resetting Panda app fixture dir {}", fixture_root.display())
        })?;
    }
    fs::create_dir_all(&fixture_root)
        .with_context(|| format!("creating Panda app fixture dir {}", fixture_root.display()))?;

    if let Some(template_name) = fixture.template_name {
        run_toolchain_fixture_command(
            &toolchain_dir,
            &[
                "new",
                template_name,
                app_dir
                    .to_str()
                    .ok_or_else(|| anyhow!("non-UTF8 Panda app fixture path"))?,
            ],
            &[],
        )?;
    } else if let Some(example_dir) = fixture.example_dir {
        copy_dir_recursive(&repo_root.join(example_dir), &app_dir)?;
    }
    run_toolchain_fixture_command(
        &toolchain_dir,
        &[
            "pack",
            app_dir
                .to_str()
                .ok_or_else(|| anyhow!("non-UTF8 Panda app fixture path"))?,
            "--out",
            package_path
                .to_str()
                .ok_or_else(|| anyhow!("non-UTF8 Panda package fixture path"))?,
        ],
        &[],
    )?;
    run_toolchain_fixture_command(
        &toolchain_dir,
        &[
            "keygen",
            "--out",
            root_key_path
                .to_str()
                .ok_or_else(|| anyhow!("non-UTF8 Panda root fixture path"))?,
            "--id",
            "root.simulator.e2e",
        ],
        &[],
    )?;
    let root_key = read_toolchain_fixture_key(&root_key_path)?;
    run_toolchain_fixture_command(
        &toolchain_dir,
        &[
            "keygen",
            "--out",
            key_path
                .to_str()
                .ok_or_else(|| anyhow!("non-UTF8 Panda key fixture path"))?,
            "--id",
            "dev.simulator.e2e",
        ],
        &[],
    )?;
    let trust_environment = [
        (
            "PANDA_INTERNAL_ROOT_PRIVATE_KEY_PEM",
            root_key.private_key_pem.as_str(),
        ),
        (
            "PANDA_TRUST_ROOT_PUBLIC_KEY_PEM",
            root_key.public_key_pem.as_str(),
        ),
    ];
    run_toolchain_fixture_command(
        &toolchain_dir,
        &[
            "sign",
            package_path
                .to_str()
                .ok_or_else(|| anyhow!("non-UTF8 Panda package fixture path"))?,
            "--key",
            key_path
                .to_str()
                .ok_or_else(|| anyhow!("non-UTF8 Panda key fixture path"))?,
            "--out",
            signed_package_path
                .to_str()
                .ok_or_else(|| anyhow!("non-UTF8 Panda signed package fixture path"))?,
        ],
        &trust_environment,
    )?;
    run_toolchain_fixture_command(
        &toolchain_dir,
        &[
            "install",
            signed_package_path
                .to_str()
                .ok_or_else(|| anyhow!("non-UTF8 Panda signed package fixture path"))?,
            "--target",
            sd_root
                .to_str()
                .ok_or_else(|| anyhow!("non-UTF8 simulator SD root path"))?,
        ],
        &trust_environment,
    )?;
    sync_visible_system_dir(sd_root)?;
    fs::remove_dir_all(&fixture_root)
        .with_context(|| format!("removing Panda app fixture dir {}", fixture_root.display()))?;
    Ok(())
}

fn seed_case_specific_sd_fixtures(case: &E2ECase, sd_root: &Path) -> Result<()> {
    let case_id = case.id.trim().to_ascii_lowercase();
    let needs_books = case_id.starts_with(STORAGE_DIAGNOSTIC_CASE_PREFIX)
        || case_id.contains("file-browser")
        || case_id.contains("recent-reading")
        || case_id.contains("reader-frontlight");
    let needs_epub = case_id.contains("epub")
        || case_id.contains("settings-reader")
        || case_id.contains("ttf")
        || case_id.contains("traditional-chinese-fonts")
        || case_id.contains("status-bar");
    let needs_empty_txt = case_id.contains("empty-txt");
    let needs_recent = case_id.contains("recent-reading")
        && case_id != "dashboard-recent-reading-txt-open-back-smoke";
    let needs_ttf = case_id.contains("ttf");
    let needs_settings = case_id.starts_with("dashboard-settings-");
    let needs_opds_browser_entry = case_id.contains("opds-browser-");
    let needs_seeded_study = case_id.contains("study-seeded");
    let needs_large_canonical_study = case_id.contains("study-large-canonical");
    let panda_lua_fixture_app = panda_lua_fixture_app_for_case(&case_id);
    let needs_panda_lua_app = panda_lua_fixture_app.is_some();
    let needs_tc_benchmark = case_id.contains("tc-benchmark");
    let needs_persisted_grid_settings = case_id.contains("persist-verifier");
    let needs_grid_browser_prefs = case_id.contains("cover-card") || needs_persisted_grid_settings;
    let needs_header_grid_toggle = case_id.contains("layout-grid-epub-books");
    let needs_renderable_epub_cache = needs_grid_browser_prefs || needs_header_grid_toggle;
    let needs_library_root_epub_reading_filter =
        case_id.contains("library-root-epub-reading-filter");
    let preserve_settings = needs_persisted_grid_settings
        && simulator_system_fixture_exists(sd_root, Path::new("settings.json"));

    if !needs_books
        && !needs_epub
        && !needs_empty_txt
        && !needs_recent
        && !needs_ttf
        && !needs_settings
        && !needs_opds_browser_entry
        && !needs_seeded_study
        && !needs_large_canonical_study
        && !needs_panda_lua_app
        && !needs_tc_benchmark
    {
        return Ok(());
    }

    let settings_json = if needs_ttf {
        SEEDED_TTF_PICKER_SETTINGS_JSON
    } else if needs_grid_browser_prefs {
        SEEDED_READER_GRID_SETTINGS_JSON
    } else {
        SEEDED_READER_SETTINGS_JSON
    };
    if !preserve_settings {
        write_simulator_system_fixture(sd_root, Path::new("settings.json"), settings_json)?;
    }
    if needs_grid_browser_prefs {
        write_murphy_system_fixture(sd_root, Path::new("browser.bin"), SEEDED_BROWSER_GRID_PREFS)?;
    } else if needs_header_grid_toggle {
        write_murphy_system_fixture(sd_root, Path::new("browser.bin"), SEEDED_BROWSER_LIST_PREFS)?;
    }

    if needs_books {
        let books_dir = sd_root.join("Books");
        fs::create_dir_all(&books_dir)
            .with_context(|| format!("creating simulator Books dir {}", books_dir.display()))?;
        let txt_path = books_dir.join("sshpub.txt");
        // Any case that pages within the TXT reader needs a genuinely
        // multi-page document. A 2-line file yields a single page, so nav steps
        // cannot advance and the case fails later on an unrelated assertion
        // (typically the Back -> filebrowser transition), which reads as a
        // Reader defect rather than a fixture shortfall.
        let txt_contents = if case_id_needs_multipage_txt(case_id.as_str()) {
            (0..120)
                .map(|index| {
                    format!(
                        "Simulator E2E touch page-turn fixture line {:03}. This longer line keeps the TXT reader on a real multi-page document.\n",
                        index + 1
                    )
                })
                .collect::<String>()
        } else {
            "Simulator E2E fixture.\nThis small TXT file proves reader navigation can open seeded SD content.\n"
                .to_string()
        };
        fs::write(&txt_path, txt_contents)
            .with_context(|| format!("writing simulator TXT fixture {}", txt_path.display()))?;
        if case_id.contains("mfp-fallback") {
            provision_mfp_fallback_case_font_packs(sd_root)?;
            let fallback_path = books_dir.join(SEEDED_MFP_FALLBACK_FILENAME);
            fs::write(&fallback_path, "㐀 simulator MFP fallback fixture.\n").with_context(
                || {
                    format!(
                        "writing simulator dynamic MFP fallback fixture {}",
                        fallback_path.display()
                    )
                },
            )?;
        }
        if needs_empty_txt {
            let empty_path = books_dir.join(SEEDED_EMPTY_TXT_FILENAME);
            fs::write(&empty_path, "").with_context(|| {
                format!(
                    "writing simulator empty TXT fixture {}",
                    empty_path.display()
                )
            })?;
        }
    }

    if needs_epub {
        let (fixture_epub_path, seeded_epub_filename) = seeded_epub_fixture_for_case(&case_id);
        let fixture_epub_path = repo_relative_fixture_path(fixture_epub_path);
        let epub_path = sd_root.join("Books").join(seeded_epub_filename);
        let parent = epub_path
            .parent()
            .ok_or_else(|| anyhow!("simulator EPUB fixture path has no parent"))?;
        fs::create_dir_all(parent)
            .with_context(|| format!("creating simulator Books dir {}", parent.display()))?;
        copy_or_generate_epub_fixture(&fixture_epub_path, &epub_path, &case_id)?;
        if needs_renderable_epub_cache {
            write_simulator_system_fixture(
                sd_root,
                Path::new(SEEDED_RENDERABLE_EPUB_CACHE_PATH),
                seeded_renderable_epub_book_cache(),
            )?;
        }
    }

    if needs_library_root_epub_reading_filter {
        let (fixture_epub_path, _) = seeded_epub_fixture_for_case(&case_id);
        let fixture_epub_path = repo_relative_fixture_path(fixture_epub_path);
        // Library production discovery is rooted at /Books; keep the seeded
        // file and persisted catalog path aligned with that contract.
        let epub_path = sd_root.join("Books").join("root-reading.epub");
        copy_or_generate_epub_fixture(&fixture_epub_path, &epub_path, &case_id)?;
        write_murphy_system_fixture(
            sd_root,
            Path::new("library.bin"),
            seeded_library_root_epub_reading_index(),
        )?;
        write_murphy_system_fixture(
            sd_root,
            Path::new(&reader_progress_path("/Books/root-reading.epub")),
            seeded_reader_progress_epub(),
        )?;
    }

    if needs_tc_benchmark {
        let fixture_epub_path = repo_relative_fixture_path(FIXTURE_TC_EPUB_PATH);
        let epub_path = sd_root.join("Books").join(SEEDED_TC_EPUB_FILENAME);
        let parent = epub_path
            .parent()
            .ok_or_else(|| anyhow!("simulator TC EPUB fixture path has no parent"))?;
        fs::create_dir_all(parent)
            .with_context(|| format!("creating simulator Books dir {}", parent.display()))?;
        copy_or_generate_epub_fixture(&fixture_epub_path, &epub_path, &case_id)?;
    }

    if needs_ttf {
        let fixture_ttf_path = FIXTURE_TTF_CANDIDATES
            .iter()
            .map(|path| repo_relative_fixture_path(path))
            .find(|path| path.exists())
            .ok_or_else(|| anyhow!("no simulator fixture TTF candidate exists"))?;
        let font_contents = fs::read(&fixture_ttf_path).with_context(|| {
            format!(
                "reading simulator TTF fixture {}",
                fixture_ttf_path.display()
            )
        })?;
        write_simulator_system_fixture(sd_root, Path::new("fonts/reader.ttf"), font_contents)?;
    }

    if needs_recent {
        write_murphy_system_fixture(
            sd_root,
            Path::new("recents.bin"),
            seeded_recent_books_store(),
        )?;
    }
    if needs_opds_browser_entry {
        write_simulator_system_fixture(
            sd_root,
            Path::new("opds.json"),
            seeded_opds_servers_json_for_case(&case_id),
        )?;
        write_simulator_system_fixture(
            sd_root,
            Path::new("dashboard/shortcuts.json"),
            SEEDED_OPDS_DASHBOARD_SHORTCUTS_JSON,
        )?;
    }
    if needs_seeded_study {
        let deck_path = sd_root.join("Study/Decks/seeded_deck.json");
        fs::create_dir_all(deck_path.parent().expect("seeded deck path has parent"))
            .with_context(|| format!("creating seeded study deck dir {}", deck_path.display()))?;
        fs::write(&deck_path, SEEDED_STUDY_DECK_JSON)
            .with_context(|| format!("writing seeded study deck {}", deck_path.display()))?;
        write_murphy_system_fixture(
            sd_root,
            Path::new("study/queue.msq"),
            seeded_study_review_queue_msq(),
        )?;
        write_murphy_system_fixture(
            sd_root,
            Path::new("study/state.mst"),
            seeded_study_state_mst(),
        )?;
    }
    if needs_large_canonical_study {
        // QEMU 的 FAT 目录桥不能可靠地在运行期创建多层目录；这里只预先铺好
        // Study 启动契约需要的目录。卡组统一由串口 fixture 安装，避免预置文件
        // 与覆盖写入在 FAT 镜像中同时被扫描成两份卡组。
        for required_dir in [
            "Study/Courses",
            "Study/Decks",
            ".murphy/study",
            "murphy/study",
        ] {
            let path = sd_root.join(required_dir);
            fs::create_dir_all(&path)
                .with_context(|| format!("creating large Study fixture dir {}", path.display()))?;
        }
    }
    if let Some(fixture) = panda_lua_fixture_app {
        seed_panda_lua_app_package(sd_root, fixture)?;
    }
    Ok(())
}

async fn run_e2e_case(
    args: &Args,
    case: E2ECase,
    debug_writer: debug_transport::HeadlessDebugWriter,
    debug_broker: debug_transport::DebugTransportBroker,
) -> Result<CaseRunOutcome> {
    let board = load_board_runtime(args.board.as_deref())?;
    let reference_viewport = case.reference_viewport.validate()?;
    let artifacts_dir = args.artifacts.clone();
    fs::create_dir_all(&artifacts_dir)
        .with_context(|| format!("creating artifacts dir {}", artifacts_dir.display()))?;
    eprintln!("[e2e] artifacts dir: {}", artifacts_dir.display());
    eprintln!("[e2e] case: {} — {}", case.id, case.description);
    eprintln!("[e2e] steps: {}", case.steps.len());
    if !case.before_boot.is_empty() {
        eprintln!("[e2e] beforeBoot steps: {}", case.before_boot.len());
    }
    if !case.boot.copy_files.is_empty() {
        eprintln!(
            "[e2e] boot.copyFiles SD seeds: {}",
            case.boot.copy_files.len()
        );
    }
    eprintln!(
        "[e2e] reference viewport: {}x{}",
        reference_viewport.width, reference_viewport.height
    );

    if let Some(skip_reason) = case_skip_reason(&case, &board) {
        if args.run_skipped {
            eprintln!(
                "[e2e] running skipped case because --run-skipped was set: {} — {}",
                case.id, skip_reason
            );
        } else {
            let case_result = CaseResult {
                case_id: case.id.clone(),
                status: "SKIP".to_string(),
                error: Some(skip_reason.clone()),
                steps: Vec::new(),
                total_elapsed_ms: 0,
                screenshot: artifacts_dir
                    .join("screenshot.png")
                    .to_string_lossy()
                    .to_string(),
                transcript: artifacts_dir
                    .join("transcript.log")
                    .to_string_lossy()
                    .to_string(),
                firmware: args.firmware.to_string_lossy().to_string(),
            };
            persist_e2e_artifacts(
                &artifacts_dir,
                &case_result,
                &[],
                &vec![0xFFu8; board.geometry.fb_bytes()],
                &board.geometry,
            )?;
            eprintln!("[e2e] CASE SKIPPED: {} — {}", case.id, skip_reason);
            return Ok(CaseRunOutcome {
                case_id: case.id,
                status: "SKIP".to_string(),
                artifacts_dir,
                error: Some(skip_reason),
            });
        }
    }

    let ctx = std::sync::Arc::new(tokio::sync::Mutex::new(E2EContext::new()));

    let firmware_path = args.firmware.clone();

    let monitor_socket = args.socket.with_extension("monitor.sock");
    cleanup_stale_socket(&args.socket);
    cleanup_stale_socket(&monitor_socket);

    let socket_arg = format!(
        "socket,id=mofei,path={},server=on,wait=off",
        args.socket.display()
    );
    let monitor_arg = format!("unix:{},server=on,wait=off", monitor_socket.display());
    let sd_root = e2e_case_sd_root(args, &artifacts_dir)?;
    eprintln!("[e2e] simulator SD root: {}", sd_root.display());
    provision_murphy_default_font_packs(&sd_root)?;
    seed_boot_copy_files(&case, &sd_root)?;
    seed_case_specific_sd_fixtures(&case, &sd_root)?;
    let _simulator_location = write_simulator_location(&sd_root)?;
    sync_visible_system_dir(&sd_root)?;
    let sd_drive = e2e_sd_image_drive(&sd_root, &artifacts_dir)?;

    let mut qemu_cmd = Command::new(&args.qemu);
    add_qemu_pc_bios_dir(&mut qemu_cmd, &args.qemu);
    add_qemu_rom_env(&mut qemu_cmd, &args.qemu);
    add_qemu_heap_mode_env(&mut qemu_cmd, args.heap_mode);
    qemu_cmd.env("MOFEI_SIM_BOARD", &board.id);
    qemu_cmd.arg("-machine").arg("esp32s3");
    qemu_cmd.arg("-smp").arg("1");

    let symbol_elf_path = qemu_symbol_elf_path(&firmware_path)?;
    eprintln!("[e2e] firmware image: {}", firmware_path.display());
    eprintln!("[e2e] QEMU symbol ELF: {}", symbol_elf_path.display());
    qemu_cmd.env("MOFEI_FIRMWARE_ELF", &symbol_elf_path);

    if firmware_path.extension().and_then(|s| s.to_str()) == Some("bin") {
        let flash_img = build_flash_image(&firmware_path, Some(&artifacts_dir.join("flash.img")))?;
        qemu_cmd
            .arg("-drive")
            .arg(format!("file={},if=mtd,format=raw", flash_img.display()));
        qemu_cmd.arg("-kernel").arg(&symbol_elf_path);
        let qemu_dir = args.qemu.parent().unwrap_or(Path::new("."));
        let rom_src = qemu_dir.join("../pc-bios/esp32s3_rev0_rom.bin");
        if rom_src.exists() {
            let mut rom_data = std::fs::read(&rom_src)
                .with_context(|| format!("reading ROM from {}", rom_src.display()))?;
            patch_rom_spi_panic(&mut rom_data);
            let patched_rom = artifacts_dir.join("esp32s3_rev0_rom.patched.bin");
            std::fs::write(&patched_rom, rom_data)?;
            qemu_cmd.env("MOFEI_ROM_BINARY", &patched_rom);
        }
    } else if firmware_path.extension().and_then(|s| s.to_str()) == Some("elf") {
        qemu_cmd.arg("-kernel").arg(&symbol_elf_path);
    } else {
        return Err(anyhow!("Expected firmware.bin or firmware.elf"));
    }

    qemu_cmd.arg("-drive").arg(sd_drive.qemu_arg);

    let child = qemu_cmd
        .arg("-display")
        .arg("none")
        .arg("-serial")
        .arg("stdio")
        .arg("-S")
        .arg("-monitor")
        .arg(&monitor_arg)
        .arg("-semihosting-config")
        .arg("enable=on,target=native")
        .arg("-chardev")
        .arg(&socket_arg)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .with_context(|| format!("spawning {}", args.qemu.display()))?;
    let mut qemu = QemuChildGuard::new(child, args.socket.clone(), monitor_socket.clone());

    let (event_tx, mut event_rx) = mpsc::unbounded_channel::<HeadlessEvent>();
    let stderr = qemu
        .child
        .as_mut()
        .ok_or_else(|| anyhow!("QEMU child missing"))?
        .stderr
        .take()
        .ok_or_else(|| anyhow!("QEMU stderr pipe unavailable"))?;
    let stdout = qemu
        .child
        .as_mut()
        .ok_or_else(|| anyhow!("QEMU child missing"))?
        .stdout
        .take()
        .ok_or_else(|| anyhow!("QEMU stdout pipe unavailable"))?;
    let taps: Vec<TouchTap> = Vec::new();
    let stderr_handle = tokio::spawn(read_qemu_stderr(
        stderr,
        event_tx.clone(),
        taps,
        Some(ctx.clone()),
    ));
    let ctx_clone = ctx.clone();
    let stdout_handle = tokio::spawn(read_qemu_stdout(stdout, ctx_clone, event_tx.clone()));

    let (injection_tx, injection_rx) = mpsc::unbounded_channel::<InjectionCommand>();

    let stream = connect_unix_socket(&args.socket, 120, Duration::from_millis(100))
        .await
        .with_context(|| format!("connecting ipc socket {}", args.socket.display()))?;
    let (read_half, write_half) = stream.into_split();
    let (outbound_tx, outbound_rx) = mpsc::unbounded_channel::<debug_transport::OutFrame>();
    debug_writer.bind(outbound_tx).await;
    let (radio_peer, _radio_peer_handle) =
        start_radio_peer_bridge(args, debug_writer.clone()).await?;
    let writer_handle = tokio::spawn(async move {
        if let Err(err) = write_loop(write_half, outbound_rx).await {
            eprintln!("[headless] IPC write loop stopped: {err}");
        }
    });

    let injection_plan = InjectionPlan {
        taps: Vec::new(),
        inject_buttons: Vec::new(),
        button_hold_ms: args.button_hold_ms,
        post_tap_delay_ms: args.post_tap_delay_ms,
        post_tap_frames_before_buttons: args.post_tap_frames_before_buttons,
        startup_frames_before_input: args.startup_frames_before_input,
    };
    let reader_handle = tokio::spawn(read_loop(
        read_half,
        event_tx.clone(),
        injection_plan,
        injection_rx,
        board.geometry.clone(),
        debug_writer.clone(),
        debug_broker.clone(),
        radio_peer,
    ));
    let start_time = StdInstant::now();
    let mut last_fb: Option<Vec<u8>> = None;
    let mut step_results: Vec<StepResult> = Vec::new();
    let mut deferred_sd_file_assertions: Vec<DeferredSdFileAssertion> = Vec::new();
    let mut case_failed = false;
    let mut failure_error: Option<String> = None;
    let mut next_control_request_id = 1u32;
    for (step_idx, step) in case.before_boot.iter().enumerate() {
        eprintln!(
            "[e2e] beforeBoot step {}/{}: type={}",
            step_idx + 1,
            case.before_boot.len(),
            step.step_type
        );
        let step_start = StdInstant::now();
        let request_id = next_control_request_id;
        next_control_request_id = next_control_request_id.wrapping_add(1).max(1);
        match execute_peripheral_control(
            step,
            request_id,
            "beforeBoot",
            &debug_writer,
            &mut event_rx,
            &mut last_fb,
            Duration::from_secs(args.step_timeout),
        )
        .await
        {
            Ok(()) => step_results.push(StepResult {
                step_type: "before_boot_peripheral_control".to_string(),
                status: "PASS".to_string(),
                elapsed_ms: step_start.elapsed().as_millis() as u64,
                error: None,
            }),
            Err(error) => {
                case_failed = true;
                failure_error = Some(format!(
                    "beforeBoot peripheral_control step failed: {error}"
                ));
                step_results.push(StepResult {
                    step_type: "before_boot_peripheral_control".to_string(),
                    status: "FAIL".to_string(),
                    elapsed_ms: step_start.elapsed().as_millis() as u64,
                    error: Some(error.to_string()),
                });
                break;
            }
        }
    }
    continue_qemu(&monitor_socket)
        .await
        .with_context(|| format!("continuing QEMU via monitor {}", monitor_socket.display()))?;

    for (step_idx, step) in case.steps.iter().enumerate() {
        if case_failed {
            break;
        }
        eprintln!(
            "[e2e] step {}/{}: type={}",
            step_idx + 1,
            case.steps.len(),
            step.step_type
        );
        let step_start = StdInstant::now();

        match step.step_type.as_str() {
            "home" => {
                match wait_for_activity(
                    &ctx,
                    "Dashboard",
                    Duration::from_secs(args.step_timeout),
                    None,
                )
                .await
                {
                    Ok(true) => {
                        step_results.push(StepResult {
                            step_type: "home".to_string(),
                            status: "PASS".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: None,
                        });
                    }
                    Err(e) => {
                        case_failed = true;
                        failure_error = Some(format!("home step failed: {e}"));
                        step_results.push(StepResult {
                            step_type: "home".to_string(),
                            status: "FAIL".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: Some(format!("{e}")),
                        });
                        break;
                    }
                    _ => {}
                }
            }
            "serial_command" => {
                let line = match step.text.as_deref().map(serial_command_line) {
                    Some(Ok(line)) => line,
                    Some(Err(error)) => {
                        case_failed = true;
                        failure_error = Some(format!("serial_command step failed: {error}"));
                        step_results.push(StepResult {
                            step_type: "serial_command".to_string(),
                            status: "FAIL".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: Some(error.to_string()),
                        });
                        break;
                    }
                    None => {
                        let error = "serial_command step requires text";
                        case_failed = true;
                        failure_error = Some(error.to_string());
                        step_results.push(StepResult {
                            step_type: "serial_command".to_string(),
                            status: "FAIL".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: Some(error.to_string()),
                        });
                        break;
                    }
                };
                {
                    let mut ctx_lock = ctx.lock().await;
                    ctx_lock.mark_input_activity_boundary();
                }
                if !debug_writer
                    .send(debug_transport::OutFrame {
                        channel: CHANNEL_SERIAL_COMMAND,
                        flags: 0,
                        payload: line,
                    })
                    .await
                {
                    let error = "simulator serial command transport unavailable";
                    case_failed = true;
                    failure_error = Some(format!("serial_command write failed: {error}"));
                    step_results.push(StepResult {
                        step_type: "serial_command".to_string(),
                        status: "FAIL".to_string(),
                        elapsed_ms: step_start.elapsed().as_millis() as u64,
                        error: Some(error.to_string()),
                    });
                    break;
                }
                step_results.push(StepResult {
                    step_type: "serial_command".to_string(),
                    status: "PASS".to_string(),
                    elapsed_ms: step_start.elapsed().as_millis() as u64,
                    error: None,
                });
            }
            "peripheral_control" => {
                let request_id = next_control_request_id;
                next_control_request_id = next_control_request_id.wrapping_add(1).max(1);
                {
                    let mut ctx_lock = ctx.lock().await;
                    ctx_lock.mark_input_activity_boundary();
                }
                match execute_peripheral_control(
                    step,
                    request_id,
                    "",
                    &debug_writer,
                    &mut event_rx,
                    &mut last_fb,
                    Duration::from_secs(args.step_timeout),
                )
                .await
                {
                    Ok(()) => step_results.push(StepResult {
                        step_type: "peripheral_control".to_string(),
                        status: "PASS".to_string(),
                        elapsed_ms: step_start.elapsed().as_millis() as u64,
                        error: None,
                    }),
                    Err(error) => {
                        case_failed = true;
                        failure_error = Some(format!("peripheral_control step failed: {error}"));
                        step_results.push(StepResult {
                            step_type: "peripheral_control".to_string(),
                            status: "FAIL".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: Some(error.to_string()),
                        });
                        break;
                    }
                }
            }
            "seed_opds_browser_fixture" => {
                eprintln!(
                    "[e2e] seed_opds_browser_fixture satisfied by simulator SD fixture preseed"
                );
                step_results.push(StepResult {
                    step_type: "seed_opds_browser_fixture".to_string(),
                    status: "PASS".to_string(),
                    elapsed_ms: step_start.elapsed().as_millis() as u64,
                    error: None,
                });
            }
            "wait" => {
                step_results.push(StepResult {
                    step_type: "wait".to_string(),
                    status: "PASS".to_string(),
                    elapsed_ms: step_start.elapsed().as_millis() as u64,
                    error: None,
                });
            }
            "touch" => {
                let reference_tap = TouchTap {
                    x: step.x,
                    y: step.y,
                };
                let mapped_tap = board
                    .geometry
                    .map_reference_touch(reference_viewport, reference_tap);
                let gesture = match step.event.as_str() {
                    "" | "tap" => None,
                    "swipe_up" => Some(TouchGestureDirection::Up),
                    "swipe_down" => Some(TouchGestureDirection::Down),
                    "swipe_left" => Some(TouchGestureDirection::Left),
                    "swipe_right" => Some(TouchGestureDirection::Right),
                    other => {
                        case_failed = true;
                        failure_error =
                            Some(format!("touch step failed: unsupported event '{other}'"));
                        step_results.push(StepResult {
                            step_type: "touch".to_string(),
                            status: "FAIL".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: Some(format!("unsupported touch event '{other}'")),
                        });
                        break;
                    }
                };
                if let Some(direction) = gesture {
                    eprintln!(
                        "[e2e] injecting touch {} from ref=({}, {}) mapped=({}, {})",
                        step.event, reference_tap.x, reference_tap.y, mapped_tap.x, mapped_tap.y
                    );
                    {
                        let mut ctx_lock = ctx.lock().await;
                        ctx_lock.mark_input_activity_boundary();
                    }
                    injection_tx
                        .send(InjectionCommand::TouchSwipe {
                            x: mapped_tap.x,
                            y: mapped_tap.y,
                            direction,
                        })
                        .ok();
                } else {
                    eprintln!(
                        "[e2e] injecting touch tap ref=({}, {}) mapped=({}, {})",
                        reference_tap.x, reference_tap.y, mapped_tap.x, mapped_tap.y
                    );
                    {
                        let mut ctx_lock = ctx.lock().await;
                        ctx_lock.mark_input_activity_boundary();
                    }
                    injection_tx
                        .send(InjectionCommand::TouchTap {
                            x: mapped_tap.x,
                            y: mapped_tap.y,
                        })
                        .ok();
                }
                if let Err(e) = wait_for_injection(
                    &mut event_rx,
                    &mut last_fb,
                    Duration::from_secs(args.step_timeout),
                    step.wait_for_frame,
                )
                .await
                {
                    case_failed = true;
                    failure_error = Some(format!("touch step failed: {e}"));
                    step_results.push(StepResult {
                        step_type: "touch".to_string(),
                        status: "FAIL".to_string(),
                        elapsed_ms: step_start.elapsed().as_millis() as u64,
                        error: Some(format!("{e}")),
                    });
                    break;
                }
                step_results.push(StepResult {
                    step_type: "touch".to_string(),
                    status: "PASS".to_string(),
                    elapsed_ms: step_start.elapsed().as_millis() as u64,
                    error: None,
                });
            }
            "touch_event" => {
                let touch_type = match synthetic_touch_event_type(&step.event) {
                    Ok(touch_type) => touch_type,
                    Err(e) => {
                        case_failed = true;
                        failure_error = Some(format!("touch_event step failed: {e}"));
                        step_results.push(StepResult {
                            step_type: "touch_event".to_string(),
                            status: "FAIL".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: Some(format!("{e}")),
                        });
                        break;
                    }
                };

                let reference_tap = TouchTap {
                    x: step.x,
                    y: step.y,
                };
                let mapped_tap = board
                    .geometry
                    .map_reference_touch(reference_viewport, reference_tap);
                eprintln!(
                    "[e2e] injecting synthetic touch event {} ref=({}, {}) mapped=({}, {})",
                    touch_type.label(),
                    reference_tap.x,
                    reference_tap.y,
                    mapped_tap.x,
                    mapped_tap.y
                );
                {
                    let mut ctx_lock = ctx.lock().await;
                    ctx_lock.mark_input_activity_boundary();
                }
                injection_tx
                    .send(InjectionCommand::SyntheticTouchEvent {
                        touch_type,
                        x: mapped_tap.x,
                        y: mapped_tap.y,
                    })
                    .ok();
                if let Err(e) = wait_for_injection(
                    &mut event_rx,
                    &mut last_fb,
                    Duration::from_secs(args.step_timeout),
                    step.wait_for_frame,
                )
                .await
                {
                    case_failed = true;
                    failure_error = Some(format!("touch_event step failed: {e}"));
                    step_results.push(StepResult {
                        step_type: "touch_event".to_string(),
                        status: "FAIL".to_string(),
                        elapsed_ms: step_start.elapsed().as_millis() as u64,
                        error: Some(format!("{e}")),
                    });
                    break;
                }
                step_results.push(StepResult {
                    step_type: "touch_event".to_string(),
                    status: "PASS".to_string(),
                    elapsed_ms: step_start.elapsed().as_millis() as u64,
                    error: None,
                });
            }
            "nav" => {
                let direction = step.direction.trim();
                if direction.is_empty() {
                    case_failed = true;
                    failure_error = Some("nav step failed: missing direction".to_string());
                    step_results.push(StepResult {
                        step_type: "nav".to_string(),
                        status: "FAIL".to_string(),
                        elapsed_ms: step_start.elapsed().as_millis() as u64,
                        error: Some("missing direction".to_string()),
                    });
                    break;
                }
                match simulator_nav_injection(&board, direction) {
                    Ok(ButtonInjection::ButtonClick { button_id }) => {
                        eprintln!(
                            "[e2e] injecting logical nav {} as simulator button_id={}",
                            direction, button_id
                        );
                        {
                            let mut ctx_lock = ctx.lock().await;
                            ctx_lock.mark_input_activity_boundary();
                        }
                        injection_tx
                            .send(InjectionCommand::ButtonClick {
                                button_id,
                                hold_ms: step.hold_ms,
                            })
                            .ok();
                        if let Err(e) = wait_for_injection(
                            &mut event_rx,
                            &mut last_fb,
                            Duration::from_secs(args.step_timeout),
                            step.wait_for_frame,
                        )
                        .await
                        {
                            case_failed = true;
                            failure_error = Some(format!("nav step failed: {e}"));
                            step_results.push(StepResult {
                                step_type: "nav".to_string(),
                                status: "FAIL".to_string(),
                                elapsed_ms: step_start.elapsed().as_millis() as u64,
                                error: Some(format!("{e}")),
                            });
                            break;
                        }
                        step_results.push(StepResult {
                            step_type: "nav".to_string(),
                            status: "PASS".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: None,
                        });
                    }
                    Ok(ButtonInjection::SyntheticTouch { touch_type }) => {
                        let touch = board_center_touch(&board.geometry);
                        eprintln!(
                            "[e2e] injecting logical nav {} as synthetic touch {} x={} y={}",
                            direction,
                            touch_type.label(),
                            touch.x,
                            touch.y
                        );
                        {
                            let mut ctx_lock = ctx.lock().await;
                            ctx_lock.mark_input_activity_boundary();
                        }
                        injection_tx
                            .send(InjectionCommand::SyntheticTouchEvent {
                                touch_type,
                                x: touch.x,
                                y: touch.y,
                            })
                            .ok();
                        if let Err(e) = wait_for_injection(
                            &mut event_rx,
                            &mut last_fb,
                            Duration::from_secs(args.step_timeout),
                            step.wait_for_frame,
                        )
                        .await
                        {
                            case_failed = true;
                            failure_error = Some(format!("nav step failed: {e}"));
                            step_results.push(StepResult {
                                step_type: "nav".to_string(),
                                status: "FAIL".to_string(),
                                elapsed_ms: step_start.elapsed().as_millis() as u64,
                                error: Some(format!("{e}")),
                            });
                            break;
                        }
                        step_results.push(StepResult {
                            step_type: "nav".to_string(),
                            status: "PASS".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: None,
                        });
                    }
                    Err(e) => {
                        case_failed = true;
                        failure_error = Some(format!("nav step failed: {e}"));
                        step_results.push(StepResult {
                            step_type: "nav".to_string(),
                            status: "FAIL".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: Some(format!("{e}")),
                        });
                        break;
                    }
                }
            }
            "button" => match simulator_button_injection(&board, &step.button) {
                Ok(ButtonInjection::ButtonClick { button_id }) => {
                    eprintln!(
                        "[e2e] injecting physical button {} as simulator button_id={}",
                        step.button, button_id
                    );
                    {
                        let mut ctx_lock = ctx.lock().await;
                        ctx_lock.mark_input_activity_boundary();
                    }
                    injection_tx
                        .send(InjectionCommand::ButtonClick {
                            button_id,
                            hold_ms: step.hold_ms,
                        })
                        .ok();
                    if let Err(e) = wait_for_injection(
                        &mut event_rx,
                        &mut last_fb,
                        Duration::from_secs(args.step_timeout),
                        step.wait_for_frame,
                    )
                    .await
                    {
                        case_failed = true;
                        failure_error = Some(format!("button step failed: {e}"));
                        step_results.push(StepResult {
                            step_type: "button".to_string(),
                            status: "FAIL".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: Some(format!("{e}")),
                        });
                        break;
                    }
                    step_results.push(StepResult {
                        step_type: "button".to_string(),
                        status: "PASS".to_string(),
                        elapsed_ms: step_start.elapsed().as_millis() as u64,
                        error: None,
                    });
                }
                Ok(ButtonInjection::SyntheticTouch { .. }) => {
                    unreachable!("physical button injection cannot synthesize touch")
                }
                Err(e) => {
                    case_failed = true;
                    failure_error = Some(format!("button step failed: {e}"));
                    step_results.push(StepResult {
                        step_type: "button".to_string(),
                        status: "FAIL".to_string(),
                        elapsed_ms: step_start.elapsed().as_millis() as u64,
                        error: Some(format!("{e}")),
                    });
                    break;
                }
            },
            "assert_activity" => {
                let expected = step.activity.as_str();
                let after_activity_event = {
                    let mut ctx_lock = ctx.lock().await;
                    ctx_lock.take_pending_input_activity_boundary()
                };
                match wait_for_activity(
                    &ctx,
                    expected,
                    Duration::from_secs(args.step_timeout),
                    after_activity_event,
                )
                .await
                {
                    Ok(true) => {
                        let current = {
                            let ctx_lock = ctx.lock().await;
                            ctx_lock
                                .current_activity
                                .clone()
                                .unwrap_or_else(|| "unknown".to_string())
                        };
                        eprintln!(
                            "[e2e] assert_activity '{}' matched (current: '{}')",
                            expected, current
                        );
                        step_results.push(StepResult {
                            step_type: "assert_activity".to_string(),
                            status: "PASS".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: None,
                        });
                    }
                    Err(e) => {
                        case_failed = true;
                        failure_error = Some(format!("assert_activity '{}' failed: {e}", expected));
                        step_results.push(StepResult {
                            step_type: "assert_activity".to_string(),
                            status: "FAIL".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: Some(format!("{e}")),
                        });
                        break;
                    }
                    _ => {}
                }
            }
            "assert_log_contains" => {
                let expected = step
                    .text
                    .as_deref()
                    .map(str::trim)
                    .filter(|value| !value.is_empty())
                    .ok_or_else(|| anyhow!("assert_log_contains step requires non-empty text"))?;
                let after_log_line = if step.after_input {
                    let mut ctx_lock = ctx.lock().await;
                    ctx_lock.take_pending_input_log_boundary()
                } else {
                    None
                };
                match wait_for_log_contains(
                    &ctx,
                    expected,
                    Duration::from_secs(args.step_timeout),
                    after_log_line,
                )
                .await
                {
                    Ok(true) => {
                        eprintln!("[e2e] assert_log_contains '{}' matched", expected);
                        step_results.push(StepResult {
                            step_type: "assert_log_contains".to_string(),
                            status: "PASS".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: None,
                        });
                    }
                    Err(e) => {
                        case_failed = true;
                        failure_error =
                            Some(format!("assert_log_contains '{}' failed: {e}", expected));
                        step_results.push(StepResult {
                            step_type: "assert_log_contains".to_string(),
                            status: "FAIL".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: Some(format!("{e}")),
                        });
                        break;
                    }
                    _ => {}
                }
            }
            "assert_screenshot_region" => {
                let frames_drained = drain_pending_e2e_events(&mut event_rx, &mut last_fb);
                let fb = match last_fb.as_deref() {
                    Some(fb) => fb,
                    None => {
                        case_failed = true;
                        failure_error = Some(
                            "assert_screenshot_region failed: no framebuffer received".to_string(),
                        );
                        step_results.push(StepResult {
                            step_type: "assert_screenshot_region".to_string(),
                            status: "FAIL".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: Some("no framebuffer received".to_string()),
                        });
                        break;
                    }
                };
                match assert_screenshot_region_with_checkpoint(
                    step,
                    fb,
                    &board.geometry,
                    reference_viewport,
                    args.capture_assertion_screenshots,
                    &artifacts_dir,
                    step_idx,
                ) {
                    Ok(summary) => {
                        eprintln!(
                            "[e2e] assert_screenshot_region matched after draining {} frame(s): {}",
                            frames_drained, summary
                        );
                        step_results.push(StepResult {
                            step_type: "assert_screenshot_region".to_string(),
                            status: "PASS".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: None,
                        });
                    }
                    Err(e) => {
                        case_failed = true;
                        failure_error = Some(format!("assert_screenshot_region failed: {e}"));
                        step_results.push(StepResult {
                            step_type: "assert_screenshot_region".to_string(),
                            status: "FAIL".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: Some(format!("{e}")),
                        });
                        break;
                    }
                }
            }
            "assert_sd_file_exists" | "assert_sd_file_contains" => {
                let step_type = step.step_type.as_str();
                let raw_path = match step
                    .path
                    .as_deref()
                    .map(str::trim)
                    .filter(|value| !value.is_empty())
                {
                    Some(path) => path,
                    None => {
                        case_failed = true;
                        failure_error = Some(format!("{step_type} step requires non-empty path"));
                        step_results.push(StepResult {
                            step_type: step_type.to_string(),
                            status: "FAIL".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: Some("missing path".to_string()),
                        });
                        break;
                    }
                };
                let contains = if step_type == "assert_sd_file_contains" {
                    match step
                        .text
                        .as_deref()
                        .map(str::trim)
                        .filter(|value| !value.is_empty())
                    {
                        Some(text) => Some(text.to_string()),
                        None => {
                            case_failed = true;
                            failure_error = Some(
                                "assert_sd_file_contains step requires non-empty text".to_string(),
                            );
                            step_results.push(StepResult {
                                step_type: step_type.to_string(),
                                status: "FAIL".to_string(),
                                elapsed_ms: step_start.elapsed().as_millis() as u64,
                                error: Some("missing text".to_string()),
                            });
                            break;
                        }
                    }
                } else {
                    None
                };
                match safe_sd_relative_path(raw_path) {
                    Ok(relative_path) => {
                        eprintln!(
                            "[e2e] deferring {step_type} '{}' until SD image sync",
                            raw_path
                        );
                        step_results.push(StepResult {
                            step_type: step_type.to_string(),
                            status: "PENDING".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: None,
                        });
                        deferred_sd_file_assertions.push(DeferredSdFileAssertion {
                            result_index: step_results.len() - 1,
                            step_type: step_type.to_string(),
                            relative_path,
                            display_path: raw_path.to_string(),
                            contains,
                            step_start,
                        });
                    }
                    Err(e) => {
                        case_failed = true;
                        failure_error = Some(format!("{step_type} failed: {e}"));
                        step_results.push(StepResult {
                            step_type: step_type.to_string(),
                            status: "FAIL".to_string(),
                            elapsed_ms: step_start.elapsed().as_millis() as u64,
                            error: Some(format!("{e}")),
                        });
                        break;
                    }
                }
            }
            other => {
                case_failed = true;
                failure_error = Some(format!("unknown step type: {other}"));
                step_results.push(StepResult {
                    step_type: other.to_string(),
                    status: "FAIL".to_string(),
                    elapsed_ms: step_start.elapsed().as_millis() as u64,
                    error: Some(format!("unknown step type: {other}")),
                });
                break;
            }
        }

        if step.delay_ms > 0 && !case_failed {
            time::sleep(Duration::from_millis(step.delay_ms)).await;
        }
    }

    let total_elapsed = start_time.elapsed().as_millis() as u64;
    let final_frames_drained = drain_pending_e2e_events(&mut event_rx, &mut last_fb);
    if final_frames_drained > 0 {
        eprintln!("[e2e] final drain captured {final_frames_drained} framebuffer(s)");
    }

    let fb = last_fb.unwrap_or_else(|| vec![0xFFu8; board.geometry.fb_bytes()]);

    qemu.cleanup().await;
    debug_writer.unbind().await;
    reader_handle.abort();
    writer_handle.abort();
    stderr_handle.abort();
    stdout_handle.abort();
    if let Some(image_path) = sd_drive.image_path.as_deref() {
        sd_image::sync_sd_image_to_dir(image_path, &sd_root)?;
    }

    for assertion in &deferred_sd_file_assertions {
        let assertion_result = match assertion.contains.as_deref() {
            Some(expected) => assert_sd_file_contains(
                &sd_root,
                &assertion.relative_path,
                &assertion.display_path,
                expected,
            ),
            None => {
                assert_sd_file_exists(&sd_root, &assertion.relative_path, &assertion.display_path)
            }
        };
        match assertion_result {
            Ok(summary) => {
                eprintln!("[e2e] {} matched: {summary}", assertion.step_type);
                if let Some(result) = step_results.get_mut(assertion.result_index) {
                    result.status = "PASS".to_string();
                    result.elapsed_ms = assertion.step_start.elapsed().as_millis() as u64;
                    result.error = None;
                }
            }
            Err(e) => {
                case_failed = true;
                if failure_error.is_none() {
                    failure_error = Some(format!("{e}"));
                }
                if let Some(result) = step_results.get_mut(assertion.result_index) {
                    result.status = "FAIL".to_string();
                    result.elapsed_ms = assertion.step_start.elapsed().as_millis() as u64;
                    result.error = Some(format!("{e}"));
                }
            }
        }
    }

    let firmware_path_str = args.firmware.to_string_lossy().to_string();
    let status = if case_failed { "FAIL" } else { "PASS" };

    let case_result = CaseResult {
        case_id: case.id.clone(),
        status: status.to_string(),
        error: failure_error.clone(),
        steps: step_results,
        total_elapsed_ms: total_elapsed,
        screenshot: artifacts_dir
            .join("screenshot.png")
            .to_string_lossy()
            .to_string(),
        transcript: artifacts_dir
            .join("transcript.log")
            .to_string_lossy()
            .to_string(),
        firmware: firmware_path_str,
    };

    let transcript_lines = {
        let ctx_lock = ctx.lock().await;
        ctx_lock.transcript.clone()
    };
    persist_e2e_artifacts(
        &artifacts_dir,
        &case_result,
        &transcript_lines,
        &fb,
        &board.geometry,
    )?;

    if case_failed {
        eprintln!(
            "[e2e] CASE FAILED: {} — {}",
            case.id,
            failure_error.clone().unwrap_or_default()
        );
    } else {
        eprintln!("[e2e] CASE PASSED: {} ({total_elapsed}ms)", case.id);
    }

    Ok(CaseRunOutcome {
        case_id: case.id,
        status: status.to_string(),
        artifacts_dir,
        error: failure_error,
    })
}

async fn run_e2e_case_dir(
    args: &Args,
    case_dir: &Path,
    debug_writer: debug_transport::HeadlessDebugWriter,
    debug_broker: debug_transport::DebugTransportBroker,
) -> Result<()> {
    let cases = load_e2e_case_dir(case_dir)?;
    let root_artifacts = args.artifacts.clone();
    fs::create_dir_all(&root_artifacts)
        .with_context(|| format!("creating artifacts dir {}", root_artifacts.display()))?;

    eprintln!(
        "[e2e] running {} case(s) from {}",
        cases.len(),
        case_dir.display()
    );

    let mut outcomes = Vec::new();
    let total_cases = cases.len();
    for (index, (case_path, case)) in cases.into_iter().enumerate() {
        let artifact_name = case_artifact_name(&case_path, &case);
        let mut case_args = args.clone();
        case_args.case = None;
        case_args.case_dir = None;
        case_args.artifacts = root_artifacts.join(&artifact_name);
        case_args.socket = path_with_suffix(&args.socket, &artifact_name);
        if let Some(sd_root) = &args.sd_root {
            case_args.sd_root = Some(sd_root.join(&artifact_name));
        } else {
            case_args.sd_root = Some(root_artifacts.join(".sd").join(&artifact_name));
        }

        eprintln!(
            "[e2e] batch case {}/{}: {} ({})",
            index + 1,
            total_cases,
            case.id,
            case_path.display()
        );
        let outcome_result =
            run_e2e_case(&case_args, case, debug_writer.clone(), debug_broker.clone()).await;
        if args.sd_root.is_none() {
            // 批次案例各自建立可再生的 SD 工件；結果、截圖與 transcript 仍保留在 case artifacts。
            cleanup_e2e_batch_sd_artifacts(
                case_args
                    .sd_root
                    .as_deref()
                    .expect("batch case must have an isolated SD root"),
                &case_args.artifacts,
            )?;
        }
        let outcome = outcome_result?;
        outcomes.push(outcome);
    }

    let failed: Vec<&CaseRunOutcome> = outcomes
        .iter()
        .filter(|outcome| outcome.status == "FAIL")
        .collect();
    let skipped = outcomes
        .iter()
        .filter(|outcome| outcome.status == "SKIP")
        .count();
    let passed = outcomes
        .iter()
        .filter(|outcome| outcome.status == "PASS")
        .count();
    eprintln!(
        "[e2e] batch summary: {passed}/{} passed, {skipped} skipped, {} failed",
        outcomes.len(),
        failed.len()
    );
    for outcome in &outcomes {
        eprintln!(
            "[e2e] {} {} artifacts={}",
            outcome.status,
            outcome.case_id,
            outcome.artifacts_dir.display()
        );
        if let Some(error) = &outcome.error {
            eprintln!("[e2e]   error={error}");
        }
    }

    if !failed.is_empty() {
        return Err(anyhow!("{} simulator E2E case(s) failed", failed.len()));
    }

    Ok(())
}

fn cleanup_e2e_batch_sd_artifacts(sd_root: &Path, artifacts_dir: &Path) -> Result<()> {
    if sd_root.exists() {
        fs::remove_dir_all(sd_root)
            .with_context(|| format!("removing batch simulator SD root {}", sd_root.display()))?;
    }
    let image_path = artifacts_dir.join("sdcard.img");
    if image_path.exists() {
        fs::remove_file(&image_path).with_context(|| {
            format!("removing batch simulator SD image {}", image_path.display())
        })?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn mofei_test_geometry() -> BoardGeometry {
        load_board_runtime(Some("mofei")).unwrap().geometry
    }

    fn s37uc_test_geometry() -> BoardGeometry {
        load_board_runtime(Some("s37uc")).unwrap().geometry
    }

    fn lilygo_test_geometry() -> BoardGeometry {
        load_board_runtime(Some("lilygo-t5s3-pro"))
            .unwrap()
            .geometry
    }

    fn s37uc_test_board() -> BoardRuntime {
        load_board_runtime(Some("s37uc")).unwrap()
    }

    fn s3r8_test_board() -> BoardRuntime {
        load_board_runtime(Some("s3r8")).unwrap()
    }

    fn temp_test_dir(name: &str) -> PathBuf {
        let tmp_dir =
            std::env::temp_dir().join(format!("mofei-headless-{}-{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&tmp_dir);
        fs::create_dir_all(&tmp_dir).unwrap();
        tmp_dir
    }

    fn minimal_case(id: &str) -> E2ECase {
        E2ECase {
            id: id.to_string(),
            description: "fixture test".to_string(),
            skip_reason: None,
            required_capabilities: Vec::new(),
            reference_viewport: ReferenceViewport::default(),
            boot: E2EBoot::default(),
            before_boot: Vec::new(),
            steps: Vec::new(),
        }
    }

    fn peripheral_step(value: serde_json::Value) -> CaseStep {
        serde_json::from_value(value).unwrap()
    }

    fn partition_entry(
        entry_type: u8,
        subtype: u8,
        offset: u32,
        label: &str,
    ) -> [u8; PARTITION_ENTRY_SIZE] {
        let mut entry = [0u8; PARTITION_ENTRY_SIZE];
        entry[0..2].copy_from_slice(&PARTITION_MAGIC);
        entry[2] = entry_type;
        entry[3] = subtype;
        entry[4..8].copy_from_slice(&offset.to_le_bytes());
        let label_bytes = label.as_bytes();
        entry[12..12 + label_bytes.len()].copy_from_slice(label_bytes);
        entry
    }

    #[test]
    fn mofei_reference_geometry_is_identity() {
        let geometry = mofei_test_geometry();

        assert_eq!(
            geometry.map_reference_touch(ReferenceViewport::default(), TouchTap { x: 100, y: 475 }),
            TouchTap { x: 100, y: 475 }
        );
        assert_eq!(
            geometry
                .map_reference_screenshot_region(ReferenceViewport::default(), 20, 85, 440, 300)
                .unwrap(),
            ScreenshotRegion {
                x: 20,
                y: 85,
                width: 440,
                height: 300
            }
        );
    }

    #[test]
    fn lilygo_gray16_geometry_and_content_use_published_portrait_framebuffer() {
        let geometry = lilygo_test_geometry();

        assert_eq!(geometry.framebuffer_width, 540);
        assert_eq!(geometry.framebuffer_height, 960);
        assert_eq!(geometry.framebuffer_format, FramebufferFormat::Gray16);
        assert_eq!(geometry.fb_bytes(), 259_200);
        assert_eq!(geometry.source_pixel_for_output(539, 959), Some((539, 959)));

        let mut frame = vec![0u8; geometry.fb_bytes()];
        assert!(!framebuffer_has_content(&frame, &geometry));
        frame[0] = 0x10;
        assert!(framebuffer_has_content(&frame, &geometry));
        assert_eq!(framebuffer_gray_value(&frame, &geometry, 0, 0), 238);
        assert_eq!(framebuffer_gray_value(&frame, &geometry, 1, 0), 255);
    }

    #[test]
    fn s3r8_alias_uses_mofei_geometry_and_button_mapping() {
        let s3r8 = s3r8_test_board();

        assert_eq!(s3r8.geometry.raw_width, 800);
        assert_eq!(s3r8.geometry.raw_height, 480);
        assert_eq!(s3r8.geometry.output_width, 480);
        assert_eq!(s3r8.geometry.output_height, 800);
        assert_eq!(
            simulator_button_injection(&s3r8, "confirm").unwrap(),
            ButtonInjection::ButtonClick { button_id: 1 }
        );
        assert_eq!(
            simulator_button_injection(&s3r8, "back").unwrap(),
            ButtonInjection::ButtonClick { button_id: 0 }
        );
        assert_eq!(
            simulator_nav_injection(&s3r8, "down").unwrap(),
            ButtonInjection::ButtonClick { button_id: 5 }
        );
        assert_eq!(
            simulator_nav_injection(&s3r8, "up").unwrap(),
            ButtonInjection::ButtonClick { button_id: 4 }
        );
    }

    #[test]
    fn custom_reference_viewport_maps_to_board_space() {
        let geometry = s37uc_test_geometry();
        let viewport = ReferenceViewport {
            width: 240,
            height: 416,
        };

        assert_eq!(
            geometry.map_reference_touch(viewport, TouchTap { x: 120, y: 208 }),
            TouchTap { x: 120, y: 208 }
        );
        assert_eq!(
            geometry
                .map_reference_screenshot_region(viewport, 0, 0, 240, 416)
                .unwrap(),
            ScreenshotRegion {
                x: 0,
                y: 0,
                width: 240,
                height: 416
            }
        );
    }

    #[test]
    fn s37uc_maps_reference_touch_to_board_space() {
        let geometry = s37uc_test_geometry();

        assert_eq!(
            geometry.map_reference_touch(ReferenceViewport::default(), TouchTap { x: 100, y: 475 }),
            TouchTap { x: 50, y: 247 }
        );
        assert_eq!(
            geometry.map_reference_touch(ReferenceViewport::default(), TouchTap { x: 480, y: 800 }),
            TouchTap { x: 239, y: 415 }
        );
    }

    #[test]
    fn s37uc_scales_reference_screenshot_region_and_pixel_thresholds() {
        let geometry = s37uc_test_geometry();
        let region = geometry
            .map_reference_screenshot_region(ReferenceViewport::default(), 0, 0, 480, 800)
            .unwrap();
        assert_eq!(
            region,
            ScreenshotRegion {
                x: 0,
                y: 0,
                width: 240,
                height: 416
            }
        );

        let mut fb = vec![0xFFu8; geometry.fb_bytes()];
        for x in 0..90 {
            set_output_pixel(&mut fb, &geometry, x, 0, true);
        }
        let mut step: CaseStep = serde_json::from_str(
            r#"{
                "type": "assert_screenshot_region",
                "label": "scaled-full-screen",
                "x": 0,
                "y": 0,
                "width": 480,
                "height": 800,
                "minDarkPixels": 346,
                "maxDarkRatio": 0.01
            }"#,
        )
        .unwrap();
        let summary =
            assert_screenshot_region(&step, &fb, &geometry, ReferenceViewport::default()).unwrap();
        assert!(summary.contains("mapped=(0,0 240x416)"));
        assert!(summary.contains("dark=90/99840"));

        step.min_dark_pixels = Some(347);
        let err = assert_screenshot_region(&step, &fb, &geometry, ReferenceViewport::default())
            .unwrap_err();
        assert!(format!("{err}").contains("dark pixels 90 below min 91"));
    }

    #[test]
    fn s37uc_nav_falls_back_to_synthetic_touch_without_physical_dpad() {
        let mofei = load_board_runtime(Some("mofei")).unwrap();
        assert_eq!(
            simulator_nav_injection(&mofei, "down").unwrap(),
            ButtonInjection::ButtonClick { button_id: 5 }
        );

        let s37uc = s37uc_test_board();
        assert_eq!(
            simulator_button_injection(&s37uc, "back").unwrap(),
            ButtonInjection::ButtonClick { button_id: 0 }
        );
        assert!(simulator_button_injection(&s37uc, "down").is_err());
        assert_eq!(
            simulator_nav_injection(&s37uc, "down").unwrap(),
            ButtonInjection::SyntheticTouch {
                touch_type: SyntheticTouchEventType::SwipeUp
            }
        );
        assert_eq!(
            simulator_nav_injection(&s37uc, "up").unwrap(),
            ButtonInjection::SyntheticTouch {
                touch_type: SyntheticTouchEventType::SwipeDown
            }
        );
        assert_eq!(
            board_center_touch(&s37uc.geometry),
            TouchTap { x: 120, y: 208 }
        );
    }

    #[test]
    fn parses_mofei_sim_partition_offsets() {
        let mut table = Vec::new();
        table.extend_from_slice(&partition_entry(0x01, 0x02, 0x9000, "nvs"));
        table.extend_from_slice(&partition_entry(0x01, 0x00, 0xe000, "otadata"));
        table.extend_from_slice(&partition_entry(0x00, 0x10, 0x10000, "app0"));
        table.extend_from_slice(&PARTITION_END_MAGIC);
        table.resize(0x100, 0xff);

        let layout = parse_flash_layout(&table).unwrap();
        assert_eq!(layout.boot_app0_offset, 0xe000);
        assert_eq!(layout.app_offset, 0x10000);
    }

    #[test]
    fn parses_production_mofei_partition_offsets() {
        let mut table = Vec::new();
        table.extend_from_slice(&partition_entry(0x01, 0x02, 0x9000, "nvs"));
        table.extend_from_slice(&partition_entry(0x01, 0x00, 0xf000, "otadata"));
        table.extend_from_slice(&partition_entry(0x00, 0x10, 0x20000, "app0"));
        table.extend_from_slice(&PARTITION_END_MAGIC);
        table.resize(0x100, 0xff);

        let layout = parse_flash_layout(&table).unwrap();
        assert_eq!(layout.boot_app0_offset, 0xf000);
        assert_eq!(layout.app_offset, 0x20000);
    }

    #[test]
    fn rejects_overlapping_flash_segments() {
        let mut img = vec![0xff; 0x100];
        let mut occupied = Vec::new();
        copy_flash_segment(&mut img, &mut occupied, 0x20, &[1; 0x20], "first").unwrap();

        let error =
            copy_flash_segment(&mut img, &mut occupied, 0x30, &[2; 0x20], "second").unwrap_err();
        assert!(error.to_string().contains("overlaps first"));
    }

    #[test]
    fn derives_symbol_elf_for_bin_firmware() {
        let tmp_dir =
            std::env::temp_dir().join(format!("mofei-headless-symbol-elf-{}", std::process::id()));
        let _ = fs::remove_dir_all(&tmp_dir);
        fs::create_dir_all(&tmp_dir).unwrap();
        let bin_path = tmp_dir.join("firmware.bin");
        let elf_path = tmp_dir.join("firmware.elf");
        fs::write(&bin_path, [0u8]).unwrap();
        fs::write(&elf_path, [0u8]).unwrap();

        assert_eq!(qemu_symbol_elf_path(&bin_path).unwrap(), elf_path);

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn derives_rom_path_next_to_qemu_binary() {
        let tmp_dir = temp_test_dir("qemu-rom-path");
        let build_dir = tmp_dir.join("qemu/build");
        let bios_dir = tmp_dir.join("qemu/pc-bios");
        fs::create_dir_all(&build_dir).unwrap();
        fs::create_dir_all(&bios_dir).unwrap();
        let qemu_path = build_dir.join("qemu-system-xtensa");
        let rom_path = bios_dir.join("esp32s3_rev0_rom.bin");
        fs::write(&qemu_path, [0u8]).unwrap();
        fs::write(&rom_path, [0u8]).unwrap();

        assert_eq!(
            fs::canonicalize(qemu_rom_path(&qemu_path).unwrap()).unwrap(),
            fs::canonicalize(&rom_path).unwrap()
        );

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn rejects_bin_firmware_without_matching_symbol_elf() {
        let tmp_dir = temp_test_dir("missing-symbol-elf");
        let bin_path = tmp_dir.join("firmware.bin");
        fs::write(&bin_path, [0u8]).unwrap();

        let error = qemu_symbol_elf_path(&bin_path).unwrap_err();
        assert!(error
            .to_string()
            .contains("Matching firmware ELF not found"));

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn recent_reading_fixture_overwrites_inherited_ttf_reader_settings() {
        let tmp_dir = temp_test_dir("recent-fixture-settings");
        write_simulator_system_fixture(
            &tmp_dir,
            Path::new("settings.json"),
            "{\"fontFamily\":2,\"ttfFontName\":\"reader.ttf\",\"frontButtonBack\":0,\"frontButtonConfirm\":1,\"frontButtonLeft\":2,\"frontButtonRight\":3}\n",
        )
        .unwrap();

        seed_case_specific_sd_fixtures(
            &minimal_case("dashboard-recent-reading-open-back-smoke"),
            &tmp_dir,
        )
        .unwrap();

        let hidden_settings = fs::read_to_string(tmp_dir.join(".mofei/settings.json")).unwrap();
        let visible_settings = fs::read_to_string(tmp_dir.join("mofei/settings.json")).unwrap();
        assert_eq!(hidden_settings, SEEDED_READER_SETTINGS_JSON);
        assert_eq!(visible_settings, SEEDED_READER_SETTINGS_JSON);
        assert!(!tmp_dir.join(".murphy/browser.bin").exists());
        assert!(!tmp_dir.join("murphy/browser.bin").exists());
        assert!(tmp_dir.join("Books/sshpub.txt").exists());
        let hidden_recents = fs::read(tmp_dir.join(".murphy/recents.bin")).unwrap();
        let visible_recents = fs::read(tmp_dir.join("murphy/recents.bin")).unwrap();
        assert_eq!(hidden_recents, visible_recents);
        assert_eq!(&hidden_recents[0..4], &0x4243_524du32.to_le_bytes());
        assert_eq!(hidden_recents[4], 2);
        assert_eq!(
            u16::from_le_bytes([hidden_recents[6], hidden_recents[7]]),
            1
        );
        assert!(!tmp_dir.join(".mofei/recent.json").exists());

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn frontlight_selector_fixture_seeds_reader_txt_book() {
        let tmp_dir = temp_test_dir("frontlight-selector-fixture");

        seed_case_specific_sd_fixtures(
            &minimal_case("dashboard-reader-frontlight-selector-smoke"),
            &tmp_dir,
        )
        .unwrap();

        let hidden_settings = fs::read_to_string(tmp_dir.join(".mofei/settings.json")).unwrap();
        let visible_settings = fs::read_to_string(tmp_dir.join("mofei/settings.json")).unwrap();
        assert_eq!(hidden_settings, SEEDED_READER_SETTINGS_JSON);
        assert_eq!(visible_settings, SEEDED_READER_SETTINGS_JSON);
        assert!(!tmp_dir.join(".murphy/browser.bin").exists());
        assert!(!tmp_dir.join("murphy/browser.bin").exists());
        assert!(tmp_dir.join("Books/sshpub.txt").exists());

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn large_canonical_study_fixture_prepares_runtime_directories() {
        let tmp_dir = temp_test_dir("large-canonical-study-fixture");
        seed_case_specific_sd_fixtures(
            &minimal_case("dashboard-study-large-canonical-deck-bounded-session-smoke"),
            &tmp_dir,
        )
        .unwrap();

        assert!(tmp_dir.join("Study/Decks").is_dir());
        assert!(!tmp_dir
            .join("Study/Decks/panda-study-canonical-e2e.json")
            .exists());
        assert!(tmp_dir.join("Study/Courses").is_dir());
        assert!(tmp_dir.join(".murphy/study").is_dir());
        assert!(tmp_dir.join("murphy/study").is_dir());

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn storage_diagnostic_fixture_seeds_production_read_target() {
        for case_id in [
            "lilygo-storage-retry-production-console-smoke",
            "lilygo-storage-initial-absence-recovery",
            "lilygo-storage-read-fault-recovery",
            "lilygo-storage-write-fault-recovery",
            "lilygo-storage-corrupt-fault-recovery",
        ] {
            let tmp_dir = temp_test_dir(case_id);
            seed_case_specific_sd_fixtures(&minimal_case(case_id), &tmp_dir).unwrap();
            let seed = fs::read_to_string(tmp_dir.join("Books/sshpub.txt")).unwrap();
            assert!(seed.contains("Simulator E2E fixture."));
            fs::remove_dir_all(tmp_dir).unwrap();
        }
    }

    #[test]
    fn cover_card_fixture_seeds_grid_layout_setting() {
        let tmp_dir = temp_test_dir("cover-card-grid-fixture");

        seed_case_specific_sd_fixtures(
            &minimal_case("dashboard-file-browser-epub-cover-card-pixels-smoke"),
            &tmp_dir,
        )
        .unwrap();

        let hidden_settings = fs::read_to_string(tmp_dir.join(".mofei/settings.json")).unwrap();
        let visible_settings = fs::read_to_string(tmp_dir.join("mofei/settings.json")).unwrap();
        assert_eq!(hidden_settings, SEEDED_READER_GRID_SETTINGS_JSON);
        assert_eq!(visible_settings, SEEDED_READER_GRID_SETTINGS_JSON);
        assert_eq!(
            fs::read(tmp_dir.join(".murphy/browser.bin")).unwrap(),
            SEEDED_BROWSER_GRID_PREFS
        );
        assert_eq!(
            fs::read(tmp_dir.join("murphy/browser.bin")).unwrap(),
            SEEDED_BROWSER_GRID_PREFS
        );
        assert_eq!(
            fs::read(tmp_dir.join(".murphy/browser.bin")).unwrap(),
            SEEDED_BROWSER_GRID_PREFS
        );
        assert_eq!(
            fs::read(tmp_dir.join("murphy/browser.bin")).unwrap(),
            SEEDED_BROWSER_GRID_PREFS
        );
        assert!(tmp_dir.join("Books/sshpub.txt").exists());
        assert!(tmp_dir
            .join("Books/aaa_test_kerning_ligature.epub")
            .exists());
        assert!(tmp_dir.join(".mofei/epub_3316698266/book.bin").exists());
        assert!(tmp_dir.join("mofei/epub_3316698266/book.bin").exists());

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn layout_grid_epub_books_fixture_seeds_renderable_epub_cover() {
        let tmp_dir = temp_test_dir("layout-grid-epub-books-fixture");

        seed_case_specific_sd_fixtures(
            &minimal_case("dashboard-settings-file-browser-layout-grid-epub-books-smoke"),
            &tmp_dir,
        )
        .unwrap();

        let hidden_settings = fs::read_to_string(tmp_dir.join(".mofei/settings.json")).unwrap();
        let visible_settings = fs::read_to_string(tmp_dir.join("mofei/settings.json")).unwrap();
        assert_eq!(hidden_settings, SEEDED_READER_SETTINGS_JSON);
        assert_eq!(visible_settings, SEEDED_READER_SETTINGS_JSON);
        assert_eq!(
            fs::read(tmp_dir.join(".murphy/browser.bin")).unwrap(),
            SEEDED_BROWSER_LIST_PREFS
        );
        assert_eq!(
            fs::read(tmp_dir.join("murphy/browser.bin")).unwrap(),
            SEEDED_BROWSER_LIST_PREFS
        );
        assert!(tmp_dir.join("Books/sshpub.txt").exists());
        assert!(tmp_dir
            .join("Books/aaa_test_kerning_ligature.epub")
            .exists());
        assert!(tmp_dir.join(".mofei/epub_3316698266/book.bin").exists());
        assert!(tmp_dir.join("mofei/epub_3316698266/book.bin").exists());
        assert!(!tmp_dir
            .join("mofei/epub_3316698266/thumb_224x315.bmp")
            .exists());
        assert!(!tmp_dir.join("Books/aaa_test_display_none.epub").exists());

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn persist_verifier_preserves_existing_settings_while_seeding_books() {
        let tmp_dir = temp_test_dir("persist-verifier-fixture");
        write_simulator_system_fixture(
            &tmp_dir,
            Path::new("settings.json"),
            SEEDED_READER_GRID_SETTINGS_JSON,
        )
        .unwrap();

        seed_case_specific_sd_fixtures(
            &minimal_case("dashboard-file-browser-layout-persist-verifier-epub-books-smoke"),
            &tmp_dir,
        )
        .unwrap();

        let hidden_settings = fs::read_to_string(tmp_dir.join(".mofei/settings.json")).unwrap();
        let visible_settings = fs::read_to_string(tmp_dir.join("mofei/settings.json")).unwrap();
        assert_eq!(hidden_settings, SEEDED_READER_GRID_SETTINGS_JSON);
        assert_eq!(visible_settings, SEEDED_READER_GRID_SETTINGS_JSON);
        assert_eq!(
            fs::read(tmp_dir.join(".murphy/browser.bin")).unwrap(),
            SEEDED_BROWSER_GRID_PREFS
        );
        assert_eq!(
            fs::read(tmp_dir.join("murphy/browser.bin")).unwrap(),
            SEEDED_BROWSER_GRID_PREFS
        );
        assert!(tmp_dir.join("Books/sshpub.txt").exists());
        assert!(tmp_dir.join("Books/aaa_test_display_none.epub").exists());

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn persist_verifier_seeds_persisted_grid_settings_when_isolated() {
        let tmp_dir = temp_test_dir("persist-verifier-isolated-fixture");

        seed_case_specific_sd_fixtures(
            &minimal_case("dashboard-file-browser-layout-persist-verifier-epub-books-smoke"),
            &tmp_dir,
        )
        .unwrap();

        let hidden_settings = fs::read_to_string(tmp_dir.join(".mofei/settings.json")).unwrap();
        let visible_settings = fs::read_to_string(tmp_dir.join("mofei/settings.json")).unwrap();
        assert_eq!(hidden_settings, SEEDED_READER_GRID_SETTINGS_JSON);
        assert_eq!(visible_settings, SEEDED_READER_GRID_SETTINGS_JSON);
        assert!(tmp_dir.join("Books/sshpub.txt").exists());
        assert!(tmp_dir.join("Books/aaa_test_display_none.epub").exists());

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn settings_fixtures_reset_reader_settings_for_batch_runs() {
        let tmp_dir = temp_test_dir("settings-fixture-reset");
        seed_case_specific_sd_fixtures(
            &minimal_case("dashboard-settings-status-bar-smoke"),
            &tmp_dir,
        )
        .unwrap();

        let hidden_settings = fs::read_to_string(tmp_dir.join(".mofei/settings.json")).unwrap();
        let visible_settings = fs::read_to_string(tmp_dir.join("mofei/settings.json")).unwrap();
        assert_eq!(hidden_settings, SEEDED_READER_SETTINGS_JSON);
        assert_eq!(visible_settings, SEEDED_READER_SETTINGS_JSON);

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn seeded_study_fixture_matches_production_storage_contract() {
        let tmp_dir = temp_test_dir("seeded-study-production-contract");
        seed_case_specific_sd_fixtures(
            &minimal_case("dashboard-study-seeded-review-queue-cycle-smoke"),
            &tmp_dir,
        )
        .unwrap();

        assert!(tmp_dir.join("Study/Decks/seeded_deck.json").exists());
        assert!(!tmp_dir.join(".mofei/study/seeded_deck.json").exists());

        for system_dir in [".murphy", "murphy"] {
            let queue = fs::read(tmp_dir.join(system_dir).join("study/queue.msq")).unwrap();
            assert_eq!(&queue[0..4], b"MSQ1");
            assert_eq!(queue[4], 2);
            assert_eq!(u16::from_le_bytes([queue[8], queue[9]]), 2);

            let state = fs::read(tmp_dir.join(system_dir).join("study/state.mst")).unwrap();
            assert_eq!(&state[0..4], b"MST1");
            assert_eq!(state[4], 1);
            assert_eq!(u16::from_le_bytes([state[6], state[7]]), 3);
        }
        assert!(!tmp_dir.join(".mofei/study/review_queue.json").exists());
        assert!(!tmp_dir.join(".mofei/study/state.json").exists());
        assert!(!tmp_dir.join(".mofei/study/history.json").exists());

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn migrated_reader_settings_fixtures_seed_epub_book() {
        for case_id in [
            "dashboard-settings-ttf-button-nav-cancel-smoke",
            "dashboard-settings-traditional-chinese-fonts-button-nav-cancel-smoke",
            "dashboard-settings-status-bar-toggle-preview-smoke",
        ] {
            let tmp_dir = temp_test_dir(case_id);
            seed_case_specific_sd_fixtures(&minimal_case(case_id), &tmp_dir).unwrap();
            assert!(tmp_dir.join("Books/aaa_test_display_none.epub").exists());
            fs::remove_dir_all(tmp_dir).unwrap();
        }
    }

    #[test]
    fn reader_boundary_fixtures_seed_requested_content() {
        let large_dir = temp_test_dir("large-epub-fixture");
        seed_case_specific_sd_fixtures(
            &minimal_case("dashboard-file-browser-large-epub-open-back-smoke"),
            &large_dir,
        )
        .unwrap();
        assert!(large_dir.join("Books/aaa_test_tables.epub").exists());
        assert!(!large_dir.join("Books/aaa_test_display_none.epub").exists());
        fs::remove_dir_all(large_dir).unwrap();

        let image_dir = temp_test_dir("image-heavy-epub-fixture");
        seed_case_specific_sd_fixtures(
            &minimal_case("dashboard-file-browser-image-heavy-epub-open-back-smoke"),
            &image_dir,
        )
        .unwrap();
        assert!(image_dir.join("Books/aaa_test_jpeg_images.epub").exists());
        fs::remove_dir_all(image_dir).unwrap();

        let empty_txt_dir = temp_test_dir("empty-txt-fixture");
        seed_case_specific_sd_fixtures(
            &minimal_case("dashboard-file-browser-empty-txt-open-back-smoke"),
            &empty_txt_dir,
        )
        .unwrap();
        assert!(empty_txt_dir.join("Books/aaa_empty.txt").exists());
        assert_eq!(
            fs::read_to_string(empty_txt_dir.join("Books/aaa_empty.txt")).unwrap(),
            ""
        );
        fs::remove_dir_all(empty_txt_dir).unwrap();
    }

    #[test]
    fn library_root_epub_reading_filter_fixture_seeds_unknown_total_progress() {
        let tmp_dir = temp_test_dir("library-root-epub-reading-filter-fixture");

        seed_case_specific_sd_fixtures(
            &minimal_case("dashboard-library-root-epub-reading-filter-smoke"),
            &tmp_dir,
        )
        .unwrap();

        let expected_index = seeded_library_root_epub_reading_index();
        assert!(tmp_dir.join("Books/root-reading.epub").exists());
        assert!(tmp_dir.join("Books/aaa_test_display_none.epub").exists());
        assert_eq!(
            fs::read(tmp_dir.join(".murphy/library.bin")).unwrap(),
            expected_index
        );
        assert_eq!(
            fs::read(tmp_dir.join("murphy/library.bin")).unwrap(),
            expected_index
        );
        let progress_path = reader_progress_path("/Books/root-reading.epub");
        assert_eq!(
            fs::read(tmp_dir.join(".murphy").join(&progress_path)).unwrap(),
            seeded_reader_progress_epub()
        );
        assert_eq!(
            fs::read(tmp_dir.join("murphy").join(&progress_path)).unwrap(),
            seeded_reader_progress_epub()
        );

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn txt_touch_page_turn_fixture_uses_multipage_content() {
        let tmp_dir = temp_test_dir("txt-touch-page-turn-fixture");

        seed_case_specific_sd_fixtures(&minimal_case(TXT_TOUCH_PAGE_TURN_CASE_ID), &tmp_dir)
            .unwrap();

        let txt_contents = fs::read_to_string(tmp_dir.join("Books/sshpub.txt")).unwrap();
        assert!(txt_contents.contains("touch page-turn fixture line 120"));
        assert!(
            txt_contents.len() > 8_000,
            "touch page-turn fixture must be long enough to create multiple TXT pages"
        );

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn tc_benchmark_fixture_seeds_only_tc_epub_and_reader_settings() {
        let tmp_dir = temp_test_dir("tc-benchmark-fixture");

        seed_case_specific_sd_fixtures(
            &minimal_case("dashboard-tc-benchmark-page-turn-smoke"),
            &tmp_dir,
        )
        .unwrap();

        let hidden_settings = fs::read_to_string(tmp_dir.join(".mofei/settings.json")).unwrap();
        let visible_settings = fs::read_to_string(tmp_dir.join("mofei/settings.json")).unwrap();
        assert_eq!(hidden_settings, SEEDED_READER_SETTINGS_JSON);
        assert_eq!(visible_settings, SEEDED_READER_SETTINGS_JSON);
        assert!(tmp_dir.join("Books/aaa_test_tc.epub").exists());
        // Verify the EPUB is a valid ZIP file with PK magic bytes
        let epub_bytes = fs::read(tmp_dir.join("Books/aaa_test_tc.epub")).unwrap();
        assert_eq!(
            &epub_bytes[0..2],
            b"PK",
            "TC EPUB fixture must be a valid ZIP (EPUB) file starting with PK header"
        );
        assert!(!tmp_dir.join("Books/sshpub.txt").exists());
        assert!(!tmp_dir.join("Books/aaa_test_display_none.epub").exists());

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn ttf_fixture_keeps_picker_settings_and_font_file() {
        let tmp_dir = temp_test_dir("ttf-fixture-settings");

        seed_case_specific_sd_fixtures(
            &minimal_case(
                "dashboard-settings-ttf-empty-state-refresh-trace-back-to-dashboard-smoke",
            ),
            &tmp_dir,
        )
        .unwrap();

        let hidden_settings = fs::read_to_string(tmp_dir.join(".mofei/settings.json")).unwrap();
        let visible_settings = fs::read_to_string(tmp_dir.join("mofei/settings.json")).unwrap();
        assert_eq!(hidden_settings, SEEDED_TTF_PICKER_SETTINGS_JSON);
        assert_eq!(visible_settings, SEEDED_TTF_PICKER_SETTINGS_JSON);
        assert!(tmp_dir.join(".mofei/fonts/reader.ttf").exists());
        assert!(tmp_dir.join("mofei/fonts/reader.ttf").exists());

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn opds_browser_fixture_seeds_server_and_dashboard_shortcut() {
        let tmp_dir = temp_test_dir("opds-browser-entry-fixture");

        seed_case_specific_sd_fixtures(
            &minimal_case("dashboard-opds-browser-search-smoke"),
            &tmp_dir,
        )
        .unwrap();

        let hidden_opds = fs::read_to_string(tmp_dir.join(".mofei/opds.json")).unwrap();
        let visible_opds = fs::read_to_string(tmp_dir.join("mofei/opds.json")).unwrap();
        assert_eq!(hidden_opds, SEEDED_OPDS_SERVERS_JSON);
        assert_eq!(visible_opds, SEEDED_OPDS_SERVERS_JSON);

        let hidden_shortcuts =
            fs::read_to_string(tmp_dir.join(".mofei/dashboard/shortcuts.json")).unwrap();
        let visible_shortcuts =
            fs::read_to_string(tmp_dir.join("mofei/dashboard/shortcuts.json")).unwrap();
        assert_eq!(hidden_shortcuts, SEEDED_OPDS_DASHBOARD_SHORTCUTS_JSON);
        assert_eq!(visible_shortcuts, SEEDED_OPDS_DASHBOARD_SHORTCUTS_JSON);
        assert!(hidden_shortcuts.contains("\"opds_browser\""));

        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn opds_error_fixtures_seed_case_specific_server_urls() {
        for (case_id, expected_json) in [
            (
                "dashboard-opds-browser-empty-feed-smoke",
                SEEDED_OPDS_EMPTY_SERVERS_JSON,
            ),
            (
                "dashboard-opds-browser-malformed-feed-smoke",
                SEEDED_OPDS_MALFORMED_SERVERS_JSON,
            ),
            (
                "dashboard-opds-browser-fetch-failed-smoke",
                SEEDED_OPDS_FETCH_FAILED_SERVERS_JSON,
            ),
        ] {
            let tmp_dir = temp_test_dir(case_id);
            seed_case_specific_sd_fixtures(&minimal_case(case_id), &tmp_dir).unwrap();
            let hidden_opds = fs::read_to_string(tmp_dir.join(".mofei/opds.json")).unwrap();
            let visible_opds = fs::read_to_string(tmp_dir.join("mofei/opds.json")).unwrap();
            assert_eq!(hidden_opds, expected_json);
            assert_eq!(visible_opds, expected_json);
            fs::remove_dir_all(tmp_dir).unwrap();
        }
    }

    #[test]
    fn panda_lua_app_fixture_installs_signed_pap_package() {
        for (case_id, fixture, expected_app_id, expected_name, storage_requested) in [
            (
                "dashboard-panda-apps-lua-app-smoke",
                PANDA_HELLO_FIXTURE_APP,
                "ai.pandacat.app.hello",
                "Hello Panda",
                false,
            ),
            (
                "dashboard-panda-apps-lua-debug-breakpoint-smoke",
                PANDA_HELLO_FIXTURE_APP,
                "ai.pandacat.app.hello",
                "Hello Panda",
                false,
            ),
            (
                "dashboard-panda-apps-lua-counter-storage-denied-smoke",
                PANDA_COUNTER_FIXTURE_APP,
                "ai.pandacat.example.counter",
                "Persistent Counter",
                true,
            ),
        ] {
            let tmp_dir = temp_test_dir(case_id);

            seed_case_specific_sd_fixtures(&minimal_case(case_id), &tmp_dir).unwrap();

            let hidden_index =
                fs::read_to_string(tmp_dir.join(".mofei/apps/installed.json")).unwrap();
            let visible_index =
                fs::read_to_string(tmp_dir.join("mofei/apps/installed.json")).unwrap();
            for index in [&hidden_index, &visible_index] {
                let parsed: serde_json::Value = serde_json::from_str(index).unwrap();
                let app = &parsed["apps"][0];
                assert_eq!(app["id"], expected_app_id);
                assert_eq!(app["name"], expected_name);
                assert_eq!(
                    app["packagePath"],
                    format!(
                        "/.mofei/apps/{}/{}",
                        expected_app_id, fixture.signed_package_name
                    )
                );
                assert_eq!(app["storageRequested"], storage_requested);
                assert_eq!(app["storageGranted"], false);
            }
            assert!(tmp_dir
                .join(format!(
                    ".mofei/apps/{}/{}",
                    expected_app_id, fixture.signed_package_name
                ))
                .exists());
            assert!(tmp_dir
                .join(format!(
                    "mofei/apps/{}/{}",
                    expected_app_id, fixture.signed_package_name
                ))
                .exists());

            fs::remove_dir_all(tmp_dir).unwrap();
        }
    }

    #[test]
    fn parses_case_skip_reason() {
        let case: E2ECase = serde_json::from_str(
            r#"{
                "id": "blocked-case",
                "description": "known simulator blocker",
                "skipReason": "blocked by simulator allocator issue",
                "steps": [
                    {"type": "home"}
                ]
            }"#,
        )
        .unwrap();

        assert_eq!(
            case.skip_reason.as_deref(),
            Some("blocked by simulator allocator issue")
        );
    }

    // Pins the flag name against a repeat of the brand rename, which rewrote the
    // `release-beta.sh` export to PANDA_ but left this reader on MURPHY_. The two
    // halves silently desynchronised and the three console-driven Study cases went
    // back to timing out as if Study were at fault.
    #[test]
    fn debug_console_flag_accepts_both_spellings() {
        let keys = [
            "PANDA_SIMULATOR_E2E_DEBUG_CONSOLE",
            "MURPHY_SIMULATOR_E2E_DEBUG_CONSOLE",
        ];
        let restore: Vec<_> = keys.iter().map(|k| (*k, std::env::var(k).ok())).collect();
        for key in keys {
            std::env::remove_var(key);
        }

        assert!(
            !debug_console_unavailable(),
            "no flag set must mean the console is assumed present"
        );

        // The name release-beta.sh actually exports today.
        std::env::set_var("PANDA_SIMULATOR_E2E_DEBUG_CONSOLE", "0");
        assert!(
            debug_console_unavailable(),
            "PANDA_ spelling must be honoured"
        );
        std::env::remove_var("PANDA_SIMULATOR_E2E_DEBUG_CONSOLE");

        // Kept working so a stale caller does not silently stop skipping.
        std::env::set_var("MURPHY_SIMULATOR_E2E_DEBUG_CONSOLE", "0");
        assert!(
            debug_console_unavailable(),
            "MURPHY_ spelling must still work"
        );
        std::env::remove_var("MURPHY_SIMULATOR_E2E_DEBUG_CONSOLE");

        // Only "0" disables; a console-present build must still run the cases.
        std::env::set_var("PANDA_SIMULATOR_E2E_DEBUG_CONSOLE", "1");
        assert!(
            !debug_console_unavailable(),
            "\"1\" must not trigger the skip"
        );

        for (key, value) in restore {
            match value {
                Some(v) => std::env::set_var(key, v),
                None => std::env::remove_var(key),
            }
        }
    }

    #[test]
    fn run_skipped_defaults_to_false() {
        let args = Args::parse_from(["mofei-sim-headless", "--firmware", "/tmp/firmware.bin"]);

        assert!(!args.run_skipped);
    }

    #[test]
    fn heap_mode_defaults_to_inherited_environment() {
        let args = Args::parse_from(["mofei-sim-headless", "--firmware", "/tmp/firmware.bin"]);

        assert_eq!(args.heap_mode, None);
    }

    #[test]
    fn parses_device_heap_mode() {
        let args = Args::parse_from([
            "mofei-sim-headless",
            "--firmware",
            "/tmp/firmware.bin",
            "--heap-mode",
            "device",
        ]);

        assert_eq!(args.heap_mode, Some(HeapMode::Device));
        assert_eq!(args.heap_mode.unwrap().as_env_value(), "device");
    }

    #[test]
    fn parses_test_heap_mode() {
        let args = Args::parse_from([
            "mofei-sim-headless",
            "--firmware",
            "/tmp/firmware.bin",
            "--heap-mode",
            "test",
        ]);

        assert_eq!(args.heap_mode, Some(HeapMode::Test));
        assert_eq!(args.heap_mode.unwrap().as_env_value(), "test");
    }

    #[test]
    fn parses_run_skipped_flag() {
        let args = Args::parse_from([
            "mofei-sim-headless",
            "--firmware",
            "/tmp/firmware.bin",
            "--run-skipped",
        ]);

        assert!(args.run_skipped);
    }

    #[test]
    fn parses_per_step_button_hold_override() {
        let case: E2ECase = serde_json::from_str(
            r#"{
                "id": "short-reader-back",
                "description": "uses an instant simulator Back click",
                "steps": [
                    {
                        "type": "button",
                        "button": "back",
                        "holdMs": 0
                    }
                ]
            }"#,
        )
        .unwrap();

        assert_eq!(case.steps[0].button, "back");
        assert_eq!(case.steps[0].hold_ms, Some(0));
    }

    #[test]
    fn defaults_per_step_button_hold_to_global_setting() {
        let case: E2ECase = serde_json::from_str(
            r#"{
                "id": "default-button-hold",
                "description": "keeps existing button semantics",
                "steps": [
                    {
                        "type": "button",
                        "button": "confirm"
                    }
                ]
            }"#,
        )
        .unwrap();

        assert_eq!(case.steps[0].hold_ms, None);
    }

    #[test]
    fn encodes_button_tap_as_adjacent_down_up_frames() {
        let down = encode_button_frame(0, true);
        let up = encode_button_frame(0, false);
        let mut tap = [0u8; (HEADER_LEN + 2) * 2];
        tap[..down.len()].copy_from_slice(&down);
        tap[down.len()..].copy_from_slice(&up);

        assert_eq!(&tap[..HEADER_LEN + 2], &down);
        assert_eq!(&tap[HEADER_LEN + 2..], &up);
    }

    #[test]
    fn parses_touch_swipe_event() {
        let case: E2ECase = serde_json::from_str(
            r#"{
                "id": "touch-swipe",
                "description": "supports swipe touch events",
                "steps": [
                    {
                        "type": "touch",
                        "event": "swipe_up",
                        "x": 240,
                        "y": 400
                    }
                ]
            }"#,
        )
        .unwrap();

        assert_eq!(case.steps[0].step_type, "touch");
        assert_eq!(case.steps[0].event, "swipe_up");
        assert_eq!(case.steps[0].x, 240);
        assert_eq!(case.steps[0].y, 400);
    }

    #[test]
    fn parses_synthetic_touch_event_step() {
        let case: E2ECase = serde_json::from_str(
            r#"{
                "id": "synthetic-touch-swipe",
                "description": "injects an already-recognized simulator touch event",
                "steps": [
                    {
                        "type": "touch_event",
                        "event": "swipe_down",
                        "x": 240,
                        "y": 250
                    }
                ]
            }"#,
        )
        .unwrap();

        assert_eq!(case.steps[0].step_type, "touch_event");
        assert_eq!(case.steps[0].event, "swipe_down");
        assert_eq!(case.steps[0].x, 240);
        assert_eq!(case.steps[0].y, 250);
        assert_eq!(
            synthetic_touch_event_type(&case.steps[0].event).unwrap(),
            SyntheticTouchEventType::SwipeDown
        );
    }

    #[test]
    fn encodes_synthetic_touch_event_frame() {
        let frame =
            encode_synthetic_touch_event_frame(SyntheticTouchEventType::SwipeDown, 240, 250);

        assert_eq!(frame[0], CHANNEL_SYNTHETIC_TOUCH_EVENT);
        assert_eq!(&frame[4..8], &(5u32).to_le_bytes());
        assert_eq!(frame[8], SyntheticTouchEventType::SwipeDown.code());
        assert_eq!(&frame[9..11], &240u16.to_le_bytes());
        assert_eq!(&frame[11..13], &250u16.to_le_bytes());
    }

    #[test]
    fn parses_assert_log_contains_text() {
        let case: E2ECase = serde_json::from_str(
            r#"{
                "id": "file-browser-log",
                "description": "asserts runtime log evidence",
                "steps": [
                    {
                        "type": "assert_log_contains",
                        "text": "E2E:FILE_BROWSER:path=/Books:entries=",
                        "afterInput": true
                    }
                ]
            }"#,
        )
        .unwrap();

        assert_eq!(case.steps[0].step_type, "assert_log_contains");
        assert_eq!(
            case.steps[0].text.as_deref(),
            Some("E2E:FILE_BROWSER:path=/Books:entries=")
        );
        assert!(case.steps[0].after_input);
    }

    #[test]
    fn parses_serial_command_text() {
        let case: E2ECase = serde_json::from_str(
            r#"{
                "id": "storage-retry",
                "description": "runs a production console command",
                "steps": [{"type": "serial_command", "text": "CMD:STORAGE_RETRY"}]
            }"#,
        )
        .unwrap();
        assert_eq!(case.steps[0].step_type, "serial_command");
        assert_eq!(case.steps[0].text.as_deref(), Some("CMD:STORAGE_RETRY"));
    }

    #[test]
    fn parses_wait_for_frame_input_contract() {
        let case: E2ECase = serde_json::from_str(
            r#"{
                "id": "wait-for-native-frame",
                "description": "waits for guest rendering after physical input",
                "steps": [{"type": "button", "button": "confirm", "waitForFrame": true}]
            }"#,
        )
        .unwrap();

        assert!(case.steps[0].wait_for_frame);
    }

    #[tokio::test]
    async fn wait_for_frame_requires_a_frame_after_injection() {
        let (event_tx, mut event_rx) = mpsc::unbounded_channel();
        let mut last_fb = None;
        event_tx.send(HeadlessEvent::Frame(vec![0x11])).unwrap();
        event_tx.send(HeadlessEvent::Injected).unwrap();
        event_tx.send(HeadlessEvent::Frame(vec![0x22])).unwrap();

        let frames =
            wait_for_injection(&mut event_rx, &mut last_fb, Duration::from_millis(20), true)
                .await
                .unwrap();

        assert_eq!(frames, 2);
        assert_eq!(last_fb, Some(vec![0x22]));
    }

    #[tokio::test]
    async fn wait_for_frame_reports_stream_close_after_injection() {
        let (event_tx, mut event_rx) = mpsc::unbounded_channel();
        let mut last_fb = None;
        event_tx.send(HeadlessEvent::Injected).unwrap();
        drop(event_tx);

        let error =
            wait_for_injection(&mut event_rx, &mut last_fb, Duration::from_millis(20), true)
                .await
                .unwrap_err();

        assert!(error.to_string().contains("framebuffer after injection"));
    }

    #[test]
    fn serial_command_line_is_bounded_and_single_line() {
        assert_eq!(
            serial_command_line(" CMD:STORAGE_RETRY ").unwrap(),
            b"CMD:STORAGE_RETRY\n"
        );
        assert!(serial_command_line("").is_err());
        assert!(serial_command_line("CMD:ONE\nCMD:TWO").is_err());
        assert!(serial_command_line(&"x".repeat(MAX_SERIAL_COMMAND_BYTES + 1)).is_err());
    }

    #[test]
    fn parses_before_boot_storage_control() {
        let case: E2ECase = serde_json::from_str(
            r#"{
                "id": "storage-initially-absent",
                "description": "removes storage before the guest starts",
                "beforeBoot": [{
                    "type": "peripheral_control",
                    "device": "storage",
                    "operation": "set_state",
                    "field": "present",
                    "value": false
                }],
                "steps": [{"type": "home"}]
            }"#,
        )
        .unwrap();
        validate_before_boot_steps(&case).unwrap();
        assert_eq!(case.before_boot[0].operation, "set_state");
    }

    #[test]
    fn encodes_storage_control_requests_and_rejects_invalid_one_shot() {
        let presence: E2ECase = serde_json::from_str(
            r#"{
                "id": "storage-remove",
                "description": "removes simulator storage",
                "steps": [{
                    "type": "peripheral_control",
                    "device": "storage",
                    "operation": "set_state",
                    "field": "present",
                    "value": false
                }]
            }"#,
        )
        .unwrap();
        let (device, payload) =
            peripheral_control_request(&presence.steps[0], 0x4433_2211).unwrap();
        assert_eq!(device, PERIPHERAL_CONTROL_STORAGE_DEVICE);
        assert_eq!(payload, [1, 1, 3, 0, 0x11, 0x22, 0x33, 0x44, 2, 0, 1, 0]);

        let corrupt: E2ECase = serde_json::from_str(
            r#"{
                "id": "storage-corrupt-fault",
                "description": "rejects unsupported transient corruption",
                "steps": [{
                    "type": "peripheral_control",
                    "device": "storage",
                    "operation": "set_fault",
                    "field": "corrupt",
                    "value": true,
                    "oneShot": true
                }]
            }"#,
        )
        .unwrap();
        assert!(peripheral_control_request(&corrupt.steps[0], 1).is_err());
    }

    #[test]
    fn encodes_bq27220_state_control_request() {
        let case: E2ECase = serde_json::from_str(
            r#"{
                "id": "bq27220-state",
                "description": "injects production fuel-gauge state",
                "steps": [
                    {
                        "type": "peripheral_control",
                        "device": "bq27220",
                        "operation": "set_state",
                        "voltageMv": 4100,
                        "stateOfCharge": 87
                    }
                ]
            }"#,
        )
        .unwrap();

        let (device, payload) = peripheral_control_request(&case.steps[0], 0x4433_2211).unwrap();
        assert_eq!(device, PERIPHERAL_CONTROL_BQ27220_DEVICE);
        assert_eq!(
            payload,
            [1, 1, 4, 0, 0x11, 0x22, 0x33, 0x44, 3, 0, 0x04, 0x10, 87]
        );
    }

    #[test]
    fn encodes_bq27220_one_shot_nack_and_reset_requests() {
        let case: E2ECase = serde_json::from_str(
            r#"{
                "id": "bq27220-fault-reset",
                "description": "injects one transaction failure and resets state",
                "steps": [
                    {
                        "type": "peripheral_control",
                        "device": "battery",
                        "operation": "set_fault",
                        "field": "nack",
                        "oneShot": true
                    },
                    {
                        "type": "peripheral_control",
                        "device": "bq27220",
                        "operation": "reset"
                    }
                ]
            }"#,
        )
        .unwrap();

        let (_, fault) = peripheral_control_request(&case.steps[0], 7).unwrap();
        assert_eq!(fault, [1, 2, 4, 1, 7, 0, 0, 0, 1, 0, 1]);
        let (_, reset) = peripheral_control_request(&case.steps[1], 8).unwrap();
        assert_eq!(reset, [1, 3, 4, 0, 8, 0, 0, 0, 0, 0]);
    }

    #[test]
    fn rejects_malformed_bq27220_state_and_fault_controls() {
        let invalid_soc: E2ECase = serde_json::from_str(
            r#"{
                "id": "bq27220-invalid-soc",
                "description": "rejects invalid state atomically",
                "steps": [{
                    "type": "peripheral_control",
                    "device": "bq27220",
                    "operation": "set_state",
                    "voltageMv": 4100,
                    "stateOfCharge": 101
                }]
            }"#,
        )
        .unwrap();
        assert!(peripheral_control_request(&invalid_soc.steps[0], 1).is_err());

        let missing_voltage: E2ECase = serde_json::from_str(
            r#"{
                "id": "bq27220-missing-voltage",
                "description": "requires both state fields",
                "steps": [{
                    "type": "peripheral_control",
                    "device": "bq27220",
                    "operation": "set_state",
                    "stateOfCharge": 50
                }]
            }"#,
        )
        .unwrap();
        assert!(peripheral_control_request(&missing_voltage.steps[0], 2).is_err());

        let fault_with_state: E2ECase = serde_json::from_str(
            r#"{
                "id": "bq27220-fault-with-state",
                "description": "keeps fault and state payloads distinct",
                "steps": [{
                    "type": "peripheral_control",
                    "device": "bq27220",
                    "operation": "set_fault",
                    "voltageMv": 3700
                }]
            }"#,
        )
        .unwrap();
        assert!(peripheral_control_request(&fault_with_state.steps[0], 3).is_err());
    }

    #[test]
    fn lilygo_declares_modeled_peripherals_without_audio_capabilities() {
        let board = load_board_runtime(Some("lilygo-t5s3-pro")).unwrap();

        for capability in [
            "location.gnss",
            "power.charger",
            "time.rtc",
            "power.epd-status",
        ] {
            assert!(
                board.capabilities.contains(capability),
                "missing LilyGo capability {capability}"
            );
        }
        assert!(!board.capabilities.contains("audio.playback"));
        assert!(!board.capabilities.contains("audio.capture"));
    }

    #[test]
    fn encodes_lilygo_power_peripheral_control_requests() {
        let charger = peripheral_step(serde_json::json!({
            "type": "peripheral_control",
            "device": "bq25896",
            "operation": "set_state",
            "status": 0x54,
            "latchedFault": 0x12,
            "currentFault": 0x34
        }));
        let (device, payload) = peripheral_control_request(&charger, 0x4433_2211).unwrap();
        assert_eq!(device, PERIPHERAL_CONTROL_BQ25896_DEVICE);
        assert_eq!(
            payload,
            [1, 1, 6, 0, 0x11, 0x22, 0x33, 0x44, 3, 0, 0x54, 0x12, 0x34]
        );

        let charger_nack_once = peripheral_step(serde_json::json!({
            "type": "peripheral_control",
            "device": "charger",
            "operation": "set_fault",
            "field": "nack",
            "oneShot": true
        }));
        let (_, payload) = peripheral_control_request(&charger_nack_once, 7).unwrap();
        assert_eq!(payload, [1, 2, 6, 0, 7, 0, 0, 0, 1, 0, 1]);

        let rtc = peripheral_step(serde_json::json!({
            "type": "peripheral_control",
            "device": "pcf8563",
            "operation": "set_state",
            "year": 26,
            "month": 7,
            "day": 14,
            "weekday": 2,
            "hour": 12,
            "minute": 34,
            "second": 56,
            "oscillatorStopped": true
        }));
        let (device, payload) = peripheral_control_request(&rtc, 8).unwrap();
        assert_eq!(device, PERIPHERAL_CONTROL_PCF8563_DEVICE);
        assert_eq!(
            payload,
            [1, 1, 7, 0, 8, 0, 0, 0, 8, 0, 26, 7, 14, 2, 12, 34, 56, 1]
        );

        let rtc_invalid = peripheral_step(serde_json::json!({
            "type": "peripheral_control",
            "device": "rtc",
            "operation": "set_fault",
            "field": "invalid",
            "payloadHex": "0102030405060708"
        }));
        let (_, payload) = peripheral_control_request(&rtc_invalid, 9).unwrap();
        assert_eq!(
            payload,
            [1, 2, 7, 0, 9, 0, 0, 0, 9, 0, 3, 1, 2, 3, 4, 5, 6, 7, 8]
        );

        let epd_power = peripheral_step(serde_json::json!({
            "type": "peripheral_control",
            "device": "tps651851",
            "operation": "set_state",
            "field": "hold_not_ready",
            "value": true
        }));
        let (device, payload) = peripheral_control_request(&epd_power, 10).unwrap();
        assert_eq!(device, PERIPHERAL_CONTROL_TPS651851_DEVICE);
        assert_eq!(payload, [1, 1, 9, 0, 10, 0, 0, 0, 1, 0, 1]);

        let epd_clear = peripheral_step(serde_json::json!({
            "type": "peripheral_control",
            "device": "epd-power",
            "operation": "set_fault",
            "field": "clear"
        }));
        let (_, payload) = peripheral_control_request(&epd_clear, 11).unwrap();
        assert_eq!(payload, [1, 2, 9, 0, 11, 0, 0, 0, 1, 0, 0]);
    }

    #[test]
    fn encodes_lilygo_gnss_control_requests() {
        let l76k = peripheral_step(serde_json::json!({
            "type": "peripheral_control",
            "device": "gnss",
            "operation": "set_state",
            "variant": "l76k",
            "fixture": "no_fix",
            "powered": true
        }));
        let (device, payload) = peripheral_control_request(&l76k, 12).unwrap();
        assert_eq!(device, PERIPHERAL_CONTROL_GNSS_DEVICE);
        assert_eq!(payload, [1, 1, 8, 0, 12, 0, 0, 0, 3, 0, 0, 1, 1]);

        let ublox_stale = peripheral_step(serde_json::json!({
            "type": "peripheral_control",
            "device": "gps",
            "operation": "set_state",
            "variant": "ublox-m10",
            "fixture": "stale",
            "powered": false
        }));
        let (_, payload) = peripheral_control_request(&ublox_stale, 13).unwrap();
        assert_eq!(payload, [1, 1, 8, 0, 13, 0, 0, 0, 3, 0, 1, 4, 0]);

        let silent = peripheral_step(serde_json::json!({
            "type": "peripheral_control",
            "device": "gnss",
            "operation": "set_fault",
            "field": "silent"
        }));
        let (_, payload) = peripheral_control_request(&silent, 14).unwrap();
        assert_eq!(payload, [1, 2, 8, 0, 14, 0, 0, 0, 1, 0, 2]);
    }

    #[test]
    fn rejects_unrelated_lilygo_peripheral_fields() {
        for step in [
            peripheral_step(serde_json::json!({
                "type": "peripheral_control",
                "device": "bq25896",
                "operation": "set_state",
                "status": 1,
                "latchedFault": 2,
                "currentFault": 3,
                "endpointId": 99
            })),
            peripheral_step(serde_json::json!({
                "type": "peripheral_control",
                "device": "pcf8563",
                "operation": "set_state",
                "year": 26,
                "month": 7,
                "day": 14,
                "weekday": 2,
                "hour": 12,
                "minute": 34,
                "second": 56,
                "voltageMv": 4000
            })),
            peripheral_step(serde_json::json!({
                "type": "peripheral_control",
                "device": "gnss",
                "operation": "set_state",
                "variant": "l76k",
                "fixture": "fixed",
                "powered": true,
                "rssiDbm": -70
            })),
            peripheral_step(serde_json::json!({
                "type": "peripheral_control",
                "device": "tps651851",
                "operation": "set_state",
                "field": "hold_not_ready",
                "value": false,
                "payloadHex": "aa"
            })),
        ] {
            assert!(peripheral_control_request(&step, 1).is_err());
        }
    }

    #[test]
    fn peripheral_control_request_enforces_64_byte_wire_bound() {
        let max_packet = peripheral_step(serde_json::json!({
            "type": "peripheral_control",
            "device": "sx1262",
            "operation": "schedule_receive",
            "payloadHex": "aa".repeat(SX1262_SCRIPTED_MAX_PACKET_BYTES)
        }));
        let (_, payload) = peripheral_control_request(&max_packet, 15).unwrap();
        assert_eq!(payload.len(), PERIPHERAL_CONTROL_MAX_PAYLOAD_BYTES);

        let oversized_packet = peripheral_step(serde_json::json!({
            "type": "peripheral_control",
            "device": "sx1262",
            "operation": "schedule_receive",
            "payloadHex": "aa".repeat(SX1262_SCRIPTED_MAX_PACKET_BYTES + 1)
        }));
        assert!(peripheral_control_request(&oversized_packet, 16).is_err());
    }

    #[test]
    fn encodes_sx1262_endpoint_scripted_receive_and_fault_controls() {
        let case: E2ECase = serde_json::from_str(
            r#"{
                "id": "sx1262-controls",
                "description": "encodes typed radio controls",
                "steps": [
                    {
                        "type": "peripheral_control",
                        "device": "sx1262",
                        "operation": "set_endpoint",
                        "endpointId": 72623859790382856
                    },
                    {
                        "type": "peripheral_control",
                        "device": "radio",
                        "operation": "schedule_receive",
                        "payloadHex": "A1B2",
                        "deliveryDelayUs": 1000,
                        "rssiDbm": -70,
                        "snrDb": 7
                    },
                    {
                        "type": "peripheral_control",
                        "device": "lora",
                        "operation": "set_fault",
                        "field": "crc_error"
                    }
                ]
            }"#,
        )
        .unwrap();

        let (device, endpoint) = peripheral_control_request(&case.steps[0], 9).unwrap();
        assert_eq!(device, PERIPHERAL_CONTROL_SX1262_DEVICE);
        assert_eq!(
            endpoint,
            [1, 1, 5, 0, 9, 0, 0, 0, 9, 0, 1, 8, 7, 6, 5, 4, 3, 2, 1]
        );

        let (_, receive) = peripheral_control_request(&case.steps[1], 10).unwrap();
        assert_eq!(
            receive,
            [1, 1, 5, 0, 10, 0, 0, 0, 11, 0, 2, 0xE8, 0x03, 0, 0, 0xBA, 0xFF, 7, 2, 0xA1, 0xB2]
        );

        let (_, fault) = peripheral_control_request(&case.steps[2], 11).unwrap();
        assert_eq!(fault, [1, 2, 5, 0, 11, 0, 0, 0, 1, 0, 4]);
    }

    #[test]
    fn converts_sx1262_transmit_event_to_matched_broker_delivery() {
        let mut event = vec![1, 0x84, 5, 0];
        event.extend_from_slice(&2u32.to_le_bytes());
        event.extend_from_slice(&101u64.to_le_bytes());
        event.extend_from_slice(&55u64.to_le_bytes());
        event.extend_from_slice(&920_000_000u32.to_le_bytes());
        event.extend_from_slice(&[1, 9, 4, 7]);
        event.extend_from_slice(&0x1424u16.to_le_bytes());
        event.push(3);
        event.extend_from_slice(&[0xAA, 0xBB, 0xCC]);

        let (sequence, endpoint, control) =
            radio_tx_event_to_broker_control(&event, 0x8000_0001).unwrap();

        assert_eq!(sequence, 2);
        assert_eq!(endpoint, 101);
        assert_eq!(&control[..10], &[1, 1, 5, 0, 1, 0, 0, 0x80, 30, 0]);
        assert_eq!(control[10], SX1262_CONTROL_DELIVER_BROKER_PACKET);
        assert_eq!(&control[11..19], &101u64.to_le_bytes());
        assert_eq!(&control[19..29], &event[24..34]);
        assert_eq!(&control[29..33], &1000u32.to_le_bytes());
        assert_eq!(&control[33..35], &(-70i16).to_le_bytes());
        assert_eq!(control[35], 7);
        assert_eq!(control[36], 3);
        assert_eq!(&control[37..], &[0xAA, 0xBB, 0xCC]);
    }

    #[test]
    fn radio_broker_requires_bind_and_peer_paths_together() {
        assert!(Args::try_parse_from([
            "mofei-sim-headless",
            "--firmware",
            "/tmp/firmware.bin",
            "--radio-broker-bind",
            "/tmp/radio-a.sock"
        ])
        .is_err());
    }

    fn sx1262_tx_event(sequence: u32, endpoint_id: u64, payload: &[u8]) -> Vec<u8> {
        let mut event = vec![
            1,
            PERIPHERAL_CONTROL_RADIO_TX,
            PERIPHERAL_CONTROL_SX1262_DEVICE,
            0,
        ];
        event.extend_from_slice(&sequence.to_le_bytes());
        event.extend_from_slice(&endpoint_id.to_le_bytes());
        event.extend_from_slice(&55u64.to_le_bytes());
        event.extend_from_slice(&920_000_000u32.to_le_bytes());
        event.extend_from_slice(&[1, 9, 4, 7]);
        event.extend_from_slice(&0x1424u16.to_le_bytes());
        event.push(payload.len() as u8);
        event.extend_from_slice(payload);
        event
    }

    fn radio_broker_args(bind: &Path, peer: &Path) -> Args {
        Args::parse_from([
            "mofei-sim-headless".into(),
            "--firmware".into(),
            "/tmp/firmware.bin".into(),
            "--radio-broker-bind".into(),
            bind.as_os_str().to_owned(),
            "--radio-broker-peer".into(),
            peer.as_os_str().to_owned(),
        ])
    }

    #[tokio::test]
    async fn radio_peer_bridge_orders_packets_and_tears_down_run_state() {
        let root = temp_test_dir("radio-peer-bridge");
        let socket_a = root.join("a.sock");
        let socket_b = root.join("b.sock");
        let args_a = radio_broker_args(&socket_a, &socket_b);
        let args_b = radio_broker_args(&socket_b, &socket_a);
        let writer_a = debug_transport::HeadlessDebugWriter::new();
        let writer_b = debug_transport::HeadlessDebugWriter::new();
        let (tx_a, mut rx_a) = mpsc::unbounded_channel();
        let (tx_b, mut rx_b) = mpsc::unbounded_channel();
        writer_a.bind(tx_a).await;
        writer_b.bind(tx_b).await;

        let (sender_a, handle_a) = start_radio_peer_bridge(&args_a, writer_a.clone())
            .await
            .unwrap();
        let (_sender_b, handle_b) = start_radio_peer_bridge(&args_b, writer_b.clone())
            .await
            .unwrap();
        let sender_a = sender_a.unwrap();
        sender_a
            .forward(&sx1262_tx_event(1, 101, &[0xAA, 0xBB, 0xCC]))
            .await
            .unwrap();
        let first = time::timeout(Duration::from_millis(200), rx_b.recv())
            .await
            .unwrap()
            .unwrap();
        assert_eq!(first.channel, CHANNEL_CONTROL);
        assert_eq!(first.payload[10], SX1262_CONTROL_DELIVER_BROKER_PACKET);
        assert_eq!(
            &first.payload[first.payload.len() - 3..],
            &[0xAA, 0xBB, 0xCC]
        );

        sender_a
            .forward(&sx1262_tx_event(1, 101, &[0xDD]))
            .await
            .unwrap();
        assert!(time::timeout(Duration::from_millis(50), rx_b.recv())
            .await
            .is_err());
        sender_a
            .forward(&sx1262_tx_event(2, 101, &[0xDD]))
            .await
            .unwrap();
        let second = time::timeout(Duration::from_millis(200), rx_b.recv())
            .await
            .unwrap()
            .unwrap();
        assert_eq!(*second.payload.last().unwrap(), 0xDD);
        assert!(rx_a.try_recv().is_err());

        drop(sender_a);
        drop(handle_a);
        drop(handle_b);
        assert!(!socket_a.exists());
        assert!(!socket_b.exists());

        let (sender_a, handle_a) = start_radio_peer_bridge(&args_a, writer_a).await.unwrap();
        let (_sender_b, handle_b) = start_radio_peer_bridge(&args_b, writer_b).await.unwrap();
        sender_a
            .unwrap()
            .forward(&sx1262_tx_event(1, 101, &[0xEE]))
            .await
            .unwrap();
        let restarted = time::timeout(Duration::from_millis(200), rx_b.recv())
            .await
            .unwrap()
            .unwrap();
        assert_eq!(*restarted.payload.last().unwrap(), 0xEE);
        drop(handle_a);
        drop(handle_b);
        assert!(!socket_a.exists());
        assert!(!socket_b.exists());
        let _ = fs::remove_dir_all(root);
    }

    #[tokio::test]
    async fn peripheral_control_ack_wait_matches_request_and_preserves_frame() {
        let (event_tx, mut event_rx) = mpsc::unbounded_channel();
        let mut last_fb = None;
        event_tx
            .send(HeadlessEvent::Frame(vec![0xAA, 0x55]))
            .unwrap();
        event_tx
            .send(HeadlessEvent::PeripheralControl(vec![
                1, 0x83, 3, 0, 9, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0,
            ]))
            .unwrap();
        event_tx
            .send(HeadlessEvent::PeripheralControl(vec![
                1, 0x80, 3, 1, 9, 0, 0, 0, 0, 0, 0, 0,
            ]))
            .unwrap();

        wait_for_peripheral_control_ack(
            &mut event_rx,
            &mut last_fb,
            PERIPHERAL_CONTROL_STORAGE_DEVICE,
            9,
            Duration::from_millis(20),
        )
        .await
        .unwrap();
        assert_eq!(last_fb, Some(vec![0xAA, 0x55]));
    }

    #[test]
    fn peripheral_control_ack_reports_rejection_error() {
        let rejection = [1, 0x80, 3, 0, 4, 0, 0, 0, 7, 0, 0, 0];
        let error = peripheral_control_ack_result(&rejection, 3, 4)
            .expect("matching ACK")
            .unwrap_err();
        assert!(error.to_string().contains("accepted=0 error=7"));
    }

    #[test]
    fn peripheral_control_ack_requires_exact_12_byte_payload() {
        let valid = [1, 0x80, 6, 1, 4, 3, 2, 1, 0, 0, 0, 0];
        assert!(peripheral_control_ack_result(&valid, 6, 0x0102_0304)
            .expect("matching ACK")
            .is_ok());

        let short = &valid[..11];
        assert!(peripheral_control_ack_result(short, 6, 0x0102_0304)
            .expect("matching short ACK")
            .is_err());

        let mut long = valid.to_vec();
        long.push(0);
        assert!(peripheral_control_ack_result(&long, 6, 0x0102_0304)
            .expect("matching long ACK")
            .is_err());
    }

    fn peripheral_trace(data: &[u8]) -> Vec<u8> {
        let mut trace = vec![
            PERIPHERAL_CONTROL_VERSION,
            PERIPHERAL_CONTROL_TRACE,
            PERIPHERAL_CONTROL_TPS651851_DEVICE,
            0,
        ];
        trace.extend_from_slice(&0x4433_2211u32.to_le_bytes());
        trace.extend_from_slice(&0x0807_0605_0403_0201u64.to_le_bytes());
        trace.extend_from_slice(&[1, 0x68, 1, 0x0b, data.len() as u8, 0]);
        trace.extend_from_slice(data);
        trace
    }

    #[test]
    fn peripheral_control_trace_summary_accepts_bounded_trace() {
        let trace = peripheral_trace(&[0xaa, 0xbb, 0xcc]);
        let summary = peripheral_control_trace_summary(&trace)
            .expect("TRACE message")
            .unwrap();

        assert_eq!(
            summary,
            "peripheral_trace device=9 sequence=1144201745 virtual_time_ns=578437695752307201 result=0 bus=1 address=0x68 direction=read register=0x0b bytes=3 data=aabbcc"
        );
    }

    #[test]
    fn peripheral_control_trace_summary_rejects_oversize_and_malformed_traces() {
        let oversized = peripheral_trace(&[0; PERIPHERAL_CONTROL_TRACE_DATA_MAX_BYTES + 1]);
        assert!(peripheral_control_trace_summary(&oversized)
            .expect("TRACE message")
            .is_err());

        let truncated_header = vec![
            PERIPHERAL_CONTROL_VERSION,
            PERIPHERAL_CONTROL_TRACE,
            PERIPHERAL_CONTROL_GNSS_DEVICE,
        ];
        assert!(peripheral_control_trace_summary(&truncated_header)
            .expect("TRACE message")
            .is_err());

        let mut length_mismatch = peripheral_trace(&[0xaa, 0xbb]);
        length_mismatch[20] = 3;
        assert!(peripheral_control_trace_summary(&length_mismatch)
            .expect("TRACE message")
            .is_err());

        assert!(peripheral_control_trace_summary(&[1, PERIPHERAL_CONTROL_ACK]).is_none());
    }

    #[test]

    fn parses_assert_screenshot_region_thresholds() {
        let case: E2ECase = serde_json::from_str(
            r#"{
                "id": "pixel-region",
                "description": "asserts screenshot pixels",
                "steps": [
                    {
                        "type": "assert_screenshot_region",
                        "label": "cover-card-title-band",
                        "x": 16,
                        "y": 282,
                        "width": 218,
                        "height": 24,
                        "minDarkRatio": 0.01,
                        "maxDarkRatio": 0.60,
                        "minWhitePixels": 100
                    }
                ]
            }"#,
        )
        .unwrap();

        let step = &case.steps[0];
        assert_eq!(step.step_type, "assert_screenshot_region");
        assert_eq!(step.label.as_deref(), Some("cover-card-title-band"));
        assert_eq!(step.x, 16);
        assert_eq!(step.y, 282);
        assert_eq!(step.width, 218);
        assert_eq!(step.height, 24);
        assert_eq!(step.min_dark_ratio, Some(0.01));
        assert_eq!(step.max_dark_ratio, Some(0.60));
        assert_eq!(step.min_white_pixels, Some(100));
    }

    #[test]
    fn parses_assert_sd_file_exists_path() {
        let case: E2ECase = serde_json::from_str(
            r#"{
                "id": "sd-artifact",
                "description": "asserts SD artifact",
                "steps": [
                    {
                        "type": "assert_sd_file_exists",
                        "path": "mofei/epub_3316698266/thumb_224x315.bmp"
                    }
                ]
            }"#,
        )
        .unwrap();

        let step = &case.steps[0];
        assert_eq!(step.step_type, "assert_sd_file_exists");
        assert_eq!(
            step.path.as_deref(),
            Some("mofei/epub_3316698266/thumb_224x315.bmp")
        );
    }

    #[test]
    fn parses_assert_sd_file_contains_text() {
        let case: E2ECase = serde_json::from_str(
            r#"{
                "id": "sd-artifact-content",
                "description": "asserts SD artifact content",
                "steps": [
                    {
                        "type": "assert_sd_file_contains",
                        "path": "mofei/study/user_words.json",
                        "text": "\"front\":\"q\""
                    }
                ]
            }"#,
        )
        .unwrap();

        let step = &case.steps[0];
        assert_eq!(step.step_type, "assert_sd_file_contains");
        assert_eq!(step.path.as_deref(), Some("mofei/study/user_words.json"));
        assert_eq!(step.text.as_deref(), Some("\"front\":\"q\""));
    }

    #[test]
    fn sd_file_assertion_rejects_paths_outside_sd_root() {
        assert_eq!(
            safe_sd_relative_path("mofei/epub_3316698266/thumb_224x315.bmp").unwrap(),
            PathBuf::from("mofei/epub_3316698266/thumb_224x315.bmp")
        );
        assert!(safe_sd_relative_path("").is_err());
        assert!(safe_sd_relative_path("/tmp/thumb.bmp").is_err());
        assert!(safe_sd_relative_path("../thumb.bmp").is_err());
        assert!(safe_sd_relative_path("mofei/../thumb.bmp").is_err());
    }

    #[test]
    fn boot_copy_files_seed_repository_asset_inside_sd_root() {
        let tmp_dir = temp_test_dir("boot-copy-files");
        let mut case = minimal_case("boot-copy-files");
        case.boot.copy_files.push(E2EBootCopyFile {
            path: "/Study/.course-staging/demo/release/course.json".to_string(),
            source: "apps/panda-os/tools/study/demos/ja-travel-zh-hant-demo/course.json"
                .to_string(),
        });

        validate_boot_copy_files(&case).unwrap();
        seed_boot_copy_files(&case, &tmp_dir).unwrap();

        let seeded =
            fs::read(tmp_dir.join("Study/.course-staging/demo/release/course.json")).unwrap();
        let source = fs::read(
            repo_root_path()
                .join("apps/panda-os/tools/study/demos/ja-travel-zh-hant-demo/course.json"),
        )
        .unwrap();
        assert_eq!(seeded, source);
        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn boot_copy_files_reject_paths_outside_declared_roots_and_duplicates() {
        assert!(safe_sd_boot_destination("Study/course.json").is_err());
        assert!(safe_sd_boot_destination("/../course.json").is_err());
        assert!(safe_repo_copy_source("/tmp/course.json").is_err());
        assert!(safe_repo_copy_source("../course.json").is_err());

        let mut case = minimal_case("duplicate-boot-copy");
        case.boot.copy_files = vec![
            E2EBootCopyFile {
                path: "/Study/course.json".to_string(),
                source: "apps/panda-os/tools/study/demos/ja-travel-zh-hant-demo/course.json"
                    .to_string(),
            },
            E2EBootCopyFile {
                path: "/Study/course.json".to_string(),
                source: "apps/panda-os/tools/study/demos/ja-travel-zh-hant-demo/lessons/001.json"
                    .to_string(),
            },
        ];
        let error = validate_boot_copy_files(&case).unwrap_err();
        assert!(error.to_string().contains("duplicate destination"));
    }

    #[test]
    fn boot_copy_files_fail_when_declared_repository_source_is_missing() {
        let tmp_dir = temp_test_dir("boot-copy-missing-source");
        let mut case = minimal_case("boot-copy-missing-source");
        case.boot.copy_files.push(E2EBootCopyFile {
            path: "/Study/course.json".to_string(),
            source: "apps/panda-os/tools/study/demos/does-not-exist/course.json".to_string(),
        });

        validate_boot_copy_files(&case).unwrap();
        let error = seed_boot_copy_files(&case, &tmp_dir).unwrap_err();
        assert!(error.to_string().contains("does not exist"));
        assert!(!tmp_dir.join("Study/course.json").exists());
        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn sd_file_contains_assertion_reads_expected_text() {
        let tmp_dir = std::env::temp_dir().join(format!(
            "mofei-headless-sd-file-contains-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&tmp_dir);
        let file_path = tmp_dir.join("mofei/study/user_words.json");
        fs::create_dir_all(file_path.parent().unwrap()).unwrap();
        fs::write(
            &file_path,
            r#"{"title":"User Words","cards":[{"front":"q","back":"q"}]}"#,
        )
        .unwrap();

        assert!(assert_sd_file_contains(
            &tmp_dir,
            Path::new("mofei/study/user_words.json"),
            "mofei/study/user_words.json",
            r#""front":"q""#
        )
        .is_ok());
        assert!(assert_sd_file_contains(
            &tmp_dir,
            Path::new("mofei/study/user_words.json"),
            "mofei/study/user_words.json",
            r#""front":"missing""#
        )
        .is_err());
        fs::remove_dir_all(&tmp_dir).unwrap();
    }

    fn set_output_pixel(fb: &mut [u8], geometry: &BoardGeometry, x: u32, y: u32, dark: bool) {
        let (source_x, source_y) = geometry.source_pixel_for_output(x, y).unwrap();
        let pixel = source_y * geometry.raw_width + source_x;
        let byte_idx = (pixel / 8) as usize;
        let bit = pixel % 8;
        let mask = 0x80 >> bit;
        if dark {
            fb[byte_idx] &= !mask;
        } else {
            fb[byte_idx] |= mask;
        }
    }

    #[test]
    fn framebuffer_region_stats_uses_portrait_screenshot_coordinates() {
        let geometry = mofei_test_geometry();
        let mut fb = vec![0xFFu8; geometry.fb_bytes()];
        set_output_pixel(&mut fb, &geometry, 10, 20, true);
        set_output_pixel(&mut fb, &geometry, 11, 20, true);
        set_output_pixel(&mut fb, &geometry, 10, 21, true);

        let stats = framebuffer_region_stats(&fb, &geometry, 10, 20, 2, 2).unwrap();

        assert_eq!(
            stats,
            ScreenshotRegionStats {
                total_pixels: 4,
                dark_pixels: 3,
                white_pixels: 1
            }
        );
        assert!(framebuffer_pixel_is_dark(&fb, &geometry, 10, 20).unwrap());
        assert!(!framebuffer_pixel_is_dark(&fb, &geometry, 11, 21).unwrap());
    }

    #[test]
    fn screenshot_region_thresholds_pass_and_fail_with_stats() {
        let geometry = mofei_test_geometry();
        let mut fb = vec![0xFFu8; geometry.fb_bytes()];
        set_output_pixel(&mut fb, &geometry, 10, 20, true);
        set_output_pixel(&mut fb, &geometry, 11, 20, true);

        let mut step: CaseStep = serde_json::from_str(
            r#"{
                "type": "assert_screenshot_region",
                "label": "two-dark-pixels",
                "x": 10,
                "y": 20,
                "width": 2,
                "height": 2,
                "minDarkPixels": 2,
                "maxDarkRatio": 0.50
            }"#,
        )
        .unwrap();
        let summary =
            assert_screenshot_region(&step, &fb, &geometry, ReferenceViewport::default()).unwrap();
        assert!(summary.contains("two-dark-pixels"));
        assert!(summary.contains("dark=2/4"));

        step.min_dark_pixels = Some(3);
        let err = assert_screenshot_region(&step, &fb, &geometry, ReferenceViewport::default())
            .unwrap_err();
        assert!(format!("{err}").contains("dark pixels 2 below min 3"));
    }

    #[test]
    fn assertion_checkpoint_capture_is_opt_in_safe_and_step_unique() {
        let geometry = mofei_test_geometry();
        let mut fb = vec![0xFFu8; geometry.fb_bytes()];
        set_output_pixel(&mut fb, &geometry, 10, 20, true);
        let step: CaseStep = serde_json::from_str(
            r#"{
                "type": "assert_screenshot_region",
                "label": "../same label",
                "x": 10,
                "y": 20,
                "width": 1,
                "height": 1,
                "minDarkPixels": 1
            }"#,
        )
        .unwrap();
        let tmp_dir = temp_test_dir("assertion-checkpoint-opt-in");

        assert_screenshot_region_with_checkpoint(
            &step,
            &fb,
            &geometry,
            ReferenceViewport::default(),
            false,
            &tmp_dir,
            3,
        )
        .unwrap();
        assert!(!tmp_dir.join("checkpoints").exists());

        assert_screenshot_region_with_checkpoint(
            &step,
            &fb,
            &geometry,
            ReferenceViewport::default(),
            true,
            &tmp_dir,
            3,
        )
        .unwrap();
        assert_screenshot_region_with_checkpoint(
            &step,
            &fb,
            &geometry,
            ReferenceViewport::default(),
            true,
            &tmp_dir,
            4,
        )
        .unwrap();

        let first = tmp_dir.join("checkpoints").join("004-same-label.png");
        let second = tmp_dir.join("checkpoints").join("005-same-label.png");
        assert!(first.is_file());
        assert!(second.is_file());
        let image = image::open(&first).unwrap().to_luma8();
        assert_eq!(
            image.dimensions(),
            (geometry.output_width, geometry.output_height)
        );
        assert_eq!(image.get_pixel(10, 20).0[0], 0);

        fs::remove_dir_all(&tmp_dir).unwrap();
    }

    #[test]
    fn failed_screenshot_assertion_does_not_write_checkpoint() {
        let geometry = mofei_test_geometry();
        let fb = vec![0xFFu8; geometry.fb_bytes()];
        let step: CaseStep = serde_json::from_str(
            r#"{
                "type": "assert_screenshot_region",
                "label": "must-fail",
                "x": 10,
                "y": 20,
                "width": 1,
                "height": 1,
                "minDarkPixels": 1
            }"#,
        )
        .unwrap();
        let tmp_dir = temp_test_dir("failed-assertion-checkpoint");

        assert!(assert_screenshot_region_with_checkpoint(
            &step,
            &fb,
            &geometry,
            ReferenceViewport::default(),
            true,
            &tmp_dir,
            0,
        )
        .is_err());
        assert!(!tmp_dir.join("checkpoints").exists());

        fs::remove_dir_all(&tmp_dir).unwrap();
    }

    #[test]
    fn framebuffer_region_stats_supports_s37uc_native_coordinates() {
        let geometry = s37uc_test_geometry();
        let mut fb = vec![0xFFu8; geometry.fb_bytes()];
        set_output_pixel(&mut fb, &geometry, 10, 20, true);
        set_output_pixel(&mut fb, &geometry, 11, 20, true);

        let stats = framebuffer_region_stats(&fb, &geometry, 10, 20, 2, 1).unwrap();

        assert_eq!(
            stats,
            ScreenshotRegionStats {
                total_pixels: 2,
                dark_pixels: 2,
                white_pixels: 0
            }
        );
        assert!(framebuffer_pixel_is_dark(&fb, &geometry, 11, 20).unwrap());
    }

    #[test]
    fn parses_concrete_reader_trace_before_generic_reader() {
        let mut ctx = E2EContext::new();
        ctx.parse_activity_transition("[MOFEI-TRACE] TxtReaderActivity::render");

        assert_eq!(ctx.current_activity.as_deref(), Some("TxtReader"));
    }

    #[test]
    fn parses_extended_activity_traces() {
        let mut ctx = E2EContext::new();
        ctx.parse_activity_transition("[MOFEI-TRACE] OpdsServerListActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("OpdsServerList"));

        ctx.parse_activity_transition("[MOFEI-TRACE] KeyboardEntryActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("KeyboardEntry"));

        ctx.parse_activity_transition("[MOFEI-TRACE] ReaderFrontlightSelectionActivity::render");
        assert_eq!(
            ctx.current_activity.as_deref(),
            Some("ReaderFrontlightSelection")
        );

        ctx.parse_activity_transition("[MOFEI-TRACE] EpubSearchResultsActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("EpubSearchResults"));

        ctx.parse_activity_transition("[MOFEI-TRACE] EpubReaderPercentSelectionActivity::render");
        assert_eq!(
            ctx.current_activity.as_deref(),
            Some("EpubReaderPercentSelection")
        );

        ctx.parse_activity_transition("[MOFEI-TRACE] TxtSearchResultsActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("TxtSearchResults"));

        ctx.parse_activity_transition("[MOFEI-TRACE] TxtBookmarksActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("TxtBookmarks"));

        ctx.parse_activity_transition("[MOFEI-TRACE] EpubBookmarksActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("EpubBookmarks"));

        ctx.parse_activity_transition("[MOFEI-TRACE] EpubReaderFootnotesActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("EpubReaderFootnotes"));

        ctx.parse_activity_transition("[MOFEI-TRACE] ReadingHubActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("Reading"));

        ctx.parse_activity_transition("[MOFEI-TRACE] StudyHubActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("Study"));

        ctx.parse_activity_transition("[MOFEI-TRACE] StudyCardsTodayActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("StudyCardsToday"));

        ctx.parse_activity_transition("[MOFEI-TRACE] Game2048Activity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("Game2048"));

        ctx.parse_activity_transition("[MOFEI-TRACE] ButtonRemapActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("ButtonRemap"));

        ctx.parse_activity_transition("[MOFEI-TRACE] DeviceDiagnosticsActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("DeviceDiagnostics"));

        ctx.parse_activity_transition("[MOFEI-TRACE] StatusBarSettingsActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("StatusBarSettings"));

        ctx.parse_activity_transition("[MOFEI-TRACE] TimeZoneSelectActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("TimeZoneSelect"));

        ctx.parse_activity_transition("[MOFEI-TRACE] TraditionalChineseFontsActivity::render");
        assert_eq!(
            ctx.current_activity.as_deref(),
            Some("TraditionalChineseFonts")
        );

        ctx.parse_activity_transition("[MOFEI-TRACE] LanguageSelectActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("LanguageSelect"));

        ctx.parse_activity_transition("[MOFEI-TRACE] SleepWallpaperActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("SleepWallpaper"));

        ctx.parse_activity_transition("[MOFEI-TRACE] AppletsActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("Applets"));

        ctx.parse_activity_transition("[MOFEI-TRACE] LuaAppActivity::render");
        assert_eq!(ctx.current_activity.as_deref(), Some("LuaApp"));
    }

    #[test]
    fn parses_ui_refresh_activity_markers() {
        let mut ctx = E2EContext::new();

        ctx.parse_activity_transition(
            "UIREFRESH activity=Applets op=displayBuffer scope=full mode=half:entries=0",
        );
        assert_eq!(ctx.current_activity.as_deref(), Some("Applets"));

        ctx.parse_activity_transition(
            "UIREFRESH activity=LuaApp op=displayBuffer scope=full mode=half:entries=0",
        );
        assert_eq!(ctx.current_activity.as_deref(), Some("LuaApp"));
    }

    #[tokio::test]
    async fn log_contains_matching_is_case_sensitive() {
        let ctx = std::sync::Arc::new(tokio::sync::Mutex::new(E2EContext::new()));
        {
            let mut ctx_lock = ctx.lock().await;
            ctx_lock.push_line("E2E:FILE_BROWSER:path=/Books:entries=2");
        }

        assert!(wait_for_log_contains(
            &ctx,
            "E2E:FILE_BROWSER:path=/Books:entries=",
            Duration::from_millis(10),
            None
        )
        .await
        .unwrap());
        assert!(wait_for_log_contains(
            &ctx,
            "e2e:file_browser:path=/books:entries=",
            Duration::from_millis(10),
            None
        )
        .await
        .is_err());
    }

    #[tokio::test]
    async fn log_contains_honors_input_boundary() {
        let ctx = std::sync::Arc::new(tokio::sync::Mutex::new(E2EContext::new()));
        let after_line_count = {
            let mut ctx_lock = ctx.lock().await;
            ctx_lock.push_line("E2E:FILE_BROWSER:path=/:entries=2");
            ctx_lock.mark_input_activity_boundary();
            ctx_lock.take_pending_input_log_boundary().unwrap()
        };

        assert!(wait_for_log_contains(
            &ctx,
            "E2E:FILE_BROWSER:path=/:entries=",
            Duration::from_millis(10),
            Some(after_line_count)
        )
        .await
        .is_err());

        {
            let mut ctx_lock = ctx.lock().await;
            ctx_lock.push_line("E2E:FILE_BROWSER:path=/:entries=2");
        }

        assert!(wait_for_log_contains(
            &ctx,
            "E2E:FILE_BROWSER:path=/:entries=",
            Duration::from_millis(10),
            Some(after_line_count)
        )
        .await
        .unwrap());
    }

    #[tokio::test]
    async fn activity_assertion_accepts_stable_current_after_input_boundary() {
        let ctx = std::sync::Arc::new(tokio::sync::Mutex::new(E2EContext::new()));
        let after_event_count = {
            let mut ctx_lock = ctx.lock().await;
            ctx_lock.parse_activity_transition("[MOFEI-TRACE] FileBrowserActivity::render");
            ctx_lock.mark_input_activity_boundary();
            ctx_lock.take_pending_input_activity_boundary().unwrap()
        };

        assert!(wait_for_activity(
            &ctx,
            "filebrowser",
            Duration::from_millis(ASSERT_ACTIVITY_STABLE_MS + 100),
            Some(after_event_count)
        )
        .await
        .unwrap());
    }

    #[test]
    fn sanitizes_case_artifact_names() {
        assert_eq!(
            safe_artifact_name("Dashboard / Weather Smoke"),
            "Dashboard---Weather-Smoke"
        );
        assert_eq!(safe_artifact_name("!!!"), "case");
    }

    #[test]
    fn loads_e2e_case_dir_recursively() {
        let tmp_dir = temp_test_dir("recursive-e2e-case-dir");
        let arcade_dir = tmp_dir.join("arcade").join("games");
        let settings_dir = tmp_dir.join("settings").join("root");
        fs::create_dir_all(&arcade_dir).unwrap();
        fs::create_dir_all(&settings_dir).unwrap();

        let write_case = |path: &Path, id: &str| {
            let raw = serde_json::to_string_pretty(&serde_json::json!({
                "id": id,
                "description": "recursive fixture",
                "steps": [
                    {
                        "type": "wait",
                        "delayMs": 1
                    }
                ]
            }))
            .unwrap();
            fs::write(path, raw).unwrap();
        };
        write_case(
            &arcade_dir.join("dashboard_arcade_memory_smoke.json"),
            "dashboard-arcade-memory-smoke",
        );
        write_case(
            &settings_dir.join("dashboard_settings_smoke.json"),
            "dashboard-settings-smoke",
        );
        fs::write(tmp_dir.join("README.txt"), "ignored").unwrap();
        fs::write(tmp_dir.join("arcade").join("notes.md"), "ignored").unwrap();

        let cases = load_e2e_case_dir(&tmp_dir).unwrap();
        let relative_paths: Vec<String> = cases
            .iter()
            .map(|(path, _)| {
                path.strip_prefix(&tmp_dir)
                    .unwrap()
                    .to_string_lossy()
                    .replace('\\', "/")
            })
            .collect();
        let case_ids: Vec<&str> = cases.iter().map(|(_, case)| case.id.as_str()).collect();

        assert_eq!(
            relative_paths,
            vec![
                "arcade/games/dashboard_arcade_memory_smoke.json",
                "settings/root/dashboard_settings_smoke.json"
            ]
        );
        assert_eq!(
            case_ids,
            vec!["dashboard-arcade-memory-smoke", "dashboard-settings-smoke"]
        );
    }

    #[test]
    fn derives_batch_socket_paths_without_losing_extension() {
        assert_eq!(
            path_with_suffix(Path::new("/tmp/mofei.sock"), "dashboard-settings").to_string_lossy(),
            "/tmp/mofei-dashboard-settings.sock"
        );
        assert_eq!(
            path_with_suffix(Path::new("/tmp/mofei"), "dashboard-settings").to_string_lossy(),
            "/tmp/mofei-dashboard-settings"
        );
    }

    #[test]
    fn shortens_long_batch_socket_paths_for_monitor_socket() {
        let socket = path_with_suffix(
            Path::new("/tmp/panda-sim-e2e-case-dir-0603-organized.sock"),
            "dashboard-file-browser-epub-bookmarks-smoke",
        );
        let monitor_socket = socket.with_extension("monitor.sock");
        let socket_file_name = socket.file_name().unwrap().to_string_lossy();

        assert!(socket_path_fits_unix_limit(&socket));
        assert!(socket_path_fits_unix_limit(&monitor_socket));
        assert!(socket_file_name.ends_with(".sock"));
        assert!(socket_file_name.contains("dashboard-file-browser-epub"));
    }

    #[test]
    fn normalizes_batch_artifacts_without_re_resolving_relative_qemu() {
        let mut args = Args {
            board: None,
            firmware: PathBuf::from("/tmp/firmware.bin"),
            qemu: PathBuf::from("/tmp/qemu-system-xtensa"),
            heap_mode: None,
            debug_port: None,
            socket: PathBuf::from("/tmp/mofei-sim.sock"),
            duration: 10,
            out: None,
            sd_root: Some(PathBuf::from("/tmp/sd-root")),
            serial: false,
            inject_touch_tap: false,
            tap_x: 240,
            tap_y: 400,
            extra_touch_tap: Vec::new(),
            inject_buttons: Vec::new(),
            button_hold_ms: 250,
            post_tap_delay_ms: 250,
            post_tap_frames_before_buttons: 1,
            startup_frames_before_input: 1,
            case: None,
            case_dir: None,
            artifacts: PathBuf::from("/tmp/artifacts"),
            capture_assertion_screenshots: false,
            step_timeout: DEFAULT_STEP_TIMEOUT_S,
            run_skipped: false,
            radio_broker_bind: None,
            radio_broker_peer: None,
        };
        let artifact_name = "dashboard-settings";
        args.artifacts = args.artifacts.join(artifact_name);
        args.socket = path_with_suffix(&args.socket, artifact_name);
        args.sd_root = args.sd_root.map(|sd_root| sd_root.join(artifact_name));

        assert_eq!(args.qemu.to_string_lossy(), "/tmp/qemu-system-xtensa");
        assert_eq!(args.firmware.to_string_lossy(), "/tmp/firmware.bin");
        assert_eq!(
            args.artifacts.to_string_lossy(),
            "/tmp/artifacts/dashboard-settings"
        );
        assert_eq!(
            args.socket.to_string_lossy(),
            "/tmp/mofei-sim-dashboard-settings.sock"
        );
        assert_eq!(
            args.sd_root.unwrap().to_string_lossy(),
            "/tmp/sd-root/dashboard-settings"
        );
    }

    #[test]
    fn directory_sd_drive_uses_vvfat_directory_backend() {
        let drive = sd_directory_drive(Path::new("/tmp/sd-root"));

        assert_eq!(drive.qemu_arg, "file=fat:rw:/tmp/sd-root,if=sd");
    }

    #[test]
    fn e2e_sd_image_drive_uses_raw_image_backend() {
        let image_path = Path::new("/tmp/artifacts").join("sdcard.img");
        let drive = SdDrive {
            qemu_arg: format!("file={},if=sd,format=raw", image_path.display()),
            image_path: Some(image_path.clone()),
        };

        assert_eq!(
            drive.qemu_arg,
            "file=/tmp/artifacts/sdcard.img,if=sd,format=raw"
        );
        assert!(
            !drive.qemu_arg.contains("file=fat:rw:"),
            "E2E SD image backend must not use QEMU vvfat directory writes"
        );
        assert_eq!(drive.image_path.as_deref(), Some(image_path.as_path()));
    }

    #[test]
    fn batch_sd_artifact_cleanup_preserves_case_result() {
        let tmp_dir = temp_test_dir("batch-sd-artifact-cleanup");
        let sd_root = tmp_dir.join(".sd/case");
        let artifacts_dir = tmp_dir.join("case");
        fs::create_dir_all(&sd_root).unwrap();
        fs::create_dir_all(&artifacts_dir).unwrap();
        fs::write(sd_root.join("settings.json"), b"{}").unwrap();
        fs::write(artifacts_dir.join("sdcard.img"), b"raw image").unwrap();
        fs::write(artifacts_dir.join("result.json"), b"{\"status\":\"PASS\"}").unwrap();

        cleanup_e2e_batch_sd_artifacts(&sd_root, &artifacts_dir).unwrap();

        assert!(!sd_root.exists());
        assert!(!artifacts_dir.join("sdcard.img").exists());
        assert!(artifacts_dir.join("result.json").is_file());
        fs::remove_dir_all(tmp_dir).unwrap();
    }

    #[test]
    fn e2e_case_defaults_sd_root_under_artifacts() {
        let args = Args {
            board: None,
            firmware: PathBuf::from("/tmp/firmware.bin"),
            qemu: PathBuf::from("/tmp/qemu-system-xtensa"),
            heap_mode: None,
            debug_port: None,
            socket: PathBuf::from("/tmp/mofei-sim.sock"),
            duration: 10,
            out: None,
            sd_root: None,
            serial: false,
            inject_touch_tap: false,
            tap_x: 240,
            tap_y: 400,
            extra_touch_tap: Vec::new(),
            inject_buttons: Vec::new(),
            button_hold_ms: 250,
            post_tap_delay_ms: 250,
            post_tap_frames_before_buttons: 1,
            startup_frames_before_input: 1,
            case: None,
            case_dir: None,
            artifacts: PathBuf::from("/tmp/artifacts"),
            capture_assertion_screenshots: false,
            step_timeout: DEFAULT_STEP_TIMEOUT_S,
            run_skipped: false,
            radio_broker_bind: None,
            radio_broker_peer: None,
        };

        let sd_root = e2e_case_sd_root(&args, Path::new("/tmp/artifacts/case")).unwrap();

        assert_eq!(sd_root.to_string_lossy(), "/tmp/artifacts/case/.sd");
    }

    #[test]
    fn default_font_fixture_includes_active_mofei_source_complete_pack() {
        assert!(PANDA_DEFAULT_FONT_PACKS.contains(&"notosans_tc_26_source_complete.mfp"));
        assert!(!PANDA_DEFAULT_FONT_PACKS.contains(&"notosans_tc_32_source_complete.mfp"));
    }

    #[test]
    fn default_font_fixture_uses_only_the_canonical_hidden_font_directory() {
        let tmp_dir = temp_test_dir("default-font-fixture-layout");
        provision_murphy_default_font_packs(&tmp_dir).unwrap();

        for font_name in PANDA_DEFAULT_FONT_PACKS {
            assert!(tmp_dir.join(".murphy/fonts").join(font_name).is_file());
            assert!(!tmp_dir.join("murphy/fonts").join(font_name).exists());
        }
    }

    #[test]
    fn mfp_fallback_fixture_seeds_only_its_legacy_source_complete_font() {
        let tmp_dir = temp_test_dir("mfp-fallback-font-fixture-layout");
        provision_murphy_default_font_packs(&tmp_dir).unwrap();
        provision_mfp_fallback_case_font_packs(&tmp_dir).unwrap();

        for font_name in PANDA_DEFAULT_FONT_PACKS {
            assert!(tmp_dir.join(".murphy/fonts").join(font_name).is_file());
        }
        assert!(tmp_dir
            .join("murphy/fonts/notosans_tc_26_source_complete.mfp")
            .is_file());
        assert_eq!(
            fs::read_dir(tmp_dir.join("murphy/fonts")).unwrap().count(),
            PANDA_MFP_FALLBACK_CASE_PACKS.len()
        );
    }

    #[test]
    fn e2e_sd_image_contains_seeded_fixture_tree() {
        let tmp_dir = temp_test_dir("e2e-sd-image-fixtures");
        let sd_root = tmp_dir.join("sd-root");
        let image_path = tmp_dir.join("sdcard.img");
        fs::create_dir_all(sd_root.join(".mofei/fonts")).unwrap();
        fs::create_dir_all(sd_root.join(".mofei/apps/ai.pandacat.app.hello")).unwrap();
        fs::create_dir_all(sd_root.join("mofei/apps/ai.pandacat.app.hello")).unwrap();
        fs::create_dir_all(sd_root.join("Books")).unwrap();
        fs::write(sd_root.join(".mofei/settings.json"), b"{}\n").unwrap();
        fs::write(sd_root.join(".mofei/fonts/reader.ttf"), b"font").unwrap();
        fs::write(
            sd_root.join(".mofei/apps/installed.json"),
            br#"{"schemaVersion":1,"apps":[{"id":"ai.pandacat.app.hello"}]}"#,
        )
        .unwrap();
        fs::write(
            sd_root.join(".mofei/apps/ai.pandacat.app.hello/hello.signed.pap"),
            b"pap",
        )
        .unwrap();
        fs::write(
            sd_root.join("mofei/apps/installed.json"),
            br#"{"schemaVersion":1,"apps":[{"id":"ai.pandacat.app.hello"}]}"#,
        )
        .unwrap();
        fs::write(
            sd_root.join("mofei/apps/ai.pandacat.app.hello/hello.signed.pap"),
            b"pap",
        )
        .unwrap();
        fs::write(sd_root.join("Books/book.txt"), b"book").unwrap();

        sd_image::create_sd_image(&sd_root, &image_path, 64 * 1024 * 1024).unwrap();

        let image = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(&image_path)
            .unwrap();
        let fat = fatfs::FileSystem::new(image, fatfs::FsOptions::new()).unwrap();
        let root = fat.root_dir();
        assert!(root.open_file(".mofei/settings.json").is_ok());
        assert!(root.open_file(".mofei/fonts/reader.ttf").is_ok());
        assert!(root.open_file(".mofei/apps/installed.json").is_ok());
        assert!(root
            .open_file(".mofei/apps/ai.pandacat.app.hello/hello.signed.pap")
            .is_ok());
        assert!(root.open_file("mofei/apps/installed.json").is_ok());
        assert!(root
            .open_file("mofei/apps/ai.pandacat.app.hello/hello.signed.pap")
            .is_ok());
        assert!(root.open_file("Books/book.txt").is_ok());
    }

    #[test]
    fn e2e_sd_image_syncs_written_state_back_to_sd_root() {
        let tmp_dir = temp_test_dir("e2e-sd-image-sync-back");
        let sd_root = tmp_dir.join("sd-root");
        let image_path = tmp_dir.join("sdcard.img");
        fs::create_dir_all(sd_root.join(".mofei")).unwrap();
        fs::write(
            sd_root.join(".mofei/settings.json"),
            b"{\"fileBrowserLayoutMode\":0}\n",
        )
        .unwrap();
        sd_image::create_sd_image(&sd_root, &image_path, 64 * 1024 * 1024).unwrap();

        {
            let image = std::fs::OpenOptions::new()
                .read(true)
                .write(true)
                .open(&image_path)
                .unwrap();
            let fat = fatfs::FileSystem::new(image, fatfs::FsOptions::new()).unwrap();
            let root = fat.root_dir();
            let mut settings = root.open_file(".mofei/settings.json").unwrap();
            settings.truncate().unwrap();
            settings
                .write_all(b"{\"fileBrowserLayoutMode\":1}\n")
                .unwrap();
            root.create_dir("Books").unwrap();
            let mut marker = root.create_file("Books/persisted.txt").unwrap();
            marker.write_all(b"persisted").unwrap();
        }

        sd_image::sync_sd_image_to_dir(&image_path, &sd_root).unwrap();

        assert_eq!(
            fs::read_to_string(sd_root.join(".mofei/settings.json")).unwrap(),
            "{\"fileBrowserLayoutMode\":1}\n"
        );
        assert_eq!(
            fs::read_to_string(sd_root.join("Books/persisted.txt")).unwrap(),
            "persisted"
        );

        fs::remove_dir_all(tmp_dir).unwrap();
    }
}
