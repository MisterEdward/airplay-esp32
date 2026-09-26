#pragma once
// Scripted RNG: bytes queued with fake_random_queue() come out first (so a
// test can pin the salt and secret), then a fixed pseudo-random stream.
#include <stddef.h>
#include <stdint.h>

void fake_random_queue(const uint8_t *bytes, size_t len);
void esp_fill_random(void *buf, size_t len);
