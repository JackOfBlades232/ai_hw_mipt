#include "pathfinder.h"
#include "dungeonUtils.h"
#include "math.h"
#include <algorithm>
#include <cstddef>
#include <cfloat>

static constexpr size_t SPLIT_TILES = 10;
static constexpr float INVALID_TILE_VALUE = FLT_MAX;

std::vector<float> gen_sector_to_portal_dmap(const DungeonData &dd, const PathPortal &p, IVec2 coordBase, IVec2 coordCap)
{
  if (coordBase.x < 0)
    coordBase.x = 0;
  if (coordBase.y < 0)
    coordBase.y = 0;
  if (coordCap.x > dd.width)
    coordCap.x = dd.width;
  if (coordCap.y > dd.height)
    coordCap.y = dd.height;
  if (coordCap.x <= coordBase.x || coordCap.y <= coordBase.y)
    return {};

  std::vector<float> dmap{};

  IVec2 coordSize = coordCap - coordBase;
  dmap.resize(coordSize.x * coordSize.y);
  std::fill(dmap.begin(), dmap.end(), INVALID_TILE_VALUE);

  auto coordInBounds = [&](size_t x, size_t y) {
    return x >= coordBase.x && y >= coordBase.y && x < coordCap.x && y < coordCap.y;
  };
  auto getTile = [&](size_t x, size_t y) { return dd.tiles[y * dd.width + x]; };
  auto getMapAt = [&](size_t x, size_t y, float def) {
    if (coordInBounds(x, y) && getTile(x, y) == dungeon::floor)
    {
      y -= coordBase.y;
      x -= coordBase.x;
      return dmap[y * coordSize.x + x];
    }
    return def;
  };
  auto setMapAt = [&](size_t x, size_t y, float val) {
    if (coordInBounds(x, y) && getTile(x, y) == dungeon::floor)
    {
      y -= coordBase.y;
      x -= coordBase.x;
      dmap[y * coordSize.x + x] = val;
    }
  };

  for (size_t y = p.startY; y <= p.endY; ++y)
    for (size_t x = p.startX; x <= p.endX; ++x)
      setMapAt(x, y, 0.f);

  bool done = false;
  auto getMinNei = [&](size_t x, size_t y) {
    float val = getMapAt(x, y, INVALID_TILE_VALUE);
    val = std::min(val, getMapAt(x - 1, y + 0, val));
    val = std::min(val, getMapAt(x + 1, y + 0, val));
    val = std::min(val, getMapAt(x + 0, y - 1, val));
    val = std::min(val, getMapAt(x + 0, y + 1, val));
    return val;
  };
  while (!done)
  {
    done = true;
    for (size_t y = coordBase.y; y < coordCap.y; ++y)
      for (size_t x = coordBase.x; x < coordCap.x; ++x)
      {
        if (getTile(x, y) != dungeon::floor)
          continue;
        const float myVal = getMapAt(x, y, INVALID_TILE_VALUE);
        const float minVal = getMinNei(x, y);
        if (minVal < myVal - 1.f)
        {
          setMapAt(x, y, minVal + 1.f);
          done = false;
        }
      }
  }

  return dmap;
}

template <class T>
float heuristic(T lhs, T rhs)
{
  return sqrtf(sqr(float(lhs.x - rhs.x)) + sqr(float(lhs.y - rhs.y)));
};

template <class T>
IVec2 to_ivec2(T p)
{
  return {int(floorf(float(p.x))), int(floorf(float(p.y)))};
}

static std::vector<IVec2> reconstruct_dungeon_path(std::vector<IVec2> prev, IVec2 to, size_t width)
{
  IVec2 curPos = to;
  std::vector<IVec2> res = {curPos};
  while (prev[coord_to_idx(curPos.x, curPos.y, width)] != IVec2{-1, -1})
  {
    curPos = prev[coord_to_idx(curPos.x, curPos.y, width)];
    res.insert(res.begin(), curPos);
  }
  return res;
}

