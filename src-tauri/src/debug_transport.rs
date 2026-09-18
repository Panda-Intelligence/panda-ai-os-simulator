use crate::ipc::{IpcWriter, OutFrame, CHANNEL_DEBUG};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener as StdTcpListener};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::Duration;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::tcp::OwnedWriteHalf;
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{broadcast, watch};
use tokio::time::Instant;

pub const DEBUG_PORT_ENV: &str = "PANDA_SIM_DEBUG_PORT";
const DEBUG_BROKER_CAPACITY: usize = 128;
const RUNTIME_RESPONSE_TIMEOUT: Duration = Duration::from_secs(3);
const RUNTIME_POLL_INTERVAL: Duration = Duration::from_millis(25);
const WRITER_BIND_TIMEOUT: Duration = Duration::from_millis(500);
pub const COMMAND_INITIALIZE: &str = "initialize";
pub const COMMAND_SET_BREAKPOINTS: &str = "setBreakpoints";
pub const COMMAND_CONTINUE: &str = "continue";
pub const COMMAND_NEXT: &str = "next";
pub const COMMAND_STEP_IN: &str = "stepIn";
pub const COMMAND_STEP_OUT: &str = "stepOut";
pub const COMMAND_PAUSE: &str = "pause";
pub const COMMAND_STACK_TRACE: &str = "stackTrace";
pub const COMMAND_SCOPES: &str = "scopes";
pub const COMMAND_VARIABLES: &str = "variables";
pub const COMMAND_EVALUATE: &str = "evaluate";
pub const COMMAND_DISCONNECT: &str = "disconnect";

pub const SUPPORTED_COMMANDS: &[&str] = &[
    COMMAND_INITIALIZE,
    COMMAND_SET_BREAKPOINTS,
    COMMAND_CONTINUE,
    COMMAND_NEXT,
    COMMAND_STEP_IN,
    COMMAND_STEP_OUT,
    COMMAND_PAUSE,
    COMMAND_STACK_TRACE,
    COMMAND_SCOPES,
    COMMAND_VARIABLES,
    COMMAND_EVALUATE,
    COMMAND_DISCONNECT,
];

pub const EVENT_INITIALIZED: &str = "initialized";
pub const EVENT_STOPPED: &str = "stopped";
pub const EVENT_CONTINUED: &str = "continued";
pub const EVENT_OUTPUT: &str = "output";
pub const EVENT_TERMINATED: &str = "terminated";

pub const SUPPORTED_EVENTS: &[&str] = &[
    EVENT_INITIALIZED,
    EVENT_STOPPED,
    EVENT_CONTINUED,
    EVENT_OUTPUT,
    EVENT_TERMINATED,
];

#[derive(Debug, Deserialize, Serialize)]
pub struct DebugRequest {
    pub seq: u64,
    pub command: String,
    #[serde(default)]
    pub arguments: Value,
}

#[derive(Debug, Serialize)]
struct DebugResponse<'a> {
    seq: u64,
    request_seq: u64,
    success: bool,
    command: &'a str,
    body: Value,
    #[serde(skip_serializing_if = "Option::is_none")]
    message: Option<String>,
}

#[derive(Debug, Serialize)]
struct DebugEvent<'a> {
    seq: u64,
    event: &'a str,
    body: Value,
}

#[derive(Clone)]
pub struct DebugTransportBroker {
    tx: broadcast::Sender<Vec<u8>>,
}

impl DebugTransportBroker {
    pub fn new() -> Self {
        let (tx, _) = broadcast::channel(DEBUG_BROKER_CAPACITY);
        Self { tx }
    }

    pub fn publish(&self, payload: Vec<u8>) {
        let _ = self.tx.send(payload);
    }

    fn subscribe(&self) -> broadcast::Receiver<Vec<u8>> {
        self.tx.subscribe()
    }
}

impl Default for DebugTransportBroker {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Clone)]
struct IpcDebugBridge {
    writer: Arc<IpcWriter>,
    broker: DebugTransportBroker,
}

pub struct DebugServerHandle {
    port: u16,
    shutdown: Option<watch::Sender<bool>>,
    thread: Option<JoinHandle<()>>,
}

impl DebugServerHandle {
    pub fn port(&self) -> u16 {
        self.port
    }

