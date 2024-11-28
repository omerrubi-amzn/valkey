project(jemalloc)
include(CheckCSourceCompiles)

# Helper method: check if `CODE_TO_CHECK` can be compiled
function (check_compilable LABEL INCL_STMNT CODE_TO_CHECK OUT_VARIABLE)
    set(SRC
        "${INCL_STMNT}
  int main(int argc, char* argv[]) {
    ${CODE_TO_CHECK}
    return 0;
  }")

    check_c_source_compiles("${SRC}" __COMPILE_STATUS)
    if (__COMPILE_STATUS)
        set(${OUT_VARIABLE}
            1
            PARENT_SCOPE)
        message(STATUS "Check ${LABEL} is compilable...yes")
    else ()
        set(${OUT_VARIABLE}
            0
            PARENT_SCOPE)
        message(STATUS "Check ${LABEL} is compilable...no")
    endif ()
    unset(__COMPILE_STATUS CACHE)
endfunction (check_compilable)

set(JEMALLOC_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/src/jemalloc.c
    ${CMAKE_CURRENT_LIST_DIR}/src/arena.c
    ${CMAKE_CURRENT_LIST_DIR}/src/background_thread.c
    ${CMAKE_CURRENT_LIST_DIR}/src/base.c
    ${CMAKE_CURRENT_LIST_DIR}/src/bin.c
    ${CMAKE_CURRENT_LIST_DIR}/src/bin_info.c
    ${CMAKE_CURRENT_LIST_DIR}/src/bitmap.c
    ${CMAKE_CURRENT_LIST_DIR}/src/buf_writer.c
    ${CMAKE_CURRENT_LIST_DIR}/src/cache_bin.c
    ${CMAKE_CURRENT_LIST_DIR}/src/ckh.c
    ${CMAKE_CURRENT_LIST_DIR}/src/counter.c
    ${CMAKE_CURRENT_LIST_DIR}/src/ctl.c
    ${CMAKE_CURRENT_LIST_DIR}/src/decay.c
    ${CMAKE_CURRENT_LIST_DIR}/src/div.c
    ${CMAKE_CURRENT_LIST_DIR}/src/ecache.c
    ${CMAKE_CURRENT_LIST_DIR}/src/edata.c
    ${CMAKE_CURRENT_LIST_DIR}/src/edata_cache.c
    ${CMAKE_CURRENT_LIST_DIR}/src/ehooks.c
    ${CMAKE_CURRENT_LIST_DIR}/src/emap.c
    ${CMAKE_CURRENT_LIST_DIR}/src/eset.c
    ${CMAKE_CURRENT_LIST_DIR}/src/exp_grow.c
    ${CMAKE_CURRENT_LIST_DIR}/src/extent.c
    ${CMAKE_CURRENT_LIST_DIR}/src/extent_dss.c
    ${CMAKE_CURRENT_LIST_DIR}/src/extent_mmap.c
    ${CMAKE_CURRENT_LIST_DIR}/src/fxp.c
    ${CMAKE_CURRENT_LIST_DIR}/src/san.c
    ${CMAKE_CURRENT_LIST_DIR}/src/san_bump.c
    ${CMAKE_CURRENT_LIST_DIR}/src/hook.c
    ${CMAKE_CURRENT_LIST_DIR}/src/hpa.c
    ${CMAKE_CURRENT_LIST_DIR}/src/hpa_hooks.c
    ${CMAKE_CURRENT_LIST_DIR}/src/hpdata.c
    ${CMAKE_CURRENT_LIST_DIR}/src/inspect.c
    ${CMAKE_CURRENT_LIST_DIR}/src/large.c
    ${CMAKE_CURRENT_LIST_DIR}/src/log.c
    ${CMAKE_CURRENT_LIST_DIR}/src/malloc_io.c
    ${CMAKE_CURRENT_LIST_DIR}/src/mutex.c
    ${CMAKE_CURRENT_LIST_DIR}/src/nstime.c
    ${CMAKE_CURRENT_LIST_DIR}/src/pa.c
    ${CMAKE_CURRENT_LIST_DIR}/src/pa_extra.c
    ${CMAKE_CURRENT_LIST_DIR}/src/pai.c
    ${CMAKE_CURRENT_LIST_DIR}/src/pac.c
    ${CMAKE_CURRENT_LIST_DIR}/src/pages.c
    ${CMAKE_CURRENT_LIST_DIR}/src/peak_event.c
    ${CMAKE_CURRENT_LIST_DIR}/src/prof.c
    ${CMAKE_CURRENT_LIST_DIR}/src/prof_data.c
    ${CMAKE_CURRENT_LIST_DIR}/src/prof_log.c
    ${CMAKE_CURRENT_LIST_DIR}/src/prof_recent.c
    ${CMAKE_CURRENT_LIST_DIR}/src/prof_stats.c
    ${CMAKE_CURRENT_LIST_DIR}/src/prof_sys.c
    ${CMAKE_CURRENT_LIST_DIR}/src/psset.c
    ${CMAKE_CURRENT_LIST_DIR}/src/rtree.c
    ${CMAKE_CURRENT_LIST_DIR}/src/safety_check.c
    ${CMAKE_CURRENT_LIST_DIR}/src/sc.c
    ${CMAKE_CURRENT_LIST_DIR}/src/sec.c
    ${CMAKE_CURRENT_LIST_DIR}/src/stats.c
    ${CMAKE_CURRENT_LIST_DIR}/src/sz.c
    ${CMAKE_CURRENT_LIST_DIR}/src/tcache.c
    ${CMAKE_CURRENT_LIST_DIR}/src/test_hooks.c
    ${CMAKE_CURRENT_LIST_DIR}/src/thread_event.c
    ${CMAKE_CURRENT_LIST_DIR}/src/ticker.c
    ${CMAKE_CURRENT_LIST_DIR}/src/tsd.c
    ${CMAKE_CURRENT_LIST_DIR}/src/witness.c)

# Configure jemalloc_preamble.h
set(install_suffix "")
set(private_namespace "je_")
configure_file(${CMAKE_CURRENT_LIST_DIR}/include/jemalloc/internal/jemalloc_preamble.h.in
               ${CMAKE_CURRENT_LIST_DIR}/include/jemalloc/internal/jemalloc_preamble.h)

# Configure jemalloc_internal_defs.h
check_compilable("pause instruction" "" "__asm__ volatile(\"pause\");" HAS_CPU_SPINWAIT)

add_library(jemalloc STATIC ${JEMALLOC_SRCS})
target_compile_definitions(jemalloc PRIVATE _GNU_SOURCE)
target_compile_definitions(jemalloc PRIVATE _REENTRANT)
target_compile_definitions(jemalloc PRIVATE JEMALLOC_NO_PRIVATE_NAMESPACE)
target_include_directories(jemalloc PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")

unset(HAS_CPU_SPINWAIT CACHE)
