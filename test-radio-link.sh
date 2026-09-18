#!/usr/bin/env bash
#
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0
#
# test-radio-link.sh — build, flash and compare the radio testbeds (Linux only).
#
# Builds radio-tx-testbed and radio-rx-testbed for samr21_xpro, checks that
# two SAM R21 Xplained Pro boards are plugged in (they show up as
# /dev/ttyACM*), flashes the TX image to one and the RX image to the other,
# then tails both serial consoles and compares what was sent with what was
# received (per-seq match, field match, loss report).
#
# Requirements: west, arm-none-eabi-gcc (sets GNUARMEMB_TOOLCHAIN_PATH from
# /usr if unset), a flasher (openocd via `west flash`, or the `edbg` tool with
# --flasher edbg), python3 + pyserial.
#
# Usage: ./test-radio-link.sh [options]
#   --tx-probe SN --rx-probe SN  EDBG USB serials to flash (auto-detected)
#   --tx-port DEV --rx-port DEV  serial ports to monitor (auto-detected)
#   --swap                       swap the automatic TX/RX board assignment
#   --yes                        skip the flash confirmation prompt
#   --no-build                   reuse the existing build dirs
#   --no-flash                   build + monitor only (boards already flashed)
#   --monitor-only               monitor only (implies --no-build --no-flash)
#   --build-only                 build both images and exit
#   --duration SECS              monitor window, 0 = until Ctrl-C (default 30)
#   --settle SECS                wait after flashing for USB to re-enumerate
#                                (default 8)
#   --baud RATE                  console baud rate (default 115200)
#   --flasher TOOL               openocd (default) or edbg. openocd goes
#                                through `west flash` with a cmsis_dap_serial
#                                pre-init to pick the probe; edbg talks to the
#                                EDBG directly (`edbg -t samr21 -s SN`) and
#                                selects the probe natively — faster and no
#                                pre-init hack, but needs the `edbg` binary
#                                (https://github.com/ataradov/edbg).
#   --log-dir DIR                save tx.log, rx.log, per-seq.csv and
#                                summary.txt there (default: none; hardware
#                                time is precious, every run should leave
#                                artifacts, so pass this on real runs)
#   --bulk                       pull the RX on-target ring buffer in bulk
#                                with periodic `dump` commands, verify the
#                                seq-keyed payload pattern and write bulk.csv
#   --dump-interval SECS         seconds between bulk dumps (default 2)
#   -h, --help                   this help
#
# Exit status: 0 when every transmitted seq was received with matching
# fields, 1 otherwise (or on any setup failure).

set -euo pipefail

BOARD=samr21_xpro
TX_APP=radio-tx-testbed
RX_APP=radio-rx-testbed
TX_BUILD=build-radio-tx
RX_BUILD=build-radio-rx
BAUD=115200
DURATION=30
SETTLE=8
DRAIN=3

TX_PROBE=""; RX_PROBE=""
TX_PORT=""; RX_PORT=""; LOGDIR=""; FLASHER=openocd
SWAP=0; YES=0; BULK=0; DUMP_PERIOD=2
DO_BUILD=1; DO_FLASH=1; DO_MONITOR=1

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \?//'; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tx-probe) TX_PROBE="$2"; shift 2 ;;
        --rx-probe) RX_PROBE="$2"; shift 2 ;;
        --tx-port)  TX_PORT="$2";  shift 2 ;;
        --rx-port)  RX_PORT="$2";  shift 2 ;;
        --swap)         SWAP=1;        shift ;;
        --yes)          YES=1;         shift ;;
        --no-build)     DO_BUILD=0;    shift ;;
        --no-flash)     DO_FLASH=0;    shift ;;
        --monitor-only) DO_BUILD=0; DO_FLASH=0; shift ;;
        --build-only)   DO_BUILD=1; DO_FLASH=0; DO_MONITOR=0; shift ;;
        --duration) DURATION="$2"; shift 2 ;;
        --settle)   SETTLE="$2";   shift 2 ;;
        --baud)     BAUD="$2";     shift 2 ;;
        --flasher)  FLASHER="$2";  shift 2 ;;
        --log-dir)  LOGDIR="$2";   shift 2 ;;
        --bulk)         BULK=1;         shift ;;
        --dump-interval) DUMP_PERIOD="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option $1 (see --help)" >&2; exit 2 ;;
    esac
