use super::*;

const FENCE_REQUEST_ID: u32 = 0x0123_4567;

fn observe(ctx: &mut E2EContext, line: &str) {
    ctx.push_line(line);
    ctx.parse_activity_transition(line);
}

fn reader_input() -> (Arc<tokio::sync::Mutex<E2EContext>>, u64) {
    let mut ctx = E2EContext::new();
    ctx.mark_input_activity_boundary();
    observe(&mut ctx, "UIREFRESH activity=Reader mode=fast");
    let generation = ctx.gray4_draw.frame_generation().load(Ordering::Acquire);
    (Arc::new(tokio::sync::Mutex::new(ctx)), generation)
}

fn frame(generation: u64, gray16: bool, pixel: u8) -> CapturedFramebuffer {
    CapturedFramebuffer {
        pixels: vec![pixel],
        format: if gray16 {
            FramebufferFormat::Gray16
        } else {
            FramebufferFormat::Mono1
        },
        draw_generation: generation,
    }
}

async fn complete_reader(ctx: &Arc<tokio::sync::Mutex<E2EContext>>) {
    observe(
        &mut *ctx.lock().await,
        "SIMGRAY4: event=present phase=direct activity=Reader",
    );
}

async fn fence_transport() -> (
    debug_transport::HeadlessDebugWriter,
    mpsc::UnboundedReceiver<debug_transport::OutFrame>,
) {
    let writer = debug_transport::HeadlessDebugWriter::new();
    let (tx, rx) = mpsc::unbounded_channel();
    writer.bind(tx).await;
    (writer, rx)
}

fn assert_query(query: &debug_transport::OutFrame) {
    assert_eq!(query.channel, CHANNEL_CONTROL);
    assert_eq!(query.flags, 0);
    assert_eq!(query.payload, [1, 4, 3, 0, 0x67, 0x45, 0x23, 0x01, 0, 0]);
}

async fn take_query(queries: &mut mpsc::UnboundedReceiver<debug_transport::OutFrame>) {
    let query = time::timeout(Duration::from_secs(2), queries.recv())
        .await
        .unwrap()
        .unwrap();
    assert_query(&query);
}

fn ack_payload() -> Vec<u8> {
    vec![1, 0x80, 3, 1, 0x67, 0x45, 0x23, 0x01, 0, 0, 0, 0]
}

async fn acknowledge(
    queries: &mut mpsc::UnboundedReceiver<debug_transport::OutFrame>,
    events: &mpsc::UnboundedSender<HeadlessEvent>,
) {
    take_query(queries).await;
    events
        .send(HeadlessEvent::PeripheralControl(ack_payload()))
        .unwrap();
}

async fn wait(
    events: &mut mpsc::UnboundedReceiver<HeadlessEvent>,
    frame: &mut Option<CapturedFramebuffer>,
    ctx: &Arc<tokio::sync::Mutex<E2EContext>>,
    writer: &debug_transport::HeadlessDebugWriter,
    timeout: Duration,
) -> Result<()> {
    wait_for_gray4_present(
        events,
        frame,
        ctx,
        "reader",
        writer,
        FENCE_REQUEST_ID,
        timeout,
    )
    .await
}

#[test]
fn gray4_wait_is_opt_in_and_keeps_region_thresholds() {
    let step: CaseStep = serde_json::from_str(
        r#"{"type":"assert_screenshot_region","activity":"reader","waitForGray4Present":true,
            "minDarkPixels":300,"minWhiteRatio":0.4}"#,
    )
    .unwrap();
    assert!(step.wait_for_gray4_present);
    assert_eq!(step.activity, "reader");
    assert_eq!(step.min_dark_pixels, Some(300));
    assert_eq!(step.min_white_ratio, Some(0.4));
    let ordinary: CaseStep =
        serde_json::from_str(r#"{"type":"assert_screenshot_region"}"#).unwrap();
    assert!(!ordinary.wait_for_gray4_present);
}

