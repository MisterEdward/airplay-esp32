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
# SRP (pair-setup) against mbedtls' bignum from the ESP-IDF tree.
MBEDTLS_DIR="${MBEDTLS_DIR:-$HOME/.platformio/packages/framework-espidf/components/mbedtls/mbedtls}"
if [ -d "$MBEDTLS_DIR/library" ]; then
  L="$MBEDTLS_DIR/library"
  $CC $FLAGS -Wno-unused-function -I tests/host/fakes_hap -I main/hap \
    -I "$MBEDTLS_DIR/include" -I "$L" tests/host/test_srp.c \
    "$L/bignum.c" "$L/bignum_core.c" "$L/constant_time.c" \
    "$L/platform_util.c" "$L/platform.c" -o "$out/test_srp"
  "$out/test_srp"
else
  echo "srp: skipped (no mbedtls at $MBEDTLS_DIR)"
fi
