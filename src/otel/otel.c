/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* OpenTelemetry (OTLP/HTTP) metrics exporter. See otel.h and otel/README.md.
 *
 * Data flow:
 *
 *   main thread (serverCron)                    background exporter thread
 *   --------------------------                  ---------------------------
 *   otelCron():                                 otelExporterThread():
 *     if enabled && interval elapsed:             wait on cond for a snapshot
 *       run genValkeyInfoToEmitter() with the     take snapshot (steal pointer)
 *       OTLP backend -> typed record list         encode OTLP/JSON from records
 *       (reads live server state)                 HTTP POST /v1/metrics
 *       lock; drop-oldest pending; store; signal  free snapshot
 *
 * The main thread only reads state it already maintains and builds a compact
 * typed record list. It never touches the network and never blocks: a busy
 * exporter causes the previous unsent snapshot to be dropped (drop-oldest)
 * rather than queued unbounded. */

#include "../server.h"
#include "otel.h"

#ifdef USE_OTEL

#include "otel_emitter.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define OTEL_STR_MAX 256
#define OTEL_DEFAULT_PORT 4318
#define OTEL_NET_TIMEOUT_MS 2000
#define OTEL_HTTP_PATH "/v1/metrics"

/* Snapshot handed from the main thread to the exporter thread: a typed metric
 * record list plus resource attributes and timestamps. */
typedef struct otelSnapshot {
    uint64_t time_unix_nano;
    uint64_t start_time_unix_nano;
    char service_name[OTEL_STR_MAX];
    char instance_id[OTEL_STR_MAX];
    char version[64];
    char endpoint[OTEL_STR_MAX];
    otelMetricList metrics;
} otelSnapshot;

static struct otelState {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    otelSnapshot *pending;
    int shutdown;
    int running;
    long long last_push_ms;
    long long consecutive_failures;
} otel;

/* ------------------------------------------------------------------------- */
/* Sampling (main thread)                                                    */
/* ------------------------------------------------------------------------- */

/* Build a snapshot by running INFO generation into the OTLP backend. Runs on
 * the main thread (INFO reads live server state); no formatting or I/O here. */
static otelSnapshot *otelSampleSnapshot(void) {
    otelSnapshot *s = zcalloc(sizeof(*s));
    s->time_unix_nano = (uint64_t)mstime() * 1000000ULL;
    s->start_time_unix_nano = (uint64_t)server.stat_starttime * 1000000000ULL;

    const char *svc = server.otel_service_name ? server.otel_service_name : "valkey";
    const char *ep = server.otel_endpoint ? server.otel_endpoint : "127.0.0.1:4318";
    valkey_strlcpy(s->service_name, svc, sizeof(s->service_name));
    valkey_strlcpy(s->instance_id, server.runid, sizeof(s->instance_id));
    valkey_strlcpy(s->version, VALKEY_VERSION, sizeof(s->version));
    valkey_strlcpy(s->endpoint, ep, sizeof(s->endpoint));

    otelMetricListInit(&s->metrics);
    otelOtlpEmitter oe;
    int sections = 0;
    otelOtlpEmitterInit(&oe, &s->metrics, &sections);

    /* "everything": all sections including per-command stats and modules. */
    int all = 0, everything = 0;
    robj *argv[1];
    argv[0] = createStringObject("everything", strlen("everything"));
    dict *section_dict = genInfoSectionDict(argv, 1, NULL, &all, &everything);
    genValkeyInfoToEmitter(&oe.e, &sections, section_dict, all, everything);
    releaseInfoSectionDict(section_dict);
    decrRefCount(argv[0]);
    return s;
}

static void otelFreeSnapshot(otelSnapshot *s) {
    if (!s) return;
    otelMetricListFree(&s->metrics);
    zfree(s);
}

/* ------------------------------------------------------------------------- */
/* Minimal HTTP client (exporter thread)                                     */
/* ------------------------------------------------------------------------- */