#[tokio::test(start_paused = true)]
async fn gray4_wait_delayed_present_replaces_the_old_one_shot_drain() {
    let (ctx, generation) = reader_input();
    let (event_tx, mut event_rx) = mpsc::unbounded_channel();
    let (writer, mut queries) = fence_transport().await;
    let mut last_fb = Some(frame(0, false, 0xff));
    let producer_ctx = ctx.clone();
    let producer = tokio::spawn(async move {
        time::sleep(Duration::from_millis(3000)).await;
        assert!(queries.try_recv().is_err());
        event_tx
            .send(HeadlessEvent::Frame(frame(generation, true, 0x55)))
            .unwrap();
        complete_reader(&producer_ctx).await;
        event_tx.send(HeadlessEvent::SerialLine).unwrap();
        acknowledge(&mut queries, &event_tx).await;
    });
    let started = time::Instant::now();
    wait(
        &mut event_rx,
        &mut last_fb,
        &ctx,
        &writer,
        Duration::from_secs(5),
    )
    .await
    .unwrap();
    assert!(started.elapsed() >= Duration::from_millis(3000));
    assert_eq!(last_fb, Some(frame(generation, true, 0x55)));
    producer.await.unwrap();
}

#[tokio::test(start_paused = true)]
async fn gray4_wait_preserves_early_completion_and_both_arrival_orders() {
    for order in ["frame-first", "completion-first", "already-complete"] {
        let (ctx, generation) = reader_input();
        let (event_tx, mut event_rx) = mpsc::unbounded_channel();
        let (writer, mut queries) = fence_transport().await;
        let mut last_fb = Some(frame(0, false, 0xff));
        if order != "completion-first" {
            event_tx
                .send(HeadlessEvent::Frame(frame(generation, true, 0x55)))
                .unwrap();
        }
        if order != "frame-first" {
            complete_reader(&ctx).await;
        }
        let producer_ctx = ctx.clone();
        let producer = tokio::spawn(async move {
            time::sleep(Duration::from_millis(50)).await;
            if order == "frame-first" {
                complete_reader(&producer_ctx).await;
                event_tx.send(HeadlessEvent::SerialLine).unwrap();
            } else if order == "completion-first" {
                event_tx
                    .send(HeadlessEvent::Frame(frame(generation, true, 0x55)))
                    .unwrap();
            }
            acknowledge(&mut queries, &event_tx).await;
        });
        wait(
            &mut event_rx,
            &mut last_fb,
            &ctx,
            &writer,
            Duration::from_secs(1),
        )
        .await
        .unwrap();
        assert_eq!(last_fb, Some(frame(generation, true, 0x55)));
        producer.await.unwrap();
    }
}

#[tokio::test(start_paused = true)]
async fn gray4_wait_rejects_old_input_frames_and_completion_before_reader() {
    for only_old_frame in [true, false] {
        let mut context = E2EContext::new();
        context.mark_input_activity_boundary();
        observe(&mut context, "UIREFRESH activity=FileBrowser mode=fast");
        let old_generation = context
            .gray4_draw
            .frame_generation()
            .load(Ordering::Acquire);
        context.mark_input_activity_boundary();
        observe(
            &mut context,
            "SIMGRAY4: event=present phase=direct activity=Reader",
        );
        observe(&mut context, "UIREFRESH activity=Reader mode=fast");
        let generation = context
            .gray4_draw
            .frame_generation()
            .load(Ordering::Acquire);
        let ctx = Arc::new(tokio::sync::Mutex::new(context));
        let (event_tx, mut event_rx) = mpsc::unbounded_channel();
        let (writer, mut queries) = fence_transport().await;
        let mut last_fb = Some(frame(
            if only_old_frame {
                old_generation
            } else {
                generation
            },
            true,
            0x55,
        ));
        let producer = if only_old_frame {
            complete_reader(&ctx).await;
            Some(tokio::spawn(async move {
                acknowledge(&mut queries, &event_tx).await;
            }))
        } else {
            None
        };
        let error = wait(
            &mut event_rx,
            &mut last_fb,
            &ctx,
            &writer,
            Duration::from_millis(100),
        )
        .await
        .unwrap_err();
        let detail = error.to_string();
        assert!(
            detail.contains(if only_old_frame {
                "without a matching"
            } else {
                "timed out"
            }),
            "{detail}"
        );
        assert!(detail.contains("Reader"), "{detail}");
        assert!(detail.contains("generation"), "{detail}");
        if let Some(producer) = producer {
            producer.await.unwrap();
        }
    }
}