done

cd "$(dirname "$0")"

# Project-local binaries (e.g. an `edbg` drop-in for --flasher edbg).
# Prepended so local-tools/ wins over the system PATH without sudo/install.
export PATH="$PWD/local-tools:$PATH"

die() { echo "error: $*" >&2; exit 1; }
log() { echo "==> $*"; }

need() { command -v "$1" >/dev/null 2>&1 || die "missing '$1' ($2)"; }

# ---------------------------------------------------------------- toolchain --
need west "install the Zephyr SDK environment first"
need python3 "python3 is required for the serial monitor"
python3 -c "import serial" 2>/dev/null \
    || die "python3 module 'serial' not found (pip install pyserial)"
[[ $FLASHER == openocd || $FLASHER == edbg ]] \
    || die "--flasher must be openocd or edbg (got '$FLASHER')"
if [[ $DO_FLASH == 1 ]]; then
    if [[ $FLASHER == edbg ]]; then
        need edbg "drop an edbg binary in ./local-tools/ (see local-tools/README.md) or use --flasher openocd"
    else
        need openocd "sudo apt install openocd (or use --flasher edbg)"
    fi
fi

if [[ -z "${GNUARMEMB_TOOLCHAIN_PATH:-}" ]]; then
    if [[ -x /usr/bin/arm-none-eabi-gcc ]]; then
        export GNUARMEMB_TOOLCHAIN_PATH=/usr
        log "GNUARMEMB_TOOLCHAIN_PATH defaulted to /usr"
    else
        die "GNUARMEMB_TOOLCHAIN_PATH is not set and no arm-none-eabi-gcc in /usr"
    fi
fi

# ------------------------------------------------------------------- build --
if [[ $DO_BUILD == 1 ]]; then
    log "building $TX_APP for $BOARD"
    west build -b "$BOARD" --build-dir "$TX_BUILD" "$TX_APP"
    log "building $RX_APP for $BOARD"
    west build -b "$BOARD" --build-dir "$RX_BUILD" "$RX_APP"
fi
[[ $DO_MONITOR == 0 ]] && { log "build-only, exiting"; exit 0; }

# ------------------------------------------------- detect boards and ports --
# EDBG (the Xplained Pro debugger) is VID 03eb / PID 2111; its USB iSerial is
# the unit serial used both for openocd probe selection and for matching the
# /dev/ttyACM* console to its debugger.

edbg_serials() {
    local line busdev serial
    lsusb -d 03eb:2111 2>/dev/null | sed -n 's/^Bus \([0-9]*\) Device \([0-9]*\):.*/\1:\2/p' \
    | while read -r busdev; do
        serial=$(lsusb -v -s "$busdev" 2>/dev/null | awk '/iSerial/{print $3; exit}')
        [[ -n "$serial" ]] && echo "$serial"
    done | sort -u
}

acm_ports() {
    local p
    for p in /dev/ttyACM* /dev/ttyUSB*; do
        [[ -e "$p" ]] && echo "$p"
    done 2>/dev/null | sort -u
}

# USB serial of the device behind a tty (empty when udev has nothing).
port_serial() {
    udevadm info -q property -n "$1" 2>/dev/null \
        | sed -n 's/^ID_SERIAL_SHORT=//p' | head -n 1
}

# Resolve a debugger serial back to its current console port (ports can move
# across re-enumeration after flashing).
port_for_serial() {
    local want="$1" p s
    while read -r p; do
        s=$(port_serial "$p")
        [[ "$s" == "$want" ]] && { echo "$p"; return 0; }
    done < <(acm_ports)
    return 1
}

# Flash one image with the standalone `edbg` tool (ataradov/edbg):
# erase + program + verify, then the target resets. Probe selection is
# native (`-s SN`), unlike the openocd path below. Prefers zephyr.bin,
# falls back to .hex/.elf (edbg reads all three).
flash_edbg() {
    local build="$1" sn="$2" img=""
    local cand
    for cand in "$build/zephyr/zephyr.bin" "$build/zephyr/zephyr.hex" \
                "$build/zephyr/zephyr.elf"; do
        if [[ -f "$cand" ]]; then
            img="$cand"
            break
        fi
    done
    [[ -n "$img" ]] || die "no image in $build/zephyr (run a build first)"
    edbg -t samr21 -s "$sn" -e -p -v -f "$img" \
        || die "edbg flash failed for probe $sn"
}

