#pragma once
#include <stdint.h>

/* Powers the touch rails, logs the CPU clock; returns the raw RTC bytes
 * (sec, min, hour, weekday, day, month, year, next) little-endian. */
int pmic_read(unsigned reg);
int pmic_write(unsigned reg, uint8_t val);
uint64_t pmic_init(void (*kv)(const char *, uint64_t), void (*out)(const char *));

/* A53 clock through the APM firmware; lit_khz 0 only reports. rec gets
 * diagnostic words for the boot-args. */
void acpm_cpu_clocks(uint32_t lit_khz, void (*rec)(uint32_t));

/* Mali-G71 power-up probe (ID registers), then power-off; rec gets words. */
void gpu_probe(void (*rec)(uint32_t));
