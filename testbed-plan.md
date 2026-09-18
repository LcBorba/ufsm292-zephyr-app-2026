# Testbed plan — radio link bring-up on two SAM R21s

Status: everything builds and is tested as far as possible without hardware.
Next step (needs real hardware): plug in 2 SAM R21 Xplained Pro boards and run
`./test-radio-link.sh` (add `--bulk` for lossless per-frame capture). See
"Resume checklist" at the bottom.

Related: `lib/radio/TX-PLAN.md` (sensor transmit path), `lib/radio/RX-PLAN.md`
(gateway receive path, incl. §6.4 RX activity LED).

---

## 1. What exists

| Piece | Location | Purpose |
|---|---|---|
| TX image | `radio-tx-testbed/` | Raw-mode 802.15.4 transmitter (samr21_xpro only) |
| RX image | `radio-rx-testbed/` | Raw-mode receiver / link verifier (samr21_xpro only) |
| Shared RX path | `lib/radio/radio_testbed.c`, `include/app/lib/radio_testbed.h` | Single `net_recv_data()`: classifies frames (ok/foreign/malformed), LED, counters, on-target link stats, per-frame ring buffer, `dump`/`reset` API |
| Codec | `lib/radio/sensor_frame.c` | Unchanged; shared 27-byte PSDU encode/decode, plus header-only parse for classification |
| Test pattern | `lib/radio/radio_test_pattern.c`, `include/app/lib/radio_test_pattern.h` | Deterministic seq-keyed payload both ends (and the monitor) can recompute |
| Bulk protocol | `radio-rx-testbed/src/main.c` | Console `dump`/`reset`/`stats` commands; `BULK*` log lines the monitor collects |
| Bench script | `test-radio-link.sh` (repo root, Linux only) | Build → detect → flash → compare (full-field match, PER/RSSI/LQI/latency summary, `--log-dir` artifacts: tx.log, rx.log, per-seq.csv, summary.txt) |

---

## 2. Decisions and reasoning

### 2.1 Both testbeds use raw mode (`CONFIG_IEEE802154_RAW_MODE`)

The gateway will eventually need a custom 802.15.4 L2 plus a ~4-line rf2xx
patch (`RX-PLAN.md` §1–2), neither of which exists yet. Raw mode needs no
patch and no L2: the app calls `radio_api->tx()` directly and the driver calls
the app's `net_recv_data()` back. That makes the testbeds buildable *now* and
isolates the RF question (channel, power, antennas, range) from the L2
question. The sensor image will also be raw-mode (`TX-PLAN.md` §3), so the TX
testbed is a faithful stand-in for it.

### 2.2 One `net_recv_data()` in `lib/radio`, not one per app

Raw mode shrinks the net stack to `net_pkt.c`, so *no* `net_recv_data()` exists
and every raw image must supply it — the TX-only image failed to link without
one. Rather than a drop-stub in TX and a real handler in RX, both link a single
shared implementation (`lib/radio/radio_testbed.c`): decode via
`sensor_frame_decode()`, LED0 toggle + log + RSSI/LQI on valid frames, counted
drop otherwise. It runs in the rf2xx RX thread, so it never blocks and always
unrefs the packet.

### 2.3 `radio_testbed.c` compiles only under `CONFIG_IEEE802154_RAW_MODE`

`gateway/` also sets `CONFIG_RADIO=y` but is *not* raw-mode: there the net stack
provides `net_recv_data()`, so compiling ours too would be a duplicate-symbol
link error. The `ifdef` keeps the gateway build clean (verified — it links).

### 2.4 LED0 blinks on valid sensor frames only, not on any RF energy

Per `RX-PLAN.md` §6.4: the blink means "decodable sensor data", which is what
you want to see when pointing boards at each other across a room. Uses the
`led0` alias from the base board DTS (no overlay nodes needed), guarded by
`DT_HAS_ALIAS`, configured at boot with fail-open lazy retry on first frame.

### 2.5 RX testbed is promiscuous; the production gateway will not be