if [[ -z "$TX_PORT$RX_PORT$TX_PROBE$RX_PROBE" ]]; then
    # Full auto-detection: need exactly 2 EDBG probes and 2 console ports.
    mapfile -t PROBES < <(edbg_serials)
    mapfile -t PORTS < <(acm_ports)
    [[ ${#PROBES[@]} -eq 2 ]] \
        || die "need 2 SAM R21 (EDBG 03eb:2111) debuggers, found ${#PROBES[@]}"
    [[ ${#PORTS[@]} -eq 2 ]] \
        || die "need 2 console ports (/dev/ttyACM*), found ${#PORTS[@]}: ${PORTS[*]:-<none>}"

    # Pair each port with its debugger via the USB serial. Fall back to
    # sorted-order pairing when udev reports nothing.
    PAIR_OK=1
    declare -A PORT2PROBE=()
    for p in "${PORTS[@]}"; do
        s=$(port_serial "$p")
        if [[ -n "$s" ]] && printf '%s\n' "${PROBES[@]}" | grep -qx "$s"; then
            PORT2PROBE["$p"]="$s"
        else
            PAIR_OK=0
        fi
    done

    if [[ $PAIR_OK == 1 ]]; then
        TX_PORT="${PORTS[0]}"; RX_PORT="${PORTS[1]}"
        TX_PROBE="${PORT2PROBE[$TX_PORT]}"; RX_PROBE="${PORT2PROBE[$RX_PORT]}"
        log "paired by USB serial: $TX_PORT<->${TX_PROBE}, $RX_PORT<->${RX_PROBE}"
    else
        log "WARNING: cannot correlate ports to debuggers, pairing by sorted order"
        TX_PORT="${PORTS[0]}"; RX_PORT="${PORTS[1]}"
        TX_PROBE="${PROBES[0]}"; RX_PROBE="${PROBES[1]}"
    fi

    if [[ $SWAP == 1 ]]; then
        p="$TX_PORT"; TX_PORT="$RX_PORT"; RX_PORT="$p"
        p="$TX_PROBE"; TX_PROBE="$RX_PROBE"; RX_PROBE="$p"
    fi
else
    # Manual overrides: fill in whatever is missing, same pairing as above.
    if [[ -z "$TX_PORT" || -z "$RX_PORT" ]]; then
        mapfile -t PORTS < <(acm_ports)
        [[ ${#PORTS[@]} -eq 2 ]] || die "need --tx-port/--rx-port (found ${#PORTS[@]} ports)"
        [[ -z "$TX_PORT" ]] && TX_PORT="${PORTS[0]}"
        [[ -z "$RX_PORT" ]] && RX_PORT="${PORTS[1]}"
        [[ "$TX_PORT" == "$RX_PORT" ]] && die "--tx-port and --rx-port must differ"
    fi
    if [[ $DO_FLASH == 1 && (-z "$TX_PROBE" || -z "$RX_PROBE") ]]; then
        mapfile -t PROBES < <(edbg_serials)
        TS=$(port_serial "$TX_PORT"); RS=$(port_serial "$RX_PORT")
        [[ -z "$TX_PROBE" ]] && TX_PROBE="$TS"
        [[ -z "$RX_PROBE" ]] && RX_PROBE="$RS"
        if [[ -z "$TX_PROBE" || -z "$RX_PROBE" ]]; then
            [[ ${#PROBES[@]} -eq 2 ]] \
                && die "cannot map ports to debuggers, pass --tx-probe/--rx-probe explicitly"
            die "need --tx-probe/--rx-probe (found ${#PROBES[@]} debuggers)"
        fi
    fi
fi

[[ -e "$TX_PORT" ]] || die "TX port $TX_PORT does not exist"
[[ -e "$RX_PORT" ]] || die "RX port $RX_PORT does not exist"
[[ "$TX_PORT" != "$RX_PORT" ]] || die "TX and RX ports are the same device"

# ------------------------------------------------------------------- flash --
if [[ $DO_FLASH == 1 ]]; then
    [[ -n "$TX_PROBE" && -n "$RX_PROBE" ]] || die "need --tx-probe/--rx-probe"
    [[ "$TX_PROBE" != "$RX_PROBE" ]] || die "TX and RX probes are the same unit"
    echo "  TX image $TX_APP -> probe $TX_PROBE (console $TX_PORT)"
    echo "  RX image $RX_APP -> probe $RX_PROBE (console $RX_PORT)"
    if [[ $YES == 0 ]]; then
        read -rp "Flash both boards? [Y/n] " ans
        [[ "${ans:-Y}" =~ ^[Yy]$ ]] || die "aborted"
    fi
    # openocd path: the board openocd.cfg ignores _ZEPHYR_BOARD_SERIAL, so
    # select the CMSIS-DAP probe explicitly with a pre-init command (runs
    # before init). edbg path: probe selection is native (`-s SN`), no hack.
    if [[ $FLASHER == edbg ]]; then
        log "flashing $TX_APP to $TX_PROBE (edbg)"
        flash_edbg "$TX_BUILD" "$TX_PROBE"
        log "flashing $RX_APP to $RX_PROBE (edbg)"
        flash_edbg "$RX_BUILD" "$RX_PROBE"
    else
        log "flashing $TX_APP to $TX_PROBE"
        west flash --build-dir "$TX_BUILD" -- \
            --cmd-pre-init "cmsis_dap_serial $TX_PROBE"
        log "flashing $RX_APP to $RX_PROBE"
        west flash --build-dir "$RX_BUILD" -- \
            --cmd-pre-init "cmsis_dap_serial $RX_PROBE"
    fi

    # Flashing resets the boards and USB re-enumerates; re-resolve the
    # consoles by debugger serial in case the ttyACM numbers moved.
    log "waiting ${SETTLE}s for USB re-enumeration"
    sleep "$SETTLE"
    if [[ -n "${TX_PROBE:-}" ]]; then
        TX_PORT=$(port_for_serial "$TX_PROBE" || echo "$TX_PORT")
        RX_PORT=$(port_for_serial "$RX_PROBE" || echo "$RX_PORT")
    fi
    log "monitoring TX=$TX_PORT RX=$RX_PORT"
fi

# ----------------------------------------------------------------- monitor --
[[ $DURATION =~ ^[0-9]+$ ]] || die "--duration must be a non-negative integer"
[[ $DUMP_PERIOD =~ ^[0-9]+([.][0-9]+)?$ ]] || die "--dump-interval must be a positive number"

log "comparing TX=$TX_PORT against RX=$RX_PORT for ${DURATION}s (baud $BAUD)"
[[ -n "$LOGDIR" ]] && { mkdir -p "$LOGDIR"; log "artifacts -> $LOGDIR/"; }
python3 - "$TX_PORT" "$RX_PORT" "$BAUD" "$DURATION" "$DRAIN" "${LOGDIR:-}" "$BULK" "$DUMP_PERIOD" <<'EOF'
import csv, datetime, os, re, sys, time
import serial

tx_port, rx_port, baud, duration, drain = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
logdir = sys.argv[6] if len(sys.argv) > 6 else ''
bulk = int(sys.argv[7]) if len(sys.argv) > 7 else 0
dump_period = float(sys.argv[8]) if len(sys.argv) > 8 else 2.0

# Deterministic payload pattern, mirroring radio_test_pattern_fill() in
# lib/radio/radio_test_pattern.c (pinned by the host unit tests). The monitor
# recomputes the expected fields from the seq alone, so a stuck or mis-mapped
# field fails the run even if TX and RX happen to agree with each other.
def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v

def pattern(seq, node):
    s = seq & 0xFFFF
    return {
        'node': node,
        'seq': seq,
        'light': (s * 40503 + 12345) & 0xFFFF,
        'temp': 1800 + (s % 600),
        'ax': s16(s * 3 + 40000),
        'ay': s16(s * 5 + 30000),
        'az': s16(s * 7 + 20000),
        'flags': (s ^ (s >> 8)) & 0xFF,
    }

PATTERN_KEYS = ('light', 'temp', 'ax', 'ay', 'az', 'flags')

# Full-field patterns: every payload byte the TX side emits must come back
# on the RX line, so the monitor verifies the whole codec round-trip
# (accel + flags included), not just the header-adjacent fields.
TX_RE = re.compile(r'tx node=(\d+) seq=(\d+) light=(\d+) temp_c_x100=(-?\d+) '
                   r'accel=(-?\d+),(-?\d+),(-?\d+) uptime=(\d+) flags=(\d+) mac_seq=(\d+)')
RX_RE = re.compile(r'rx node=(\d+) seq=(\d+) light=(\d+) temp_c_x100=(-?\d+) '
                   r'accel=(-?\d+),(-?\d+),(-?\d+) uptime=(\d+) flags=(\d+) '
                   r'rssi=(-?\d+) lqi=(\d+)')
RX_OLD_RE = re.compile(r'rx node=(\d+) seq=(\d+) light=(\d+) temp_c_x100=(-?\d+) '
                        r'uptime=(\d+) rssi=(-?\d+) lqi=(\d+)')
LINK_RE = re.compile(r'link ok=(?P<ok>\d+) dropped=(?P<dropped>\d+) '
                      r'foreign=(?P<foreign>\d+) malformed=(?P<malformed>\d+) '
                      r'gaps=(?P<gaps>\d+) dups=(?P<dups>\d+) last_seq=(?P<last_seq>\d+) '
                      r'rssi_min=(?P<rssi_min>-?\d+) rssi_max=(?P<rssi_max>-?\d+) rssi_avg=(?P<rssi_avg>-?\d+) '
                      r'lqi_min=(?P<lqi_min>\d+) lqi_max=(?P<lqi_max>\d+) lqi_avg=(?P<lqi_avg>\d+)')
TX_STATS_RE = re.compile(r'tx_stats ok=(\d+) busy=(\d+) fail=(\d+)')
BULK_BEGIN_RE = re.compile(r'BULK_BEGIN total=(\d+) count=(\d+)')
BULK_END_RE = re.compile(r'BULK_END total=(\d+) count=(\d+)')
# `idx` is the record's own global append index (new firmware). Older firmware
# omits it and the host falls back to positional indexing within the dump.
BULK_RE = re.compile(r'BULK (?:idx=(\d+) )?seq=(\d+) node=(\d+) status=(\d+) rssi=(-?\d+) lqi=(\d+) '
                     r'light=(\d+) temp=(-?\d+) ax=(-?\d+) ay=(-?\d+) az=(-?\d+) '
                     r'flags=(\d+) uptime=(\d+)')
BULK_KEYS = ('idx', 'seq', 'node', 'status', 'rssi', 'lqi', 'light', 'temp',
             'ax', 'ay', 'az', 'flags', 'uptime')
CMP = ('node', 'seq', 'light', 'temp', 'ax', 'ay', 'az', 'uptime', 'flags')

tx, sent_all, rx_orphan, mismatch, expected_mismatch = {}, {}, 0, 0, 0
matched = loss = 0
rssi_vals, lqi_vals, dt_vals = [], [], []
rows = []  # per-seq CSV: seq, node, dt_s, rssi, lqi, verdict, expected
last_link = None
last_tx_stats = None
bulk_buf = []          # records of the dump currently arriving
bulk_records = {}      # global ring index -> record dict, deduped across dumps
bulk_dumps = 0
ftx_log = frx_log = None
if logdir:
    os.makedirs(logdir, exist_ok=True)
    ftx_log = open(os.path.join(logdir, 'tx.log'), 'w', buffering=1)
    frx_log = open(os.path.join(logdir, 'rx.log'), 'w', buffering=1)

def stats(v):
    return f'min={min(v)} max={max(v)} avg={sum(v)/len(v):.1f} n={len(v)}' if v else 'n=0'

def open_port(dev):
    try:
        return serial.Serial(dev, baud, timeout=0.1)
    except serial.SerialException as e:
        print(f'error: cannot open {dev}: {e}', file=sys.stderr)
        sys.exit(2)

ftx, frx = open_port(tx_port), open_port(rx_port)
buf = {ftx: b'', frx: b''}
t0 = time.monotonic()

def elapsed():
    return time.monotonic() - t0

def handle(line, src):
    global matched, loss, rx_orphan, mismatch, expected_mismatch
    global last_link, last_tx_stats, bulk_buf, bulk_records, bulk_dumps
    if ftx_log is not None and src == 'TX':
        ftx_log.write(line + '\n')
    if frx_log is not None and src == 'RX':
        frx_log.write(line + '\n')
    m = LINK_RE.search(line)
    if m:
        last_link = m.groupdict()
        print(f'[RX-LINK {elapsed():7.1f}s] {line.strip()}', flush=True)
        return
    m = TX_STATS_RE.search(line)
    if m:
        last_tx_stats = tuple(map(int, m.groups()))
        print(f'[TX-STATS {elapsed():7.1f}s] {line.strip()}', flush=True)
        return
    if src == 'TX':
        m = TX_RE.search(line)
        if m:
            d = dict(zip(('node', 'seq', 'light', 'temp', 'ax', 'ay', 'az',
                           'uptime', 'flags', 'mac_seq'), map(int, m.groups())))
            tx[d['seq']] = (elapsed(), d)
            sent_all[d['seq']] = d
            print(f'[TX {elapsed():7.1f}s] seq={d["seq"]} node={d["node"]} uptime={d["uptime"]}', flush=True)
        return

    # RX stream: bulk dumps first, then the per-frame (live) lines.
    m = BULK_BEGIN_RE.search(line)
    if m:
        bulk_buf = []
        return
    m = BULK_RE.search(line)
    if m:
        g = list(m.groups())
        rec = dict(zip(BULK_KEYS[1:], map(int, g[1:])))
        rec['idx'] = int(g[0]) if g[0] is not None else None
        bulk_buf.append(rec)
        return
    m = BULK_END_RE.search(line)
    if m:
        total, count = map(int, m.groups())
        base = total - count
        for i, rec in enumerate(bulk_buf):
            # Prefer the record's own global index over its position in this
            # dump. A slot overwritten while the dump streamed out then shows
            # up as a hole in the index sequence (missing_indices) instead of
            # being filed under a neighbour's index and corrupting PER.
            idx = rec['idx'] if rec['idx'] is not None else base + i
            bulk_records[idx] = rec
        bulk_dumps += 1
        print(f'[RX-BULK {elapsed():7.1f}s] dump {bulk_dumps}: total={total} count={count}', flush=True)
        bulk_buf = []
        return

    m = RX_RE.search(line)
    if m:
        d = dict(zip(('node', 'seq', 'light', 'temp', 'ax', 'ay', 'az',
                       'uptime', 'flags', 'rssi', 'lqi'),
                     map(int, m.groups())))
    else:
        m = RX_OLD_RE.search(line)  # pre-upgrade RX firmware: no accel/flags
        if not m:
            return
        d = dict(zip(('node', 'seq', 'light', 'temp', 'uptime', 'rssi', 'lqi'),
                     map(int, m.groups())))
        d.update({'ax': None, 'ay': None, 'az': None, 'flags': None})
        print(f'[RX {elapsed():7.1f}s] seq={d["seq"]} OLD-FMT(no accel/flags, reflash RX)', flush=True)

    # Independent expected-payload check from the seq alone.
    exp_verdict = '-'
    if d.get('ax') is not None:
        exp = pattern(d['seq'], d['node'])
        exp_diffs = [f'{k} got={d[k]} want={exp[k]}' for k in PATTERN_KEYS
                     if exp[k] != d[k]]
        exp_verdict = 'OK' if not exp_diffs else 'DIFF'
        if exp_diffs:
            expected_mismatch += 1
            print(f'[RX {elapsed():7.1f}s] seq={d["seq"]} EXPECTED=DIFF({", ".join(exp_diffs)})', flush=True)

    if d['seq'] in tx:
        t_tx, sent = tx.pop(d['seq'])
        dt = elapsed() - t_tx
        keys = [k for k in CMP if d[k] is not None]
        diffs = [f'{k} tx={sent[k]} rx={d[k]}' for k in keys if sent[k] != d[k]]
        rssi_vals.append(d['rssi']); lqi_vals.append(d['lqi']); dt_vals.append(dt)
        if diffs:
            mismatch += 1
            rows.append((d['seq'], d['node'], dt, d['rssi'], d['lqi'], 'MISMATCH', exp_verdict))
            print(f'[RX {elapsed():7.1f}s] seq={d["seq"]} FIELDS=DIFF({", ".join(diffs)})', flush=True)
        else:
            matched += 1
            rows.append((d['seq'], d['node'], dt, d['rssi'], d['lqi'], 'OK', exp_verdict))
            print(f'[RX {elapsed():7.1f}s] seq={d["seq"]} node={d["node"]} '
                  f'rssi={d["rssi"]} lqi={d["lqi"]} dt={dt:.2f}s FIELDS=OK', flush=True)
    else:
        rx_orphan += 1
        print(f'[RX {elapsed():7.1f}s] seq={d["seq"]} (no TX record, monitor started late?)', flush=True)

next_dump = t0 + dump_period
try:
    while duration == 0 or elapsed() < duration:
        for f, src in ((ftx, 'TX'), (frx, 'RX')):
            chunk = f.read(256)
            if chunk:
                buf[f] += chunk
                while b'\n' in buf[f]:
                    line, buf[f] = buf[f].split(b'\n', 1)
                    handle(line.decode(errors='replace'), src)
        if bulk and time.monotonic() >= next_dump:
            frx.write(b'dump\n')
            next_dump = time.monotonic() + dump_period
    if bulk:
        # Final snapshot so the tail of the window is included.
        frx.write(b'dump\n')
    # Drain: frames sent at the end of the window still need air time (and a
    # final bulk dump needs time to stream out).
    t_end = time.monotonic()
    while time.monotonic() - t_end < drain:
        chunk = frx.read(256)
        if chunk:
            buf[frx] += chunk
            while b'\n' in buf[frx]:
                line, buf[frx] = buf[frx].split(b'\n', 1)
                handle(line.decode(errors='replace'), 'RX')
except KeyboardInterrupt:
    print('\ninterrupted, summarizing...', flush=True)

loss = len(tx)
total = matched + mismatch + loss
per = (100.0 * loss / total) if total else 0.0

# Bulk ring statistics, deduped across dumps by global ring index.
bulk_ok = bulk_foreign = bulk_malformed = 0
bulk_expected_mismatch = 0
bulk_expected_details = []
bulk_ok_seqs = set()
for idx in sorted(bulk_records):
    r = bulk_records[idx]
    if r['status'] == 0:
        bulk_ok += 1
        bulk_ok_seqs.add(r['seq'])
        exp = pattern(r['seq'], r['node'])
        bad = [k for k in PATTERN_KEYS if exp[k] != r[k]]
        if bad:
            bulk_expected_mismatch += 1
            if len(bulk_expected_details) < 20:
                bulk_expected_details.append((r['seq'], bad))
    elif r['status'] == 1:
        bulk_foreign += 1
    else:
        bulk_malformed += 1

# Missing ring indices between dumps: the ring lapped the reader.
bulk_missing = 0
if bulk_records:
    lo, hi = min(bulk_records), max(bulk_records)
    bulk_missing = (hi - lo + 1) - len(bulk_records)

# Bulk loss: seqs the TX side sent inside the received range that the ring
# never saw. This is the PER path that still works with per-frame logging off.
bulk_lost = []
if bulk and bulk_ok_seqs:
    lo_s, hi_s = min(bulk_ok_seqs), max(bulk_ok_seqs)
    bulk_lost = sorted(s for s in sent_all
                       if lo_s <= s <= hi_s and s not in bulk_ok_seqs)
bulk_denom = len(bulk_lost) + bulk_ok
bulk_per = (100.0 * len(bulk_lost) / bulk_denom) if bulk_denom else 0.0

print(f'--- sent={total} received_ok={matched} field_mismatch={mismatch} '
      f'expected_mismatch={expected_mismatch} lost={loss} PER={per:.1f}% '
      f'rx_without_tx_record={rx_orphan} ---', flush=True)
print(f'--- rssi[dBm]: {stats(rssi_vals)} | lqi: {stats(lqi_vals)} | latency[s]: {stats([round(v, 3) for v in dt_vals])} ---', flush=True)
if last_link is not None:
    L = last_link
    print(f"--- target link: ok={L['ok']} dropped={L['dropped']} "
          f"foreign={L['foreign']} malformed={L['malformed']} "
          f"gaps={L['gaps']} dups={L['dups']} last_seq={L['last_seq']} "
          f"rssi={L['rssi_min']}..{L['rssi_max']} avg {L['rssi_avg']} "
          f"lqi={L['lqi_min']}..{L['lqi_max']} avg {L['lqi_avg']} ---", flush=True)
if last_tx_stats is not None:
    print(f'--- target tx stats: ok={last_tx_stats[0]} busy={last_tx_stats[1]} fail={last_tx_stats[2]} ---', flush=True)
if bulk:
    print(f'--- bulk: dumps={bulk_dumps} records={len(bulk_records)} ok={bulk_ok} '
          f'foreign={bulk_foreign} malformed={bulk_malformed} '
          f'expected_mismatch={bulk_expected_mismatch} lost={len(bulk_lost)} '
          f'PER={bulk_per:.1f}% missing_indices={bulk_missing} ---', flush=True)
    for seq, bad in bulk_expected_details:
        print(f'    bulk seq={seq} EXPECTED=DIFF({", ".join(bad)})', flush=True)
    if bulk_lost:
        print(f'    bulk missing seqs: {bulk_lost[:20]}', flush=True)
    if not bulk_records:
        print('    no bulk records: is RX running new-enough firmware (dump cmd)?', flush=True)
if tx:
    print(f'missing seqs: {sorted(tx)[:20]}', flush=True)
    for s in sorted(tx):
        rows.append((s, tx[s][1]['node'], None, None, None, 'LOST', '-'))

if logdir:
    import subprocess
    try:
        sha = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'],
                             capture_output=True, text=True).stdout.strip()
    except Exception:
        sha = 'unknown'
    with open(os.path.join(logdir, 'per-seq.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['seq', 'node', 'dt_s', 'rssi_dbm', 'lqi', 'verdict', 'expected'])
        w.writerows(sorted(rows))
    if bulk:
        with open(os.path.join(logdir, 'bulk.csv'), 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['index', 'status', 'seq', 'node', 'rssi_dbm', 'lqi',
                        'light', 'temp_c_x100', 'ax', 'ay', 'az', 'flags',
                        'uptime_ms', 'expected'])
            for idx in sorted(bulk_records):
                r = bulk_records[idx]
                exp = ''
                if r['status'] == 0:
                    exp = 'OK' if not [k for k in PATTERN_KEYS
                                       if pattern(r['seq'], r['node'])[k] != r[k]] else 'DIFF'
                w.writerow([idx, r['status'], r['seq'], r['node'], r['rssi'],
                            r['lqi'], r['light'], r['temp'], r['ax'], r['ay'],
                            r['az'], r['flags'], r['uptime'], exp])
    with open(os.path.join(logdir, 'summary.txt'), 'w') as f:
        f.write(f'date={datetime.datetime.now().isoformat(timespec="seconds")}\n'
                f'git={sha}\n'
                f'tx_port={tx_port} rx_port={rx_port} baud={baud} duration={duration}\n'
                f'sent={total} received_ok={matched} field_mismatch={mismatch} '
                f'expected_mismatch={expected_mismatch} lost={loss} PER={per:.1f}% '
                f'rx_without_tx_record={rx_orphan}\n'
                f'rssi[{stats(rssi_vals)}] lqi[{stats(lqi_vals)}]\n'
                f'target_link={last_link}\n'
                f'target_tx={last_tx_stats}\n'
                f'bulk_dumps={bulk_dumps} bulk_records={len(bulk_records)} '
                f'bulk_ok={bulk_ok} bulk_foreign={bulk_foreign} '
                f'bulk_malformed={bulk_malformed} '
                f'bulk_expected_mismatch={bulk_expected_mismatch} '
                f'bulk_lost={len(bulk_lost)} bulk_PER={bulk_per:.1f}% '
                f'bulk_missing_indices={bulk_missing}\n'
                f'missing={sorted(tx)[:50]}\n')
    arts = f'{logdir}/tx.log {logdir}/rx.log {logdir}/per-seq.csv {logdir}/summary.txt'
    if bulk:
        arts += f' {logdir}/bulk.csv'
    print(f'artifacts: {arts}', flush=True)
    if ftx_log: ftx_log.close()
    if frx_log: frx_log.close()

# Bulk mode trusts the ring (works with per-frame logging off); live mode
# trusts the per-frame log stream. Either way, an expected-payload mismatch
# or a MAC/RX field mismatch fails the run.
live_pass = (loss == 0 and mismatch == 0 and expected_mismatch == 0 and total > 0)
bulk_pass = (bulk_ok > 0 and bulk_expected_mismatch == 0 and not bulk_lost
             and bulk_missing == 0)
if bulk:
    ok = bulk_pass and mismatch == 0 and expected_mismatch == 0
else:
    ok = live_pass
sys.exit(0 if ok else 1)
EOF
