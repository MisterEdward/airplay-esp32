// Exercise the production timing loop with deterministic PCM and clock fakes.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../../main/audio/audio_timing.c"

static int64_t now_us;
static unsigned flushes, returns, deferred_logs, late_logs, cancelled_logs;
static struct {
  audio_frame_header_t header;
  int16_t pcm[16];
} frames[8];
static unsigned queued, taken;
static audio_timing_t timing;
static audio_buffer_t buffer;
static audio_stats_t stats;
static audio_stream_t stream = {
    .type = AUDIO_STREAM_BUFFERED,
    .format = {.sample_rate = 44100, .frame_size = 1024, .channels = 2}};

int64_t esp_timer_get_time(void) {
  return now_us;
}
bool ntp_clock_is_locked(void) {
  return false;
}
int64_t ntp_clock_get_offset_ns(void) {
  return 0;
}
bool ptp_clock_is_locked(void) {
  return false;
}
bool ptp_clock_is_locked_to(uint64_t id) {
  (void)id;
  return false;
}
int64_t ptp_clock_get_offset_ns(void) {
  return 0;
}
uint64_t ptp_clock_get_tracked_clock_id(void) {
  return 0;
}
void ptp_clock_get_stats(ptp_stats_t *out) {
  memset(out, 0, sizeof(*out));
}
uint32_t audio_output_get_hardware_latency_us(void) {
  return 0;
}
uint32_t audio_output_get_underruns(void) {
  return 0;
}
bool audio_output_get_pipeline_us(int64_t *now, uint32_t *pipeline) {
  *now = now_us;
  *pipeline = 0;
  return true;
}
bool audio_stream_uses_buffer(audio_stream_type_t type) {
  return type == AUDIO_STREAM_BUFFERED;
}
void test_log(const char *tag, const char *format, ...) {
  (void)tag;
  if (strstr(format, "Deferred flush drop:"))
    deferred_logs++;
  if (strstr(format, "Late-frame drop:"))
    late_logs++;
  if (strstr(format, "Deferred flush cancelled:"))
    cancelled_logs++;
}
int audio_buffer_get_frame_count(audio_buffer_t *b) {
  (void)b;
  return (int)(queued - taken);
}
bool audio_buffer_take(audio_buffer_t *b, void **item, size_t *size,
                       TickType_t ticks) {
  (void)b;
  (void)ticks;
  if (taken == queued)
    return false;
  *item = &frames[taken++];
  *size = sizeof(frames[0]);
  return true;
}
void audio_buffer_return(audio_buffer_t *b, void *item) {
  (void)b;
  assert(item >= (void *)frames && item < (void *)(frames + 8));
  returns++;
}
void audio_buffer_flush(audio_buffer_t *b) {
  (void)b;
  flushes++;
  // As in the real pool, the checked-out frame is not part of the flush.
  while (taken < queued)
    memset(&frames[taken++], 0, sizeof(frames[0]));
}
bool audio_buffer_bulk_start_rtp(audio_buffer_t *b, uint32_t *rtp) {
  (void)b;
  (void)rtp;
  return false;
}
bool audio_buffer_peek_newest_rtp(audio_buffer_t *b, uint32_t *rtp) {
  (void)b;
  (void)rtp;
  return false;
}
static void setup(uint32_t from) {
  memset(&timing, 0, sizeof(timing));
  memset(&stats, 0, sizeof(stats));
  timing.playing = true;
  timing.playout_started = true;
  timing.deferred_flush_pending = true;
  timing.flush_from_ts = from;
  timing.flush_until_ts = from + 61 * 44100;
  now_us = 1000000;
  flushes = returns = deferred_logs = late_logs = cancelled_logs = 0;
  queued = taken = 0;
}
static void enqueue(uint32_t rtp) {
  if (queued == taken)
    queued = taken = 0;
  assert(queued < 8);
  frames[queued].header = (audio_frame_header_t){
      .rtp_timestamp = rtp, .samples_per_channel = 8, .channels = 2};
  for (unsigned i = 0; i < 16; i++)
    frames[queued].pcm[i] = 1234;
  queued++;
}
static bool read_media(void) {
  int16_t out[16] = {0};
  size_t count = audio_timing_read(&timing, &buffer, &stream, &stats, out, 8);
  if (count && timing.read_has_media) {
    assert(count == 8 && out[0] == 1234 && out[15] == 1234);
    return true;
  }
  return false;
}
static void test_stall_recovery(void) {
  setup(2063305733);
  // Normal media before from must keep the watchdog fresh for any duration.
  for (unsigned i = 0; i < 10; i++) {
    now_us += 1000000;
    enqueue(timing.flush_from_ts - 1000 + i * 8);
    assert(read_media() && timing.deferred_flush_pending && flushes == 0);
  }
  enqueue(timing.flush_from_ts);
  assert(!read_media() && timing.deferred_boundary_reached);
  now_us += 2000000;
  enqueue(timing.flush_from_ts + 8);
  assert(!read_media() && flushes == 0); // Strictly greater than 2 seconds.
  now_us++;
  // A stale anchor and continuity must not discard the rescued frame.
  timing.anchor_valid = timing.expected_rtp_valid = true;
  timing.expected_rtp = timing.flush_until_ts;
  enqueue(timing.flush_from_ts + 16);
  enqueue(timing.flush_from_ts + 24);
  assert(read_media());
  assert(flushes == 1 && cancelled_logs == 1 && queued == taken);
  assert(!timing.deferred_flush_pending && !timing.anchor_valid);
  assert(timing.expected_rtp == timing.flush_from_ts + 24);
  enqueue(timing.flush_from_ts + 32);
  assert(read_media() && flushes == 1); // No startup wait after recovery.
}
static void test_backwards_timeline(uint32_t from) {
  setup(from);
  enqueue(from - 44101);
  assert(read_media() && flushes == 0); // Before boundary is normal.
  enqueue(from - 8);
  assert(read_media() && timing.deferred_boundary_reached);
  enqueue(from - 44100);
  assert(read_media() && flushes == 0); // Exactly one second is allowed.
  enqueue(from - 44101);
  assert(read_media() && flushes == 1 && cancelled_logs == 1);
  assert(!timing.deferred_flush_pending);
}
static void test_normal_completion(void) {
  setup(UINT32_MAX - 44100);
  enqueue(timing.flush_from_ts);
  assert(!read_media());
  enqueue(timing.flush_until_ts - 1);
  assert(!read_media());
  uint32_t rtp = timing.flush_until_ts;
  // The old anchor remains valid when the declared endpoint really arrives.
  timing.anchor_valid = true;
  timing.anchor_rtp_time = rtp;
  timing.anchor_local_time_ns = (now_us + PIPELINE_LATENCY_US) * 1000;
  enqueue(rtp);
  assert(read_media() && timing.anchor_valid);
  assert(!timing.deferred_flush_pending && flushes == 0);
}
static void test_pause_and_empty(void) {
  setup(100000);
  enqueue(timing.flush_from_ts);
  assert(!read_media());
  audio_timing_set_playing(&timing, false);
  now_us += 10000000;
  assert(!read_media() && flushes == 0);
  audio_timing_set_playing(&timing, true);
  enqueue(timing.flush_from_ts + 8);
  assert(!read_media() && flushes == 0);
  now_us += 3000000;
  assert(!read_media() && flushes == 0); // Empty input alone is not recovery.
  enqueue(timing.flush_from_ts + 16);
  assert(read_media() && flushes == 1);
}
static void test_pending_recovery(void) {
  setup(100000);
  timing.anchor_valid = true;
  timing.anchor_rtp_time = timing.flush_from_ts - 8;
  timing.anchor_local_time_ns = (now_us + 5000000) * 1000;
  uint8_t pending[sizeof(frames[0])];
  timing.pending_frame = pending;
  timing.pending_frame_capacity = sizeof(pending);
  enqueue(timing.flush_from_ts - 8);
  assert(!read_media() && timing.pending_valid);
  now_us += 2000001;
  assert(!read_media() && flushes == 0); // Just one held early frame.
  enqueue(timing.flush_from_ts);
  assert(read_media() && flushes == 1 && !timing.pending_valid);
  assert(returns == 1); // Pending memory is never returned to the PCM pool.
}

static void test_drop_diagnostics(void) {
  setup(100000);
  for (unsigned i = 0; i < 205; i++) {
    enqueue(timing.flush_from_ts + i * 8);
    assert(!read_media());
  }
  assert(deferred_logs == 3 && timing.deferred_dropped == 205);
  setup(100000);
  timing.deferred_flush_pending = false;
  timing.anchor_valid = true;
  for (unsigned i = 0; i < 205; i++) {
    enqueue(i * 8);
    assert(!read_media());
  }
  assert(late_logs == 3 && stats.late_frames == 205);
}
int main(void) {
  test_stall_recovery();
  test_backwards_timeline(2063305733);
  test_backwards_timeline(100); // Backwards timeline across RTP wraparound.
  test_normal_completion();
  test_pause_and_empty();
  test_pending_recovery();
  test_drop_diagnostics();
  puts("timing tests passed");
  return 0;
}
