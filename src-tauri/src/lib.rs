// Panda Simulator — Tauri backend.
//
// Spawns the Espressif QEMU fork as a child process and bridges its custom
// peripherals to the Tauri webview through a Unix-domain-socket chardev
// (single socket, length-prefixed binary multiplex — see ipc.rs).
//
// Outbound (QEMU → UI): serial log (PR1), framebuffer (PR2).
// Inbound (UI → QEMU): touch events, button events (PR3).
//
// QEMU integration of the board-specific e-paper + touch peripherals into the
// Espressif fork's machine model is partial — see
// apps/simulator/qemu-peripherals/INTEGRATION.md. Today the IPC plumbing is
// fully exercised end-to-end on the host side; the firmware-facing side
// requires the SoC machine patch to land.

mod debug_transport;
mod ipc;
mod simulator_resolver;

use serde::Deserialize;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::sync::{Arc, Mutex, MutexGuard};
use tauri::{AppHandle, State};

const FLASH_SIZE_BYTES: usize = 16 * 1024 * 1024;
const BOOTLOADER_OFFSET: usize = 0x0000;
const PARTITION_TABLE_OFFSET: usize = 0x8000;
const PARTITION_ENTRY_SIZE: usize = 32;
const PARTITION_MAGIC: [u8; 2] = [0xAA, 0x50];
const PARTITION_END_MAGIC: [u8; 2] = [0xEB, 0xEB];
const PERIPHERAL_CONTROL_MIN_PAYLOAD: usize = 10;
const PERIPHERAL_CONTROL_MAX_PAYLOAD: usize = 64;

struct SimState {
    process: Mutex<Option<Child>>,
    writer: Arc<ipc::IpcWriter>,
    debug_broker: debug_transport::DebugTransportBroker,
    display_geometry: Arc<Mutex<ipc::BoardDisplayGeometry>>,
    release_button_ids: Arc<Mutex<Vec<u8>>>,
    debug_server: Mutex<Option<debug_transport::DebugServerHandle>>,
}

impl Drop for SimState {
    fn drop(&mut self) {
        if let Ok(mut debug_server) = self.debug_server.lock() {
            debug_server.take();
        }
        if let Ok(mut process_guard) = self.process.lock() {
            stop_owned_sim_process(&mut process_guard, &self.writer);
        } else {
            self.writer.unbind();
            cleanup_stale_socket(&ipc_socket_path());
        }
    }
}