static std::vector<IVec2> find_dungeon_path_a_star(const DungeonData &dd, IVec2 from, IVec2 to, IVec2 lim_min, IVec2 lim_max)
{
  if (from.x < 0 || from.y < 0 || from.x >= int(dd.width) || from.y >= int(dd.height))
    return std::vector<IVec2>();
  size_t inpSize = dd.width * dd.height;

  std::vector<float> g(inpSize, std::numeric_limits<float>::max());
  std::vector<float> f(inpSize, std::numeric_limits<float>::max());
  std::vector<IVec2> prev(inpSize, {-1, -1});

  auto getG = [&](IVec2 p) -> float { return g[coord_to_idx(p.x, p.y, dd.width)]; };
  auto getF = [&](IVec2 p) -> float { return f[coord_to_idx(p.x, p.y, dd.width)]; };

  g[coord_to_idx(from.x, from.y, dd.width)] = 0;
  f[coord_to_idx(from.x, from.y, dd.width)] = heuristic(from, to);

  std::vector<IVec2> openList = {from};
  std::vector<IVec2> closedList;

  while (!openList.empty())
  {
    size_t bestIdx = 0;
    float bestScore = getF(openList[0]);
    for (size_t i = 1; i < openList.size(); ++i)
    {
      float score = getF(openList[i]);
      if (score < bestScore)
      {
        bestIdx = i;
        bestScore = score;
      }
    }
    if (openList[bestIdx] == to)
      return reconstruct_dungeon_path(prev, to, dd.width);
    IVec2 curPos = openList[bestIdx];
    openList.erase(openList.begin() + bestIdx);
    if (std::find(closedList.begin(), closedList.end(), curPos) != closedList.end())
      continue;
    closedList.emplace_back(curPos);
    auto checkNeighbour = [&](IVec2 p) {
      // out of bounds
      if (p.x < lim_min.x || p.y < lim_min.y || p.x >= lim_max.x || p.y >= lim_max.y)
        return;
      size_t idx = coord_to_idx(p.x, p.y, dd.width);
      // not empty
      if (dd.tiles[idx] == dungeon::wall)
        return;
      float edgeWeight = 1.f;
      float gScore = getG(curPos) + 1.f * edgeWeight; // we're exactly 1 unit away
      if (gScore < getG(p))
      {
        prev[idx] = curPos;
        g[idx] = gScore;
        f[idx] = gScore + heuristic(p, to);
      }
      bool found = std::find(openList.begin(), openList.end(), p) != openList.end();
      if (!found)
        openList.emplace_back(p);
    };
    checkNeighbour({curPos.x + 1, curPos.y + 0});
    checkNeighbour({curPos.x - 1, curPos.y + 0});
    checkNeighbour({curPos.x + 0, curPos.y + 1});
    checkNeighbour({curPos.x + 0, curPos.y - 1});
  }
  // empty path
  return std::vector<IVec2>();
}


