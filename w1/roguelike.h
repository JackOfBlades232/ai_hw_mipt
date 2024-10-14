#pragma once

#include <flecs.h>

// Settings for args
enum DemoType
{
  DEMO_NEW_ENEMIES = 0b01,
  DEMO_NEW_ALLY = 0b10,
  DEMO_NEW_ENEMIES_AND_ALLIES = DEMO_NEW_ALLY | DEMO_NEW_ENEMIES,
  DEMO_INTIAL = 0b100,
  DEMO_ALL_COMBAT = DEMO_INTIAL | DEMO_NEW_ENEMIES_AND_ALLIES,
  DEMO_CRAFTER = 0b1000,
  DEMO_ALL = DEMO_CRAFTER | DEMO_ALL_COMBAT
};

inline float draw_scale = 0.3f;
inline DemoType demo_type = DEMO_ALL;

void init_roguelike(flecs::world &ecs);
void process_turn(flecs::world &ecs);
void print_stats(flecs::world &ecs);
