/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* OpenTelemetry (OTLP/HTTP) metrics exporter for Valkey.
 *
 * When enabled, a background thread periodically exports Valkey metrics to an
 * OpenTelemetry collector. Metrics are produced by running the INFO generator
 * (genValkeyInfoToEmitter) with an OTLP emitter backend (src/otel/otel_emitter),
 * so every numeric INFO field -- core and module-provided -- becomes a typed
 * OTLP metric with correct counter/gauge and unit semantics, with no INFO
 * string round-trip.
 *
 * Design invariants:
 *   - Zero hot-path cost: no instrumentation on command processing; sampling
 *     runs in serverCron (off the command path) and all network I/O runs on a
 *     dedicated background thread (drop-oldest back-pressure, never blocks).
 *   - Zero cost when not compiled: gated behind USE_OTEL; the entry points below
 *     become empty static inline no-ops otherwise.
 *   - No new link dependencies (libc + pthread + POSIX sockets only). */

#ifndef VALKEY_OTEL_H
#define VALKEY_OTEL_H

#ifdef USE_OTEL

void otelInit(void);    /* Spawn the exporter thread (after bioInit). */
void otelCron(void);    /* Called from serverCron; samples + hands off when due. */
void otelCleanup(void); /* Stop the exporter thread on clean shutdown. */

#else

static inline void otelInit(void) {
}
static inline void otelCron(void) {
}
static inline void otelCleanup(void) {
}

#endif /* USE_OTEL */

#endif /* VALKEY_OTEL_H */
