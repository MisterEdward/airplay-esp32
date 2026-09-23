// Host test for the PTP offset filter (main/network/ptp_clock.c), compiled
// against minimal fakes.  Covers the master-timescale step: follow a real
// step, never a congested link.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int64_t fake_now_us = 1000000;

#include "../../main/network/ptp_clock.c"

#define TRUE_OFFSET 154003110392833LL // ns, as measured on the board

static void reset(void) {
  memset(&ptp, 0, sizeof(ptp));
  fake_now_us = 1000000;
}

// Feed n Sync samples at 8 Hz, offset = base - delay, delay from fn.
static void feed(int n, int64_t base, int64_t (*delay)(int)) {
  for (int i = 0; i < n; i++) {
    fake_now_us += 125000;
    update_offset(base - delay(i));
  }
}

static int64_t wifi_delay(int i) {
  return 2000000LL + (i * 7919 % 5) * 1000000LL;
}
static int64_t congested(int i) {
  // 200-900 ms queueing, varying sample to sample like the PS5 evening.
  return 200000000LL + (int64_t)((i * 104729) % 700) * 1000000LL;
}
static int64_t congested_steady(int i) {
  // A sustained 600 ms queue that agrees with itself to 10 ms.
  return 600000000LL + (int64_t)(i % 10) * 1000000LL;
}

int main(void) {
  // 1. Lock, then an 11.37 s step (the 2026-09-23 incident): followed.
  reset();
  feed(40, TRUE_OFFSET, wifi_delay);
  assert(ptp.locked);
  int64_t stepped = TRUE_OFFSET - 11371810000LL;
  feed(20, stepped, wifi_delay);
  assert(ptp.step_count == 1);
  int64_t err = ptp.filtered_offset_ns - (stepped - 2000000LL);
  assert(llabs(err) < 10000000LL);
  assert(ptp.locked);
  printf("step followed: residual %lld us\n", (long long)(err / 1000));

  // 2. Varying congestion (hundreds of ms): rejected, never followed.
  reset();
  feed(40, TRUE_OFFSET, wifi_delay);
  int64_t before = ptp.filtered_offset_ns;
  feed(200, TRUE_OFFSET, congested);
  assert(ptp.step_count == 0);
  assert(llabs(ptp.filtered_offset_ns - before) < 5000000LL);

  // 3. Even a steady 600 ms queue is under the 1 s floor: not followed.
  reset();
  feed(40, TRUE_OFFSET, wifi_delay);
  feed(200, TRUE_OFFSET, congested_steady);
  assert(ptp.step_count == 0);

  // 4. A short burst of a stepped timescale (under 1.5 s) is not enough.
  reset();
  feed(40, TRUE_OFFSET, wifi_delay);
  feed(8, stepped, wifi_delay);
  feed(10, TRUE_OFFSET, wifi_delay);
  assert(ptp.step_count == 0);

  printf("host ptp tests passed\n");
  return 0;
}