void prebuild_map(flecs::world &ecs)
{
  auto mapQuery = ecs.query<const DungeonData>();

  ecs.defer([&]() {
    mapQuery.each([&](flecs::entity e, const DungeonData &dd) {
      // go through each super tile
      const size_t width = dd.width / SPLIT_TILES;
      const size_t height = dd.height / SPLIT_TILES;

      auto check_border = [&](size_t xx, size_t yy, size_t dir_x, size_t dir_y, int offs_x, int offs_y,
                            std::vector<PathPortal> &portals) {
        int spanFrom = -1;
        int spanTo = -1;
        for (size_t i = 0; i < SPLIT_TILES; ++i)
        {
          size_t x = xx * SPLIT_TILES + i * dir_x;
          size_t y = yy * SPLIT_TILES + i * dir_y;
          size_t nx = x + offs_x;
          size_t ny = y + offs_y;
          if (dd.tiles[y * dd.width + x] != dungeon::wall && dd.tiles[ny * dd.width + nx] != dungeon::wall)
          {
            if (spanFrom < 0)
              spanFrom = i;
            spanTo = i;
          }
          else if (spanFrom >= 0)
          {
            // write span
            portals.push_back({xx * SPLIT_TILES + spanFrom * dir_x + offs_x, yy * SPLIT_TILES + spanFrom * dir_y + offs_y,
              xx * SPLIT_TILES + spanTo * dir_x, yy * SPLIT_TILES + spanTo * dir_y});
            spanFrom = -1;
          }
        }
        if (spanFrom >= 0)
        {
          portals.push_back({xx * SPLIT_TILES + spanFrom * dir_x + offs_x, yy * SPLIT_TILES + spanFrom * dir_y + offs_y,
            xx * SPLIT_TILES + spanTo * dir_x, yy * SPLIT_TILES + spanTo * dir_y});
        }
      };

      std::vector<PathPortal> portals;
      std::vector<std::vector<size_t>> tilePortalsIndices;
      std::vector<std::vector<std::vector<float>>> tilePortalsDmaps;

      auto push_portals = [&](size_t x, size_t y, int offs_x, int offs_y, std::vector<PathPortal> &new_portals) {
        for (PathPortal &portal : new_portals)
        {
          size_t idx = portals.size();
          portals.push_back(portal);

          const size_t firstSecIdx = y * width + x;
          const size_t secondSecIdx = (y + offs_y) * width + x + offs_x;

          tilePortalsIndices[firstSecIdx].push_back(idx);
          tilePortalsIndices[secondSecIdx].push_back(idx);

          tilePortalsDmaps[firstSecIdx].push_back(gen_sector_to_portal_dmap(dd, portal,
            {int(x * SPLIT_TILES) - 1, int(y * SPLIT_TILES) - 1}, {int((x + 1) * SPLIT_TILES) + 1, int((y + 1) * SPLIT_TILES) + 1}));
          tilePortalsDmaps[secondSecIdx].push_back(
            gen_sector_to_portal_dmap(dd, portal, {int((x + offs_x) * SPLIT_TILES) - 1, int((y + offs_y) * SPLIT_TILES) - 1},
              {int((x + offs_x + 1) * SPLIT_TILES) + 1, int((y + offs_y + 1) * SPLIT_TILES) + 1}));
        }
      };
      for (size_t y = 0; y < height; ++y)
        for (size_t x = 0; x < width; ++x)
        {
          tilePortalsIndices.push_back({});
          tilePortalsDmaps.push_back({});
          // check top
          if (y > 0)
          {
            std::vector<PathPortal> topPortals;
            check_border(x, y, 1, 0, 0, -1, topPortals);
            push_portals(x, y, 0, -1, topPortals);
          }
          // left
          if (x > 0)
          {
            std::vector<PathPortal> leftPortals;
            check_border(x, y, 0, 1, -1, 0, leftPortals);
            push_portals(x, y, -1, 0, leftPortals);
          }
        }
      for (size_t tidx = 0; tidx < tilePortalsIndices.size(); ++tidx)
      {
        const auto &indices = tilePortalsIndices[tidx];
        size_t x = tidx % width;
        size_t y = tidx / width;
        IVec2 limMin{int((x + 0) * SPLIT_TILES), int((y + 0) * SPLIT_TILES)};
        IVec2 limMax{int((x + 1) * SPLIT_TILES), int((y + 1) * SPLIT_TILES)};
        for (size_t i = 0; i < indices.size(); ++i)
        {
          PathPortal &firstPortal = portals[indices[i]];
          for (size_t j = i + 1; j < indices.size(); ++j)
          {
            PathPortal &secondPortal = portals[indices[j]];
            // check path from i to j
            // check each position (to find closest dist) (could be made more optimal)
            bool noPath = false;
            size_t minDist = 0xffffffff;
            for (size_t fromY = std::max(firstPortal.startY, size_t(limMin.y));
                 fromY <= std::min(firstPortal.endY, size_t(limMax.y - 1)) && !noPath; ++fromY)
            {
              for (size_t fromX = std::max(firstPortal.startX, size_t(limMin.x));
                   fromX <= std::min(firstPortal.endX, size_t(limMax.x - 1)) && !noPath; ++fromX)
              {
                for (size_t toY = std::max(secondPortal.startY, size_t(limMin.y));
                     toY <= std::min(secondPortal.endY, size_t(limMax.y - 1)) && !noPath; ++toY)
                {
                  for (size_t toX = std::max(secondPortal.startX, size_t(limMin.x));
                       toX <= std::min(secondPortal.endX, size_t(limMax.x - 1)) && !noPath; ++toX)
                  {
                    IVec2 from{int(fromX), int(fromY)};
                    IVec2 to{int(toX), int(toY)};
                    std::vector<IVec2> path = find_dungeon_path_a_star(dd, from, to, limMin, limMax);
                    if (path.empty() && from != to)
                    {
                      noPath = true; // if we found that there's no path at all - we can break out
                      break;
                    }
                    minDist = std::min(minDist, path.size());
                  }
                }
              }
            }
            // write pathable data and length
            if (noPath)
              continue;
            firstPortal.conns.push_back({indices[j], tidx, float(minDist)});
            secondPortal.conns.push_back({indices[i], tidx, float(minDist)});
          }
        }
      }

      // Fake mutable portals for portal a* search exactly point to point
      // (on search, we put the from and to points there as fake portals)
      portals.emplace_back();
      portals.emplace_back();

      e.set(DungeonPortals{SPLIT_TILES, portals, tilePortalsIndices, tilePortalsDmaps});
    });
  });
}

