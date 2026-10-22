#pragma once
#include <stdint.h>

/* Powers the touch rails, logs the CPU clock; returns the raw RTC bytes
 * (sec, min, hour, weekday, day, month, year, next) little-endian. */
int pmic_read(unsigned reg);
int pmic_write(unsigned reg, uint8_t val);
uint64_t pmic_init(void (*kv)(const char *, uint64_t), void (*out)(const char *));
