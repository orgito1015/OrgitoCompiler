#!/usr/bin/env bash
# OrgitoCompiler test suite.
#
# Three kinds of checks:
#   run_output_test  - a valid program; stdout must match a golden file.
#   run_error_test   - an invalid program; must fail, and stderr must
#                       contain every given substring.
#   run_warning_test - a valid program (exit 0) whose stderr must still
#                       contain every given warning substring.
#
# Deliberately avoids ever reading $? as a variable after a command (only
# `if CMD; then ... else ... fi`) - unreliable on at least one of this
# project's development machines when invoked through certain WSL setups.

cd "$(dirname "$0")/.."

BIN="./bin/orgitoc"
PASS=0
FAIL=0
TMP_OUT=$(mktemp)
TMP_ERR=$(mktemp)
TMP_OUT2=$(mktemp)
trap 'rm -f "$TMP_OUT" "$TMP_ERR" "$TMP_OUT2"' EXIT

pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

fail() {
    local name="$1"
    shift
    echo "  FAIL: $name"
    for line in "$@"; do
        echo "        $line"
    done
    FAIL=$((FAIL + 1))
}

run_output_test() {
    local name="$1" src="$2" expected="$3"
    if "$BIN" "$src" >"$TMP_OUT" 2>"$TMP_ERR"; then
        if diff -q "$expected" "$TMP_OUT" >/dev/null 2>&1; then
            pass "$name"
        else
            fail "$name" "output did not match $expected" "--- diff (expected vs actual) ---" "$(diff "$expected" "$TMP_OUT")"
        fi
    else
        fail "$name" "expected success but the program failed" "$(cat "$TMP_ERR")"
    fi
}

run_error_test() {
    local name="$1" src="$2"
    shift 2
    if "$BIN" "$src" >"$TMP_OUT" 2>"$TMP_ERR"; then
        fail "$name" "expected failure but the program succeeded"
        return
    fi
    local all_ok=1
    local missing=()
    for substr in "$@"; do
        if ! grep -qF "$substr" "$TMP_ERR"; then
            all_ok=0
            missing+=("missing expected substring: $substr")
        fi
    done
    if [ "$all_ok" -eq 1 ]; then
        pass "$name"
    else
        fail "$name" "${missing[@]}" "--- actual stderr ---" "$(cat "$TMP_ERR")"
    fi
}

run_warning_test() {
    local name="$1" src="$2"
    shift 2
    if "$BIN" "$src" >"$TMP_OUT" 2>"$TMP_ERR"; then
        local all_ok=1
        local missing=()
        for substr in "$@"; do
            if ! grep -qF "$substr" "$TMP_ERR"; then
                all_ok=0
                missing+=("missing expected warning substring: $substr")
            fi
        done
        if [ "$all_ok" -eq 1 ]; then
            pass "$name"
        else
            fail "$name" "${missing[@]}" "--- actual stderr ---" "$(cat "$TMP_ERR")"
        fi
    else
        fail "$name" "expected success (warnings only) but the program failed" "$(cat "$TMP_ERR")"
    fi
}

echo "== output tests (examples/) =="
run_output_test hello         examples/hello.oc         tests/expected/hello.expected
run_output_test structs       examples/structs.oc       tests/expected/structs.expected
run_output_test pointers      examples/pointers.oc      tests/expected/pointers.expected
run_output_test arrays_sizeof examples/arrays_sizeof.oc tests/expected/arrays_sizeof.expected
run_output_test malloc_free   examples/malloc_free.oc   tests/expected/malloc_free.expected
run_output_test strings       examples/strings.oc       tests/expected/strings.expected
run_output_test switch        examples/switch.oc        tests/expected/switch.expected
run_output_test casts         examples/casts.oc         tests/expected/casts.expected
run_output_test globals       examples/globals.oc       tests/expected/globals.expected
run_output_test include       examples/include_main.oc  tests/expected/include.expected

echo "== optimizer: -O0 and -O1 must produce identical stdout =="
for f in structs pointers arrays_sizeof malloc_free strings switch casts globals; do
    src="examples/$f.oc"
    if "$BIN" -O0 "$src" >"$TMP_OUT" 2>/dev/null && "$BIN" -O1 "$src" >"$TMP_OUT2" 2>/dev/null \
       && diff -q "$TMP_OUT" "$TMP_OUT2" >/dev/null 2>&1; then
        pass "optimizer-parity-$f"
    else
        fail "optimizer-parity-$f" "-O0 and -O1 produced different stdout"
    fi
done

echo "== optimizer: -O1 must visibly shrink an obviously-foldable program =="
n0=$("$BIN" -O0 --emit-bytecode tests/optimize_fold.oc 2>/dev/null | grep -vc '^;')
n1=$("$BIN" -O1 --emit-bytecode tests/optimize_fold.oc 2>/dev/null | grep -vc '^;')
if [ "$n1" -lt "$n0" ]; then
    pass "optimizer-fold-shrink ($n0 -> $n1 instructions)"
else
    fail "optimizer-fold-shrink" "-O1 ($n1 instructions) did not shrink versus -O0 ($n0 instructions)"
fi

echo "== error tests (tests/errors/) =="
run_error_test err_type_mismatch tests/errors/err_type_mismatch.oc \
    "cannot initialize 'int' with a value of type 'char*'"
run_error_test err_bad_member tests/errors/err_bad_member.oc \
    "struct 'Point' has no member 'z'"
run_error_test err_lvalue tests/errors/err_lvalue.oc \
    "left-hand side of assignment is not assignable"
run_error_test err_multi tests/errors/err_multi.oc \
    "use of undeclared identifier 'y'" \
    "struct 'Point' has no member 'z'" \
    "expects 2 arguments, got 3"
run_error_test err_double_free tests/errors/err_double_free.oc \
    "double free or invalid free"

echo "== warning tests (tests/warnings/) =="
run_warning_test warn_unused_unreachable tests/warnings/warn_unused_unreachable.oc \
    "unused variable 'unused_var'" \
    "unreachable code"

echo "== --emit-ast / --emit-bytecode smoke tests =="
if "$BIN" --emit-ast examples/structs.oc >"$TMP_OUT" 2>"$TMP_ERR"; then
    pass "emit-ast-smoke"
else
    fail "emit-ast-smoke" "$(cat "$TMP_ERR")"
fi
if "$BIN" --emit-bytecode examples/structs.oc >"$TMP_OUT" 2>"$TMP_ERR"; then
    pass "emit-bytecode-smoke"
else
    fail "emit-bytecode-smoke" "$(cat "$TMP_ERR")"
fi

echo ""
echo "passed: $PASS, failed: $FAIL"
if [ "$FAIL" -eq 0 ]; then
    exit 0
else
    exit 1
fi