    fn stop_inner(&mut self) {
        if let Some(shutdown) = self.shutdown.take() {
            let _ = shutdown.send(true);
        }
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

impl Drop for DebugServerHandle {
    fn drop(&mut self) {
        self.stop_inner();
    }
}

pub fn start_from_env(
    writer: Arc<IpcWriter>,
    broker: DebugTransportBroker,
) -> Result<Option<DebugServerHandle>, String> {
    let Some(port) = debug_port_from_env()? else {
        return Ok(None);
    };
    start(port, writer, broker).map(Some)
}

pub fn debug_port_from_env() -> Result<Option<u16>, String> {
    let Some(value) = std::env::var_os(DEBUG_PORT_ENV) else {
        return Ok(None);
    };
    let value = value.to_string_lossy();
    let value = value.trim();
    if value.is_empty() {
        return Ok(None);
    }
    parse_debug_port(value).map(Some)
}

fn parse_debug_port(value: &str) -> Result<u16, String> {
    let port = value
        .parse::<u16>()
        .map_err(|err| format!("{DEBUG_PORT_ENV} must be a TCP port: {err}"))?;
    if port == 0 {
        return Err(format!("{DEBUG_PORT_ENV} must be greater than 0"));
    }
    Ok(port)
}

fn start(
    port: u16,
    writer: Arc<IpcWriter>,
    broker: DebugTransportBroker,
) -> Result<DebugServerHandle, String> {
    let addr = SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), port);
    let listener = StdTcpListener::bind(addr)
        .map_err(|err| format!("failed to bind Panda debug transport on {addr}: {err}"))?;
    listener
        .set_nonblocking(true)
        .map_err(|err| format!("failed to set Panda debug transport nonblocking mode: {err}"))?;
    let (shutdown_tx, shutdown_rx) = watch::channel(false);
    let bridge = IpcDebugBridge { writer, broker };
    let thread = std::thread::Builder::new()
        .name("panda-sim-debug".to_string())
        .spawn(move || {
            let rt = match tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
            {
                Ok(rt) => rt,
                Err(err) => {
                    eprintln!("[tauri] debug transport runtime failed: {err}");
                    return;
                }
            };
            if let Err(err) = rt.block_on(run_server(listener, bridge, shutdown_rx)) {
                eprintln!("[tauri] debug transport stopped: {err}");
            }
        })
        .map_err(|err| format!("failed to spawn debug transport thread: {err}"))?;

    Ok(DebugServerHandle {
        port,
        shutdown: Some(shutdown_tx),
        thread: Some(thread),
    })
}

async fn run_server(
    listener: StdTcpListener,
    bridge: IpcDebugBridge,
    mut shutdown: watch::Receiver<bool>,
) -> std::io::Result<()> {
    let addr = listener.local_addr()?;
    let listener = TcpListener::from_std(listener)?;
    eprintln!("[tauri] Panda debug transport listening on {addr}");

    loop {
        tokio::select! {
            changed = shutdown.changed() => {
                let _ = changed;
                return Ok(());
            }
            accepted = listener.accept() => {
                let (stream, peer) = accepted?;
                eprintln!("[tauri] Panda debug client connected from {peer}");
                if let Err(err) = handle_client(stream, bridge.clone(), shutdown.clone()).await {
                    eprintln!("[tauri] Panda debug client disconnected with error: {err}");
                }
            }
        }
    }
}

async fn handle_client(
    stream: TcpStream,
    bridge: IpcDebugBridge,
    mut shutdown: watch::Receiver<bool>,
) -> std::io::Result<()> {
    let (read_half, mut write_half) = stream.into_split();
    let mut lines = BufReader::new(read_half).lines();
    let mut debug_rx = bridge.subscribe();
    let mut debug_poll = tokio::time::interval_at(
        Instant::now() + RUNTIME_POLL_INTERVAL,
        RUNTIME_POLL_INTERVAL,
    );
    debug_poll.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
    let mut next_seq = 1u64;

    loop {
        tokio::select! {
            changed = shutdown.changed() => {
                let _ = changed;
                return Ok(());
            }
            line = lines.next_line() => {
                let Some(line) = line? else {
                    return Ok(());
                };
                let line = line.trim();
                if line.is_empty() {
                    continue;
                }
                let request = match serde_json::from_str::<DebugRequest>(line) {
                    Ok(request) => request,
                    Err(err) => {
                        let response = DebugResponse {
                            seq: allocate_seq(&mut next_seq),
                            request_seq: 0,
                            success: false,
                            command: "parse",
                            body: json!({}),
                            message: Some(format!("invalid debug request JSON: {err}")),
                        };
                        write_json_line(&mut write_half, &response).await?;
                        continue;
                    }
                };
                if process_request(&request, &bridge, &mut debug_rx, &mut write_half, &mut next_seq).await? {
                    return Ok(());
                }
            }
            frame = debug_rx.recv() => {
                match frame {
                    Ok(payload) => {
                        let _ = forward_debug_payload(&mut write_half, &payload).await?;
                    }
                    Err(broadcast::error::RecvError::Lagged(_)) => {}
                    Err(broadcast::error::RecvError::Closed) => return Ok(()),
                }
            }
            _ = debug_poll.tick() => {
                bridge.poll();
            }
        }
    }
}

