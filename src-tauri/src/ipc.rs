// IPC frame protocol — see
// .trellis/tasks/05-04-mofei-simulator-bringup/research/qemu-peripheral-ipc.md
//
// Wire format (8-byte header):
//
//     u8  channel
//     u8  flags
//     u16 reserved
//     u32 payload_len  (little-endian)
//     u8  payload[payload_len]
//
// Channels:
//     0x01 framebuffer       (PR2, outbound — QEMU → Tauri)
//     0x02 serial-log        (PR1, outbound)
//     0x03 gpio-state        (PR2/3, outbound)
//     0x04 touch-event       (PR3, inbound — Tauri → QEMU)
//     0x05 button-event      (PR3, inbound)
//     0x06 control           (bidi)
//     0x08 panda-debug       (bidi, opt-in debug builds only)
//     0xFF heartbeat / error
//
// PR3 wires inbound 0x04/0x05 in addition to the PR1/PR2 outbound channels.
// One Unix-domain socket multiplexes both directions.

use crate::debug_transport::DebugTransportBroker;
use base64::{engine::general_purpose::STANDARD as BASE64_STANDARD, Engine};
use std::path::Path;
use std::sync::Mutex;
use std::sync::{Arc, MutexGuard};
use tauri::{AppHandle, Emitter};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixStream;
use tokio::sync::mpsc;

const HEADER_LEN: usize = 8;
const MAX_PAYLOAD_LEN: u32 = 1 << 20; // 1 MiB safety cap

pub const CHANNEL_FRAMEBUFFER: u8 = 0x01;
pub const CHANNEL_SERIAL_LOG: u8 = 0x02;
pub const CHANNEL_GPIO_STATE: u8 = 0x03;
pub const CHANNEL_TOUCH_EVENT: u8 = 0x04;
pub const CHANNEL_BUTTON_EVENT: u8 = 0x05;
pub const CHANNEL_CONTROL: u8 = 0x06;
pub const CHANNEL_DEBUG: u8 = 0x08;
pub const CHANNEL_HEARTBEAT: u8 = 0xFF;

#[derive(Clone, Copy, Debug, serde::Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum FramebufferFormat {
    Mono1,
    Gray16,
}

#[derive(Clone, Debug)]
pub struct BoardDisplayGeometry {
    pub width: u32,
    pub height: u32,
    pub format: FramebufferFormat,
}

impl BoardDisplayGeometry {
    pub fn new(width: u32, height: u32, format: FramebufferFormat) -> Self {
        Self {
            width,
            height,
            format,
        }
    }

    fn fb_bytes(&self) -> usize {
        let pixels = self.width as usize * self.height as usize;
        match self.format {
            FramebufferFormat::Mono1 => pixels.div_ceil(8),
            FramebufferFormat::Gray16 => pixels.div_ceil(2),
        }
    }
}

/// Outbound frame queued to the QEMU process.
#[derive(Debug, Clone)]
pub struct OutFrame {
    pub channel: u8,
    pub flags: u8,
    pub payload: Vec<u8>,
}

/// Sender handle exposed to Tauri commands so the frontend can inject touch
/// and button events. The sender survives across simulator launch/stop
/// cycles by being re-bound when `bind_writer` is called from spawn_io.
#[derive(Default)]
pub struct IpcWriter {
    tx: Mutex<Option<mpsc::UnboundedSender<OutFrame>>>,
}

impl IpcWriter {
    pub fn new() -> Self {
        Self {
            tx: Mutex::new(None),
        }
    }

    pub fn bind(&self, sender: mpsc::UnboundedSender<OutFrame>) {
        if let Ok(mut guard) = self.tx.lock() {
            *guard = Some(sender);
        }
    }

    pub fn unbind(&self) {
        if let Ok(mut guard) = self.tx.lock() {
            *guard = None;
        }
    }

    /// Try to enqueue a frame for the QEMU peripheral. Returns false if no
    /// IPC connection is currently bound (i.e. the simulator is not
    /// running) or if the channel is closed.
    pub fn try_send(&self, frame: OutFrame) -> bool {
        let guard = match self.tx.lock() {
            Ok(g) => g,
            Err(_) => return false,
        };
        match guard.as_ref() {
            Some(tx) => tx.send(frame).is_ok(),
            None => false,
        }
    }
}

