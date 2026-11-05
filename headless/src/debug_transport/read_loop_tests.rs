use super::*;
use crate::{BoardGeometry, FramebufferFormat, HeadlessEvent, InjectionCommand, InjectionPlan};
use std::io::{Read, Write};
use tokio::task::JoinHandle;

struct ReadLoopFixture {
    peer: std::os::unix::net::UnixStream,
    commands: mpsc::UnboundedSender<InjectionCommand>,
    events: mpsc::UnboundedReceiver<HeadlessEvent>,
    debug: broadcast::Receiver<Vec<u8>>,
    writer: HeadlessDebugWriter,
    reader_task: JoinHandle<Result<()>>,
    writer_task: JoinHandle<Result<()>>,
    pending_wire: Vec<u8>,
}

impl Drop for ReadLoopFixture {
    fn drop(&mut self) {
        self.reader_task.abort();
        self.writer_task.abort();
    }
}

fn manual_plan() -> InjectionPlan {
    InjectionPlan {
        taps: Vec::new(),
        inject_buttons: Vec::new(),
        button_hold_ms: 30,
        post_tap_delay_ms: 0,
        post_tap_frames_before_buttons: 0,
        startup_frames_before_input: 1,
    }
}

async fn settle() {
    // 讓真實 Unix 就緒事件與 Tokio 工作輪流執行；暫停的時鐘保持不動。
    for _ in 0..256 {
        tokio::task::yield_now().await;
    }
}

async fn advance(ms: u64) {
    tokio::time::advance(Duration::from_millis(ms)).await;
    settle().await;
}

impl ReadLoopFixture {
    async fn start(plan: InjectionPlan) -> Self {
        let (host, peer) = tokio::net::UnixStream::pair().unwrap();
        let (read_half, write_half) = host.into_split();
        let writer = HeadlessDebugWriter::new();
        let broker = DebugTransportBroker::new();
        let debug = broker.subscribe();
        let (out_tx, out_rx) = mpsc::unbounded_channel();
        writer.bind(out_tx).await;
        let (event_tx, events) = mpsc::unbounded_channel();
        let (commands, command_rx) = mpsc::unbounded_channel();
        let geometry = BoardGeometry {
            raw_width: 8,
            raw_height: 8,
            framebuffer_width: 8,
            framebuffer_height: 8,
            framebuffer_format: FramebufferFormat::Mono1,
            output_width: 8,
            output_height: 8,
        };
        let reader_task = tokio::spawn(crate::read_loop(
            read_half,
            event_tx,
            plan,
            command_rx,
            geometry,
            writer.clone(),
            broker,
            None,
        ));
        let writer_task = tokio::spawn(crate::write_loop(write_half, out_rx));
        Self {
            peer: peer.into_std().unwrap(),
            commands,
            events,
            debug,
            writer,
            reader_task,
            writer_task,
            pending_wire: Vec::new(),
        }
    }

    fn send(&mut self, channel: u8, payload: &[u8]) {
        let mut packet = vec![channel, 0, 0, 0];
        packet.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        packet.extend_from_slice(payload);
        self.peer.write_all(&packet).unwrap();
    }

    fn assert_wire(&mut self, expected: &[(u8, &[u8])]) {
        let mut buffer = [0; 1024];
        loop {
            match self.peer.read(&mut buffer) {
                Ok(0) => break,
                Ok(length) => self.pending_wire.extend_from_slice(&buffer[..length]),
                Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => break,
                Err(error) => panic!("mock guest IPC read failed: {error}"),
            }
        }
        let mut actual = Vec::new();
        while self.pending_wire.len() >= crate::HEADER_LEN {
            let length = u32::from_le_bytes(self.pending_wire[4..8].try_into().unwrap()) as usize;
            assert!(self.pending_wire.len() >= crate::HEADER_LEN + length);
            actual.push((
                self.pending_wire[0],
                self.pending_wire[crate::HEADER_LEN..crate::HEADER_LEN + length].to_vec(),
            ));
            self.pending_wire.drain(..crate::HEADER_LEN + length);
        }
        let expected: Vec<_> = expected
            .iter()
            .map(|(channel, payload)| (*channel, payload.to_vec()))
            .collect();
        assert_eq!(actual, expected);
        assert!(self.pending_wire.is_empty());
    }

    fn assert_frame(&mut self, payload: &[u8]) {
        assert!(
            matches!(self.events.try_recv(), Ok(HeadlessEvent::Frame(bytes)) if bytes == payload)
        );
    }

