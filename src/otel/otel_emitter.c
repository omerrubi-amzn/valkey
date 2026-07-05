/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* OTLP metrics backend for the info emitter. See otel_emitter.h. */

#include "otel_emitter.h"
#include "../zmalloc.h"
#include <string.h>

#ifdef USE_OTEL

/* ------------------------------------------------------------------------- */
/* Metric list                                                               */
/* ------------------------------------------------------------------------- */

void otelMetricListInit(otelMetricList *l) {
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
}

void otelMetricListFree(otelMetricList *l) {
    for (size_t i = 0; i < l->count; i++) {
        sdsfree(l->items[i].name);
        if (l->items[i].attr_val) sdsfree(l->items[i].attr_val);
    }
    zfree(l->items);
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
}

static otelMetric *otelMetricListPush(otelMetricList *l) {
    if (l->count == l->capacity) {
        l->capacity = l->capacity ? l->capacity * 2 : 32;
        l->items = zrealloc(l->items, l->capacity * sizeof(*l->items));
    }
    otelMetric *m = &l->items[l->count++];
    memset(m, 0, sizeof(*m));
    return m;
}

const char *otelUnitString(infoUnit unit) {
    switch (unit) {
    case INFO_UNIT_BYTES: return "By";
    case INFO_UNIT_SECONDS: return "s";
    case INFO_UNIT_MILLISECONDS: return "ms";
    case INFO_UNIT_MICROSECONDS: return "us";
    case INFO_UNIT_PERCENT: return "%";
    case INFO_UNIT_NONE:
    default: return "1";
    }
}

/* ------------------------------------------------------------------------- */
/* Emitter backend                                                           */
/* ------------------------------------------------------------------------- */

static otelOtlpEmitter *otlpOf(infoEmitter *e) {
    return (otelOtlpEmitter *)e;
}

/* Append "valkey.<section>.<key>" to dst, sanitizing to valid OTLP name chars. */
static sds otelCatName(sds dst, const char *s) {
    for (const char *p = s; *p; p++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
        dst = sdscatlen(dst, ok ? &c : "_", 1);
    }
    return dst;
}

static sds otelBuildName(otelOtlpEmitter *oe, const char *key) {
    sds name = sdsnew("valkey.");
    name = otelCatName(name, oe->section);
    name = sdscatlen(name, ".", 1);
    name = otelCatName(name, key);
    return name;
}

/* Create a record for a scalar or dict field, attaching the current dict label
 * when inside a dict. */
static otelMetric *otelAddMetric(otelOtlpEmitter *oe, const char *key, infoKind kind, infoUnit unit) {
    otelMetric *m = otelMetricListPush(oe->list);
    m->name = otelBuildName(oe, key);
    m->kind = kind;
    m->unit = unit;
    if (oe->in_dict && oe->dict_label) {
        m->attr_key = oe->dict_label;
        m->attr_val = sdsnew(oe->dict_entity);
    }
    return m;
}

static void otlpBeginSection(infoEmitter *e, const char *name) {
    otelOtlpEmitter *oe = otlpOf(e);
    if (oe->section_counter) (*oe->section_counter)++;
    size_t j = 0;
    for (size_t i = 0; name[i] && j < sizeof(oe->section) - 1; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        oe->section[j++] = c;
    }
    oe->section[j] = '\0';
}

static void otlpFieldLL(infoEmitter *e, const char *key, long long v, infoKind kind, infoUnit unit) {
    otelMetric *m = otelAddMetric(otlpOf(e), key, kind, unit);
    m->vtype = OTEL_VT_I64;
    m->i64 = v;
}

static void otlpFieldULL(infoEmitter *e, const char *key, unsigned long long v, infoKind kind, infoUnit unit) {
    otelMetric *m = otelAddMetric(otlpOf(e), key, kind, unit);
    m->vtype = OTEL_VT_U64;
    m->u64 = v;
}

static void otlpFieldDouble(infoEmitter *e, const char *key, double v, int prec, infoKind kind, infoUnit unit) {
    (void)prec;
    otelMetric *m = otelAddMetric(otlpOf(e), key, kind, unit);
    m->vtype = OTEL_VT_F64;
    m->f64 = v;
}