/// Spawn the IPC reader+writer pair on a dedicated OS thread.
pub fn spawn_io(
    app: AppHandle,
    writer: std::sync::Arc<IpcWriter>,
    debug_broker: DebugTransportBroker,
    geometry: Arc<Mutex<BoardDisplayGeometry>>,
    socket_path: std::path::PathBuf,
) {
    std::thread::Builder::new()
        .name("mofei-sim-ipc".to_string())
        .spawn(move || {
            let rt = match tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
            {
                Ok(rt) => rt,
                Err(e) => {
                    let _ = app.emit("simulator-error", format!("ipc tokio runtime failed: {e}"));
                    return;
                }
            };
            rt.block_on(async move {
                let stream = match connect_with_retry(&socket_path, 40, 100).await {
                    Ok(s) => s,
                    Err(e) => {
                        let _ = app.emit("simulator-error", format!("ipc connect failed: {e}"));
                        return;
                    }
                };
                let (read_half, write_half) = stream.into_split();
                let (tx, rx) = mpsc::unbounded_channel::<OutFrame>();
                writer.bind(tx);

                let read_task =
                    tokio::spawn(read_loop(app.clone(), read_half, debug_broker, geometry));
                let write_task = tokio::spawn(write_loop(app.clone(), write_half, rx));

                let _ = tokio::join!(read_task, write_task);
                writer.unbind();
            });
        })
        .expect("failed to spawn mofei-sim-ipc thread");
}

async fn connect_with_retry(
    path: &Path,
    attempts: u32,
    delay_ms: u64,
) -> std::io::Result<UnixStream> {
    let mut last_err: Option<std::io::Error> = None;
    for _ in 0..attempts {
        match UnixStream::connect(path).await {
            Ok(s) => return Ok(s),
            Err(e) => last_err = Some(e),
        }
        tokio::time::sleep(std::time::Duration::from_millis(delay_ms)).await;
    }
    Err(last_err.unwrap_or_else(|| {
        std::io::Error::new(std::io::ErrorKind::TimedOut, "no attempts performed")
    }))
}

async fn read_loop(
    app: AppHandle,
    mut read_half: tokio::net::unix::OwnedReadHalf,
    debug_broker: DebugTransportBroker,
    geometry: Arc<Mutex<BoardDisplayGeometry>>,
) -> std::io::Result<()> {
    let mut header = [0u8; HEADER_LEN];
    loop {
        read_half.read_exact(&mut header).await?;
        let channel = header[0];
        let flags = header[1];
        let payload_len = u32::from_le_bytes([header[4], header[5], header[6], header[7]]);
        if payload_len > MAX_PAYLOAD_LEN {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                format!("frame payload_len {payload_len} exceeds cap {MAX_PAYLOAD_LEN}"),
            ));
        }
        let mut payload = vec![0u8; payload_len as usize];
        if payload_len > 0 {
            read_half.read_exact(&mut payload).await?;
        }
        dispatch(&app, channel, flags, payload, &debug_broker, &geometry);
    }
}

async fn write_loop(
    app: AppHandle,
    mut write_half: tokio::net::unix::OwnedWriteHalf,
    mut rx: mpsc::UnboundedReceiver<OutFrame>,
) {
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
        if let Err(e) = write_half.write_all(&header).await {
            let _ = app.emit("simulator-error", format!("ipc write header failed: {e}"));
            return;
        }
        if !frame.payload.is_empty() {
            if let Err(e) = write_half.write_all(&frame.payload).await {
                let _ = app.emit("simulator-error", format!("ipc write payload failed: {e}"));
                return;
            }
        }
    }
}