    fn assert_injected(&mut self) {
        assert!(matches!(
            self.events.try_recv(),
            Ok(HeadlessEvent::Injected)
        ));
    }

    fn assert_no_events(&mut self) {
        assert!(self.events.try_recv().is_err());
    }
}

#[tokio::test(start_paused = true)]
async fn read_loop_drains_all_channels_during_hold_and_preserves_manual_fifo() {
    let mut fixture = ReadLoopFixture::start(manual_plan()).await;
    for command in [
        InjectionCommand::TouchTap { x: 2, y: 3 },
        InjectionCommand::ButtonClick {
            button_id: 4,
            hold_ms: None,
        },
        InjectionCommand::SyntheticTouchEvent {
            touch_type: crate::SyntheticTouchEventType::Tap,
            x: 5,
            y: 6,
        },
        InjectionCommand::TouchSwipe {
            x: 2,
            y: 3,
            direction: crate::TouchGestureDirection::Right,
        },
    ] {
        fixture.commands.send(command).unwrap();
    }
    settle().await;
    fixture.assert_wire(&[(crate::CHANNEL_TOUCH_EVENT, &[1, 2, 0, 3, 0, 0])]);
    let hold_started = Instant::now();
    fixture.send(crate::CHANNEL_FRAMEBUFFER, &[0; 8]);
    // 同一條串流中後面的 control/debug 可見，證明 serial-log payload 已完整排空。
    fixture.send(0x02, b"guest serial during contact\n");
    fixture.send(crate::CHANNEL_CONTROL, &[1, 3, 0x80]);
    fixture.send(CHANNEL_DEBUG, b"runtime debug during contact");
    settle().await;
    assert_eq!(Instant::now(), hold_started);
    fixture.assert_frame(&[0; 8]);
    assert!(
        matches!(fixture.events.try_recv(), Ok(HeadlessEvent::PeripheralControl(bytes)) if bytes == [1, 3, 0x80])
    );
    assert_eq!(
        fixture.debug.try_recv().unwrap(),
        b"runtime debug during contact"
    );
    fixture.assert_wire(&[]);
    fixture.assert_no_events();
    advance(249).await;
    fixture.assert_wire(&[]);
    fixture.assert_no_events();
    advance(1).await;
    fixture.assert_wire(&[
        (crate::CHANNEL_TOUCH_EVENT, &[3, 2, 0, 3, 0, 0]),
        (crate::CHANNEL_BUTTON_EVENT, &[4, 1]),
    ]);
    fixture.assert_injected();
    fixture.assert_no_events();
    advance(29).await;
    fixture.assert_wire(&[]);
    fixture.assert_no_events();
    advance(1).await;
    fixture.assert_wire(&[
        (crate::CHANNEL_BUTTON_EVENT, &[4, 0]),
        (crate::CHANNEL_SYNTHETIC_TOUCH_EVENT, &[1, 5, 0, 6, 0]),
        (crate::CHANNEL_TOUCH_EVENT, &[1, 2, 0, 3, 0, 0]),
    ]);
    fixture.assert_injected();
    fixture.assert_injected();
    fixture.assert_no_events();
    advance(250).await;
    fixture.assert_wire(&[]);
    for x in [3, 5, 6] {
        advance(50).await;
        fixture.assert_wire(&[(crate::CHANNEL_TOUCH_EVENT, &[2, x, 0, 3, 0, 0])]);
        fixture.assert_no_events();
    }
    advance(50).await;
    fixture.assert_wire(&[(crate::CHANNEL_TOUCH_EVENT, &[3, 6, 0, 3, 0, 0])]);
    fixture.assert_injected();
    fixture.assert_no_events();
}

