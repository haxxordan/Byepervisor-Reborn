#pragma once

#include <stdint.h>

typedef struct {
  uint8_t *mapping;
  uint8_t *page;
  uint64_t pa;
  uint64_t dmap;
  int locked;
} carrier1270_t;

int carrier1270_acquire(carrier1270_t *carrier, uint64_t dmap);
int carrier1270_translate(const carrier1270_t *carrier, uint64_t *pa);
int carrier1270_release(carrier1270_t *carrier);
