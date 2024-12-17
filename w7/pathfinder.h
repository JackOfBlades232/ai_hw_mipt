#pragma once
#include <flecs.h>
#include <vector>

#include "ecsTypes.h"

struct PortalConnection
{
  size_t connIdx;
  size_t sectorIdx;
  float score;
};

struct PathPortal
{
  size_t startX, startY;
  size_t endX, endY;
  std::vector<PortalConnection> conns;
};

struct DungeonPortals
{
  size_t tileSplit;
  std::vector<PathPortal> portals;
  std::vector<std::vector<size_t>> tilePortalsIndices;
  std::vector<std::vector<std::vector<float>>> tilePortalsDmaps;
  std::vector<size_t> portalsToHighlight;
};

struct PathSearchRes
{
  std::vector<Position> path{};
  std::vector<size_t> portalIndices{};
};

void prebuild_map(flecs::world &ecs);

PathSearchRes construct_path_hierarchical(flecs::world &ecs, Position from, Position to);

template <typename T>
inline size_t coord_to_idx(T x, T y, size_t w)
{
  return size_t(y) * w + size_t(x);
}