struct StartResult {
    status: String,
    sd_root: PathBuf,
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
    framebuffer_width: u32,
    framebuffer_height: u32,
    framebuffer_format: ipc::FramebufferFormat,
    key_map: Vec<BoardKey>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct IntegrationRegistry {
    boards: Vec<IntegrationBoard>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct IntegrationBoard {
    id: String,
    murphy_board: String,
    firmware: String,
}

#[derive(Clone, Debug, Deserialize)]
struct BoardKey {
    id: u8,
}

#[derive(Clone, Debug, Deserialize)]
struct HostLocation {
    name: String,
    timezone: String,
    latitude: f64,
    longitude: f64,
}

struct SimulatorLocation {
    name: String,
    timezone: String,
    latitude: f64,
    longitude: f64,
}

impl Default for SimulatorLocation {
    fn default() -> Self {
        Self {
            name: "London".to_string(),
            timezone: "Europe%2FLondon".to_string(),
            latitude: 51.5074,
            longitude: -0.1278,
        }
    }
}

fn standalone_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap_or_else(|| Path::new("."))
        .to_path_buf()
}

fn board_registry() -> Result<BoardRegistry, String> {
    serde_json::from_str(include_str!("../../boards.json")).map_err(|e| e.to_string())
}

fn simulator_board_profile(board_id: Option<&str>) -> Result<BoardProfile, String> {
    let registry = board_registry()?;
    let selected_id = board_id
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .unwrap_or(&registry.default_board);
    let supported = registry
        .boards
        .iter()
        .map(|board| board.id.as_str())
        .collect::<Vec<_>>()
        .join(", ");
    registry
        .boards
        .into_iter()
        .find(|board| board.id == selected_id)
        .ok_or_else(|| {
            format!("unsupported simulator board '{selected_id}'; supported boards: {supported}")
        })
}

fn qemu_binary_path() -> Result<PathBuf, String> {
    simulator_resolver::qemu_path(None, &standalone_root())
}

fn integration_board(profile: &BoardProfile) -> Result<IntegrationBoard, String> {
    let integration_path = simulator_resolver::integration_path()?.ok_or_else(|| {
        format!(
            "no quicklaunch firmware mapping for board '{}'; choose an explicit firmware file or set PANDA_SIMULATOR_INTEGRATION",
            profile.id
        )
    })?;
    let registry: IntegrationRegistry = serde_json::from_str(
        &std::fs::read_to_string(&integration_path).map_err(|error| {
            format!(
                "failed to read PANDA_SIMULATOR_INTEGRATION {}: {}",
                integration_path.display(), error
            )
        })?,
    )
    .map_err(|error| {
        format!(
            "failed to parse PANDA_SIMULATOR_INTEGRATION {}: {}",
            integration_path.display(), error
        )
    })?;
    registry
        .boards
        .into_iter()
        .find(|board| board.id == profile.id)
        .ok_or_else(|| {
            format!(
                "PANDA_SIMULATOR_INTEGRATION {} has no firmware mapping for board '{}'",
                integration_path.display(), profile.id
            )
        })
}

fn board_firmware_path(profile: &BoardProfile) -> Result<PathBuf, String> {
    if let Some(explicit) = simulator_resolver::explicit_path_env("PANDA_SIMULATOR_FIRMWARE")? {
        return Ok(explicit);
    }
    let mapping = integration_board(profile)?;
    let project_root = simulator_resolver::project_root()?;
    simulator_resolver::project_path(&mapping.firmware, project_root.as_deref(), "firmware")
}

fn murphy_build_hint(profile: &BoardProfile) -> String {
    let Ok(mapping) = integration_board(profile) else {
        return "choose an explicit firmware file, or set PANDA_SIMULATOR_INTEGRATION and PANDA_SIMULATOR_PROJECT_ROOT".to_string();
    };
    let board_env = if mapping.murphy_board == "default" {
        "BOARD=default".to_string()
    } else {
        format!("BOARD={}", mapping.murphy_board)
    };
    let build_dir = if mapping.murphy_board == "default" {
        "apps/panda-os/device/build".to_string()
    } else {
        format!("apps/panda-os/device/build-{}", mapping.murphy_board)
    };
    format!("{board_env} PANDA_BUILD_DIR={build_dir} apps/panda-os/tools/build-device.sh")
}

fn firmware_build_dir(firmware_bin: &Path) -> &Path {
    firmware_bin.parent().unwrap_or(Path::new("."))
}

fn firmware_sibling_or_nested(build_dir: &Path, sibling: &str, nested: &str) -> PathBuf {
    let direct = build_dir.join(sibling);
    if direct.exists() {
        return direct;
    }
    build_dir.join(nested)
}

fn default_sd_root() -> PathBuf {
    standalone_root().join("sdcard")
}

fn simulator_sd_root() -> Result<PathBuf, String> {
    let root = std::env::var_os("MOFEI_SIM_SD_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(default_sd_root);
    std::fs::create_dir_all(&root).map_err(|e| {
        format!(
            "failed to create simulator SD root {}: {}",
            root.display(),
            e
        )
    })?;
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
            .unwrap_or_else(|| "Simulated Location".to_string()),
        timezone: normalize_timezone_for_firmware(
            &std::env::var("MOFEI_SIM_LOCATION_TIMEZONE")
                .ok()
                .filter(|value| !value.trim().is_empty())
                .unwrap_or_else(|| "Europe%2FLondon".to_string()),
        ),
        latitude: lat,
        longitude: lon,
    };
    is_valid_location(&location).then_some(location)
}

fn host_location_from_frontend(host_location: Option<HostLocation>) -> Option<SimulatorLocation> {
    let host = host_location?;
    let location = SimulatorLocation {
        name: if host.name.trim().is_empty() {
            "Simulated Location".to_string()
        } else {
            host.name
        },
        timezone: normalize_timezone_for_firmware(&host.timezone),
        latitude: host.latitude,
        longitude: host.longitude,
    };
    is_valid_location(&location).then_some(location)
}

fn write_simulator_location(
    sd_root: &Path,
    host_location: Option<HostLocation>,
) -> Result<SimulatorLocation, String> {
    let location = host_location_from_frontend(host_location)
        .or_else(env_simulator_location)
        .unwrap_or_default();
    let system_dir = sd_root.join(".mofei");
    std::fs::create_dir_all(&system_dir).map_err(|e| {
        format!(
            "failed to create simulator system directory {}: {}",
            system_dir.display(),
            e
        )
    })?;
    let payload = serde_json::json!({
        "name": location.name,
        "timezone": location.timezone,
        "latitude": location.latitude,
        "longitude": location.longitude,
    });
    let path = system_dir.join("simulator_location.json");
    std::fs::write(&path, format!("{}\n", payload)).map_err(|e| {
        format!(
            "failed to write simulator location {}: {}",
            path.display(),
            e
        )
    })?;
    Ok(location)
}

fn copy_file_if_newer(source: &Path, dest: &Path) -> Result<(), String> {
    let should_copy = match (std::fs::metadata(source), std::fs::metadata(dest)) {
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
        std::fs::create_dir_all(parent).map_err(|e| {
            format!(
                "failed to create simulator visible system parent {}: {}",
                parent.display(),
                e
            )
        })?;
    }
    std::fs::copy(source, dest).map_err(|e| {
        format!(
            "failed to copy newer simulator system file {} -> {}: {}",
            source.display(),
            dest.display(),
            e
        )
    })?;
    Ok(())
}

fn sync_newer_dir_recursive(from: &Path, to: &Path) -> Result<(), String> {
    std::fs::create_dir_all(to).map_err(|e| {
        format!(
            "failed to create simulator visible system dir {}: {}",
            to.display(),
            e
        )
    })?;
    if !from.exists() {
        return Ok(());
    }
    for entry in std::fs::read_dir(from).map_err(|e| {
        format!(
            "failed to read simulator system dir {}: {}",
            from.display(),
            e
        )
    })? {
        let entry = entry.map_err(|e| e.to_string())?;
        let source = entry.path();
        let dest = to.join(entry.file_name());
        let file_type = entry.file_type().map_err(|e| e.to_string())?;
        if file_type.is_dir() {
            sync_newer_dir_recursive(&source, &dest)?;
        } else if file_type.is_file() {
            copy_file_if_newer(&source, &dest)?;
        }
    }
    Ok(())
}

fn sync_visible_system_dir(sd_root: &Path) -> Result<(), String> {
    let hidden = sd_root.join(".mofei");
    let visible = sd_root.join("mofei");
    std::fs::create_dir_all(&hidden).map_err(|e| {
        format!(
            "failed to create simulator hidden system dir {}: {}",
            hidden.display(),
            e
        )
    })?;
    std::fs::create_dir_all(&visible).map_err(|e| {
        format!(
            "failed to create simulator visible system dir {}: {}",
            visible.display(),
            e
        )
    })?;
    sync_newer_dir_recursive(&visible, &hidden)?;
    sync_newer_dir_recursive(&hidden, &visible)
}

#[derive(Clone, Copy)]
struct FlashLayout {
    boot_app0_offset: usize,
    app_offset: usize,
}

fn partition_label(entry: &[u8]) -> String {
    let label = &entry[12..28];
    let end = label.iter().position(|b| *b == 0).unwrap_or(label.len());
    String::from_utf8_lossy(&label[..end]).to_string()
}

fn partition_offset(entry: &[u8]) -> usize {
    u32::from_le_bytes([entry[4], entry[5], entry[6], entry[7]]) as usize
}

fn parse_flash_layout(partition_table: &[u8]) -> Result<FlashLayout, String> {
    let mut boot_app0_offset = None;
    let mut app_offset = None;
    let mut first_app_offset = None;

    for entry in partition_table.chunks_exact(PARTITION_ENTRY_SIZE) {
        let magic = [entry[0], entry[1]];
        if magic == PARTITION_END_MAGIC {
            break;
        }
        if magic != PARTITION_MAGIC {
            return Err(format!(
                "invalid partition table magic {:02x}{:02x}",
                entry[0], entry[1]
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

    let boot_app0_offset = boot_app0_offset
        .ok_or_else(|| "partition table has no otadata entry for boot_app0.bin".to_string())?;
    let app_offset = app_offset
        .or(first_app_offset)
        .ok_or_else(|| "partition table has no app entry for firmware.bin".to_string())?;

    Ok(FlashLayout {
        boot_app0_offset,
        app_offset,
    })
}

/// Patch adc_hal_self_calibration in the firmware binary so it returns
/// immediately (movi a2, 0; retw.n). The ADC calibration hangs in QEMU
/// because the internal REGI2C bus is not emulated.
fn patch_adc_calibration(fw: &mut [u8]) {
    if fw.len() < 24 || fw[0] != 0xE9 {
        println!("[tauri] patch_adc: not an ESP image");
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
        println!(
            "[tauri] seg {seg_idx}: load=0x{load_addr:08x} size=0x{seg_size:x} data_at=0x{:x}",
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
                    println!("[tauri] Patched adc_hal_self_calibration at seg offset 0x{i:x}");
                    return;
                }
            }
            println!("[tauri] adc calibration needle not found in .flash.text");
        }
        pos += seg_size;
    }
}

/// Patch the bootloader's software-reset infinite loop so the function
/// returns instead of spinning forever. On real hardware the bootloader
/// triggers SW_PROCPU_RESET via RTC_CNTL and the chip resets immediately.
/// In QEMU the reset may not fully restart the bootloader, leaving the CPU
/// stuck in a `j self` loop. Replacing the loop with `retw.n` lets the
/// function return and execution continues past the reset attempt.
fn patch_bootloader_reset(bl: &mut [u8]) {
    if bl.len() < 24 || bl[0] != 0xE9 {
        println!("[tauri] patch_bootloader_reset: not an ESP image");
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
                    println!(
                        "[tauri] Patched bootloader reset loop at seg2 offset 0x{:x} → retw.n",
                        RESET_LOOP_SEG_OFFSET
                    );
                } else {
                    println!(
                        "[tauri] Bootloader reset loop bytes don't match: {:02x} {:02x} {:02x}",
                        bl[patch_offset],
                        bl[patch_offset + 1],
                        bl[patch_offset + 2]
                    );
                }
            } else {
                println!(
                    "[tauri] Bootloader seg2 too small for reset loop patch (size=0x{:x})",
                    seg_size
                );
            }
        }
        pos += seg_size;
    }
}

