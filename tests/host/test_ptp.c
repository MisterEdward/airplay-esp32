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

static void put_id(uint8_t *p, uint64_t id) {
  for (int i = 7; i >= 0; i--) {
    p[i] = (uint8_t)id;
    id >>= 8;
  }
}

// Minimal Announce from `source` naming `gm` as grandmaster.
static void announce(uint64_t source, uint64_t gm) {
  uint8_t msg[64] = {0};
  msg[0] = PTP_MSG_ANNOUNCE;
  put_id(&msg[20], source);
  put_id(&msg[PTP_HEADER_SIZE + 19], gm);
  process_ptp_message(msg, sizeof(msg), false);
}

#define PHONE 0xa8817e25f4d60008ULL
#define NEWGM 0xcc6146fffefc2f15ULL
#define MAC   0x3c06307f6ae40008ULL

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

  // 5. The phone relays a new grandmaster's time and the anchor names that
  //    grandmaster (2026-09-23, after an airplane-mode reset): keep the lock
  //    on the phone instead of waiting for Sync from a clock that sends none.
  reset();
  memset(ptp.announced, 0, sizeof(ptp.announced));
  ptp.tracked_clock_id = PHONE;
  announce(PHONE, PHONE);
  feed(40, TRUE_OFFSET, wifi_delay);
  assert(ptp.locked);
  announce(PHONE, NEWGM); // phone now relays NEWGM: filter restarts
  assert(!ptp.locked && ptp.sample_count == 0);
  int64_t relayed = TRUE_OFFSET - 156750245000000LL;
  feed(40, relayed, wifi_delay);
  assert(ptp.locked);
  assert(ptp.step_count == 0); // re-locked on the announce, no step needed
  ptp_clock_set_master_clock_id(NEWGM);
  assert(ptp.tracked_clock_id == PHONE && ptp.expected_clock_id == PHONE);
  assert(ptp_clock_is_locked_to(NEWGM));
  assert(llabs(ptp.filtered_offset_ns - (relayed - 2000000LL)) < 10000000LL);

  // 6. The anchor names a grandmaster relayed by ANOTHER talker: switch to
  //    that talker rather than to a clock that never sends Sync.
  reset();
  memset(ptp.announced, 0, sizeof(ptp.announced));
  ptp.tracked_clock_id = PHONE;
  announce(PHONE, PHONE);
  announce(MAC, NEWGM);
  feed(40, TRUE_OFFSET, wifi_delay);
  ptp_clock_set_master_clock_id(NEWGM);
  assert(ptp.tracked_clock_id == MAC && ptp.expected_clock_id == MAC);

  // 7. Plain case unchanged: the anchor names the tracked source itself.
  reset();
  memset(ptp.announced, 0, sizeof(ptp.announced));
  ptp.tracked_clock_id = PHONE;
  announce(PHONE, PHONE);
  feed(40, TRUE_OFFSET, wifi_delay);
  ptp_clock_set_master_clock_id(PHONE);
  assert(ptp_clock_is_locked_to(PHONE) && ptp.sample_count > 0);

  printf("host ptp tests passed\n");
  return 0;
}
