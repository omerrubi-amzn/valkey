# OpenTelemetry (OTLP) metrics export

Design of Valkey's OpenTelemetry metrics exporter. Usage, configuration and the
full metric list live in `src/otel/README.md`; this document records the
architecture and the rationale.

## Goal

Export Valkey's server metrics to an OpenTelemetry collector (OTLP/HTTP) with:

- **Full coverage** of every numeric `INFO` field, across all sections and
  module-provided fields.
- **Correct semantics** (counter vs gauge, and units) taken from the code that
  produces each value.
- **Zero hot-path cost**, **zero cost when not compiled**, and **no new link
  dependencies**.

## Built on the info emitter

`genValkeyInfoString()` was refactored (see `design-docs/info-emitter.md`) so
that INFO field generation flows through a backend vtable
(`infoEmitter` / `infoEmitterOps`), with each numeric field carrying
`infoKind` (GAUGE/COUNTER) and `infoUnit` (bytes/seconds/ms/us/percent).

The exporter adds a **second backend**: instead of formatting text, its
callbacks collect typed metric records. A new entry point
`genValkeyInfoToEmitter(infoEmitter *e, int *section_counter, dict *, int, int)`
drives generation into any backend; the classic `genValkeyInfoString()` is now
a thin wrapper that uses the text backend. Module INFO callbacks are driven
through the same emitter (`modulesCollectInfoToEmitter`), so module fields are
covered automatically.

This eliminates the INFO-string round trip (format to text, then parse back)
and removes all name-based counter/unit heuristics: the exporter receives typed
values with authoritative metadata.

## Components

- `src/otel/otel_emitter.{c,h}` — the OTLP backend (an `infoEmitterOps`
  implementation), the typed record model (`otelMetric`/`otelMetricList`), and
  the pure JSON encoder (`otelEncodeJson`). Self-contained and unit-tested
  (`src/unit/test_otel_emitter.cpp`).
- `src/otel/otel.{c,h}` — the exporter: sampling in `serverCron`, a background
  thread (mutex + condvar, drop-oldest), a minimal OTLP/HTTP client
  (getaddrinfo + non-blocking connect + `POST /v1/metrics`), config and
  lifecycle (`otelInit`/`otelCron`/`otelCleanup`).

## Threading

INFO generation reads live server state, so the OTLP backend runs on the **main
thread** (in `serverCron`, off the command path). It only builds a compact
typed record list — no formatting, no I/O. The list is handed to the background
thread, which JSON-encodes and POSTs it. If the exporter is still busy, the
pending snapshot is replaced (drop-oldest), so the main thread never blocks.

## Metric mapping

- name: `valkey.<section>.<field>` (invalid chars sanitized to `_`).
- COUNTER -> OTLP `sum` (cumulative, monotonic, with `startTimeUnixNano`);
  otherwise `gauge`. Integers -> `asInt`, doubles -> `asDouble`.
- unit -> UCUM (`By`/`s`/`ms`/`us`/`%`/`1`). `field_usec` -> microseconds int.
- composite (dict) fields -> labeled data points (`command`/`db`/`error`/...).
- strings -> not metrics (identity via resource attributes); `raw` output
  (listeners, cluster info, module version list) -> skipped by this backend.

## Performance

- Main thread: run INFO generation once per push interval (default 15s) plus
  record collection — off the command path. INFO generation itself became
  faster with the info-emitter refactor.
- Background thread: JSON encode + HTTP send.
- Command throughput (SET/GET) is unaffected. Verified with interleaved,
  CPU-pinned `valkey-benchmark`: baseline vs compiled-but-disabled vs enabled
  (500ms) are all within measurement noise.

## Alternatives considered

- **Parse the INFO string** (a prototype): generate INFO text, split it, parse
  values, guess counter/gauge by name. Works and is fully decoupled, but
  redundant and heuristic. Superseded by the emitter backend, which is exact.
- **Encode JSON on the main thread**: simpler but moves formatting onto the main
  thread. Rejected in favour of the typed-record hand-off.

## Limitations / future work

- OTLP/HTTP + JSON only (no TLS, no protobuf, no auth headers yet).
- A config to select which sections are exported (to bound cardinality from
  per-command stats) is a natural follow-up.