The testbed accepts any on-channel frame (`IEEE802154_CONFIG_PROMISCUOUS`,
fail-open) so a channel/PAN typo on one end still shows *something* arriving —
much easier to debug than mutual silence. The gateway will use a PAN/short-addr
filter instead (`RX-PLAN.md` §4).

### 2.6 `CONFIG_TEST_RANDOM_GENERATOR=y` in both testbeds

The rf2xx driver calls `sys_rand_get()` (CSMA backoff) and the SAM R21 board
has no TRNG driver wired up, so both images use the test RNG — same as
`samples/net/wpan_serial`, which is the reference raw-mode app.

### 2.7 TX log prints the full reading, RX echoes it back field-for-field

`LOG_INF("tx node=… seq=… light=… temp_c_x100=… accel=… uptime=… flags=…")`.
Costs one log line per second and lets the monitor compare field-by-field
(node/seq/light/temp/**accel/flags**/uptime) rather than just counting seqs —
i.e. it verifies the codec round-trip over the air, not just that *something*
arrived. The RX line carries the same fields plus `rssi/lqi/rx_uptime/ok`, in
the same order, so TX↔RX diffs are mechanical. (An old-format RX line without
accel/flags still parses, degraded, with an `OLD-FMT` warning — reflash RX.)

### 2.8 `test-radio-link.sh` targets probes via `cmsis_dap_serial` pre-init

