#!/usr/bin/env bash
# Memory-error / leak gate. Runs the engine under Valgrind memcheck and asserts no invalid
# read/write, no invalid/double free, and no DEFINITE leak.
#
# Why this and not -fsanitize=address: Valgrind instruments the exact artifact the parity
# gate ships, with no sanitizer rebuild. `./build.sh test` already runs the unit suite under
# ASan+UBSan, but that is a DIFFERENT binary -- built at -O1 with instrumentation -- and it
# drives the engine through the test harness, not through the UCI front end. This drives
# the shipped release binary the way an operator does.
#
# Uninitialized-value checking is DISABLED (--undef-value-errors=no): the NNUE eval is heavy
# SSE and reads whole vector lanes including padding, which memcheck reports as false
# "uninitialised value" use. Leak / invalid-access / bad-free detection is unaffected and
# reliable. (The race half is `./build.sh tsan`, not this: the TT has benign data races by
# design that a race gate would flag.)
#
# ON `Threads`: the loop below runs a REAL concurrency session. This note used to say the
# opposite -- that the search was single-threaded and `Threads` accepted and ignored, so the
# second session was an option-path test "until the search is actually driven across
# workers". That day came: Lazy-SMP is wired and `Threads` builds a worker set
# (docs/04-multithreading.md). The claim survived because nothing ran this script, which is
# the whole argument for giving a tool a lane.
#
# Usage: valgrind.sh <binary>   (run from the dir holding the net: mcfish -> resources/)
set -u

BIN="${1:?usage: valgrind.sh <binary>   (run from the dir holding the net: mcfish -> resources/)}"
DEPTH="${VG_DEPTH:-9}"            # search depth per session (kept short; memcheck is ~20-50x)
WATCHDOG="${VG_WATCHDOG:-600}"    # seconds per valgrind session
THREADS=(1 2)

command -v valgrind >/dev/null 2>&1 || { echo "valgrind: SKIP -- valgrind not installed" >&2; exit 0; }

fail() { echo "valgrind: FAIL -- $*" >&2; exit 1; }

for tc in "${THREADS[@]}"; do
    echo "valgrind: memcheck session Threads=${tc} (go depth ${DEPTH})"
    log="$(mktemp)"
    printf 'uci\nsetoption name Threads value %d\nucinewgame\nposition startpos\ngo depth %d\nposition startpos moves e2e4 e7e5 g1f3\ngo depth %d\nquit\n' \
        "${tc}" "${DEPTH}" "${DEPTH}" \
        | timeout "${WATCHDOG}" valgrind \
            --tool=memcheck \
            --leak-check=full \
            --errors-for-leak-kinds=definite \
            --undef-value-errors=no \
            --error-exitcode=99 \
            "${BIN}" >/dev/null 2>"${log}"
    rc=$?
    if (( rc == 124 )); then rm -f "${log}"; fail "Threads=${tc} timed out under valgrind (${WATCHDOG}s)"; fi
    if (( rc == 99 )); then
        grep -iE "Invalid (read|write|free)|definitely lost|ERROR SUMMARY" "${log}" | head -12 >&2
        rm -f "${log}"
        fail "Threads=${tc} memcheck reported a memory error / definite leak"
    fi
    if (( rc != 0 )); then
        tail -8 "${log}" >&2
        rm -f "${log}"
        fail "Threads=${tc} exited ${rc} under valgrind"
    fi
    # Defensive: assert the summary is actually clean even if the exit code slipped.
    #
    # TWO spellings are clean, and accepting only the first is a FALSE FAILURE.
    # Valgrind prints a LEAK SUMMARY -- and with it `definitely lost: 0 bytes in 0
    # blocks` -- only when something is still allocated at exit. When every block
    # was freed it prints `All heap blocks were freed -- no leaks are possible`
    # and NO summary at all. mcfish takes that second path (61 allocs, 61 frees on
    # a depth-6 run), so checking for the summary line alone reported a definite
    # leak on a run valgrind had just declared leak-free.
    if ! grep -q "All heap blocks were freed" "${log}" \
       && ! grep -q "definitely lost: 0 bytes in 0 blocks" "${log}"; then
        grep -i "definitely lost" "${log}" | head -3 >&2
        rm -f "${log}"
        fail "Threads=${tc} has a definite leak"
    fi
    rm -f "${log}"
done

echo "valgrind: OK (memcheck clean across Threads {${THREADS[*]}}: no leak / bad access)"
