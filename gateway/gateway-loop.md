# Gateway loop — feasibility of the two-thread radio/HTTP plan

Evaluation of the high-level gateway plan:

> One thread loops receiving from the radio and sticking data into a sensor
> table; another serves HTTP requests (reads from the sensor table need to be
> atomic somehow).

Companion documents: `lib/radio/RX-PLAN.md` (receive path, sensor table API,
custom L2), `testbed-plan.md` (radio link bench). Ethernet hardware is
**deferred**; this document evaluates the concurrency architecture that both
the radio-only image today and the HTTP image later will sit on.

**Verdict: feasible, and it is the right shape.** Two corrections before it
becomes code:

1. **"Two threads" is right in spirit but wrong in mechanism.** There is no
   radio-receive loop to write. The rf2xx driver already owns an RX thread and
   calls *our* frame handler inside it; Zephyr's HTTP server likewise owns a
   thread and calls our resource handler inside it. The plan is really **two
   execution contexts sharing one table** — one writer callback, one reader.
   That is a simpler problem than two loops.
2. **"Atomic somehow" has a standard answer: one `k_mutex` + copy-out.** The
   requirement is *no torn reads* (never see `temp` from frame N beside `seq`
   from frame N+1), not lock-free machinery. See §3.

---

## 1. Terminology mapping (plan wording → Zephyr terms)

| Plan wording | Zephyr term | What it is |
|---|---|---|
| "a thread that loops receiving from the radio" | **driver-owned thread** + **callback** | The rf2xx driver creates its own thread (stack `CONFIG_IEEE802154_RF2XX_RX_STACK_SIZE`). Our frame handler runs *inside that thread* when the driver calls it (`gateway_l2_recv`, `RX-PLAN.md` §6.3). We write the callback body, not the loop. |
| "stick data into a sensor table" | **shared state** guarded by a **`k_mutex`** | Plain RAM written by one context, read by another → needs a critical section. |
| "a thread that serves http" | **`CONFIG_HTTP_SERVER` thread** + **resource handler** | Zephyr's HTTP server spawns one thread (`http_server_thread`, `K_THREAD_DEFINE` in `subsys/net/lib/http/http_server_core.c`, stack `CONFIG_HTTP_SERVER_STACK_SIZE`, 3072 default) that `zsock_poll()`s all client sockets and calls our `/sensors` handler *in that thread*. We write the handler, not the loop. |
| "reads need to be atomic" | **consistent snapshot / no torn read** | Not `atomic_t` — that protects a single word only. A ~20-byte entry is multi-word, so `atomic_t` cannot protect it. |
| (implicit) who may take locks | **thread context** vs **ISR context** | ISRs may only do non-blocking calls. The frame handler runs in *thread* context (the driver's RX thread), so `k_mutex_lock()` is legal there. If it ever ran in ISR context, a mutex would be illegal and the design would change. |
| (not in the plan, will exist) | **workqueue / work item** (`k_work`) | Deferred work run by a workqueue's thread (e.g. the HTTP server's inactivity timers). Do not put table writes here unless needed. |

Other threads exist (network RX/TX, logger, system workqueue) but none touch
the table, so the two-context model holds exactly where it matters.

**Inherited constraint (still relevant while HTTP is deferred):**
`CONFIG_IEEE802154_RAW_MODE` — the easy radio mode used by the testbeds —
selects `CONFIG_NET_RAW_MODE`, which compiles out the entire IP stack (no TCP,
no sockets, no HTTP ever). The gateway receive path must therefore go through
the custom-L2 glue (`gateway_l2.c` + the ~4-line rf2xx patch, `RX-PLAN.md`
§1–2), which is exactly what keeps the door open for Ethernet/HTTP later. The
testbed's raw-mode shortcut must not leak into the gateway image.

## 2. Why this is low-risk

- The writer path is microseconds of work: decode (already unit-tested,
  byte-wise, safe on Cortex-M0+) and a 25-byte store under a lock.
- The reader path is a copy under the same lock plus formatting outside it.
- The chip is single-core Cortex-M0+ (no SMP): no cache-coherency problems, no
  torn *word* accesses — the only hazard is preemption between field stores,
  which the copy-out removes.
- Frame rates are 1 Hz–1 kHz scale. There is no contention problem for a mutex
  to make worse; priority inheritance (free with `k_mutex`) is a bonus, not a
  need.

