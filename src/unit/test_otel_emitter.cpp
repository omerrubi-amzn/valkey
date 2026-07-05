/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* Unit tests for the OTLP metrics backend (src/otel/otel_emitter). Compiled
 * only when built with USE_OTEL; otherwise this is an empty translation unit. */

#ifdef USE_OTEL

#include "gtest/gtest.h"
#include <string>

/* otel_emitter.h transitively includes sds.h, whose C99 flexible array members
 * trip -Werror=pedantic when compiled as C++. sds.h is effectively a system
 * header here, so silence just that diagnostic around the include. Using a
 * pragma (rather than include ordering) keeps this stable under the unit tests'
 * SortIncludes clang-format setting. */
extern "C" {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "../otel/otel_emitter.h"
#pragma GCC diagnostic pop
}

static std::string js(sds s) {
    return std::string(s, sdslen(s));
}
static bool has(const std::string &h, const char *n) {
    return h.find(n) != std::string::npos;
}

/* infoUnit -> UCUM string mapping. */
TEST(OtelEmitterTest, UnitStrings) {
    EXPECT_STREQ("By", otelUnitString(INFO_UNIT_BYTES));
    EXPECT_STREQ("s", otelUnitString(INFO_UNIT_SECONDS));
    EXPECT_STREQ("ms", otelUnitString(INFO_UNIT_MILLISECONDS));
    EXPECT_STREQ("us", otelUnitString(INFO_UNIT_MICROSECONDS));
    EXPECT_STREQ("%", otelUnitString(INFO_UNIT_PERCENT));
    EXPECT_STREQ("1", otelUnitString(INFO_UNIT_NONE));
}

/* The backend collects typed records with correct name/kind/unit/value, skips
 * strings, and labels dict fields. */
TEST(OtelEmitterTest, RecordModel) {
    otelMetricList list;
    otelMetricListInit(&list);
    otelOtlpEmitter oe;
    int sc = 0;
    otelOtlpEmitterInit(&oe, &list, &sc);
    infoEmitter *e = &oe.e;

    infoEmitBeginSection(e, "Stats");
    infoEmitCounterLL(e, "total_commands_processed", 42);
    infoEmitMetricULL(e, "some_bytes", 1000, INFO_KIND_GAUGE, INFO_UNIT_BYTES);
    infoEmitMetricDouble(e, "a_perc", 98.5, 2, INFO_KIND_GAUGE, INFO_UNIT_PERCENT);
    infoEmitFieldUsec(e, "used_cpu_sys", 14214);
    infoEmitFieldStr(e, "run_id", "abc"); /* skipped */
    infoEmitBeginSection(e, "Commandstats");
    infoEmitBeginDict(e, "cmdstat_get");
    infoEmitDictCounterLL(e, "calls", 5);
    infoEmitDictStr(e, "note", "x"); /* skipped */
    infoEmitEndDict(e);

    ASSERT_EQ((size_t)5, list.count);
    EXPECT_EQ(2, sc); /* two sections seen */

    otelMetric *m = &list.items[0];
    EXPECT_STREQ("valkey.stats.total_commands_processed", m->name);
    EXPECT_EQ(INFO_KIND_COUNTER, m->kind);
    EXPECT_EQ(OTEL_VT_I64, m->vtype);
    EXPECT_EQ(42, m->i64);
    EXPECT_TRUE(m->attr_key == NULL);

    EXPECT_EQ(INFO_UNIT_BYTES, list.items[1].unit);
    EXPECT_EQ(OTEL_VT_U64, list.items[1].vtype);

    EXPECT_EQ(INFO_UNIT_PERCENT, list.items[2].unit);
    EXPECT_EQ(OTEL_VT_F64, list.items[2].vtype);

    /* used_cpu_sys: microseconds int. */
    EXPECT_STREQ("valkey.stats.used_cpu_sys", list.items[3].name);
    EXPECT_EQ(INFO_UNIT_MICROSECONDS, list.items[3].unit);
    EXPECT_EQ((int64_t)14214, list.items[3].i64);

    /* dict field labeled command=get. */
    otelMetric *c = &list.items[4];
    EXPECT_STREQ("valkey.commandstats.calls", c->name);
    EXPECT_EQ(INFO_KIND_COUNTER, c->kind);
    ASSERT_TRUE(c->attr_key != NULL);
    EXPECT_STREQ("command", c->attr_key);
    EXPECT_STREQ("get", c->attr_val);

    otelMetricListFree(&list);
}