async fn process_request(
    request: &DebugRequest,
    bridge: &IpcDebugBridge,
    debug_rx: &mut broadcast::Receiver<Vec<u8>>,
    write_half: &mut OwnedWriteHalf,
    next_seq: &mut u64,
) -> std::io::Result<bool> {
    if !SUPPORTED_COMMANDS.contains(&request.command.as_str()) {
        let response = DebugResponse {
            seq: allocate_seq(next_seq),
            request_seq: request.seq,
            success: false,
            command: &request.command,
            body: json!({}),
            message: Some(format!("unsupported debug command '{}'", request.command)),
        };
        write_json_line(write_half, &response).await?;
        return Ok(false);
    }

    match request.command.as_str() {
        COMMAND_INITIALIZE => {
            let response = DebugResponse {
                seq: allocate_seq(next_seq),
                request_seq: request.seq,
                success: true,
                command: COMMAND_INITIALIZE,
                body: json!({
                    "protocol": "panda-sim-debug",
                    "bridge": "qemu-ipc-channel",
                    "debugChannel": format!("0x{CHANNEL_DEBUG:02X}"),
                    "runtimeRoundTrip": "requires debug firmware response",
                    "commands": SUPPORTED_COMMANDS,
                    "events": SUPPORTED_EVENTS
                }),
                message: None,
            };
            write_json_line(write_half, &response).await?;
            let event = DebugEvent {
                seq: allocate_seq(next_seq),
                event: EVENT_INITIALIZED,
                body: json!({}),
            };
            write_json_line(write_half, &event).await?;
            Ok(false)
        }
        _ => {
            let forwarded = bridge.forward(request).await;
            if !forwarded {
                let response = DebugResponse {
                    seq: allocate_seq(next_seq),
                    request_seq: request.seq,
                    success: false,
                    command: &request.command,
                    body: json!({
                        "bridge": "qemu-ipc-channel",
                        "debugChannel": format!("0x{CHANNEL_DEBUG:02X}"),
                        "forwarded": false
                    }),
                    message: Some("simulator IPC channel 0x08 is not connected".to_string()),
                };
                write_json_line(write_half, &response).await?;
                return Ok(request.command == COMMAND_DISCONNECT);
            }

            let matched =
                wait_for_runtime_response(request.seq, bridge, debug_rx, write_half).await?;
            if !matched {
                let response = DebugResponse {
                    seq: allocate_seq(next_seq),
                    request_seq: request.seq,
                    success: false,
                    command: &request.command,
                    body: json!({
                        "bridge": "qemu-ipc-channel",
                        "debugChannel": format!("0x{CHANNEL_DEBUG:02X}"),
                        "forwarded": true,
                        "timeoutMs": RUNTIME_RESPONSE_TIMEOUT.as_millis()
                    }),
                    message: Some(
                        "timed out waiting for firmware debug response on channel 0x08".to_string(),
                    ),
                };
                write_json_line(write_half, &response).await?;
            }
            if request.command == COMMAND_DISCONNECT {
                return Ok(true);
            }
            Ok(false)
        }
    }
}