/* Parse "host:port", "http://host:port" or ".../path" into host + port. */
static int otelParseEndpoint(const char *endpoint, char *host, size_t hostlen, int *port) {
    const char *p = endpoint;
    if (!strncasecmp(p, "http://", 7))
        p += 7;
    else if (!strncasecmp(p, "https://", 8))
        p += 8; /* TLS unsupported; connection will simply fail. */

    char hostport[OTEL_STR_MAX];
    size_t n = 0;
    while (*p && *p != '/' && n < sizeof(hostport) - 1) hostport[n++] = *p++;
    hostport[n] = '\0';
    if (n == 0) return C_ERR;

    *port = OTEL_DEFAULT_PORT;
    char *colon;
    if (hostport[0] == '[') {
        char *close = strchr(hostport, ']');
        if (!close) return C_ERR;
        size_t hlen = (size_t)(close - hostport - 1);
        if (hlen == 0 || hlen >= hostlen) return C_ERR;
        memcpy(host, hostport + 1, hlen);
        host[hlen] = '\0';
        if (*(close + 1) == ':') *port = atoi(close + 2);
    } else if ((colon = strrchr(hostport, ':')) != NULL) {
        size_t hlen = (size_t)(colon - hostport);
        if (hlen == 0 || hlen >= hostlen) return C_ERR;
        memcpy(host, hostport, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        valkey_strlcpy(host, hostport, hostlen);
    }
    if (*port <= 0 || *port > 65535) return C_ERR;
    return C_OK;
}

/* Non-blocking connect bounded by poll(); returns a connected blocking fd or -1. */
static int otelTcpConnect(const char *host, int port, int timeout_ms) {
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (rc != 0) {
            if (errno != EINPROGRESS) {
                close(fd);
                fd = -1;
                continue;
            }
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            if (poll(&pfd, 1, timeout_ms) <= 0) {
                close(fd);
                fd = -1;
                continue;
            }
            int soerr = 0;
            socklen_t slen = sizeof(soerr);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0 || soerr != 0) {
                close(fd);
                fd = -1;
                continue;
            }
        }
        fcntl(fd, F_SETFL, flags);
        struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        break;
    }
    freeaddrinfo(res);
    return fd;
}

static int otelWriteAll(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w > 0)
            off += (size_t)w;
        else if (w < 0 && errno == EINTR)
            continue;
        else
            return C_ERR;
    }
    return C_OK;
}