static Position get_portal_pos(const PathPortal &p)
{
  return {(float(p.endX) + float(p.startX)) * 0.5f, (float(p.endY) + float(p.startY)) * 0.5f};
}

static std::vector<size_t> reconstruct_portal_path(std::vector<size_t> prev, std::vector<size_t> prevConnections, size_t to)
{
  size_t curIdx = to;
  std::vector<size_t> res = {};
  while (prev[curIdx] != size_t(-1))
  {
    size_t connId = prevConnections[curIdx];
    curIdx = prev[curIdx];
    res.insert(res.begin(), connId);
  }
  return res;
}

static std::vector<size_t> find_portal_path_a_star(const DungeonPortals &dp, size_t from_id, size_t to_id)
{
  if (from_id >= dp.portals.size() || to_id >= dp.portals.size())
    return {};
  size_t inpSize = dp.portals.size();

  std::vector<float> g(inpSize, std::numeric_limits<float>::max());
  std::vector<float> f(inpSize, std::numeric_limits<float>::max());
  std::vector<size_t> prev(inpSize, size_t(-1));
  std::vector<size_t> prevConnections(inpSize, size_t(-1));

  g[from_id] = 0;
  f[from_id] = heuristic(get_portal_pos(dp.portals[from_id]), get_portal_pos(dp.portals[to_id]));

  std::vector<size_t> openList = {from_id};
  std::vector<size_t> closedList;

  while (!openList.empty())
  {
    size_t bestIdx = 0;
    float bestScore = f[openList[0]];
    for (size_t i = 1; i < openList.size(); ++i)
    {
      float score = f[openList[i]];
      if (score < bestScore)
      {
        bestIdx = i;
        bestScore = score;
      }
    }
    if (openList[bestIdx] == to_id)
      return reconstruct_portal_path(prev, prevConnections, to_id);
    size_t curPortalId = openList[bestIdx];
    openList.erase(openList.begin() + bestIdx);
    if (std::find(closedList.begin(), closedList.end(), curPortalId) != closedList.end())
      continue;
    closedList.emplace_back(curPortalId);

    auto checkConnection = [&](const PortalConnection &conn) {
      size_t portId = conn.connIdx;
      float gScore = g[curPortalId] + conn.score;
      if (gScore < g[portId])
      {
        prev[portId] = curPortalId;
        // @TODO: idk what the bug is here, this is a hack
        prevConnections[portId] = std::distance(dp.portals[curPortalId].conns.data(), &conn);
        g[portId] = gScore;
        f[portId] = gScore + heuristic(get_portal_pos(dp.portals[portId]), get_portal_pos(dp.portals[to_id]));
      }
      bool found = std::find(openList.begin(), openList.end(), portId) != openList.end();
      if (!found)
        openList.emplace_back(portId);
    };

    for (const PortalConnection &conn : dp.portals[curPortalId].conns)
      checkConnection(conn);
  }
  // empty path
  return {};
}

