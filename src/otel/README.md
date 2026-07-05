## Introduction

This directory implements an [OpenTelemetry](https://opentelemetry.io/) metrics
exporter for Valkey. When enabled, Valkey periodically pushes its server
metrics to an OpenTelemetry collector using [OTLP/HTTP](https://opentelemetry.io/docs/specs/otlp/#otlphttp)
with JSON encoding.

It is built on the **info emitter** (`src/info_emitter.{c,h}`): the exporter is
a second emitter backend. Running the INFO generator with this backend
(`genValkeyInfoToEmitter`) turns every numeric `INFO` field -- across all
sections *and* module-provided fields -- directly into a typed OTLP metric,
using the counter/gauge and unit metadata the emitter already carries. There is
no INFO-string round trip and no name-based heuristics.

## Design goals

1. **Zero cost when not compiled.** The whole exporter is gated behind
   `USE_OTEL`; a default build compiles none of it and the entry points become
   empty `static inline` no-ops.
2. **Zero hot-path cost.** No instrumentation is added to command processing.
   Sampling runs in `serverCron` (off the command path); all parsing/encoding
   and network I/O run on a dedicated background thread with drop-oldest
   back-pressure, so a slow or unreachable collector never blocks the main
   event loop.
3. **No new link dependencies.** OTLP/HTTP with JSON is emitted over raw POSIX
   sockets (libc + pthread only) -- no protobuf/gRPC/HTTP client library.

### Data flow

```
main thread (serverCron)                    background exporter thread
--------------------------                  ---------------------------
otelCron():                                 otelExporterThread():
  if enabled && interval elapsed:             wait on condvar for a snapshot
    run genValkeyInfoToEmitter() with the     take snapshot (steal pointer)
    OTLP backend -> typed record list         encode OTLP/JSON from records
    (reads live server state)                 HTTP POST /v1/metrics
    lock; drop-oldest pending; signal         free snapshot
```

The main thread only builds a compact list of typed records
(`otel_emitter.{c,h}`); the JSON encoding (`otelEncodeJson`) is a pure function
of that list and runs on the exporter thread.

## Building

```
make USE_OTEL=yes          # Makefile build
cmake .. -DBUILD_OTEL=yes  # CMake build
```

## Configuration

| Option                  | Default          | Description                                             |
|-------------------------|------------------|---------------------------------------------------------|
| `otel-enabled`          | `no`             | Enable/disable metrics export (runtime-modifiable).     |
| `otel-endpoint`         | `127.0.0.1:4318` | OTLP/HTTP collector `host:port` (path is `/v1/metrics`).|
| `otel-service-name`     | `valkey`         | `service.name` resource attribute.                      |
| `otel-push-interval-ms` | `15000`          | Export interval in milliseconds (minimum `100`).        |

All options can be changed at runtime with `CONFIG SET`.

## Metrics: naming and typing

- Each field becomes `valkey.<section>.<field>`, e.g.
  `valkey.stats.total_commands_processed`, `valkey.memory.used_memory`,
  `valkey.cpu.used_cpu_sys`.
- Counters (from the emitter's `INFO_KIND_COUNTER` metadata) are exported as
  OTLP **cumulative monotonic sums**; everything else as **gauges**. Floating
  point fields are `asDouble` gauges.
- Units come from the emitter's `infoUnit` metadata, mapped to UCUM:
  bytes -> `By`, seconds -> `s`, ms -> `ms`, microseconds -> `us`, percent ->
  `%`, none -> `1`. CPU/duration fields are exported as microseconds (`us`).
- Composite lines become labeled series:
  `valkey.commandstats.calls{command="get"}`,
  `valkey.keyspace.keys{db="db0"}`, `valkey.errorstats.count{error="ERR"}`,
  `valkey.latencystats.p50{command="get"}`.
- Non-numeric fields (version strings, `os`, `run_id`, roles) are not emitted as
  metrics; identity is carried in the OTLP resource attributes
  (`service.name`, `service.version`, `service.instance.id`,
  `telemetry.sdk.name=valkey-otel`).

A default configuration exports on the order of ~230 metrics; loaded modules and
per-command stats add more (see Cardinality).

## Quick start

Run a local OpenTelemetry collector (the standard sidecar/agent listens for
OTLP/HTTP on `4318`), then:

```
./src/valkey-server --otel-enabled yes \
    --otel-endpoint 127.0.0.1:4318 \
    --otel-push-interval-ms 15000
```

## Cardinality

`commandstats`, `errorstats` and `latencystats` produce one series per command
or error type (as attributes). This is complete but can be many series for a
metrics backend. If that is a concern, raise `otel-push-interval-ms` or drop
those series at the collector. (A config to select exported sections is a
natural follow-up.)

## Performance

Interleaved, CPU-pinned `valkey-benchmark` (`-t set,get -c 50 -P 1`, server and
benchmark on separate cores, median of 6 rounds):

| Configuration                     | SET (req/s) | GET (req/s) |
|-----------------------------------|-------------|-------------|
| baseline (built without USE_OTEL) | 85,005      | 85,008      |
| built with USE_OTEL, disabled     | 85,005      | 85,070      |
| enabled, 500ms push interval      | 85,005      | 85,034      |

All within measurement noise: the exporter is not on the command path.

## Notes / limitations

- Only OTLP/HTTP with JSON is implemented. TLS (`https://`) and the binary
  protobuf encoding are not yet supported; use a local collector / a sidecar
  that terminates TLS. No auth headers yet (a follow-up can add configurable
  headers).
- This exporter covers the **metrics** signal. Tracing is available separately
  via `src/trace/` (LTTng).