static int otelHttpPost(const char *host, int port, const char *body, size_t body_len) {
    int fd = otelTcpConnect(host, port, OTEL_NET_TIMEOUT_MS);
    if (fd < 0) return C_ERR;
    sds req = sdscatprintf(sdsempty(),
                           "POST %s HTTP/1.1\r\n"
                           "Host: %s:%d\r\n"
                           "User-Agent: valkey-otel/%s\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           OTEL_HTTP_PATH, host, port, VALKEY_VERSION, body_len);
    int ok = C_ERR;
    if (otelWriteAll(fd, req, sdslen(req)) == C_OK && otelWriteAll(fd, body, body_len) == C_OK) {
        char resp[64];
        ssize_t r = read(fd, resp, sizeof(resp) - 1);
        if (r > 0) {
            resp[r] = '\0';
            char *sp = strchr(resp, ' ');
            if (sp && sp[1] == '2') ok = C_OK;
        } else if (r == 0) {
            ok = C_OK; /* some collectors close without a body on success */
        }
    }
    sdsfree(req);
    close(fd);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Exporter thread                                                           */
/* ------------------------------------------------------------------------- */

static void otelExportSnapshot(const otelSnapshot *s) {
    char host[OTEL_STR_MAX];
    int port;
    if (otelParseEndpoint(s->endpoint, host, sizeof(host), &port) != C_OK) {
        if (otel.consecutive_failures++ == 0)
            serverLog(LL_WARNING, "OpenTelemetry: invalid otel-endpoint '%s'", s->endpoint);
        return;
    }

    sds body = otelEncodeJson(&s->metrics, s->service_name, s->version, s->instance_id, s->start_time_unix_nano,
                              s->time_unix_nano);
    int rc = otelHttpPost(host, port, body, sdslen(body));
    sdsfree(body);

    if (rc == C_OK) {
        if (otel.consecutive_failures > 0)
            serverLog(LL_NOTICE, "OpenTelemetry: metrics export to %s:%d recovered", host, port);
        otel.consecutive_failures = 0;
    } else {
        if (otel.consecutive_failures % 100 == 0)
            serverLog(LL_WARNING, "OpenTelemetry: failed to export metrics to %s:%d", host, port);
        otel.consecutive_failures++;
    }
}

static void *otelExporterThread(void *arg) {
    UNUSED(arg);
#ifdef __linux__
    pthread_setname_np(pthread_self(), "otel-exporter");
#endif
    while (1) {
        pthread_mutex_lock(&otel.lock);
        while (!otel.pending && !otel.shutdown) pthread_cond_wait(&otel.cond, &otel.lock);
        if (otel.shutdown && !otel.pending) {
            pthread_mutex_unlock(&otel.lock);
            break;
        }
        otelSnapshot *snap = otel.pending;
        otel.pending = NULL;
        pthread_mutex_unlock(&otel.lock);
        if (snap) {
            otelExportSnapshot(snap);
            otelFreeSnapshot(snap);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

void otelInit(void) {
    if (otel.running) return;
    otel.pending = NULL;
    otel.shutdown = 0;
    otel.last_push_ms = 0;
    otel.consecutive_failures = 0;
    pthread_mutex_init(&otel.lock, NULL);
    pthread_cond_init(&otel.cond, NULL);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t stacksize = 0;
    pthread_attr_getstacksize(&attr, &stacksize);
    if (!stacksize) stacksize = 1;
    while (stacksize < (1024 * 1024 * 4)) stacksize *= 2;
    pthread_attr_setstacksize(&attr, stacksize);

    int err = pthread_create(&otel.thread, &attr, otelExporterThread, NULL);
    pthread_attr_destroy(&attr);
    if (err) {
        serverLog(LL_WARNING, "OpenTelemetry: failed to start exporter thread: %s", strerror(err));
        pthread_mutex_destroy(&otel.lock);
        pthread_cond_destroy(&otel.cond);
        return;
    }
    otel.running = 1;
    serverLog(LL_NOTICE, "OpenTelemetry exporter thread initialized (metrics export %s)",
              server.otel_enabled ? "enabled" : "disabled");
}

void otelCron(void) {
    if (!otel.running || !server.otel_enabled) return;
    long long now = mstime();
    int interval = server.otel_push_interval_ms;
    if (interval < 100) interval = 100;
    if (otel.last_push_ms != 0 && (now - otel.last_push_ms) < interval) return;
    otel.last_push_ms = now;

    otelSnapshot *snap = otelSampleSnapshot();
    if (!snap) return;

    pthread_mutex_lock(&otel.lock);
    if (otel.pending) otelFreeSnapshot(otel.pending); /* drop-oldest */
    otel.pending = snap;
    pthread_cond_signal(&otel.cond);
    pthread_mutex_unlock(&otel.lock);
}

void otelCleanup(void) {
    if (!otel.running) return;
    pthread_mutex_lock(&otel.lock);
    otel.shutdown = 1;
    pthread_cond_signal(&otel.cond);
    pthread_mutex_unlock(&otel.lock);
    pthread_join(otel.thread, NULL);
    if (otel.pending) {
        otelFreeSnapshot(otel.pending);
        otel.pending = NULL;
    }
    pthread_mutex_destroy(&otel.lock);
    pthread_cond_destroy(&otel.cond);
    otel.running = 0;
}

#endif /* USE_OTEL */
