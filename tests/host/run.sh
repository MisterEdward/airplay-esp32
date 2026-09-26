#!/bin/sh
# Build and run the host-side unit tests for the pure modules (no ESP-IDF).
set -eu
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
CC="${CC:-cc}"
FLAGS="-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined"
$CC $FLAGS -I main -I main/audio -I main/network \
  tests/host/test_core.c \
  main/audio/audio_envelope.c main/network/log_journal.c \
  main/source_volume.c -lm -o "$out/test_core"
"$out/test_core"
if [ -f tests/host/test_timing.c ]; then
  $CC $FLAGS -I main -I main/audio -I main/network -I tests/host/fakes \
    tests/host/test_timing.c -lm -o "$out/test_timing"
  "$out/test_timing"
fi
$CC $FLAGS -Wno-unused-function -Wno-unused-parameter -I tests/host/fakes_ptp -I main/network \
  tests/host/test_ptp.c -o "$out/test_ptp"
"$out/test_ptp"
# SRP pair-setup: needs the software mbedtls bignum from the IDF tree.
MBEDTLS="${MBEDTLS_DIR:-$HOME/.platformio/packages/framework-espidf/components/mbedtls/mbedtls}"
if [ -f "$MBEDTLS/library/bignum.c" ]; then
  for f in bignum bignum_core constant_time platform_util sha512; do
    $CC -std=c11 -O2 -I "$MBEDTLS/include" -I "$MBEDTLS/library" \
      -c "$MBEDTLS/library/$f.c" -o "$out/mbed_$f.o"
  done
  $CC $FLAGS -I tests/host/fakes_srp -I tests/host -I main/hap \
    -I "$MBEDTLS/include" tests/host/test_srp.c main/hap/srp.c \
    "$out"/mbed_*.o -o "$out/test_srp"
  "$out/test_srp"
else
  echo "test_srp: skipped (no mbedtls at $MBEDTLS)"
fi