/* The encoder emits valid-looking OTLP/JSON: monotonic sums for counters,
 * gauges otherwise, units, attributes, and resource attributes. */
TEST(OtelEmitterTest, EncodeJson) {
    otelMetricList list;
    otelMetricListInit(&list);
    otelOtlpEmitter oe;
    int sc = 0;
    otelOtlpEmitterInit(&oe, &list, &sc);
    infoEmitter *e = &oe.e;

    infoEmitBeginSection(e, "Stats");
    infoEmitCounterLL(e, "total_commands_processed", 42);
    infoEmitBeginSection(e, "Memory");
    infoEmitMetricULL(e, "used_memory", 1000, INFO_KIND_GAUGE, INFO_UNIT_BYTES);
    infoEmitBeginSection(e, "Commandstats");
    infoEmitBeginDict(e, "cmdstat_get");
    infoEmitDictCounterLL(e, "calls", 5);
    infoEmitEndDict(e);

    sds body = otelEncodeJson(&list, "valkey", "8.0", "runid1", 1000ULL, 2000ULL);
    std::string s = js(body);

    EXPECT_TRUE(has(s, "\"resourceMetrics\""));
    EXPECT_TRUE(has(s, "\"scopeMetrics\""));
    EXPECT_TRUE(has(s, "\"service.name\",\"value\":{\"stringValue\":\"valkey\"}"));
    EXPECT_TRUE(has(s, "\"telemetry.sdk.name\",\"value\":{\"stringValue\":\"valkey-otel\"}"));
    /* counter -> monotonic cumulative sum with startTimeUnixNano + asInt. */
    EXPECT_TRUE(has(s, "\"valkey.stats.total_commands_processed\""));
    EXPECT_TRUE(has(s, "\"aggregationTemporality\":2"));
    EXPECT_TRUE(has(s, "\"isMonotonic\":true"));
    EXPECT_TRUE(has(s, "\"startTimeUnixNano\":\"1000\""));
    EXPECT_TRUE(has(s, "\"asInt\":\"42\""));
    /* gauge with unit By. */
    EXPECT_TRUE(has(s, "\"valkey.memory.used_memory\",\"unit\":\"By\""));
    EXPECT_TRUE(has(s, "\"gauge\""));
    /* dict metric with command attribute. */
    EXPECT_TRUE(has(s, "\"valkey.commandstats.calls\""));
    EXPECT_TRUE(has(s, "\"key\":\"command\",\"value\":{\"stringValue\":\"get\"}"));
    EXPECT_TRUE(has(s, "\"timeUnixNano\":\"2000\""));

    sdsfree(body);
    otelMetricListFree(&list);
}

/* Strings and raw output produce no metrics. */
TEST(OtelEmitterTest, StringsAndRawSkipped) {
    otelMetricList list;
    otelMetricListInit(&list);
    otelOtlpEmitter oe;
    int sc = 0;
    otelOtlpEmitterInit(&oe, &list, &sc);
    infoEmitter *e = &oe.e;
    infoEmitBeginSection(e, "Server");
    infoEmitFieldStr(e, "redis_version", "8.0.0");
    infoEmitFieldStrn(e, "run_id", "abcdef", 6);
    infoEmitRaw(e, "listener0:name=tcp\r\n");
    EXPECT_EQ((size_t)0, list.count);
    otelMetricListFree(&list);
}

#endif /* USE_OTEL */