fn lock_geometry(
    geometry: &Arc<Mutex<BoardDisplayGeometry>>,
) -> MutexGuard<'_, BoardDisplayGeometry> {
    geometry
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn dispatch(
    app: &AppHandle,
    channel: u8,
    flags: u8,
    payload: Vec<u8>,
    debug_broker: &DebugTransportBroker,
    geometry: &Arc<Mutex<BoardDisplayGeometry>>,
) {
    match channel {
        CHANNEL_SERIAL_LOG => {
            let text = String::from_utf8_lossy(&payload).into_owned();
            let _ = app.emit("serial-log", text);
        }
        CHANNEL_FRAMEBUFFER => {
            let geometry = lock_geometry(geometry).clone();
            let rgba = framebuffer_to_rgba(&payload, &geometry);
            let encoded = BASE64_STANDARD.encode(&rgba);
            let payload_json = serde_json::json!({
                "kind": if flags & FB_FLAG_PARTIAL != 0 { "partial" } else { "full" },
                "width": geometry.width,
                "height": geometry.height,
                "rgba_b64": encoded,
            });
            let _ = app.emit("framebuffer", payload_json);
        }
        // Channel 0x08 carries Panda debug JSON back from QEMU to the local
        // debug TCP server while still surfacing the payload to the Tauri UI.
        CHANNEL_DEBUG => {
            let text = String::from_utf8_lossy(&payload).into_owned();
            debug_broker.publish(payload);
            let _ = app.emit("debug-transport", text);
        }
        CHANNEL_CONTROL => {
            let _ = app.emit("peripheral-control", payload);
        }
        CHANNEL_GPIO_STATE | CHANNEL_TOUCH_EVENT | CHANNEL_BUTTON_EVENT | CHANNEL_HEARTBEAT => {}
        unknown => {
            let _ = app.emit(
                "simulator-error",
                format!(
                    "ipc: unknown channel 0x{unknown:02X}, flags=0x{:02X}, len={}",
                    flags,
                    payload.len()
                ),
            );
        }
    }
}

const FB_FLAG_PARTIAL: u8 = 0x01;

fn framebuffer_to_rgba(fb: &[u8], geometry: &BoardDisplayGeometry) -> Vec<u8> {
    let mut out = vec![0u8; (geometry.width as usize) * (geometry.height as usize) * 4];
    let n = fb.len().min(geometry.fb_bytes());
    let pixels_per_byte = match geometry.format {
        FramebufferFormat::Mono1 => 8,
        FramebufferFormat::Gray16 => 2,
    };
    for (byte_idx, b) in fb.iter().copied().take(n).enumerate() {
        for subpixel in 0..pixels_per_byte {
            let pixel_idx = byte_idx * pixels_per_byte + subpixel;
            let value = match geometry.format {
                FramebufferFormat::Mono1 => {
                    if (b >> (7 - subpixel)) & 1 != 0 {
                        0xFF
                    } else {
                        0x00
                    }
                }
                FramebufferFormat::Gray16 => {
                    let level = if subpixel == 0 { b >> 4 } else { b & 0x0F };
                    0xFF - level * 17
                }
            };
            let off = pixel_idx * 4;
            out[off] = value;
            out[off + 1] = value;
            out[off + 2] = value;
            out[off + 3] = 0xFF;
        }
    }
    out
}

/// Encode a touch event for channel 0x04. Board touch models consume the same
/// 6-byte payload as logical UI coordinates.
///   u8  action (1 DOWN, 2 MOVE, 3 UP)
///   u16 x      (LE)
///   u16 y      (LE)
///   u8  finger_id
pub fn encode_touch_event(action: u8, x: u16, y: u16, finger_id: u8) -> OutFrame {
    let mut payload = Vec::with_capacity(6);
    payload.push(action);
    payload.push((x & 0xFF) as u8);
    payload.push(((x >> 8) & 0xFF) as u8);
    payload.push((y & 0xFF) as u8);
    payload.push(((y >> 8) & 0xFF) as u8);
    payload.push(finger_id);
    OutFrame {
        channel: CHANNEL_TOUCH_EVENT,
        flags: 0,
        payload,
    }
}

/// Encode a button event for channel 0x05. Wire layout (2 bytes):
///   u8  button_id  (board registry keyMap id)
///   u8  pressed    (0=released, 1=pressed)
pub fn encode_button_event(button_id: u8, pressed: bool) -> OutFrame {
    OutFrame {
        channel: CHANNEL_BUTTON_EVENT,
        flags: 0,
        payload: vec![button_id, if pressed { 1 } else { 0 }],
    }
}

pub fn encode_peripheral_control(payload: Vec<u8>) -> OutFrame {
    OutFrame {
        channel: CHANNEL_CONTROL,
        flags: 0,
        payload,
    }
}