async fn wait_for_runtime_response(
    request_seq: u64,
    bridge: &IpcDebugBridge,
    debug_rx: &mut broadcast::Receiver<Vec<u8>>,
    write_half: &mut OwnedWriteHalf,
) -> std::io::Result<bool> {
    let timeout = tokio::time::sleep(RUNTIME_RESPONSE_TIMEOUT);
    tokio::pin!(timeout);
    let mut poll = tokio::time::interval(RUNTIME_POLL_INTERVAL);
    poll.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
    bridge.poll();

    loop {
        tokio::select! {
            _ = &mut timeout => return Ok(false),
            _ = poll.tick() => {
                bridge.poll();
            }
            frame = debug_rx.recv() => {
                match frame {
                    Ok(payload) => {
                        if forward_debug_payload(write_half, &payload).await? == Some(request_seq) {
                            return Ok(true);
                        }
                    }
                    Err(broadcast::error::RecvError::Lagged(_)) => {}
                    Err(broadcast::error::RecvError::Closed) => return Ok(false),
                }
            }
        }
    }
}

async fn forward_debug_payload(
    write_half: &mut OwnedWriteHalf,
    payload: &[u8],
) -> std::io::Result<Option<u64>> {
    let mut matched_request_seq = None;
    for record in debug_records(payload) {
        if let Some(request_seq) = response_request_seq(&record) {
            matched_request_seq = Some(request_seq);
        }
        write_half.write_all(record.as_bytes()).await?;
        write_half.write_all(b"\n").await?;
    }
    Ok(matched_request_seq)
}

fn debug_records(payload: &[u8]) -> Vec<String> {
    String::from_utf8_lossy(payload)
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty())
        .map(ToOwned::to_owned)
        .collect()
}

fn response_request_seq(record: &str) -> Option<u64> {
    let value = serde_json::from_str::<Value>(record).ok()?;
    value.get("request_seq").and_then(Value::as_u64)
}

impl IpcDebugBridge {
    fn subscribe(&self) -> broadcast::Receiver<Vec<u8>> {
        self.broker.subscribe()
    }

    fn poll(&self) -> bool {
        self.writer.try_send(OutFrame {
            channel: CHANNEL_DEBUG,
            flags: 0,
            payload: Vec::new(),
        })
    }

    async fn forward(&self, request: &DebugRequest) -> bool {
        let Ok(mut payload) = serde_json::to_vec(request) else {
            return false;
        };
        payload.push(b'\n');
        self.send_within(
            OutFrame {
                channel: CHANNEL_DEBUG,
                flags: 0,
                payload,
            },
            WRITER_BIND_TIMEOUT,
        )
        .await
    }

    async fn send_within(&self, frame: OutFrame, timeout: Duration) -> bool {
        if self.writer.try_send(frame.clone()) {
            return true;
        }

        let deadline = Instant::now() + timeout;
        loop {
            let now = Instant::now();
            if now >= deadline {
                return false;
            }
            tokio::time::sleep((deadline - now).min(RUNTIME_POLL_INTERVAL)).await;
            if self.writer.try_send(frame.clone()) {
                return true;
            }
        }
    }
}

fn allocate_seq(next_seq: &mut u64) -> u64 {
    let seq = *next_seq;
    *next_seq = next_seq.saturating_add(1);
    seq
}

