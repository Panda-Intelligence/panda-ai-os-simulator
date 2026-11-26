# Transport barrier for completed Gray4 presents

The CONTROL channel reserves device **0** for a transport endpoint. It has no
hardware or storage backend. Its only operation is the existing **QUERY (4)**.
This lets a runner fence completed framebuffer exports on Mofei as well as
boards that instantiate a LilyGo SPI-SD control backend.

## Wire contract

The request is exactly 10 bytes inside the existing CONTROL frame (channel
`0x06`, outer flags zero):

| Offset | Field | Required value |
| --- | --- | --- |
| 0 | Version | `1` |
| 1 | Operation | `4` (QUERY) |
| 2 | Device | `0` (reserved transport endpoint) |
| 3 | Flags | `0` |
| 4–7 | Request ID | Arbitrary little-endian `u32`, echoed exactly |
| 8 | Payload length | `0` |
| 9 | Reserved | `0` |

The normal 12-byte ACK has version `1`, message `0x80`, device `0`, accepted
`1`, the same request ID, and error `0`. The remaining bytes are zero. Other
operations, extra bytes, nonzero flags, a declared payload, or nonzero reserved
fields are rejected. Existing version and maximum-length checks still apply.
An unknown device remains unknown. A storage QUERY on Mofei still reports that
the LilyGo SPI-SD control backend is absent; transport support does not change
that result.

## Ordering contract

A completed Gray4 present first exports the real framebuffer synchronously.
The Xtensa translation hook at the firmware's
`pandaSimulatorPublishFramebuffer` return instruction calls
`mofei_inject_framebuffer` before returning to guest code. Its Gray4 path reads
the guest export and calls `ssd1677_publish_gray4_framebuffer`, which writes the
frame through the display IPC writer. Only after the firmware publish call
returns does the guest emit the `SIMGRAY4: event=present` serial log.

The headless runner observes that completed-present log and sends the transport
QUERY through the existing ordered CONTROL input. SSD1677 and UC8253C dispatch
it through the common peripheral dispatcher; its ACK uses the same display
output as framebuffer packets. The native writer submits a contiguous header
and payload in **one** `qemu_chr_fe_write_all` call. QEMU's chardev write mutex
covers that call. This matters because a TCG helper can run outside the big
QEMU lock while the main loop dispatches CONTROL input: two separate writes
could let an ACK appear inside a framebuffer packet. The barrier does not rely
on BQL ownership, an assumed accelerator mode, or a separate output queue.

On receipt, one headless socket reader decodes framebuffer and CONTROL packets
in stream order and sends both through the same event queue. Therefore, when
the matching accepted ACK is consumed, the earlier completed framebuffer has
already been processed. The runner still checks the input generation, current
activity, completed-present marker and Gray16 format. A NAK, malformed ACK,
wrong request/device, or missing matching frame cannot satisfy the fence. The
completion and ACK share the original step deadline.

The SSD1677 Wasm branch keeps its synchronous IPC bridge. The same common C
dispatcher handles the exported Wasm CONTROL entry point, and the ACK returns
through that bridge. No new channel or browser-specific operation is required.

This is a fence for a completed synchronous export. It is not a request to draw
a new frame, and does not claim that an asynchronous future render has finished.

## Focused regression coverage

`tests/transport-barrier.test.mjs` compiles the production dispatcher, real SD
handler, display input dispatch and output writer. It checks native SSD1677,
native UC8253C and the SSD1677 Wasm branch with a null LilyGo SPI-SD backend.
It checks request echoes, strict rejection, the unchanged storage NAK, and
frame/ACK order. A forced chardev-call interleaving inserts an ACK at a write
boundary; restoring the old split header/payload writes must corrupt that
boundary and fail the negative control. Native length overflow and short-write
paths are also exercised.

The Rust draw-readiness tests retain the delayed old Gray16 frame negative
control, binary socket FIFO coverage, strict ACK validation and shared deadline
checks. Their transport requests now target the reserved endpoint, and an
otherwise matching storage ACK is explicitly ignored.
