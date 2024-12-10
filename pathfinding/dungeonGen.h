#pragma once
#include <cstddef> // size_t

void gen_drunk_dungeon(char *tiles, const size_t w, const size_t h, const size_t num_iter, const size_t max_excavations,
  bool draw = true);

void spill_drunk_water(char *tiles, const size_t w, const size_t h, const size_t num_iter, const size_t max_spills);