## 3. The atomicity question in detail

The hazard: `struct sensor_entry` (~20 B) is written field-by-field. A
preemption can land between two stores, and the reader then observes half of
frame N and half of frame N+1. Options:

| Option | Writer blocks? | Reader blocks? | Cost | Comment |
|---|---|---|---|---|
| **`k_mutex` + copy-out** (chosen) | one ~25 B `memcpy` | one ~25 B `memcpy` | 1 mutex | Standard, boring, correct. |
| `irq_lock()` around the copy | ~µs (IRQs off) | same | 0 | Equivalent on single-core; only worth it if one side were an ISR. |
| Seqlock (version counter, retry) | never | retries on collision | memory-barrier care on M0+ | Solves a contention problem we don't have. |
| Double buffer + pointer swap | never | never | **2× table ≈ 11.6 KB of 32 KB SRAM** | Blows the `RX-PLAN.md` §8 budget. Rejected. |

### 3.1 The one rule that makes the mutex safe

**Never hold the table lock across I/O.** When Ethernet/HTTP lands, the natural
mistake is `lock → serialize 256 nodes to JSON → write to socket → unlock`: a
slow client then stalls the radio RX thread for the duration of a socket write
and frames are lost at the driver level. Correct shape for `GET /sensors`:

```c
/* handler runs in the HTTP server thread */
struct sensor_entry snap[MAX_NODES];   /* static buffer, not the stack */
size_t n = sensor_table_snapshot(snap, ARRAY_SIZE(snap));  /* lock+memcpy+unlock */
for (size_t i = 0; i < n; i++) {
        /* format JSON and send — no lock held */
}
```

Per-entry `sensor_table_get()` follows the same rule trivially: lock, `memcpy`
the entry out, unlock, format outside.

Note this revises `RX-PLAN.md` §7 rule 4 slightly: `sensor_table_foreach()` as
"hold the lock, call the callback, forbid re-entry" re-introduces the
slow-reader problem the moment the callback does socket I/O. Prefer a
**snapshot** call (lock + copy into a caller-provided static array + unlock),
with `foreach` either implemented on top of it or kept for non-I/O users.

### 3.2 Literal-loop variant (not chosen)

If thread 1 must literally be a loop: the driver callback does `k_msgq_put()`
of the decoded reading and our own thread blocks on `k_msgq_get()` and updates
the table — the textbook driver-to-thread pattern, keeping the driver thread
minimal. It costs an extra thread, a queue and a drop policy for ~microseconds
of decode+store. Reach for it only if profiling ever shows the store path
stealing RX time.

## 4. What to watch (ranked)

1. **Lock discipline** — the "no I/O under lock" rule (§3.1) is the only thing
   that can silently break radio reception later. It goes in the sensor table
   header docs as a contract.
2. **RAM** — 32 KB total; table ≈ 5.8 KB by design (`RX-PLAN.md` §8). Ethernet
   + IPv4 + TCP + HTTP server (3 KB stack + per-client buffers) will be the
   tight part *later*; the table design needs no change if it bites (documented
   fallback: fixed pool + `uint8_t idx[]`, drop `stats[]` first). Development
   tip: build the `/sensors` handler against `native_sim` on the host so the
   JSON path is tested before any Ethernet hardware exists.
3. **Priority ordering** — give the radio RX path a better (lower numeric)
   priority than the HTTP thread, so a table write never waits behind JSON
   formatting. Both are preemptible threads; it is just two numbers.
4. **The rf2xx custom-L2 patch** (`RX-PLAN.md` §2) is still unapplied — a
   prerequisite for the gateway receive path regardless of the table design.
   `west.yml` tracks `zephyr: main`, so the patch needs its upstream /
   patch-file decision before it can be relied on.
5. **Time semantics** — already settled correctly: monotonic `k_uptime_get()`
   stamps in the table, wall-clock/SNTP offset applied at render time. Keep
   that boundary; it is what makes the table transport-independent.

## 5. Bottom line

Implement the plan as: **driver callback writes under a mutex; the reader
snapshots under the same mutex and formats outside.** Deferred Ethernet/HTTP
then becomes a pure consumer of an already-thread-safe API (`sensor_table_*`),
and the only real work left on the radio side is the custom-L2 glue already
charted in `RX-PLAN.md` R0–R1.