/* Strings are identity/metadata, not metrics. */
static void otlpFieldStr(infoEmitter *e, const char *key, const char *v) {
    (void)e;
    (void)key;
    (void)v;
}
static void otlpFieldStrn(infoEmitter *e, const char *key, const char *v, size_t vlen) {
    (void)e;
    (void)key;
    (void)v;
    (void)vlen;
}

/* CPU/duration fields: exported as microseconds (unit us), losslessly. */
static void otlpFieldUsec(infoEmitter *e, const char *key, long long usec, infoKind kind) {
    otelMetric *m = otelAddMetric(otlpOf(e), key, kind, INFO_UNIT_MICROSECONDS);
    m->vtype = OTEL_VT_I64;
    m->i64 = usec;
}

/* Derive the attribute label + entity from a dict key ("cmdstat_get" ->
 * command=get, "db0" -> db=db0, "errorstat_ERR" -> error=ERR, ...). */
static void otlpBeginDict(infoEmitter *e, const char *key) {
    otelOtlpEmitter *oe = otlpOf(e);
    const char *label = "id";
    const char *entity = key;
    if (!strncmp(key, "cmdstat_", 8)) {
        label = "command";
        entity = key + 8;
    } else if (!strncmp(key, "errorstat_", 10)) {
        label = "error";
        entity = key + 10;
    } else if (!strncmp(key, "latency_percentiles_usec_", 25)) {
        label = "command";
        entity = key + 25;
    } else if (!strncmp(key, "engine_", 7)) {
        label = "engine";
        entity = key;
    } else if (key[0] == 'd' && key[1] == 'b') {
        label = "db";
        entity = key;
    }
    oe->dict_label = label;
    size_t n = strlen(entity);
    if (n >= sizeof(oe->dict_entity)) n = sizeof(oe->dict_entity) - 1;
    memcpy(oe->dict_entity, entity, n);
    oe->dict_entity[n] = '\0';
    oe->in_dict = 1;
}

static void otlpDictLL(infoEmitter *e, const char *sub, long long v, infoKind kind, infoUnit unit) {
    otelMetric *m = otelAddMetric(otlpOf(e), sub, kind, unit);
    m->vtype = OTEL_VT_I64;
    m->i64 = v;
}

static void otlpDictULL(infoEmitter *e, const char *sub, unsigned long long v, infoKind kind, infoUnit unit) {
    otelMetric *m = otelAddMetric(otlpOf(e), sub, kind, unit);
    m->vtype = OTEL_VT_U64;
    m->u64 = v;
}

static void otlpDictDouble(infoEmitter *e, const char *sub, double v, int prec, infoKind kind, infoUnit unit) {
    (void)prec;
    otelMetric *m = otelAddMetric(otlpOf(e), sub, kind, unit);
    m->vtype = OTEL_VT_F64;
    m->f64 = v;
}

static void otlpDictStr(infoEmitter *e, const char *sub, const char *v) {
    (void)e;
    (void)sub;
    (void)v;
}
static void otlpDictStrn(infoEmitter *e, const char *sub, const char *v, size_t vlen) {
    (void)e;
    (void)sub;
    (void)v;
    (void)vlen;
}

static void otlpEndDict(infoEmitter *e) {
    otelOtlpEmitter *oe = otlpOf(e);
    oe->in_dict = 0;
    oe->dict_label = NULL;
}

/* Irregular raw text has no typed representation; structured backends skip it. */
static void otlpRaw(infoEmitter *e, const char *fmt, va_list ap) {
    (void)e;
    (void)fmt;
    (void)ap;
}

static const infoEmitterOps otlpOps = {
    .begin_section = otlpBeginSection,
    .field_ll = otlpFieldLL,
    .field_ull = otlpFieldULL,
    .field_double = otlpFieldDouble,
    .field_str = otlpFieldStr,
    .field_strn = otlpFieldStrn,
    .field_usec = otlpFieldUsec,
    .begin_dict = otlpBeginDict,
    .dict_ll = otlpDictLL,
    .dict_ull = otlpDictULL,
    .dict_double = otlpDictDouble,
    .dict_str = otlpDictStr,
    .dict_strn = otlpDictStrn,
    .end_dict = otlpEndDict,
    .raw = otlpRaw,
};

void otelOtlpEmitterInit(otelOtlpEmitter *oe, otelMetricList *list, int *section_counter) {
    oe->e.ops = &otlpOps;
    oe->list = list;
    oe->section_counter = section_counter;
    oe->section[0] = '\0';
    oe->dict_label = NULL;
    oe->dict_entity[0] = '\0';
    oe->in_dict = 0;
}

