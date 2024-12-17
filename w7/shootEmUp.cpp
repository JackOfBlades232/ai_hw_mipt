#include "shootEmUp.h"
#include "ecsTypes.h"
#include "math.h"
#include "rlikeObjects.h"
#include "steering.h"
#include "dungeonUtils.h"
#include "pathfinder.h"
#include <raylib.h>
#include <cfloat>
#include <stdio.h>

constexpr float tile_size = 64.f;

static void register_roguelike_systems(flecs::world &ecs)
{

  ecs.system<const Position, Velocity, const MoveSpeed, const IsPlayer, AutopilotTarget>().each(
    [&](const Position &pos, Velocity &vel, const MoveSpeed &ms, const IsPlayer, AutopilotTarget &tgt) {
      velocity_selection:
        if (!tgt.path.empty())
        {
          Position tpos = tgt.path.front();
          tpos.x *= tile_size;
          tpos.y *= tile_size;
          if (dist(pos, tpos) < FLT_EPSILON)
          {
            tgt.path.erase(tgt.path.begin());
            goto velocity_selection;
          }
          Velocity toVel = {tpos.x - pos.x, tpos.y - pos.y};
          vel = Velocity{normalize(toVel) * ms.speed};
          toVel.x /= ecs.delta_time();
          toVel.y /= ecs.delta_time();

          vel.x = toVel.x >= 0.f ? std::min(vel.x, toVel.x) : std::max(vel.x, toVel.x);
          vel.y = toVel.y >= 0.f ? std::min(vel.y, toVel.y) : std::max(vel.y, toVel.y);
        }
        else
          vel = {0, 0};
    });

  ecs.system<Position, const Velocity>().each([&](Position &pos, const Velocity &vel) { pos += vel * ecs.delta_time(); });
  ecs.system<const Position, const Color>()
    .with<TextureSource>(flecs::Wildcard)
    .with<BackgroundTile>()
    .each([&](flecs::entity e, const Position &pos, const Color color) {
      const auto textureSrc = e.target<TextureSource>();
      DrawTextureQuad(*textureSrc.get<Texture2D>(), Vector2{1, 1}, Vector2{0, 0},
        Rectangle{float(pos.x), float(pos.y), tile_size, tile_size}, color);
    });
  ecs.system<const Position, const Color>()
    .with<TextureSource>(flecs::Wildcard)
    .without<BackgroundTile>()
    .each([&](flecs::entity e, const Position &pos, const Color color) {
      const auto textureSrc = e.target<TextureSource>();
      DrawTextureQuad(*textureSrc.get<Texture2D>(), Vector2{1, 1}, Vector2{0, 0},
        Rectangle{float(pos.x), float(pos.y), tile_size, tile_size}, color);
    });

  ecs.system<Texture2D>().each([&](Texture2D &tex) { SetTextureFilter(tex, TEXTURE_FILTER_POINT); });

  ecs.system<MonsterSpawner>().each([&](MonsterSpawner &ms) {
    auto playerPosQuery = ecs.query<const Position, const IsPlayer>();
    playerPosQuery.each([&](const Position &pp, const IsPlayer &) {
      ms.timeToSpawn -= ecs.delta_time();
      while (ms.timeToSpawn < 0.f)
      {
        steer::Type st = steer::Type(GetRandomValue(0, steer::Type::Num - 1));
        const Color colors[steer::Type::Num] = {WHITE, RED, BLUE, GREEN};
        const float distances[steer::Type::Num] = {800.f, 800.f, 300.f, 300.f};
        const float dist = distances[st];
        constexpr int angRandMax = 1 << 16;
        const float angle = float(GetRandomValue(0, angRandMax)) / float(angRandMax) * PI * 2.f;
        Color col = colors[st];
        steer::create_steer_beh(create_monster(ecs, {pp.x + cosf(angle) * dist, pp.y + sinf(angle) * dist}, col, "minotaur_tex"), st);
        ms.timeToSpawn += ms.timeBetweenSpawns;
      }
    });
  });

  ecs.system<const DungeonPortals, const DungeonData>().each([&](const DungeonPortals &dp, const DungeonData &dd) {
    size_t w = dd.width;
    size_t ts = dp.tileSplit;
    for (size_t y = 0; y < dd.height / ts; ++y)
      DrawLineEx(Vector2{0.f, y * ts * tile_size}, Vector2{dd.width * tile_size, y * ts * tile_size}, 1.f, GetColor(0xff000080));
    for (size_t x = 0; x < dd.width / ts; ++x)
      DrawLineEx(Vector2{x * ts * tile_size, 0.f}, Vector2{x * ts * tile_size, dd.height * tile_size}, 1.f, GetColor(0xff000080));
    auto cameraQuery = ecs.query<const Camera2D>();
    cameraQuery.each([&](Camera2D cam) {
      Vector2 mousePosition = GetScreenToWorld2D(GetMousePosition(), cam);
      size_t wd = w / ts;
      for (size_t y = 0; y < dd.height / ts; ++y)
      {
        if (mousePosition.y < y * ts * tile_size || mousePosition.y > (y + 1) * ts * tile_size)
          continue;
        for (size_t x = 0; x < dd.width / ts; ++x)
        {
          if (mousePosition.x < x * ts * tile_size || mousePosition.x > (x + 1) * ts * tile_size)
            continue;
          for (size_t idx : dp.tilePortalsIndices[y * wd + x])
          {
            const PathPortal &portal = dp.portals[idx];
            Rectangle rect{portal.startX * tile_size, portal.startY * tile_size, (portal.endX - portal.startX + 1) * tile_size,
              (portal.endY - portal.startY + 1) * tile_size};
            DrawRectangleLinesEx(rect, 5, BLACK);
          }
        }
      }
      for (const PathPortal &portal : dp.portals)
      {
        Rectangle rect{portal.startX * tile_size, portal.startY * tile_size, (portal.endX - portal.startX + 1) * tile_size,
          (portal.endY - portal.startY + 1) * tile_size};
        Vector2 fromCenter{rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f};
        DrawRectangleLinesEx(rect, 1, WHITE);
        if (mousePosition.x < rect.x || mousePosition.x > rect.x + rect.width || mousePosition.y < rect.y ||
            mousePosition.y > rect.y + rect.height)
          continue;
        DrawRectangleLinesEx(rect, 4, WHITE);
        for (const PortalConnection &conn : portal.conns)
        {
          const PathPortal &endPortal = dp.portals[conn.connIdx];
          Vector2 toCenter{
            (endPortal.startX + endPortal.endX + 1) * tile_size * 0.5f, (endPortal.startY + endPortal.endY + 1) * tile_size * 0.5f};
          DrawLineEx(fromCenter, toCenter, 1.f, WHITE);
          DrawText(TextFormat("%d", int(conn.score)), (fromCenter.x + toCenter.x) * 0.5f, (fromCenter.y + toCenter.y) * 0.5f, 16,
            WHITE);
        }
      }
    });

    for (size_t i = 1; i < dp.portalsToHighlight.size(); ++i)
    {
      const PathPortal &src = dp.portals[dp.portalsToHighlight[i - 1]];
      const PathPortal &dst = dp.portals[dp.portalsToHighlight[i]];
      Rectangle srcRect{src.startX * tile_size, src.startY * tile_size, (src.endX - src.startX + 1) * tile_size,
        (src.endY - src.startY + 1) * tile_size};
      Rectangle dstRect{dst.startX * tile_size, dst.startY * tile_size, (dst.endX - dst.startX + 1) * tile_size,
        (dst.endY - dst.startY + 1) * tile_size};
      Vector2 srcCenter{srcRect.x + srcRect.width * 0.5f, srcRect.y + srcRect.height * 0.5f};
      Vector2 dstCenter{dstRect.x + dstRect.width * 0.5f, dstRect.y + dstRect.height * 0.5f};
      DrawLineEx(srcCenter, dstCenter, 5.f, RED);
    }
  });
  steer::register_systems(ecs);
}


