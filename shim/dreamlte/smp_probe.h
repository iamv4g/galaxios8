#pragma once
#include <stdint.h>

void mngs_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *));
void psci_early(void (*kv)(const char *, uint64_t), void (*out)(const char *));
void mngs_diag(void (*kv)(const char *, uint64_t), void (*out)(const char *));
void smp_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *));