#[tokio::test(start_paused = true)]
async fn read_loop_preserves_automatic_frame_gates_delays_and_final_completion() {
    let mut fixture = ReadLoopFixture::start(InjectionPlan {
        taps: vec![
            crate::TouchTap { x: 2, y: 3 },
            crate::TouchTap { x: 4, y: 5 },
        ],
        inject_buttons: vec![1, 2],
        button_hold_ms: 30,
        post_tap_delay_ms: 40,
        post_tap_frames_before_buttons: 2,
        startup_frames_before_input: 2,
    })
    .await;
    for payload in [[0xff; 8], [1; 8], [2; 8]] {
        fixture.send(crate::CHANNEL_FRAMEBUFFER, &payload);
        settle().await;
        fixture.assert_frame(&payload);
        fixture.assert_wire(&[]);
        fixture.assert_no_events();
    }
    fixture.send(crate::CHANNEL_FRAMEBUFFER, &[3; 8]);
    settle().await;
    fixture.assert_frame(&[3; 8]);
    fixture.assert_wire(&[(crate::CHANNEL_TOUCH_EVENT, &[1, 2, 0, 3, 0, 0])]);
    fixture.send(crate::CHANNEL_FRAMEBUFFER, &[4; 8]);
    settle().await;
    fixture.assert_frame(&[4; 8]);
    fixture.assert_wire(&[]);
    advance(250).await;
    fixture.assert_wire(&[(crate::CHANNEL_TOUCH_EVENT, &[3, 2, 0, 3, 0, 0])]);
    fixture.assert_no_events();
    fixture.send(crate::CHANNEL_FRAMEBUFFER, &[5; 8]);
    settle().await;
    fixture.assert_frame(&[5; 8]);
    advance(39).await;
    fixture.assert_wire(&[]);
    fixture.assert_no_events();
    // 按住與 post-tap 延遲期間抵達的兩幀仍依原有 socket backlog 語意計數。
    advance(1).await;
    fixture.assert_wire(&[(crate::CHANNEL_TOUCH_EVENT, &[1, 4, 0, 5, 0, 0])]);
    fixture.assert_no_events();
    advance(250).await;
    fixture.assert_wire(&[(crate::CHANNEL_TOUCH_EVENT, &[3, 4, 0, 5, 0, 0])]);
    advance(40).await;
    fixture.assert_wire(&[]);
    fixture.assert_no_events();
    fixture.send(crate::CHANNEL_FRAMEBUFFER, &[6; 8]);
    settle().await;
    fixture.assert_frame(&[6; 8]);
    fixture.assert_wire(&[]);
    fixture.send(crate::CHANNEL_FRAMEBUFFER, &[7; 8]);
    settle().await;
    fixture.assert_frame(&[7; 8]);
    fixture.assert_wire(&[(crate::CHANNEL_BUTTON_EVENT, &[1, 1])]);
    advance(30).await;
    fixture.assert_wire(&[
        (crate::CHANNEL_BUTTON_EVENT, &[1, 0]),
        (crate::CHANNEL_BUTTON_EVENT, &[2, 1]),
    ]);
    fixture.assert_no_events();
    advance(30).await;
    fixture.assert_wire(&[(crate::CHANNEL_BUTTON_EVENT, &[2, 0])]);
    fixture.assert_injected();
    fixture.send(crate::CHANNEL_FRAMEBUFFER, &[8; 8]);
    settle().await;
    fixture.assert_frame(&[8; 8]);
    fixture.assert_no_events();
    fixture.assert_wire(&[]);
}

#[tokio::test(start_paused = true)]
async fn read_loop_abort_cancels_pending_input_without_detaching_worker() {
    let mut fixture = ReadLoopFixture::start(manual_plan()).await;
    fixture
        .commands
        .send(InjectionCommand::TouchTap { x: 2, y: 3 })
        .unwrap();
    fixture
        .commands
        .send(InjectionCommand::ButtonClick {
            button_id: 4,
            hold_ms: None,
        })
        .unwrap();
    settle().await;
    fixture.assert_wire(&[(crate::CHANNEL_TOUCH_EVENT, &[1, 2, 0, 3, 0, 0])]);
    fixture.reader_task.abort();
    assert!((&mut fixture.reader_task).await.unwrap_err().is_cancelled());
    advance(1000).await;
    fixture.assert_wire(&[]);
    fixture.assert_no_events();
}

#[tokio::test(start_paused = true)]
async fn read_loop_propagates_input_writer_failure_without_completion_or_next_command() {
    let mut fixture = ReadLoopFixture::start(manual_plan()).await;
    fixture
        .commands
        .send(InjectionCommand::TouchTap { x: 2, y: 3 })
        .unwrap();
    fixture
        .commands
        .send(InjectionCommand::ButtonClick {
            button_id: 4,
            hold_ms: None,
        })
        .unwrap();
    settle().await;
    fixture.assert_wire(&[(crate::CHANNEL_TOUCH_EVENT, &[1, 2, 0, 3, 0, 0])]);
    fixture.writer.unbind().await;
    advance(250).await;
    let error = (&mut fixture.reader_task).await.unwrap().unwrap_err();
    assert_eq!(error.to_string(), "headless IPC writer is not connected");
    fixture.assert_wire(&[]);
    fixture.assert_no_events();
    assert!(fixture
        .commands
        .send(InjectionCommand::TouchTap { x: 1, y: 1 })
        .is_err());
}