#[tokio::test(start_paused = true)]
async fn gray4_wait_rejects_mono_no_present_and_wrong_activity() {
    for mode in ["mono", "no-present", "wrong-activity"] {
        let (ctx, generation) = reader_input();
        let (event_tx, mut event_rx) = mpsc::unbounded_channel();
        let (writer, mut queries) = fence_transport().await;
        let mut last_fb = Some(frame(generation, mode != "mono", 0x55));
        let producer = if mode == "wrong-activity" {
            observe(
                &mut *ctx.lock().await,
                "SIMGRAY4: event=present phase=direct activity=Dashboard",
            );
            None
        } else if mode == "mono" {
            complete_reader(&ctx).await;
            Some(tokio::spawn(async move {
                acknowledge(&mut queries, &event_tx).await;
            }))
        } else {
            None
        };
        let error = wait(
            &mut event_rx,
            &mut last_fb,
            &ctx,
            &writer,
            Duration::from_millis(100),
        )
        .await
        .unwrap_err();
        assert!(
            error.to_string().contains(if mode == "mono" {
                "without a matching"
            } else {
                "timed out"
            }),
            "{mode}: {error}"
        );
        if let Some(producer) = producer {
            producer.await.unwrap();
        }
    }
}

#[tokio::test(start_paused = true)]
async fn gray4_wait_reports_stream_close_and_invalid_contract() {
    let (ctx, generation) = reader_input();
    let (event_tx, mut event_rx) = mpsc::unbounded_channel();
    let (writer, _queries) = fence_transport().await;
    let mut last_fb = Some(frame(generation, false, 0xff));
    drop(event_tx);
    let error = wait(
        &mut event_rx,
        &mut last_fb,
        &ctx,
        &writer,
        Duration::from_secs(1),
    )
    .await
    .unwrap_err();
    assert!(error.to_string().contains("stream closed"));
    let error = wait_for_gray4_present(
        &mut event_rx,
        &mut last_fb,
        &ctx,
        "",
        &writer,
        FENCE_REQUEST_ID,
        Duration::from_secs(1),
    )
    .await
    .unwrap_err();
    assert!(error.to_string().contains("requires an activity"));
    let ctx = Arc::new(tokio::sync::Mutex::new(E2EContext::new()));
    let error = wait(
        &mut event_rx,
        &mut last_fb,
        &ctx,
        &writer,
        Duration::from_secs(1),
    )
    .await
    .unwrap_err();
    assert!(error.to_string().contains("requires a preceding input"));
}

#[tokio::test(start_paused = true)]
async fn gray4_wait_accepts_frame_before_activity_and_completion() {
    let mut context = E2EContext::new();
    context.mark_input_activity_boundary();
    let generation = context
        .gray4_draw
        .frame_generation()
        .load(Ordering::Acquire);
    let ctx = Arc::new(tokio::sync::Mutex::new(context));
    let (event_tx, mut event_rx) = mpsc::unbounded_channel();
    let (writer, mut queries) = fence_transport().await;
    event_tx
        .send(HeadlessEvent::Frame(frame(generation, true, 0x55)))
        .unwrap();
    let producer_ctx = ctx.clone();
    let producer = tokio::spawn(async move {
        time::sleep(Duration::from_millis(50)).await;
        observe(
            &mut *producer_ctx.lock().await,
            "UIREFRESH activity=Reader mode=fast",
        );
        complete_reader(&producer_ctx).await;
        event_tx.send(HeadlessEvent::SerialLine).unwrap();
        acknowledge(&mut queries, &event_tx).await;
    });
    let mut last_fb = None;
    wait(
        &mut event_rx,
        &mut last_fb,
        &ctx,
        &writer,
        Duration::from_secs(1),
    )
    .await
    .unwrap();
    assert_eq!(last_fb, Some(frame(generation, true, 0x55)));
    producer.await.unwrap();
}

#[tokio::test(start_paused = true)]
async fn gray4_wait_does_not_pair_a_delayed_old_gray16_with_reader_completion() {
    let (ctx, generation) = reader_input();
    complete_reader(&ctx).await;
    let (event_tx, mut event_rx) = mpsc::unbounded_channel();
    let (writer, mut queries) = fence_transport().await;
    event_tx
        .send(HeadlessEvent::Frame(frame(generation, true, 0xaa)))
        .unwrap();
    let producer = tokio::spawn(async move {
        take_query(&mut queries).await;
        time::sleep(Duration::from_millis(50)).await;
        event_tx
            .send(HeadlessEvent::Frame(frame(generation, true, 0x55)))
            .unwrap();
        event_tx
            .send(HeadlessEvent::PeripheralControl(ack_payload()))
            .unwrap();
    });
    let mut last_fb = None;
    wait(
        &mut event_rx,
        &mut last_fb,
        &ctx,
        &writer,
        Duration::from_secs(1),
    )
    .await
    .unwrap();
    assert_eq!(
        last_fb,
        Some(frame(generation, true, 0x55)),
        "a post-input header and Reader completion do not identify the same framebuffer"
    );
    producer.await.unwrap();
}

