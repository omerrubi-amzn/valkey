/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* OTLP metrics backend for the info emitter.
 *
 * This backend implements the infoEmitter vtable (see info_emitter.h). Instead
 * of formatting INFO text, its callbacks collect a list of *typed* metric
 * records (name, kind, unit, value, optional attribute) using the (kind, unit)
 * metadata the info emitter already carries. Running genValkeyInfoToEmitter()
 * with this backend therefore turns every numeric INFO field -- core and
 * module-provided -- directly into a metric record, with no INFO string
 * round-trip and no name/heuristic guessing.
 *
 * The record list is produced on the main thread (INFO generation reads live
 * server state); the JSON encoding (otelEncodeJson) is a pure function of the
 * list and runs on the exporter thread.
 *
 * This file is self-contained (no server.h / socket / thread dependencies) so
 * it can be unit-tested directly. */

#ifndef OTEL_EMITTER_H
#define OTEL_EMITTER_H

#include "../info_emitter.h"
#include "../sds.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How a metric's numeric value is stored. */
typedef enum { OTEL_VT_I64 = 0,
               OTEL_VT_U64,
               OTEL_VT_F64 } otelValueType;

/* One collected metric. `name` and `attr_val` are owned (sds). */
typedef struct otelMetric {
    sds name;            /* "valkey.<section>.<field>" */
    infoKind kind;       /* GAUGE -> gauge, COUNTER -> monotonic cumulative sum */
    infoUnit unit;       /* UCUM unit for the OTLP "unit" field */
    otelValueType vtype; /* which of the value fields below is valid */
    int64_t i64;
    uint64_t u64;
    double f64;
    const char *attr_key; /* static string ("command"/"db"/...) or NULL */
    sds attr_val;         /* entity value (owned) or NULL */
} otelMetric;

/* Growable list of metric records. */
typedef struct otelMetricList {
    otelMetric *items;
    size_t count;
    size_t capacity;
} otelMetricList;

void otelMetricListInit(otelMetricList *l);
void otelMetricListFree(otelMetricList *l);

/* OTLP emitter: an infoEmitter backend that appends records to `list`. */
typedef struct otelOtlpEmitter {
    infoEmitter e; /* must be first member */
    otelMetricList *list;
    int *section_counter;   /* shared with genValkeyInfoToEmitter (module decision) */
    char section[128];      /* current section, lowercased */
    const char *dict_label; /* current dict label ("command"/"db"/...) or NULL */
    char dict_entity[160];  /* current dict entity value */
    int in_dict;
} otelOtlpEmitter;

void otelOtlpEmitterInit(otelOtlpEmitter *oe, otelMetricList *list, int *section_counter);

/* Map an infoUnit to its UCUM unit string ("By","s","ms","us","%","1"). */
const char *otelUnitString(infoUnit unit);

/* Encode a metric list as an OTLP/HTTP JSON ExportMetricsServiceRequest body.
 * Returns an sds the caller must sdsfree(). start_nano is used as
 * startTimeUnixNano for cumulative sums; now_nano is each point's timestamp. */
sds otelEncodeJson(const otelMetricList *list, const char *service_name, const char *service_version, const char *instance_id, uint64_t start_nano, uint64_t now_nano);

#ifdef __cplusplus
}
#endif

#endif /* OTEL_EMITTER_H */