PathSearchRes construct_path_hierarchical(flecs::world &ecs, Position from, Position to)
{
  printf("Q path: (%f, %f) -> (%f, %f)\n", from.x, from.y, to.x, to.y);

  Position fromSector = {from.x / SPLIT_TILES, from.y / SPLIT_TILES};
  Position toSector = {to.x / SPLIT_TILES, to.y / SPLIT_TILES};

  printf("Sectors: (%f, %f) -> (%f, %f)\n", fromSector.x, fromSector.y, toSector.x, toSector.y);

  IVec2 fromSecTileCoord = {int(floorf(fromSector.x) * SPLIT_TILES), int(floorf(fromSector.y) * SPLIT_TILES)};
  IVec2 fromSecTileCoordCap = {fromSecTileCoord.x + int(SPLIT_TILES), fromSecTileCoord.y + int(SPLIT_TILES)};
  IVec2 toSecTileCoord = {int(floorf(toSector.x) * SPLIT_TILES), int(floorf(toSector.y) * SPLIT_TILES)};
  IVec2 toSecTileCoordCap = {toSecTileCoord.x + int(SPLIT_TILES), toSecTileCoord.y + int(SPLIT_TILES)};

  auto mapQuery = ecs.query<const DungeonData>();
  auto portalsQuery = ecs.query<DungeonPortals>();

  PathSearchRes res{};

  // If same sector, no portal search is reqiured
  if (fromSecTileCoord == toSecTileCoord)
  {
    mapQuery.each([&](const DungeonData &dd) {
      auto path = find_dungeon_path_a_star(dd, to_ivec2(from), to_ivec2(to), fromSecTileCoord, fromSecTileCoordCap);
      res.path.resize(path.size());
      for (size_t i = 0; i < path.size(); ++i)
        res.path[i] = {float(path[i].x), float(path[i].y)};
    });

    return res;
  }

  portalsQuery.each([&](DungeonPortals &dp) {
    mapQuery.each([&](const DungeonData &dd) {
      const size_t width = dd.width / SPLIT_TILES;

      PathPortal &fromPortal = dp.portals[dp.portals.size() - 2];
      PathPortal &toPortal = dp.portals[dp.portals.size() - 1];

      fromPortal = {size_t(floorf(from.x)), size_t(floorf(from.y)), size_t(floorf(from.x)), size_t(floorf(from.y)), {}};
      toPortal = {size_t(floorf(to.x)), size_t(floorf(to.y)), size_t(floorf(to.x)), size_t(floorf(to.y)), {}};

      size_t fromSectorId = coord_to_idx(fromSector.x, fromSector.y, width);
      size_t toSectorId = coord_to_idx(toSector.x, toSector.y, width);

      fromPortal.conns.reserve(dp.tilePortalsIndices[fromSectorId].size());

      auto addPortalAsConn = [&](PathPortal &src, size_t dst_id, IVec2 limMin, IVec2 limMax, size_t sectorId) {
        const PathPortal &dst = dp.portals[dst_id];
        Position srcPos = get_portal_pos(src);
        Position dstPos = get_portal_pos(dst);
        IVec2 from{int(srcPos.x), int(srcPos.y)};
        IVec2 to{int(dstPos.x), int(dstPos.y)};
        if (from == to)
          src.conns.push_back({dst_id, sectorId, 0});
        else
        {
          std::vector<IVec2> path = find_dungeon_path_a_star(dd, from, to, {limMin.x - 1, limMin.y - 1}, {limMax.x + 1, limMax.y + 1});
          src.conns.push_back({dst_id, sectorId, path.empty() ? FLT_MAX : path.size()});
        }
      };

      for (size_t id : dp.tilePortalsIndices[fromSectorId])
        addPortalAsConn(fromPortal, id, fromSecTileCoord, fromSecTileCoordCap, fromSectorId);
      for (size_t id : dp.tilePortalsIndices[toSectorId])
        addPortalAsConn(dp.portals[id], dp.portals.size() - 1, toSecTileCoord, toSecTileCoordCap, toSectorId);

      // Also make a temp dmap and index record for the dest portal
      dp.tilePortalsIndices[toSectorId].push_back(dp.portals.size() - 1);
      dp.tilePortalsDmaps[toSectorId].push_back(gen_sector_to_portal_dmap(dd, dp.portals.back(),
        {toSecTileCoord.x - 1, toSecTileCoord.y - 1}, {toSecTileCoordCap.x + 1, toSecTileCoordCap.y + 1}));

      auto connIndices = find_portal_path_a_star(dp, dp.portals.size() - 2, dp.portals.size() - 1);

      printf("Portal path len = %lu\n", connIndices.size());

      res.portalIndices.push_back(dp.portals.size() - 2);

      size_t curPortalId = dp.portals.size() - 2;
      IVec2 curPos{int(from.x), int(from.y)};
      for (size_t i = 0; i < connIndices.size(); ++i)
      {
        const PortalConnection &conn = dp.portals[curPortalId].conns[connIndices[i]];
        size_t sectorId = conn.sectorIdx;

        res.portalIndices.push_back(conn.connIdx);

        const size_t sx = sectorId % width;
        const size_t sy = sectorId / width;

        size_t portalInSectorId = size_t(-1);
        if (i == connIndices.size() - 1)
          portalInSectorId = dp.tilePortalsIndices[sectorId].size() - 1;
        else
        {
          for (size_t j = 0; j < dp.tilePortalsIndices[sectorId].size(); ++j)
          {
            size_t id = dp.tilePortalsIndices[sectorId][j];
            if (id == conn.connIdx)
            {
              portalInSectorId = j;
              break;
            }
          }
        }

        assert(portalInSectorId != size_t(-1));

        const auto &dmap = dp.tilePortalsDmaps[sectorId][portalInSectorId];

        printf("sid = %lu, pisid = %lu (of %lu, with %lu dmaps), dmap = %p\n", sectorId, portalInSectorId,
          dp.tilePortalsIndices[sectorId].size(), dp.tilePortalsDmaps[sectorId].size(), &dmap);

        IVec2 coordBase{int(sx * SPLIT_TILES) - 1, int(sy * SPLIT_TILES) - 1};
        IVec2 coordSize = {SPLIT_TILES + 2, SPLIT_TILES + 2};
        if (coordBase.x < 0)
        {
          ++coordBase.x;
          --coordSize.x;
        }
        if (coordBase.y < 0)
        {
          ++coordBase.y;
          --coordSize.y;
        }
        if (coordBase.x + coordSize.x > dd.width)
          --coordSize.x;
        if (coordBase.y + coordSize.y > dd.height)
          --coordSize.y;

        IVec2 bestOffset = {0, 0};
        float bestScore = FLT_MAX;

        for (int y = coordBase.y; y < coordBase.y + coordSize.y; ++y) {
          for (int x = coordBase.x; x < coordBase.x + coordSize.x; ++x) {
            size_t id = (y - coordBase.y) * coordSize.x + x - coordBase.x;
            bool isStart = x == curPos.x && y == curPos.y;
            assert(curPos.x >= coordBase.x);
            assert(curPos.y >= coordBase.y);
            assert(curPos.x < coordBase.x + coordSize.x);
            assert(curPos.y < coordBase.y + coordSize.y);
            if (dmap[id] < 10)
              printf("%g%c   ", dmap[id], isStart ? '<' : ' ');
            else if (dmap[id] < 100)
              printf("%g%c  ", dmap[id], isStart ? '<' : ' ');
            else if (dmap[id] < INVALID_TILE_VALUE)
              printf("%g%c ", dmap[id], isStart ? '<' : ' ');
            else
              printf("W%c   ", isStart ? '<' : ' ');
          }
          printf("\n");
        }

        while (bestScore >= FLT_EPSILON)
        {
          auto checkNeighbour = [&](int offx, int offy) {
            int x = (curPos.x + offx) - coordBase.x;
            int y = (curPos.y + offy) - coordBase.y;
            if (x < 0 || y < 0 || y >= coordSize.y || x >= coordSize.x)
              return;
            assert(dmap.size() == coordSize.x * coordSize.y);
            float score = dmap[y * coordSize.x + x];
            if (score < bestScore)
            {
              bestOffset = {offx, offy};
              bestScore = score;
            }
          };

          bestScore = FLT_MAX - 1;
          checkNeighbour(1, 0);
          checkNeighbour(-1, 0);
          checkNeighbour(0, 1);
          checkNeighbour(0, -1);

          curPos.x += bestOffset.x;
          curPos.y += bestOffset.y;

          res.path.push_back({float(curPos.x), float(curPos.y)});
        }

        assert(bestScore < FLT_MAX);

        curPortalId = conn.connIdx;
      }

      dp.tilePortalsIndices[toSectorId].pop_back();
      dp.tilePortalsDmaps[toSectorId].pop_back();
      for (size_t id : dp.tilePortalsIndices[toSectorId])
        dp.portals[id].conns.pop_back();
    });
  });

  return res;
}