#[tokio::test(start_paused = true)]
async fn gray4_wait_ignores_wrong_binary_acks_and_requires_the_matching_ack() {
    let (ctx, generation) = reader_input();
    complete_reader(&ctx).await;
    let (event_tx, mut event_rx) = mpsc::unbounded_channel();
    let (writer, mut queries) = fence_transport().await;
    let producer = tokio::spawn(async move {
        take_query(&mut queries).await;
        for (offset, value) in [(0, 2), (1, 0x81), (2, 4), (4, 0x68)] {
            let mut wrong = ack_payload();
            wrong[offset] = value;
            event_tx
                .send(HeadlessEvent::PeripheralControl(wrong))
                .unwrap();
        }
        event_tx.send(HeadlessEvent::SerialLine).unwrap();
        time::sleep(Duration::from_millis(50)).await;
        event_tx
            .send(HeadlessEvent::Frame(frame(generation, true, 0x55)))
            .unwrap();
        event_tx
            .send(HeadlessEvent::PeripheralControl(ack_payload()))
            .unwrap();
    });
    let mut last_fb = Some(frame(generation, true, 0xaa));
    let started = time::Instant::now();
    wait(
        &mut event_rx,
        &mut last_fb,
        &ctx,
        &writer,
        Duration::from_secs(1),
    )
    .await
    .unwrap();
    assert!(started.elapsed() >= Duration::from_millis(50));
    assert_eq!(last_fb, Some(frame(generation, true, 0x55)));
    producer.await.unwrap();
}

#[tokio::test(start_paused = true)]
async fn gray4_wait_fails_on_rejected_or_malformed_ack_and_unavailable_writer() {
    for mode in ["rejected", "error", "malformed", "unavailable"] {
        let (ctx, generation) = reader_input();
        complete_reader(&ctx).await;
        let (event_tx, mut event_rx) = mpsc::unbounded_channel();
        let (writer, mut queries) = fence_transport().await;
        let mut last_fb = Some(frame(generation, true, 0x55));
        let producer = if mode == "unavailable" {
            writer.unbind().await;
            None
        } else {
            Some(tokio::spawn(async move {
                take_query(&mut queries).await;
                let mut ack = ack_payload();
                match mode {
                    "rejected" => ack[3] = 0,
                    "error" => ack[8] = 2,
                    "malformed" => ack.truncate(8),
                    _ => unreachable!(),
                }
                event_tx
                    .send(HeadlessEvent::PeripheralControl(ack))
                    .unwrap();
            }))
        };
        let error = wait(
            &mut event_rx,
            &mut last_fb,
            &ctx,
            &writer,
            Duration::from_millis(100),
        )
        .await
        .unwrap_err();
        assert!(
            error.to_string().contains(if mode == "unavailable" {
                "transport unavailable"
            } else {
                "fence rejected"
            }),
            "{mode}: {error}"
        );
        if let Some(producer) = producer {
            producer.await.unwrap();
        }
    }
}

#[tokio::test(start_paused = true)]
async fn gray4_wait_uses_one_deadline_for_completion_and_ack() {
    let (ctx, generation) = reader_input();
    let (event_tx, mut event_rx) = mpsc::unbounded_channel();
    let (writer, mut queries) = fence_transport().await;
    let producer_ctx = ctx.clone();
    let producer = tokio::spawn(async move {
        time::sleep(Duration::from_millis(80)).await;
        complete_reader(&producer_ctx).await;
        event_tx.send(HeadlessEvent::SerialLine).unwrap();
        take_query(&mut queries).await;
        time::sleep(Duration::from_millis(40)).await;
        event_tx
            .send(HeadlessEvent::PeripheralControl(ack_payload()))
            .unwrap();
    });
    let mut last_fb = Some(frame(generation, true, 0x55));
    let started = time::Instant::now();
    let error = wait(
        &mut event_rx,
        &mut last_fb,
        &ctx,
        &writer,
        Duration::from_millis(100),
    )
    .await
    .unwrap_err();
    assert!(error.to_string().contains("timed out"));
    assert_eq!(started.elapsed(), Duration::from_millis(100));
    producer.abort();
    assert!(producer.await.unwrap_err().is_cancelled());
}