async fn write_json_line<T: Serialize>(
    write_half: &mut OwnedWriteHalf,
    value: &T,
) -> std::io::Result<()> {
    let mut line = serde_json::to_vec(value).map_err(std::io::Error::other)?;
    line.push(b'\n');
    write_half.write_all(&line).await
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{BufRead, BufReader, Write};
    use std::net::{TcpListener as StdTcpListener, TcpStream as StdTcpStream};
    use std::time::Duration;

    fn free_port() -> u16 {
        StdTcpListener::bind("127.0.0.1:0")
            .unwrap()
            .local_addr()
            .unwrap()
            .port()
    }

    fn connect_with_retry(port: u16) -> StdTcpStream {
        let addr = format!("127.0.0.1:{port}");
        let deadline = std::time::Instant::now() + Duration::from_secs(2);
        loop {
            match StdTcpStream::connect(&addr) {
                Ok(stream) => {
                    stream
                        .set_read_timeout(Some(Duration::from_secs(2)))
                        .unwrap();
                    stream
                        .set_write_timeout(Some(Duration::from_secs(2)))
                        .unwrap();
                    return stream;
                }
                Err(err) if std::time::Instant::now() < deadline => {
                    let _ = err;
                    std::thread::sleep(Duration::from_millis(25));
                }
                Err(err) => panic!("failed to connect to debug transport: {err}"),
            }
        }
    }

    #[test]
    fn debug_server_accepts_initialize_client() {
        let port = free_port();
        let _server = start(
            port,
            Arc::new(IpcWriter::new()),
            DebugTransportBroker::new(),
        )
        .unwrap();
        let mut stream = connect_with_retry(port);
        stream
            .write_all(br#"{"seq":7,"command":"initialize","arguments":{}}"#)
            .unwrap();
        stream.write_all(b"\n").unwrap();

        let mut reader = BufReader::new(stream);
        let mut response = String::new();
        let mut event = String::new();
        reader.read_line(&mut response).unwrap();
        reader.read_line(&mut event).unwrap();

        let response: Value = serde_json::from_str(response.trim()).unwrap();
        assert_eq!(response["request_seq"], 7);
        assert_eq!(response["success"], true);
        assert_eq!(response["command"], COMMAND_INITIALIZE);
        assert_eq!(response["body"]["bridge"], "qemu-ipc-channel");
        assert_eq!(response["body"]["debugChannel"], "0x08");

        let event: Value = serde_json::from_str(event.trim()).unwrap();
        assert_eq!(event["event"], EVENT_INITIALIZED);
    }

    #[test]
    fn debug_server_forwards_runtime_response_from_broker() {
        let port = free_port();
        let writer = Arc::new(IpcWriter::new());
        let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<OutFrame>();
        writer.bind(tx);
        let broker = DebugTransportBroker::new();
        let _server = start(port, writer, broker.clone()).unwrap();
        let mut stream = connect_with_retry(port);
        stream
            .write_all(br#"{"seq":11,"command":"stackTrace","arguments":{}}"#)
            .unwrap();
        stream.write_all(b"\n").unwrap();

        let frame = rx.blocking_recv().unwrap();
        assert_eq!(frame.channel, CHANNEL_DEBUG);
        assert_eq!(frame.flags, 0);
        assert_eq!(
            String::from_utf8(frame.payload).unwrap(),
            "{\"seq\":11,\"command\":\"stackTrace\",\"arguments\":{}}\n"
        );

        broker.publish(
            br#"{"seq":44,"request_seq":11,"success":true,"command":"stackTrace","body":{"stackFrames":[]}}"#
                .to_vec(),
        );

        let mut reader = BufReader::new(stream);
        let mut response = String::new();
        reader.read_line(&mut response).unwrap();

        let response: Value = serde_json::from_str(response.trim()).unwrap();
        assert_eq!(response["request_seq"], 11);
        assert_eq!(response["success"], true);
        assert_eq!(response["command"], COMMAND_STACK_TRACE);
        assert_eq!(response["body"]["stackFrames"].as_array().unwrap().len(), 0);
    }

    #[test]
    fn debug_server_waits_for_delayed_writer_bind() {
        let port = free_port();
        let writer = Arc::new(IpcWriter::new());
        let broker = DebugTransportBroker::new();
        let _server = start(port, writer.clone(), broker.clone()).unwrap();
        let mut stream = connect_with_retry(port);
        stream
            .write_all(br#"{"seq":12,"command":"stackTrace","arguments":{}}"#)
            .unwrap();
        stream.write_all(b"\n").unwrap();

        let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<OutFrame>();
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(100));
            writer.bind(tx);
        });

        let frame = rx.blocking_recv().unwrap();
        assert_eq!(frame.channel, CHANNEL_DEBUG);
        assert_eq!(
            String::from_utf8(frame.payload).unwrap(),
            "{\"seq\":12,\"command\":\"stackTrace\",\"arguments\":{}}\n"
        );

        broker.publish(
            br#"{"seq":45,"request_seq":12,"success":true,"command":"stackTrace","body":{"stackFrames":[]}}"#
                .to_vec(),
        );

        let mut reader = BufReader::new(stream);
        let mut response = String::new();
        reader.read_line(&mut response).unwrap();

        let response: Value = serde_json::from_str(response.trim()).unwrap();
        assert_eq!(response["request_seq"], 12);
        assert_eq!(response["success"], true);
        assert_eq!(response["command"], COMMAND_STACK_TRACE);
    }

    #[test]
    fn parse_debug_port_rejects_zero() {
        assert!(parse_debug_port("0")
            .unwrap_err()
            .contains("greater than 0"));
        assert_eq!(parse_debug_port("4711").unwrap(), 4711);
    }
}