`west flash --serial/--dev-id` only sets `_ZEPHYR_BOARD_SERIAL`, which
samr21_xpro's `openocd.cfg` never reads — with two identical EDBG probes both
flashes would hit an arbitrary board. The script instead passes
`-- --cmd-pre-init "cmsis_dap_serial <SN>"`, which becomes `openocd -c …`
before `init`, the correct point to select a CMSIS-DAP probe.
Alternatively `--flasher edbg` skips OpenOCD entirely and programs via the
standalone `edbg` tool (`edbg -t samr21 -s <SN> -e -p -v -f zephyr.bin`),
whose `-s` selects the probe natively — faster and no pre-init hack, at the
cost of needing the `edbg` binary (not in apt; build from
https://github.com/ataradov/edbg). Probe↔console mapping, settle, and the
monitor are identical down either path.

### 2.9 Board ↔ console mapping via USB serials, `/dev/ttyACM*` as truth

Probe serials come from `lsusb -d 03eb:2111` + iSerial (no pyocd dependency);
console ports come from `/dev/ttyACM*` (per observation they always appear
there), correlated via `udevadm ID_SERIAL_SHORT`, with `/dev/serial/by-id` NOT
required. If correlation fails the script pairs by sorted order, says so loudly,
and asks for confirmation. `--tx-probe/--rx-probe/--tx-port/--rx-port/--swap`
override everything. After flashing it re-resolves ports by serial because USB
re-enumeration can renumber ttyACM devices.

### 2.12 On-target link stats survive host serial loss

A dropped/scrolled host log line must not destroy the run's accounting, so the
RX target keeps its own totals: `ok/dropped` (atomic counters) plus sequence
gap/duplicate counts and RSSI/LQI min/max/sum, exposed via
`radio_testbed_link_stats()` and printed every 5 s as a parseable `link …`
line that the monitor echoes and records. TX likewise counts `ok/busy/fail`
and prints `tx_stats …` every 10 attempts — the busy rate is the
channel-congestion signal. Decode failures log the frame length plus the first
16 bytes hex, so a silent link still shows *what* arrived (wrong PAN, foreign
traffic, unstripped FCS).

### 2.13 Every hardware run leaves artifacts (`--log-dir`)

Hardware time is the scarce resource: the monitor always prints the link
summary (PER, RSSI/LQI min/max/avg, latency min/max/avg, last on-target `link`
and `tx_stats` lines), and with `--log-dir DIR` it additionally saves raw
`tx.log`/`rx.log` transcripts, a `per-seq.csv` (seq, node, dt, rssi, lqi,
verdict OK/MISMATCH/LOST) and a `summary.txt` with date, git SHA and the
missing-seq list — enough to plot RSSI-vs-range or recompute PER offline.

### 2.10 Monitor matches by seq with a drain period, exits nonzero on loss

The embedded pyserial monitor keeps TX records keyed by app `seq`, reports
latency/RSSI/LQI/field-diffs live, then drains RX for 3 s after the window
closes (frames sent at the end still need air time) before declaring seqs lost.
Exit 0 ⇔ every sent seq received with matching fields. `--duration 0` runs
until Ctrl-C.

### 2.11 Testbeds are samr21-only on purpose

Only a `samr21_xpro.overlay` (empty — radio and LED come from the base DTS) and
`integration_platforms: [samr21_xpro]` exist. No other board can silently pick
these up; adding one is a deliberate act (new overlay + Kconfig check).

### 2.14 Foreign traffic is separated from link loss (`dropped` was a lie)

The RX is promiscuous, so a busy 2.4 GHz band delivers other people's
802.15.4 frames. Counting those as `dropped` conflated neighbour traffic with
corruption and made PER meaningless. Every frame is now classified:

- `ok` — decodes as a sensor frame **and** matches the configured filter
  (PAN `0xCAFE`, src short addr `1`); feeds link stats and LED0.
- `foreign` — a well-formed 802.15.4 data frame that is not ours (wrong PAN,
  wrong source, or non-sensor payload length); counted separately.
- `malformed` — not a supported data frame at all; counted separately.

`sensor_frame_parse_header()` (new) parses the MAC header without requiring an
18-byte payload, which is what lets us tell "valid frame from another PAN"
from "corrupt/non-data". `dropped = foreign + malformed` is kept so old
`radio_testbed_stats()` callers still work; the `link` line prints the split.

### 2.15 Payload is a deterministic seq-keyed pattern, checked independently

A constant payload cannot expose a stuck or mis-mapped field. The TX image
fills `light/temp/accel/flags` from `radio_test_pattern_fill()` keyed by the
application `seq` (`uptime_ms` stays the real sender clock). The Python monitor
mirrors the formula and checks every received payload against the value it
computes from `seq` alone — so a bug that TX and RX share is still caught. The
formula is pinned by golden unit tests (`tests/lib/radio`) and documented in
`radio_test_pattern.h`, which is the C/Python contract.

### 2.16 Per-frame ring buffer + bulk dump (beats the UART)

Per-frame log lines are the bottleneck at high rate and are lost on serial
glitches. The RX target now keeps the last `CONFIG_RADIO_TESTBED_RING_SIZE`
(128) frames in a lock-guarded ring: status, app `seq`, `node_id`, full payload,
RSSI/LQI and the receiver clock. The host sends `dump\n`; the firmware streams
`BULK_BEGIN total= count=`, one `BULK ...` line per record (oldest first) and
`BULK_END`, each followed by `log_flush()` so the 1 KB deferred log queue can
never overflow mid-dump. `total` is the cumulative append count, so the monitor
can dedupe overlapping dumps by global index and detect ring wrap
(`missing_indices`). `--bulk` makes the monitor dump every `--dump-interval`
seconds plus once at the end, verifies the pattern offline, computes PER from
sent-vs-ring, and writes `bulk.csv`. `CONFIG_RADIO_TESTBED_FRAME_LOG=n` turns
off the live lines for high-rate runs; bulk capture still works.

Every `BULK` line carries `idx=<n>`, the record's own global append index (what
`radio_testbed_ring_total()` was at that frame), rather than an implicit
position in the dump. Reception continues while a dump streams out, so a slot
that has not been sent yet can already be overwritten; keying on `idx` turns
that into a hole the monitor reports as `missing_indices` instead of filing the
record under a neighbour's sequence and silently corrupting PER.

### 2.17 `CONFIG_IEEE802154_L2_PKT_INCL_FCS=n`, explicitly

