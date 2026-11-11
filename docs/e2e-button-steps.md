# E2E physical key steps

Headless E2E cases drive a board's physical keys with three step types. `press` and `hold` are explicit
spellings of the older `button` step. They run through the same path (capability check, injection, hold timing); the
runner only adds validation on load, and the result rows keep the step type written in the case file.

```json
{"type": "press", "button": "page_forward"}
{"type": "hold",  "button": "page_forward", "holdMs": 700}
{"type": "hold",  "button": "confirm", "hold_ms": 650, "holdUntilActivityChange": true}
{"type": "button", "button": "back", "holdMs": 0}
```

- `button` names a key by one of the board's names (below), case-insensitive.
- `press` is a tap and must not set a hold. `hold` needs `holdMs` (or `hold_ms`) greater than 0.
- Holds are timed in guest time: the press edge goes in, the firmware's own read of it (`button_read` in the QEMU
  log) is the acknowledgement, and the release follows `holdMs` of guest time later. On Mofei this is what makes the
  firmware's 600 ms first / 700 ms repeat hold handlers fire as on a device. `holdUntilActivityChange` keeps the key
  down past `holdMs` until the foreground activity changes (a long Confirm leaving the Reader).
- A key the board does not have fails the case with `unsupported simulator button '<name>' for board '<id>'` and a
  list of the supported names. A step also adds `button.<name>` to the case's required capabilities, so a board that
  lacks the key is reported as missing a capability. A key is never mapped to a different one.

## Button ids and names

The id is what travels through the QEMU queue and the IPC (the QEMU peripherals accept ids 0-7); the firmware maps it to a logical button in
`apps/panda-os/device/main/SimulatorButtonIds.h`.

| id | firmware button | Mofei and s3r8 names |
| --- | --- | --- |
| 0 | Back | `back`, `lock`, `esc` |
| 1 | Confirm | `confirm`, `key1`, `enter` |
| 2 | Left | `left`, `key2`, `next` |
| 3 | Right | `right` |
| 4 | Up | `up` |
| 5 | Down | `down` |
| 6 | Power | `power`, `pwr` |
| 7 | PageForward | `page_forward`, `pageforward`, `page-forward` |

`s3wroom` has only the labels of its key map (`esc`, `enter`, `next`) plus `power` and `page_forward`.

Mofei-class boards (`mofei`, `s3r8`, `s3wroom`) have the BoardConfig key map `{Power, Confirm, PageForward}`. Their
`back` name predates the `power` name and injects Back directly; `power` injects the Power button, which goes through
the firmware's sleep-hold input (short press replayed as Back, long press sleeps). The Studio key labelled `Next`
(boards.json id 2) still injects Left; it is not remapped to PageForward here.
