#!/bin/sh
# Runs unchanged on Linux CI or in the disposable Linux VM. No root required.
set -eu
B=$(cd "$1" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/sd-app-tests.XXXXXX")
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/apps"
cp -R "$B/apps/hello" "$T/apps/hello"
R="$B/sd_app_runner"
passed=0
expect() {
    wanted=$1; label=$2; shift 2
    if "$@" >"$T/out" 2>"$T/err"; then actual=0; else actual=$?; fi
    if [ "$actual" -ne "$wanted" ]; then
        echo "FAIL: $label (expected $wanted, got $actual)"
        cat "$T/out" "$T/err"
        exit 1
    fi
    passed=$((passed+1))
    echo "PASS: $label"
}
run() { "$R" --apps "$T/apps" --cache "$T/cache" --data "$T/data" "$@"; }
manifest() {
    cat >"$T/apps/hello/app.json" <<EOF
{"schema":1,"id":"hello","name":"Hello Goggles","version":"0.1.0","entry":"bin/hello","args":$1}
EOF
}
restore() {
    cp "$B/apps/hello/app.json" "$T/apps/hello/app.json"
    cp "$B/apps/hello/bin/hello" "$T/apps/hello/bin/hello"
}

expect 0 'SHA-256 known vectors' "$B/hash_check"
expect 0 'valid static executable' run validate hello
expect 0 'catalog discovery' run list
grep -q '"id":"hello"' "$T/out"
mkdir -p "$T/search-only/apps"
cp -R "$B/apps/hello" "$T/search-only/apps/hello"
cp "$R" "$T/runner"
chmod 755 "$T" "$T/runner" "$T/search-only/apps" "$T/search-only/apps/hello" \
    "$T/search-only/apps/hello/bin" "$T/search-only/apps/hello/assets"
chmod 644 "$T/search-only/apps/hello/app.json" "$T/search-only/apps/hello/bin/hello" \
    "$T/search-only/apps/hello/assets/message.txt"
chmod 111 "$T/search-only"
expect 0 'root traversal needs search permission only' "$B/fixture" unprivileged \
    "$T/runner" --apps "$T/search-only/apps" validate hello
chmod 700 "$T" "$T/search-only"
expect 0 'stage package' run stage hello
staged=$(cat "$T/out")
[ -x "$staged/bin/hello" ] && [ ! -x "$staged/assets/message.txt" ]
expect 0 'reuse identical cache' run stage hello
[ "$staged" = "$(cat "$T/out")" ]
expect 0 'run and save data' run run hello
grep -q 'Hello Goggles completed.' "$T/cache/logs/last-run.log"
grep -q 'SD package' "$T/cache/logs/last-run.log"
[ -f "$T/data/hello/launches.txt" ]
expect 0 'clear inherited preload and library paths' env LD_PRELOAD=/no-such-library LD_LIBRARY_PATH=/no-such-path \
    "$R" --apps "$T/apps" --cache "$T/cache" --data "$T/data" run hello
manifest '["$(touch SHOULD_NOT_EXIST); *", "two words", "quote\"slash\\", "\ud83e\udd7d"]'
expect 0 'literal argv including shell metacharacters and Unicode' run run hello
grep -Fq '$(touch SHOULD_NOT_EXIST); *' "$T/cache/logs/last-run.log"
[ ! -e SHOULD_NOT_EXIST ] && [ ! -e "$staged/SHOULD_NOT_EXIST" ]

cat >"$T/apps/hello/app.json" <<'EOF'
{"schema":1,"id":"hello","id":"hello","name":"x","version":"1","entry":"bin/hello"}
EOF
expect 125 'duplicate manifest field rejected' run validate hello
manifest '[]'
sed 's/"schema":1/"schema":2/' "$T/apps/hello/app.json" >"$T/change"
cp "$T/change" "$T/apps/hello/app.json"
expect 125 'unknown schema rejected' run validate hello
manifest '[]'
sed 's|bin/hello|../hello|' "$T/apps/hello/app.json" >"$T/change"
cp "$T/change" "$T/apps/hello/app.json"
expect 125 'traversal entry rejected' run validate hello
manifest '[]'
sed 's|bin/hello|/bin/hello|' "$T/apps/hello/app.json" >"$T/change"
cp "$T/change" "$T/apps/hello/app.json"
expect 125 'absolute entry rejected' run validate hello
manifest '[]'
sed 's/"id":"hello"/"id":"other"/' "$T/apps/hello/app.json" >"$T/change"
cp "$T/change" "$T/apps/hello/app.json"
expect 125 'directory and manifest ids must match' run validate hello
manifest '["\u0000"]'
expect 125 'embedded NUL rejected' run validate hello
manifest '["\ud800"]'
expect 125 'unpaired surrogate rejected' run validate hello
printf '{"schema":1}' >"$T/apps/hello/app.json"
expect 125 'missing fields rejected' run validate hello
restore
printf '\003\000' | dd of="$T/apps/hello/bin/hello" bs=1 seek=18 conv=notrunc 2>/dev/null
expect 125 'wrong ELF architecture rejected' run validate hello
restore
printf '\003\000\000\000' | dd of="$T/apps/hello/bin/hello" bs=1 seek=64 conv=notrunc 2>/dev/null
expect 125 'ELF interpreter rejected' run validate hello
head -c 20 "$B/apps/hello/bin/hello" >"$T/apps/hello/bin/hello"
expect 125 'truncated ELF rejected' run validate hello
restore
ln -s /etc/passwd "$T/apps/hello/assets/link"
expect 125 'symlink asset rejected' run validate hello
rm "$T/apps/hello/assets/link"
ln "$T/apps/hello/assets/message.txt" "$T/apps/hello/assets/hardlink"
expect 125 'hard-linked file rejected' run validate hello
rm "$T/apps/hello/assets/hardlink"
mkfifo "$T/apps/hello/assets/fifo"
expect 125 'FIFO rejected without blocking' run validate hello
rm "$T/apps/hello/assets/fifo"
truncate -s 67108864 "$T/apps/hello/assets/oversize"
expect 125 'package byte budget enforced' run validate hello
rm "$T/apps/hello/assets/oversize"
ln -s "$T/apps" "$T/source-link"
expect 125 'symlink in source root rejected' "$R" --apps "$T/source-link" validate hello
expect 125 'unsafe selection rejected' run stage ../hello
expect 125 'source/cache overlap rejected' "$R" --apps "$T/apps" --cache "$T/apps/cache" stage hello
expect 125 'missing SD directory reported' "$R" --apps "$T/missing" list

restore
expect 0 'find baseline cache' run stage hello
staged=$(cat "$T/out")
printf 'corrupt' >"$staged/assets/message.txt"
expect 125 'modified cache never executed' run run hello
expect 0 'clean cache only' run clean hello
[ -f "$T/data/hello/launches.txt" ] && [ ! -d "$staged" ]
mkdir -p "$T/cache/packages/hello/.partial-interrupted/assets" "$T/cache/packages/hello/keep-me"
echo partial >"$T/cache/packages/hello/.partial-interrupted/assets/file"
expect 0 'interrupted staging removed before retry' run stage hello
[ ! -d "$T/cache/packages/hello/.partial-interrupted" ]
expect 0 'cleanup preserves unrelated directories' run clean hello
[ -d "$T/cache/packages/hello/keep-me" ]
truncate -s 268435457 "$T/cache/packages/too-large"
expect 125 'total cache byte budget enforced' run stage hello
rm "$T/cache/packages/too-large"

cp "$B/fixture" "$T/apps/hello/bin/hello"
manifest '["exit7"]'
expect 7 'app exit status preserved' run run hello
manifest '["crash"]'
expect 139 'app crash reported' run run hello
manifest '["ignoreterm"]'
expect 124 'hung app escalates to kill after timeout' run --timeout 1 run hello
manifest '["closeout"]'
expect 124 'closed output does not prevent timeout' run --timeout 1 run hello
manifest '["flood"]'
expect 0 'large output drained without deadlock' run run hello
[ "$(wc -c <"$T/cache/logs/last-run.log")" -eq 1048576 ]
manifest '["child"]'
expect 0 'child process group cleaned on exit' run run hello
child=$(cat "$T/data/hello/child.pid")
if [ -e "/proc/$child/stat" ]; then grep -q ' Z ' "/proc/$child/stat"; fi
manifest '["sleep"]'
"$R" --apps "$T/apps" --cache "$T/cache" --data "$T/data" run hello >"$T/background.out" 2>"$T/background.err" &
leader=$!
sleep 1
expect 125 'concurrent runner blocked' run run hello
expect 125 'cleanup blocked while app is running' run clean hello
kill -TERM "$leader"
expect 143 'runner cancellation exits after terminating app' wait "$leader"
restore
expect 0 'launch still works after crash and cancellation' run run hello
expect 0 'repeat launch' run run hello
expect 0 'final cleanup' run clean hello
[ -f "$T/data/hello/launches.txt" ]
echo "SD_APPS_TESTS_PASSED=$passed"