/// Patch the ESP32-S3 ROM binary to allow booting with incomplete SPI flash
/// emulation. See headless/src/main.rs for detailed per-patch comments.
fn patch_rom_spi_panic(rom: &mut [u8]) {
    // Patch 1: NOP the error jump after checksum failure
    const ROM_CHECKSUM_ERROR_JUMP: usize = 0x45b27;
    if ROM_CHECKSUM_ERROR_JUMP + 3 <= rom.len() {
        if rom[ROM_CHECKSUM_ERROR_JUMP] == 0x46
            && rom[ROM_CHECKSUM_ERROR_JUMP + 1] == 0x59
            && rom[ROM_CHECKSUM_ERROR_JUMP + 2] == 0xff
        {
            rom[ROM_CHECKSUM_ERROR_JUMP] = 0x00;
            rom[ROM_CHECKSUM_ERROR_JUMP + 1] = 0x00;
            rom[ROM_CHECKSUM_ERROR_JUMP + 2] = 0x20;
            println!(
                "[tauri] Patched ROM checksum error jump at offset 0x{:x} → NOP",
                ROM_CHECKSUM_ERROR_JUMP
            );
        } else {
            println!(
                "[tauri] ROM checksum error jump bytes don't match: {:02x} {:02x} {:02x}",
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
            println!(
                "[tauri] Patched ROM SPI panic loop at offset 0x{:x} → NOP",
                ROM_SPI_PANIC_OFFSET
            );
        }
    }

    // Patch 3: NOP out SHA_BUSY polling loop at 0x45109.
    const ROM_SHA_BUSY_POLL: usize = 0x45109;
    if ROM_SHA_BUSY_POLL + 3 <= rom.len() {
        if rom[ROM_SHA_BUSY_POLL] == 0x26
            && rom[ROM_SHA_BUSY_POLL + 1] == 0x18
            && rom[ROM_SHA_BUSY_POLL + 2] == 0xf7
        {
            rom[ROM_SHA_BUSY_POLL] = 0x00;
            rom[ROM_SHA_BUSY_POLL + 1] = 0x20;
            rom[ROM_SHA_BUSY_POLL + 2] = 0x20;
            println!(
                "[tauri] Patched ROM SHA BUSY poll at offset 0x{:x} → NOP",
                ROM_SHA_BUSY_POLL
            );
        }
    }

    // Patch 4: Keep ROM strcmp's ENTRY and replace the body with immediate
    // "not equal" return. ENTRY must run so Xtensa call-window state rotates
    // normally before retw.n returns to the caller.
    const ROM_STRCMP: usize = 0x55448;
    if ROM_STRCMP + 7 <= rom.len() {
        if rom[ROM_STRCMP] == 0x36 && rom[ROM_STRCMP + 1] == 0x21 && rom[ROM_STRCMP + 2] == 0x00 {
            rom[ROM_STRCMP + 3] = 0x0c; // movi.n a2, 1
            rom[ROM_STRCMP + 4] = 0x12;
            rom[ROM_STRCMP + 5] = 0x1d; // retw.n
            rom[ROM_STRCMP + 6] = 0xf0;
            println!(
                "[tauri] Patched ROM strcmp at offset 0x{:x}+3 → return 1 (not equal)",
                ROM_STRCMP
            );
        }
    }

    // Patch 5: Keep ROM strlen's ENTRY and replace the body with immediate
    // "length 0" return. Do not patch at +0; skipping ENTRY corrupts the
    // call-window state that retw.n expects.
    const ROM_STRLEN: usize = 0x55698;
    if ROM_STRLEN + 7 <= rom.len() {
        if rom[ROM_STRLEN] == 0x36 && rom[ROM_STRLEN + 1] == 0x21 && rom[ROM_STRLEN + 2] == 0x00 {
            rom[ROM_STRLEN + 3] = 0x0c; // movi.n a2, 0
            rom[ROM_STRLEN + 4] = 0x02;
            rom[ROM_STRLEN + 5] = 0x1d; // retw.n
            rom[ROM_STRLEN + 6] = 0xf0;
            println!(
                "[tauri] Patched ROM strlen at offset 0x{:x}+3 → return 0",
                ROM_STRLEN
            );
        }
    }
}

fn copy_flash_segment(
    img: &mut [u8],
    occupied: &mut Vec<(usize, usize, &'static str)>,
    offset: usize,
    data: &[u8],
    name: &'static str,
) -> Result<(), String> {
    let end = offset
        .checked_add(data.len())
        .ok_or_else(|| format!("{name} offset overflows flash image"))?;
    if end > img.len() {
        return Err(format!("{name} too large for flash offset 0x{offset:x}"));
    }
    for &(existing_offset, existing_end, existing_name) in occupied.iter() {
        if offset < existing_end && end > existing_offset {
            return Err(format!(
                "{name} at 0x{offset:x}..0x{end:x} overlaps {existing_name} at 0x{existing_offset:x}..0x{existing_end:x}"
            ));
        }
    }
    img[offset..end].copy_from_slice(data);
    occupied.push((offset, end, name));
    Ok(())
}

fn build_flash_image(firmware_bin: &std::path::Path) -> Result<PathBuf, String> {
    let build_dir = firmware_build_dir(firmware_bin);
    let bootloader =
        firmware_sibling_or_nested(build_dir, "bootloader.bin", "bootloader/bootloader.bin");
    let boot_app0 = firmware_sibling_or_nested(build_dir, "boot_app0.bin", "ota_data_initial.bin");
    let partitions = firmware_sibling_or_nested(
        build_dir,
        "partitions.bin",
        "partition_table/partition-table.bin",
    );
    let flash_img = build_dir.join("flash.img");

    if !bootloader.exists() || !boot_app0.exists() || !partitions.exists() || !firmware_bin.exists()
    {
        return Err(format!(
            "Missing required ESP-IDF image files next to {}. Expected bootloader/bootloader.bin, partition_table/partition-table.bin, ota_data_initial.bin, and panda_os.bin.",
            firmware_bin.display()
        ));
    }

    let firmware_bytes = std::fs::read(firmware_bin).map_err(|e| e.to_string())?;
    if firmware_bytes.len() < 4 || firmware_bytes[0] != 0xE9 {
        return Err(format!(
            "Firmware at {} is not an ESP image bin (missing 0xE9 header).",
            firmware_bin.display()
        ));
    }

    let mut img = vec![0xFFu8; FLASH_SIZE_BYTES];

    let mut bl_data = std::fs::read(&bootloader).map_err(|e| e.to_string())?;
    let boot_app0_data = std::fs::read(&boot_app0).map_err(|e| e.to_string())?;
    let pt_data = std::fs::read(&partitions).map_err(|e| e.to_string())?;
    let mut fw_data = firmware_bytes;
    let layout = parse_flash_layout(&pt_data)?;

    // Patch firmware and bootloader for QEMU compatibility.
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

    std::fs::write(&flash_img, img).map_err(|e| e.to_string())?;

    Ok(flash_img)
}

fn socket_dir_is_usable(dir: &Path) -> bool {
    if std::fs::create_dir_all(dir).is_err() {
        return false;
    }

    let probe = dir.join(format!(".mofei-sim-write-test-{}", std::process::id()));
    match std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&probe)
    {
        Ok(_) => {
            let _ = std::fs::remove_file(&probe);
            true
        }
        Err(_) => false,
    }
}

fn ipc_socket_path() -> PathBuf {
    let base = std::env::var_os("XDG_RUNTIME_DIR")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .filter(|path| socket_dir_is_usable(path))
        .unwrap_or_else(|| PathBuf::from("/tmp"));

    let _ = std::fs::create_dir_all(&base);
    base.join("mofei-sim.sock")
}

fn cleanup_stale_socket(path: &std::path::Path) {
    if path.exists() {
        let _ = std::fs::remove_file(path);
    }
}

fn stop_owned_sim_process(process: &mut Option<Child>, writer: &ipc::IpcWriter) -> bool {
    if let Some(mut child) = process.take() {
        let _ = child.kill();
        let _ = child.wait();
        cleanup_stale_socket(&ipc_socket_path());
        writer.unbind();
        true
    } else {
        writer.unbind();
        cleanup_stale_socket(&ipc_socket_path());
        false
    }
}

fn stop_sim_locked(
    process_guard: &mut MutexGuard<'_, Option<Child>>,
    writer: &ipc::IpcWriter,
) -> String {
    if stop_owned_sim_process(process_guard, writer) {
        "stopped".to_string()
    } else {
        "not_running".to_string()
    }
}

fn start_sim_locked(
    app: AppHandle,
    process_guard: &mut MutexGuard<'_, Option<Child>>,
    writer: Arc<ipc::IpcWriter>,
    debug_broker: debug_transport::DebugTransportBroker,
    display_geometry: Arc<Mutex<ipc::BoardDisplayGeometry>>,
    release_button_ids: Arc<Mutex<Vec<u8>>>,
    board_id: String,
    firmware_path: String,
    host_location: Option<HostLocation>,
) -> Result<StartResult, String> {
    let board = simulator_board_profile(Some(&board_id))?;
    if let Ok(mut geometry) = display_geometry.lock() {
        *geometry = ipc::BoardDisplayGeometry::new(
            board.framebuffer_width,
            board.framebuffer_height,
            board.framebuffer_format,
        );
    }
    if let Ok(mut button_ids) = release_button_ids.lock() {
        *button_ids = board.key_map.iter().map(|key| key.id).collect();
    }

    if process_guard.is_some() {
        let sd_root = simulator_sd_root()?;
        let _ = write_simulator_location(&sd_root, host_location)?;
        sync_visible_system_dir(&sd_root)?;
        return Ok(StartResult {
            status: "already_running".to_string(),
            sd_root,
        });
    }

    let qemu = qemu_binary_path()?;
    if !qemu.exists() {
        return Err(format!(
            "QEMU binary not found at {}.\n\
             Run `scripts/build-qemu.sh` first.\n\
             Required deps (macOS): brew install ninja glib pixman libgcrypt pkg-config gnutls",
            qemu.display()
        ));
    }

    let firmware = if firmware_path.trim().is_empty() {
        board_firmware_path(&board)?
    } else {
        PathBuf::from(firmware_path)
    };
    if !firmware.exists() {
        return Err(format!(
            "Firmware fixture for board '{}' not found at {}. Build Panda AI OS first: `{}`.",
            board.id,
            firmware.display(),
            murphy_build_hint(&board)
        ));
    }

    let flash_img = if firmware.extension().and_then(|s| s.to_str()) == Some("bin") {
        Some(build_flash_image(&firmware)?)
    } else {
        let bin_path = firmware.with_extension("bin");
        if bin_path.exists() {
            Some(build_flash_image(&bin_path)?)
        } else {
            None
        }
    };

    // Patch ROM binary to skip SPI flash validation panics in QEMU.
    // Write a patched copy and pass it via MOFEI_ROM_BINARY env var so
    // QEMU's flash boot path loads it instead of the default ROM.
    let rom_env = flash_img.as_ref().and_then(|flash| {
        let qemu_dir = qemu.parent().unwrap_or(std::path::Path::new("."));
        let rom_src = qemu_dir.join("../pc-bios/esp32s3_rev0_rom.bin");
        if rom_src.exists() {
            let mut rom_data = std::fs::read(&rom_src).ok()?;
            patch_rom_spi_panic(&mut rom_data);
            let patched_rom = flash
                .parent()
                .unwrap_or(std::path::Path::new("."))
                .join("esp32s3_rev0_rom.patched.bin");
            std::fs::write(&patched_rom, rom_data).ok()?;
            println!("[tauri] Wrote patched ROM to {}", patched_rom.display());
            Some(patched_rom)
        } else {
            println!(
                "[tauri] WARNING: ROM binary not found at {}, skipping ROM patch",
                rom_src.display()
            );
            None
        }
    });

    let socket = ipc_socket_path();
    cleanup_stale_socket(&socket);
    let sd_root = simulator_sd_root()?;
    let simulator_location = write_simulator_location(&sd_root, host_location)?;
    sync_visible_system_dir(&sd_root)?;

    let mut cmd = Command::new(&qemu);
    cmd.arg("-machine").arg("esp32s3");
    cmd.env("MOFEI_SIM_BOARD", &board_id);

    let ext = firmware.extension().and_then(|s| s.to_str());
    if let Some(flash) = &flash_img {
        cmd.arg("-drive")
            .arg(format!("file={},if=mtd,format=raw", flash.display()));
    }
    cmd.arg("-drive")
        .arg(format!("file=fat:rw:{},if=sd", sd_root.display()));

    // Set patched ROM binary for flash boot path.
    if let Some(rom) = &rom_env {
        cmd.env("MOFEI_ROM_BINARY", rom);
    }

    // Set firmware ELF path for QEMU dynamic symbol resolution (MOFEI_FIRMWARE_ELF).
    let elf_for_sym = if ext == Some("elf") {
        firmware.clone()
    } else {
        firmware.with_extension("elf")
    };
    if elf_for_sym.exists() {
        cmd.env("MOFEI_FIRMWARE_ELF", &elf_for_sym);
    }

    if ext == Some("elf") {
        cmd.arg("-kernel").arg(&firmware);
    } else if ext == Some("bin") {
        let elf_path = firmware.with_extension("elf");
        if elf_path.exists() {
            cmd.arg("-kernel").arg(&elf_path);
        }
    } else {
        return Err(format!(
            "Unsupported firmware format: {}.\n\
             Use Panda AI OS .bin with ESP-IDF bootloader, partition table, and ota_data_initial artifacts (preferred) or .elf.",
            firmware.display()
        ));
    }

    cmd.arg("-display")
        .arg("none")
        .arg("-serial")
        .arg("stdio")
        .arg("-semihosting-config")
        .arg("enable=on,target=native")
        .arg("-chardev")
        .arg(format!(
            "socket,id=mofei,path={},server=on,wait=off",
            socket.display()
        ));

    println!("[tauri] Simulator SD root: {}", sd_root.display());
    println!(
        "[tauri] Simulator location: {} {} {:.4},{:.4}",
        simulator_location.name,
        simulator_location.timezone,
        simulator_location.latitude,
        simulator_location.longitude
    );
    println!("Executing QEMU: {:?}", cmd);

    match cmd.spawn() {
        Ok(child) => {
            **process_guard = Some(child);
            ipc::spawn_io(app, writer, debug_broker, display_geometry, socket);
            Ok(StartResult {
                status: "started".to_string(),
                sd_root,
            })
        }
        Err(e) => Err(format!("failed to spawn qemu-system-xtensa: {}", e)),
    }
}

#[tauri::command]
fn start_sim(
    app: AppHandle,
    state: State<'_, SimState>,
    board_id: String,
    firmware_path: String,
    host_location: Option<HostLocation>,
) -> Result<String, String> {
    let mut process_guard = state.process.lock().map_err(|e| e.to_string())?;
    let result = start_sim_locked(
        app,
        &mut process_guard,
        state.writer.clone(),
        state.debug_broker.clone(),
        state.display_geometry.clone(),
        state.release_button_ids.clone(),
        board_id,
        firmware_path,
        host_location,
    )?;
    Ok(result.status)
}

#[tauri::command]
fn stop_sim(state: State<'_, SimState>) -> Result<String, String> {
    let mut process_guard = state.process.lock().map_err(|e| e.to_string())?;
    Ok(stop_sim_locked(&mut process_guard, &state.writer))
}

#[tauri::command]
fn full_reboot_sim(
    app: AppHandle,
    state: State<'_, SimState>,
    board_id: String,
    firmware_path: String,
    host_location: Option<HostLocation>,
) -> Result<String, String> {
    let mut process_guard = state.process.lock().map_err(|e| e.to_string())?;
    let _ = stop_sim_locked(&mut process_guard, &state.writer);
    std::thread::sleep(std::time::Duration::from_millis(150));
    let result = start_sim_locked(
        app,
        &mut process_guard,
        state.writer.clone(),
        state.debug_broker.clone(),
        state.display_geometry.clone(),
        state.release_button_ids.clone(),
        board_id,
        firmware_path,
        host_location,
    )?;
    Ok(format!(
        "{}; full_reboot; sd_root={}",
        result.status,
        result.sd_root.display()
    ))
}

#[tauri::command]
fn simulator_sd_root_path() -> Result<String, String> {
    simulator_sd_root().map(|path| path.display().to_string())
}

#[tauri::command]
fn release_buttons(state: State<'_, SimState>) -> Result<bool, String> {
    let mut sent = false;
    let button_ids = state
        .release_button_ids
        .lock()
        .map_err(|e| e.to_string())?
        .clone();
    for button_id in button_ids {
        let frame = ipc::encode_button_event(button_id, false);
        sent |= state.writer.try_send(frame);
    }
    Ok(sent)
}

#[tauri::command]
fn inject_touch(
    state: State<'_, SimState>,
    action: u8,
    x: u16,
    y: u16,
    finger_id: u8,
) -> Result<bool, String> {
    let frame = ipc::encode_touch_event(action, x, y, finger_id);
    Ok(state.writer.try_send(frame))
}

#[tauri::command]
fn inject_button(state: State<'_, SimState>, button_id: u8, pressed: bool) -> Result<bool, String> {
    let frame = ipc::encode_button_event(button_id, pressed);
    Ok(state.writer.try_send(frame))
}

#[tauri::command]
fn inject_peripheral_control(state: State<'_, SimState>, payload: Vec<u8>) -> Result<bool, String> {
    validate_peripheral_control_payload(&payload)?;
    Ok(state
        .writer
        .try_send(ipc::encode_peripheral_control(payload)))
}

fn validate_peripheral_control_payload(payload: &[u8]) -> Result<(), String> {
    if !(PERIPHERAL_CONTROL_MIN_PAYLOAD..=PERIPHERAL_CONTROL_MAX_PAYLOAD).contains(&payload.len()) {
        return Err(format!(
            "peripheral control payload length must be between {PERIPHERAL_CONTROL_MIN_PAYLOAD} and {PERIPHERAL_CONTROL_MAX_PAYLOAD} bytes"
        ));
    }
    Ok(())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let writer = Arc::new(ipc::IpcWriter::new());
    let debug_broker = debug_transport::DebugTransportBroker::new();
    let debug_server = match debug_transport::start_from_env(writer.clone(), debug_broker.clone()) {
        Ok(server) => server,
        Err(err) => {
            eprintln!("[tauri] Panda debug transport disabled: {err}");
            None
        }
    };
    if let Some(server) = debug_server.as_ref() {
        eprintln!(
            "[tauri] Panda debug transport enabled on 127.0.0.1:{}",
            server.port()
        );
    }
    let default_board = simulator_board_profile(None).expect("simulator boards.json default board");
    let release_button_ids = default_board.key_map.iter().map(|key| key.id).collect();
    tauri::Builder::default()
        .manage(SimState {
            process: Mutex::new(None),
            writer,
            debug_broker,
            display_geometry: Arc::new(Mutex::new(ipc::BoardDisplayGeometry::new(
                default_board.framebuffer_width,
                default_board.framebuffer_height,
                default_board.framebuffer_format,
            ))),
            release_button_ids: Arc::new(Mutex::new(release_button_ids)),
            debug_server: Mutex::new(debug_server),
        })
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![
            start_sim,
            stop_sim,
            full_reboot_sim,
            simulator_sd_root_path,
            release_buttons,
            inject_touch,
            inject_button,
            inject_peripheral_control
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

#[cfg(test)]
mod tests {
    use super::*;

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
        assert!(error.contains("overlaps first"));
    }

    #[test]
    fn loads_default_mofei_board_from_registry() {
        let board = simulator_board_profile(None).unwrap();

        assert_eq!(board.id, "mofei");
        assert_eq!(board.framebuffer_width, 800);
        assert_eq!(board.framebuffer_height, 480);
        assert_eq!(
            board.key_map.iter().map(|key| key.id).collect::<Vec<_>>(),
            vec![1, 2, 0]
        );
    }

    #[test]
    fn loads_s37uc_board_from_registry() {
        let board = simulator_board_profile(Some("s37uc")).unwrap();

        assert_eq!(board.framebuffer_width, 416);
        assert_eq!(board.framebuffer_height, 240);
        assert_eq!(
            board.key_map.iter().map(|key| key.id).collect::<Vec<_>>(),
            vec![0, 1]
        );
    }

    #[test]
    fn rejects_unknown_board() {
        let error = simulator_board_profile(Some("bad-board")).unwrap_err();

        assert!(error.contains("unsupported simulator board 'bad-board'"));
        assert!(error.contains("mofei"));
        assert!(error.contains("s37uc"));
    }

    #[test]
    fn validates_peripheral_control_payload_bounds() {
        assert!(validate_peripheral_control_payload(&[0; PERIPHERAL_CONTROL_MIN_PAYLOAD]).is_ok());
        assert!(validate_peripheral_control_payload(&[0; PERIPHERAL_CONTROL_MAX_PAYLOAD]).is_ok());
        assert!(
            validate_peripheral_control_payload(&[0; PERIPHERAL_CONTROL_MIN_PAYLOAD - 1]).is_err()
        );
        assert!(
            validate_peripheral_control_payload(&[0; PERIPHERAL_CONTROL_MAX_PAYLOAD + 1]).is_err()
        );
    }
}

#[cfg(test)]
mod simulated_location_defaults {
    use super::{is_valid_location, normalize_timezone_for_firmware, SimulatorLocation};

    #[test]
    fn default_is_simulated_london() {
        let location = SimulatorLocation::default();
        assert_eq!(location.name, "London");
        assert_eq!(location.timezone, "Europe%2FLondon");
        assert_eq!(location.latitude, 51.5074);
        assert_eq!(location.longitude, -0.1278);
        assert!(is_valid_location(&location));
        assert_eq!(normalize_timezone_for_firmware("Europe/London"), location.timezone);
    }
}