`CONFIG_IEEE802154_RAW_MODE` makes `IEEE802154_L2_PKT_INCL_FCS` **default to y**
(`drivers/ieee802154/Kconfig`), and `ieee802154_rf2xx.c` only strips the trailing
2-byte FCS when that symbol is `n`. Left at the default, `net_recv_data()` gets a
29-byte PSDU instead of 27, `sensor_frame_decode_meta()` sees a 20-byte payload
instead of 18 and rejects it, and — because the MAC header *does* parse and the
PAN/source match our filter — every frame is classified `malformed`: `ok=0`, LED0
dark, `PER=100%`, with nothing in the log to say why. Both `prj.conf`s therefore
set it to `n`, which is also what `sensor_frame.h` already documents as the codec's
contract ("the FCS is already removed"). TX is unaffected either way:
`rf2xx_iface_frame_write()` always tells the chip the frame is 2 bytes longer, so
the AT86RF233 appends the FCS in hardware.

The same pair of `prj.conf`s raises `CONFIG_IEEE802154_RF2XX_RX_STACK_SIZE` to
2048. `net_recv_data()` runs on the driver's RX thread and does decode, the
rejected-frame hexdump and log packaging there; `-fstack-usage` puts the deepest
path at ~750 B against the driver default of 800 B, and the unstripped FCS above
is exactly what used to force that path on frame one.

---

## 3. Verified without hardware

- `west build -b samr21_xpro radio-tx-testbed` links; ~14.3 KB SRAM / 32 KB.
- `west build -b samr21_xpro radio-rx-testbed` links; ~14.4 KB SRAM / 32 KB.
- `west build -b samr21_xpro gateway` still links (no duplicate `net_recv_data`).
- Codec unit tests: 23/23 pass (`west twister -T tests/lib/radio --tag unit`),
  including new header-parse/classification and test-pattern golden cases.
- Both testbeds and the gateway build for `samr21_xpro` (RX 19.0 KB RAM / 42.6 KB
  flash, TX 14.3 KB RAM / 40.8 KB flash, gateway unchanged semantics).
- Monitor logic end-to-end through PTYs with scripted console lines: clean link
  → `sent=5 received_ok=5`, exit 0; dropped frame + corrupted field →
  `lost=1 field_mismatch=1 expected_mismatch=1`, exit 1; `--bulk` with a
  corrupted ring record → `bulk expected_mismatch=1`, exit 1; a sent seq absent
  from the ring → `bulk_lost=1`, exit 1. Live and bulk paths both exercised.

## 4. NOT yet verified (needs hardware)

- `openocd` flashing at all (openocd is not installed on this machine).
- `cmsis_dap_serial` probe selection with two real EDBGs (correct per the
  openocd runner source, untested live).
- Port↔probe serial correlation against real EDBG iSerials/by-id names.
- Anything RF: actual TX→RX reception, RSSI plausibility, range.

---

## 5. Resume checklist (hardware session)

1. `sudo apt install openocd` (or build the `edbg` tool and pass
   `--flasher edbg`); confirm `lsusb -d 03eb:2111` shows two devices.
2. Plug in both boards, confirm two `/dev/ttyACM*` nodes.
3. `./test-radio-link.sh --duration 30 --log-dir logs/run1` (answer the mapping prompt; use `--yes`
   once trusted). Expect both LED0s blinking in step and `received_ok=N lost=0 PER=0.0%`.
   Add `--bulk` once the link works, to collect lossless per-frame records into
   `logs/run1/bulk.csv` (raise `--dump-interval` frequency / ring size when
   raising the TX rate).
4. If silent: check channel/PAN match (`TX_CHANNEL`/`RX_CHANNEL`, `TX_PAN_ID`
   in the two `src/main.c`s — the #1 suspect per `TX-PLAN.md` §9.8), USB serial
  mensaje mapping (`--tx-probe … --rx-probe …` to force it), then `-EBUSY` in the
   TX log (congested channel → try another from 11–26).
5. After the link is proven: implement `gateway_l2.c` + sensor table
   (`RX-PLAN.md` R0–R1), reusing the `radio_testbed.c` shape for the LED path,
   and re-run this bench with the real gateway image as the RX side.
