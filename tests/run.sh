#!/bin/sh
# Host tests (Linux/WSL, native compiler). No head unit, no USB devices, no private build inputs needed.
# Prints failures and one summary line per suite; full output with VERBOSE=1.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
cc=${CC:-cc}
flags="-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined"
$cc $flags -fPIC -shared -pthread -o "$out/libfakevmostclient.so" "$here/fake_vmostclient.c"
status=0
for test in usb_discovery_test aa_input_test vmost_listen_test; do
    # shellcheck disable=SC2086
    $cc $flags -pthread -o "$out/$test" "$here/$test.c" -ldl
    arg=; [ "$test" = vmost_listen_test ] && arg=$out/libfakevmostclient.so
    [ "$test" = usb_discovery_test ] && arg=$here/../runtime/aa/usb-device.conf
    "$out/$test" ${arg:+"$arg"} > "$out/$test.log" 2>&1 || status=1
    if [ "${VERBOSE:-0}" = 1 ]; then cat "$out/$test.log"; fi
    printf '%-20s %s\n' "$test" "$(grep -E '^(OK|FAILED):' "$out/$test.log" || echo 'no summary (crashed?)')"
    grep -E '^FAIL |runtime error|ERROR: AddressSanitizer|ThreadSanitizer' "$out/$test.log" || :
done
# The vmost ring is shared between the library's thread and ours: run that suite under ThreadSanitizer too.
tsan="-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -fsanitize=thread"
$cc $tsan -fPIC -shared -pthread -o "$out/libfakevmostclient-tsan.so" "$here/fake_vmostclient.c"
$cc $tsan -pthread -o "$out/vmost_listen_test-tsan" "$here/vmost_listen_test.c" -ldl
"$out/vmost_listen_test-tsan" "$out/libfakevmostclient-tsan.so" > "$out/tsan.log" 2>&1 || status=1
printf '%-20s %s\n' "vmost_listen (tsan)" "$(grep -E '^(OK|FAILED):' "$out/tsan.log" || echo 'no summary (crashed?)')"
grep -E '^FAIL |WARNING: ThreadSanitizer' "$out/tsan.log" && status=1 || :
# The real tool against the fake library: passive run, one shadow, output shape.
$cc $flags -pthread -o "$out/vmost-listen" "$here/../src/vmost-listen.c" -ldl
FAKE_VMOST_COUNT=2 "$out/vmost-listen" -s 1 -l "$out/libfakevmostclient.so" E5.00 > "$out/tool.log" 2>&1 || status=1
if grep -q 'VMOST_SHADOW_REGISTER fblock=0xe5 inst=0x00 rc=0' "$out/tool.log" && [ "$(grep -c '^VMOST_RX ' "$out/tool.log")" = 2 ] &&
   grep -q 'VMOST_DISCONNECT rc=0 received=2 lost=0' "$out/tool.log"; then
    printf '%-20s %s\n' vmost-listen 'OK: tool run against the fake library'
else
    printf '%-20s %s\n' vmost-listen 'FAILED: unexpected tool output'; cat "$out/tool.log"; status=1
fi
"$out/vmost-listen" -s 1 BAD > /dev/null 2>&1 && { echo 'FAILED: bad shadow accepted'; status=1; } || :
exit $status