#[tokio::test(start_paused = true)]
async fn gray4_wait_rechecks_activity_at_the_ack() {
    let (ctx, generation) = reader_input();
    complete_reader(&ctx).await;
    let (event_tx, mut event_rx) = mpsc::unbounded_channel();
    let (writer, mut queries) = fence_transport().await;
    let producer_ctx = ctx.clone();
    let producer = tokio::spawn(async move {
        take_query(&mut queries).await;
        observe(
            &mut *producer_ctx.lock().await,
            "UIREFRESH activity=FileBrowser mode=fast",
        );
        event_tx
            .send(HeadlessEvent::PeripheralControl(ack_payload()))
            .unwrap();
    });
    let mut last_fb = Some(frame(generation, true, 0x55));
    let error = wait(
        &mut event_rx,
        &mut last_fb,
        &ctx,
        &writer,
        Duration::from_millis(100),
    )
    .await
    .unwrap_err();
    assert!(error.to_string().contains("without a matching"));
    producer.await.unwrap();
}

fn packet(channel: u8, flags: u8, payload: &[u8]) -> Vec<u8> {
    let mut bytes = vec![channel, flags, 0, 0];
    bytes.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    bytes.extend_from_slice(payload);
    bytes
}

#[tokio::test]
async fn gray4_wait_receive_loop_fences_binary_frames_and_fails_on_a_short_ack() {
    for short_ack in [false, true] {
        let (ctx, generation) = reader_input();
        complete_reader(&ctx).await;
        let geometry = BoardGeometry {
            raw_width: 8,
            raw_height: 1,
            framebuffer_width: 8,
            framebuffer_height: 1,
            framebuffer_format: FramebufferFormat::Mono1,
            output_width: 8,
            output_height: 1,
        };
        let (host, mut peer) = tokio::net::UnixStream::pair().unwrap();
        let (read_half, write_half) = host.into_split();
        let (event_tx, mut event_rx) = mpsc::unbounded_channel();
        let (writer, outgoing) = fence_transport().await;
        let reader = tokio::spawn(receive_loop(
            read_half,
            event_tx,
            geometry,
            debug_transport::DebugTransportBroker::new(),
            None,
            None,
            ctx.lock().await.gray4_draw.frame_generation(),
        ));
        let sender = tokio::spawn(write_loop(write_half, outgoing));
        let producer = tokio::spawn(async move {
            let mut header = [0; HEADER_LEN];
            peer.read_exact(&mut header).await.unwrap();
            assert_eq!(&header[..4], &[CHANNEL_CONTROL, 0, 0, 0]);
            let len = u32::from_le_bytes(header[4..8].try_into().unwrap()) as usize;
            let mut query = vec![0; len];
            peer.read_exact(&mut query).await.unwrap();
            assert_query(&debug_transport::OutFrame {
                channel: header[0],
                flags: header[1],
                payload: query,
            });
            peer.write_all(&packet(CHANNEL_FRAMEBUFFER, FB_FLAG_GRAY16, &[0xaa; 4]))
                .await
                .unwrap();
            peer.write_all(&packet(CHANNEL_FRAMEBUFFER, FB_FLAG_GRAY16, &[0x55; 4]))
                .await
                .unwrap();
            let mut ack = packet(CHANNEL_CONTROL, 0, &ack_payload());
            if short_ack {
                ack.truncate(HEADER_LEN + 5);
            }
            peer.write_all(&ack).await.unwrap();
            if !short_ack {
                peer.write_all(&packet(CHANNEL_FRAMEBUFFER, FB_FLAG_GRAY16, &[0; 4]))
                    .await
                    .unwrap();
            }
            peer.shutdown().await.unwrap();
        });
        let mut last_fb = None;
        let result = wait(
            &mut event_rx,
            &mut last_fb,
            &ctx,
            &writer,
            Duration::from_secs(1),
        )
        .await;
        producer.await.unwrap();
        writer.unbind().await;
        sender.await.unwrap().unwrap();
        let received = reader.await.unwrap();
        if short_ack {
            assert!(received.is_err());
            assert!(result.unwrap_err().to_string().contains("stream closed"));
        } else {
            received.unwrap();
            result.unwrap();
            assert_eq!(
                last_fb,
                Some(CapturedFramebuffer {
                    pixels: vec![0x55; 4],
                    format: FramebufferFormat::Gray16,
                    draw_generation: generation,
                })
            );
            assert!(
                matches!(event_rx.recv().await, Some(HeadlessEvent::Frame(frame)) if frame.pixels == [0; 4])
            );
        }
    }
}
