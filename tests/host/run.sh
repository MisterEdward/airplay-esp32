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
