# OpenTelemetry (OTLP/HTTP) metrics exporter integration tests.
#
# Written to behave correctly whether or not the server was built with USE_OTEL:
#   - If the "otel-enabled" config does not exist, the binary was built without
#     OpenTelemetry support and the OTLP-specific assertions are skipped.
#   - Otherwise the exporter is exercised against an in-process Tcl HTTP
#     collector (no external dependencies).

# Minimal in-process OTLP/HTTP collector. Captures the body of each POST into
# ::otel_last_body and counts them, replying 200 OK.
set ::otel_last_body ""
set ::otel_post_count 0

proc otel_collector_accept {chan addr port} {
    fconfigure $chan -translation binary -blocking 1
    set content_length 0
    while {[gets $chan line] >= 0} {
        set line [string trim $line]
        if {$line eq ""} break
        if {[string match -nocase "content-length:*" $line]} {
            set content_length [string trim [lindex [split $line :] 1]]
        }
    }
    set body ""
    if {$content_length > 0} {
        set body [read $chan $content_length]
    }
    set ::otel_last_body $body
    incr ::otel_post_count
    set reply "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}"
    catch {puts -nonewline $chan $reply; flush $chan}
    catch {close $chan}
}

start_server {tags {"otel" "external:skip"}} {
    set otel_supported 1
    if {[catch {r config get otel-enabled} res] || $res eq {}} {
        set otel_supported 0
    }

    if {!$otel_supported} {
        test "OpenTelemetry - not compiled in (USE_OTEL unset); config absent" {
            assert_equal {} [r config get otel-enabled]
            assert_equal {} [r config get otel-endpoint]
        }
    } else {
        test "OpenTelemetry - config options exist with expected defaults" {
            assert_equal {otel-enabled no} [r config get otel-enabled]
            assert_equal {otel-endpoint 127.0.0.1:4318} [r config get otel-endpoint]
            assert_equal {otel-service-name valkey} [r config get otel-service-name]
            assert_equal {otel-push-interval-ms 15000} [r config get otel-push-interval-ms]
        }

        test "OpenTelemetry - config values can be changed at runtime" {
            r config set otel-endpoint 10.0.0.1:4318
            assert_equal {otel-endpoint 10.0.0.1:4318} [r config get otel-endpoint]
            r config set otel-service-name my-cache
            assert_equal {otel-service-name my-cache} [r config get otel-service-name]
            r config set otel-push-interval-ms 500
            assert_equal {otel-push-interval-ms 500} [r config get otel-push-interval-ms]
            assert_error "*argument*" {r config set otel-push-interval-ms 0}
        }

        test "OpenTelemetry - server stays responsive when the collector is unreachable" {
            r config set otel-endpoint 127.0.0.1:1
            r config set otel-push-interval-ms 100
            r config set otel-enabled yes
            after 300
            assert_equal PONG [r ping]
            for {set i 0} {$i < 200} {incr i} { r set unreachable:$i $i }
            assert_equal PONG [r ping]
            assert_equal 200 [r dbsize]
            r config set otel-enabled no
            r flushall
        }

        test "OpenTelemetry - metrics are exported as OTLP/JSON to a collector" {
            set port [find_available_port $::baseport $::portcount]
            set srvsock [socket -server otel_collector_accept $port]
            set ::otel_last_body ""
            set ::otel_post_count 0

            r config set otel-service-name valkey
            r config set otel-endpoint 127.0.0.1:$port
            r config set otel-push-interval-ms 100
            r config set otel-enabled yes
            for {set i 0} {$i < 40} {incr i} { r set exported:$i $i }
            r get exported:0
            r get missing-key

            set deadline [expr {[clock milliseconds] + 8000}]
            while {$::otel_last_body eq "" && [clock milliseconds] < $deadline} {
                update
                after 50
            }

            r config set otel-enabled no
            catch {close $srvsock}

            set body $::otel_last_body
            assert {[string length $body] > 0}
            # Valid OTLP/HTTP JSON envelope + resource attributes.
            assert {[string match "*resourceMetrics*" $body]}
            assert {[string match "*scopeMetrics*" $body]}
            assert {[string match "*service.name*valkey*" $body]}
            assert {[string match "*telemetry.sdk.name*valkey-otel*" $body]}
            # Full coverage: fields from multiple INFO sections, named valkey.<section>.<field>.
            assert {[string match "*valkey.stats.total_commands_processed*" $body]}
            assert {[string match "*valkey.stats.keyspace_hits*" $body]}
            assert {[string match "*valkey.memory.used_memory*" $body]}
            assert {[string match "*valkey.clients.connected_clients*" $body]}
            assert {[string match "*valkey.keyspace.keys*" $body]}
            # Per-command stats exported with a "command" attribute.
            assert {[string match "*valkey.commandstats.calls*" $body]}
            assert {[string match "*\"key\":\"command\"*" $body]}
            # Bytes carry the UCUM unit; counters are cumulative monotonic sums.
            assert {[string match "*\"unit\":\"By\"*" $body]}
            assert {[string match "*isMonotonic*true*" $body]}
        }

        test "OpenTelemetry - exporter can be toggled off and on again" {
            set port [find_available_port $::baseport $::portcount]
            set srvsock [socket -server otel_collector_accept $port]
            r config set otel-endpoint 127.0.0.1:$port
            r config set otel-push-interval-ms 100

            r config set otel-enabled no
            set ::otel_post_count 0
            after 400
            update
            assert_equal 0 $::otel_post_count

            set ::otel_last_body ""
            r config set otel-enabled yes
            set deadline [expr {[clock milliseconds] + 8000}]
            while {$::otel_last_body eq "" && [clock milliseconds] < $deadline} {
                update
                after 50
            }
            r config set otel-enabled no
            catch {close $srvsock}
            assert {$::otel_post_count > 0}
        }
    }
}