/* ------------------------------------------------------------------------- */
/* JSON encoder (pure; runs on the exporter thread)                          */
/* ------------------------------------------------------------------------- */

static sds otelJsonEscape(sds dst, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': dst = sdscatlen(dst, "\\\"", 2); break;
        case '\\': dst = sdscatlen(dst, "\\\\", 2); break;
        case '\n': dst = sdscatlen(dst, "\\n", 2); break;
        case '\r': dst = sdscatlen(dst, "\\r", 2); break;
        case '\t': dst = sdscatlen(dst, "\\t", 2); break;
        default:
            if (c < 0x20)
                dst = sdscatprintf(dst, "\\u%04x", c);
            else
                dst = sdscatlen(dst, (const char *)&c, 1);
        }
    }
    return dst;
}

/* Append the data point value ("asInt":"..." or "asDouble":...). */
static sds otelCatValue(sds dp, const otelMetric *m) {
    switch (m->vtype) {
    case OTEL_VT_I64: return sdscatprintf(dp, "\"asInt\":\"%lld\"", (long long)m->i64);
    case OTEL_VT_U64: return sdscatprintf(dp, "\"asInt\":\"%llu\"", (unsigned long long)m->u64);
    case OTEL_VT_F64:
    default: return sdscatprintf(dp, "\"asDouble\":%.17g", m->f64);
    }
}

sds otelEncodeJson(const otelMetricList *list, const char *service_name, const char *service_version, const char *instance_id, uint64_t start_nano, uint64_t now_nano) {
    sds metrics = sdsempty();
    for (size_t i = 0; i < list->count; i++) {
        const otelMetric *m = &list->items[i];
        if (i) metrics = sdscatlen(metrics, ",", 1);

        metrics = sdscat(metrics, "{\"name\":\"");
        metrics = otelJsonEscape(metrics, m->name, sdslen(m->name));
        metrics = sdscatprintf(metrics, "\",\"unit\":\"%s\",", otelUnitString(m->unit));

        sds dp = sdsempty();
        if (m->attr_key) {
            dp = sdscatprintf(dp, "\"attributes\":[{\"key\":\"%s\",\"value\":{\"stringValue\":\"", m->attr_key);
            dp = otelJsonEscape(dp, m->attr_val, sdslen(m->attr_val));
            dp = sdscat(dp, "\"}}],");
        }
        dp = sdscatprintf(dp, "\"timeUnixNano\":\"%llu\",", (unsigned long long)now_nano);
        dp = otelCatValue(dp, m);

        if (m->kind == INFO_KIND_COUNTER) {
            metrics = sdscatprintf(metrics,
                                   "\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,"
                                   "\"dataPoints\":[{\"startTimeUnixNano\":\"%llu\",%s}]}}",
                                   (unsigned long long)start_nano, dp);
        } else {
            metrics = sdscatprintf(metrics, "\"gauge\":{\"dataPoints\":[{%s}]}}", dp);
        }
        sdsfree(dp);
    }

    sds svc = otelJsonEscape(sdsempty(), service_name, strlen(service_name));
    sds ver = otelJsonEscape(sdsempty(), service_version, strlen(service_version));
    sds iid = otelJsonEscape(sdsempty(), instance_id, strlen(instance_id));
    sds body = sdscatprintf(
        sdsempty(),
        "{\"resourceMetrics\":[{\"resource\":{\"attributes\":["
        "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"%s\"}},"
        "{\"key\":\"service.version\",\"value\":{\"stringValue\":\"%s\"}},"
        "{\"key\":\"service.instance.id\",\"value\":{\"stringValue\":\"%s\"}},"
        "{\"key\":\"telemetry.sdk.name\",\"value\":{\"stringValue\":\"valkey-otel\"}},"
        "{\"key\":\"telemetry.sdk.language\",\"value\":{\"stringValue\":\"c\"}}"
        "]},\"scopeMetrics\":[{\"scope\":{\"name\":\"valkey.server\",\"version\":\"%s\"},"
        "\"metrics\":[%s]}]}]}",
        svc, ver, iid, ver, metrics);
    sdsfree(svc);
    sdsfree(ver);
    sdsfree(iid);
    sdsfree(metrics);
    return body;
}

#endif /* USE_OTEL */
