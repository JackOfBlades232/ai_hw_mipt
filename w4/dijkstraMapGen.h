#pragma once
#include <string>
#include <vector>
#include <flecs.h>

namespace dmaps
{
void gen_adversary_approach_map(flecs::world &ecs, std::vector<float> &map, int my_team);
void gen_adversary_flee_map(flecs::world &ecs, std::vector<float> &map, int my_team);
void gen_hive_pack_map(flecs::world &ecs, std::vector<float> &map);

inline std::string gen_name(const char *base, int team) { return std::string(base) + std::to_string(team); }
}; // namespace dmaps