void init_shoot_em_up(flecs::world &ecs)
{
  register_roguelike_systems(ecs);

  ecs.entity("swordsman_tex").set(Texture2D{LoadTexture("assets/swordsman.png")});
  ecs.entity("minotaur_tex").set(Texture2D{LoadTexture("assets/minotaur.png")});

  const Position walkableTile = dungeon::find_walkable_tile(ecs);
  create_player(ecs, walkableTile * tile_size, "swordsman_tex");
}

void init_dungeon(flecs::world &ecs, char *tiles, size_t w, size_t h)
{
  flecs::entity wallTex = ecs.entity("wall_tex").set(Texture2D{LoadTexture("assets/wall.png")});
  flecs::entity floorTex = ecs.entity("floor_tex").set(Texture2D{LoadTexture("assets/floor.png")});

  std::vector<char> dungeonData;
  dungeonData.resize(w * h);
  for (size_t y = 0; y < h; ++y)
    for (size_t x = 0; x < w; ++x)
      dungeonData[y * w + x] = tiles[y * w + x];
  ecs.entity("dungeon").set(DungeonData{dungeonData, w, h});

  for (size_t y = 0; y < h; ++y)
    for (size_t x = 0; x < w; ++x)
    {
      char tile = tiles[y * w + x];
      flecs::entity tileEntity =
        ecs.entity().add<BackgroundTile>().set(Position{float(x) * tile_size, float(y) * tile_size}).set(Color{255, 255, 255, 255});
      if (tile == dungeon::wall)
        tileEntity.add<TextureSource>(wallTex);
      else if (tile == dungeon::floor)
        tileEntity.add<TextureSource>(floorTex);
    }
  prebuild_map(ecs);
}

void process_game(flecs::world &) {}

void set_autopilot_target(flecs::world &ecs, float x, float y)
{
  auto playerQuery = ecs.query<AutopilotTarget, const Position>();
  auto dpQuery = ecs.query<DungeonPortals>();
  auto ddQuery = ecs.query<const DungeonData>();

  printf("Mouse pressed: (%f, %f)\n", x, y);

  Position tiledDest = {x / tile_size, y / tile_size};

  bool wall = false;
  ddQuery.each([&](const DungeonData &dd) {
    size_t tileId = coord_to_idx(tiledDest.x, tiledDest.y, dd.width);
    wall = tileId >= dd.width * dd.height || dd.tiles[tileId] == dungeon::wall;
  });

  if (wall)
    return;

  playerQuery.each([&](AutopilotTarget &tgt, const Position &pos) {
    Position tiledPos = {pos.x / tile_size, pos.y / tile_size};
    auto [path, portals] = construct_path_hierarchical(ecs, tiledPos, tiledDest);
    tgt.path = std::move(path);
    dpQuery.each([&](DungeonPortals &dp) { dp.portalsToHighlight = std::move(portals); });
  });
}
