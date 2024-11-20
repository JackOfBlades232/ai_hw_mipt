#include "roguelike.h"
#include "ecsTypes.h"
#include "raylib.h"
#include "stateMachine.h"
#include "aiLibrary.h"
#include "blackboard.h"
#include "math.h"
#include "dungeonUtils.h"
#include "dijkstraMapGen.h"
#include "dmapFollower.h"
#include <cfloat>

enum GameTeam : int
{
  TEAM_PLAYER = 0,
  TEAM_ORCS = 1,
  TEAM_HIVE = 2,
};

static constexpr GameTeam ENEMY_TEAMS[] = {TEAM_ORCS, TEAM_HIVE};

constexpr float SHOT_DISTANCE = 6.f;
constexpr float EXPLORATION_DIST = 8.f;

static int get_team(flecs::entity e)
{
  int res;
  e.get([&](const Team &team) { res = team.team; });
  return res;
}

// Common
static flecs::entity create_adversary_approacher(flecs::entity e)
{
  e.set(DmapWeights{{{dmaps::gen_name("approach_map", get_team(e)), {1.f, 1.f}}}});
  return e;
}

static flecs::entity create_adversary_fleer(flecs::entity e)
{
  e.set(DmapWeights{{{dmaps::gen_name("flee_map", get_team(e)), {1.f, 1.f}}}});
  return e;
}

static flecs::entity create_adversary_ranger(flecs::entity e)
{
  e.set(DmapWeights{{{dmaps::gen_name("range_map", get_team(e)), {1.f, 1.f}}}});
  return e;
}

// Hive
static flecs::entity create_hive_monster(flecs::entity e)
{
  auto hiveName = dmaps::gen_name("hive_map", get_team(e));
  auto approachName = dmaps::gen_name("approach_map", get_team(e));
  e.set(DmapWeights{{{hiveName, {1.f, 1.f}}, {approachName, {1.8, 0.8f}}}});
  return e;
}

// Peaceful
static flecs::entity create_map_explorer(flecs::entity e)
{
  e.add<MapExplorer>();
  e.set(DmapWeights{{{"exploration_map", {2.6f, 2.6f}}, {dmaps::gen_name("approach_map", get_team(e)), {1.f, 1.f}},
    {dmaps::gen_name("flee_map", get_team(e)), {1.f, 1.f}}}});
  return e;
}

static flecs::entity create_hive(flecs::entity e)
{
  e.add<Hive>();
  return e;
}

static Position find_free_dungeon_tile(flecs::world &ecs)
{
  static auto findMonstersQuery = ecs.query<const Position, const Hitpoints>();
  bool done = false;
  while (!done)
  {
    done = true;
    Position pos = dungeon::find_walkable_tile(ecs);
    findMonstersQuery.each([&](const Position &p, const Hitpoints &) {
      if (p == pos)
        done = false;
    });
    if (done)
      return pos;
  };
  return {0, 0};
}

static flecs::entity create_monster(flecs::world &ecs, Color col, const char *texture_src, GameTeam team, bool ranged = false)
{
  Position pos = find_free_dungeon_tile(ecs);

  flecs::entity textureSrc = ecs.entity(texture_src);
  auto e = ecs.entity()
             .set(Position{pos.x, pos.y})
             .set(MovePos{pos.x, pos.y})
             .set(Hitpoints{100.f})
             .set(Action{EA_NOP})
             .set(Color{col})
             .add<TextureSource>(textureSrc)
             .set(StateMachine{})
             .set(Team{team})
             .set(NumActions{1, 0})
             .set(MeleeDamage{20.f})
             .set(Blackboard{});
  if (ranged)
    e.set(RangedDamage{2.5f});
  return e;
}

static flecs::entity create_player(flecs::world &ecs, const char *texture_src)
{
  Position pos = find_free_dungeon_tile(ecs);

  flecs::entity textureSrc = ecs.entity(texture_src);
  return ecs.entity("player")
    .set(Position{pos.x, pos.y})
    .set(MovePos{pos.x, pos.y})
    .set(Hitpoints{100.f})
    //.set(Color{0xee, 0xee, 0xee, 0xff})
    .set(Action{EA_NOP})
    .add<IsPlayer>()
    .set(Team{TEAM_PLAYER})
    .set(PlayerInput{})
    .set(NumActions{2, 0})
    .set(Color{255, 255, 255, 255})
    .add<TextureSource>(textureSrc)
    .set(Autopilot{true})
    .set(MeleeDamage{20.f});
}

static void register_roguelike_systems(flecs::world &ecs)
{
  static auto dungeonDataQuery = ecs.query<const DungeonData>();
  static auto expDataQuery = ecs.query<const ExplorationMapData>();
  ecs.system<PlayerInput, Action, const IsPlayer, const Autopilot *>().each([&](PlayerInput &inp, Action &a, const IsPlayer, const Autopilot *autop) {
    if (autop && autop->enabled)
      return;
    bool left = IsKeyDown(KEY_LEFT);
    bool right = IsKeyDown(KEY_RIGHT);
    bool up = IsKeyDown(KEY_UP);
    bool down = IsKeyDown(KEY_DOWN);
    if (left && !inp.left)
      a.action = EA_MOVE_LEFT;
    if (right && !inp.right)
      a.action = EA_MOVE_RIGHT;
    if (up && !inp.up)
      a.action = EA_MOVE_UP;
    if (down && !inp.down)
      a.action = EA_MOVE_DOWN;
    inp.left = left;
    inp.right = right;
    inp.up = up;
    inp.down = down;

    bool pass = IsKeyDown(KEY_SPACE);
    if (pass && !inp.passed)
      a.action = EA_PASS;
    inp.passed = pass;
  });
  ecs.system<const PlayerInput, Autopilot>().each([&](const PlayerInput &, Autopilot &autop) {
    if (IsKeyDown(KEY_A))
      autop.enabled = !autop.enabled;
  });
  ecs.system<const Position, const Color>()
    .with<TextureSource>(flecs::Wildcard)
    .with<BackgroundTile>()
    .each([&](flecs::entity e, const Position &pos, const Color color) {
      const auto textureSrc = e.target<TextureSource>();
      bool discard = false;
      int w, h;
      dungeonDataQuery.each([&](const DungeonData &dd) {
        w = dd.width;
        h = dd.height;
      });
      expDataQuery.each([&](const ExplorationMapData &data) { discard = !data.map[pos.y * w + pos.x]; });
      unsigned char div = discard ? 3 : 1;
      Color c{(unsigned char)(color.r / div), (unsigned char)(color.g / div), (unsigned char)(color.b / div), color.a};
      DrawTextureQuad(*textureSrc.get<Texture2D>(), Vector2{1, 1}, Vector2{0, 0},
        Rectangle{float(pos.x) * tile_size, float(pos.y) * tile_size, tile_size, tile_size}, c);
    });
  ecs.system<const Position, const Color>().without<TextureSource>(flecs::Wildcard).each([&](const Position &pos, const Color color) {
    const Rectangle rect = {float(pos.x) * tile_size, float(pos.y) * tile_size, tile_size, tile_size};
    DrawRectangleRec(rect, color);
  });
  ecs.system<const Position, const Color>()
    .with<TextureSource>(flecs::Wildcard)
    .without<BackgroundTile>()
    .each([&](flecs::entity e, const Position &pos, const Color color) {
      const auto textureSrc = e.target<TextureSource>();
      DrawTextureQuad(*textureSrc.get<Texture2D>(), Vector2{1, 1}, Vector2{0, 0},
        Rectangle{float(pos.x) * tile_size, float(pos.y) * tile_size, tile_size, tile_size}, color);
    });
  ecs.system<const Position, const Hitpoints>().each([&](const Position &pos, const Hitpoints &hp) {
    constexpr float hpPadding = 0.05f;
    const float hpWidth = 1.f - 2.f * hpPadding;
    const Rectangle underRect = {
      float(pos.x + hpPadding) * tile_size, float(pos.y - 0.25f) * tile_size, hpWidth * tile_size, 0.1f * tile_size};
    DrawRectangleRec(underRect, BLACK);
    const Rectangle hpRect = {float(pos.x + hpPadding) * tile_size, float(pos.y - 0.25f) * tile_size,
      hp.hitpoints / 100.f * hpWidth * tile_size, 0.1f * tile_size};
    DrawRectangleRec(hpRect, RED);
  });

  ecs.system<Texture2D>().each([&](Texture2D &tex) { SetTextureFilter(tex, TEXTURE_FILTER_POINT); });
  ecs.system<const DmapWeights>().with<VisualiseMap>().each([&](const DmapWeights &wt) {
    dungeonDataQuery.each([&](const DungeonData &dd) {
      for (size_t y = 0; y < dd.height; ++y)
        for (size_t x = 0; x < dd.width; ++x)
        {
          float sum = 0.f;
          for (const auto &pair : wt.weights)
          {
            ecs.entity(pair.first.c_str()).get([&](const DijkstraMapData &dmap) {
              float v = dmap.map[y * dd.width + x];
              if (v < 1e5f)
                sum += powf(v * pair.second.mult, pair.second.pow);
              else
                sum += v;
            });
          }
          if (sum < 1e5f)
            DrawText(TextFormat("%.1f", sum), (float(x) + 0.2f) * tile_size, (float(y) + 0.5f) * tile_size, 150, WHITE);
        }
    });
  });
  ecs.system<const DijkstraMapData>().with<VisualiseMap>().each([](const DijkstraMapData &dmap) {
    dungeonDataQuery.each([&](const DungeonData &dd) {
      for (size_t y = 0; y < dd.height; ++y)
        for (size_t x = 0; x < dd.width; ++x)
        {
          const float val = dmap.map[y * dd.width + x];
          if (val < 1e5f)
            DrawText(TextFormat("%.1f", val), (float(x) + 0.2f) * tile_size, (float(y) + 0.5f) * tile_size, 150, WHITE);
        }
    });
  });
}


void init_roguelike(flecs::world &ecs)
{
  register_roguelike_systems(ecs);

  ecs.entity("swordsman_tex").set(Texture2D{LoadTexture("assets/swordsman.png")});
  ecs.entity("minotaur_tex").set(Texture2D{LoadTexture("assets/minotaur.png")});

  ecs.observer<Texture2D>().event(flecs::OnRemove).each([](Texture2D texture) { UnloadTexture(texture); });

  // Orcs
  create_adversary_approacher(create_monster(ecs, Color{0x00, 0xee, 0x00, 0xff}, "minotaur_tex", TEAM_ORCS));
  create_adversary_approacher(create_monster(ecs, Color{0x00, 0xee, 0x00, 0xff}, "minotaur_tex", TEAM_ORCS));
  create_adversary_approacher(create_monster(ecs, Color{0x00, 0xee, 0x00, 0xff}, "minotaur_tex", TEAM_ORCS));
  create_adversary_approacher(create_monster(ecs, Color{0x00, 0xee, 0x00, 0xff}, "minotaur_tex", TEAM_ORCS));
  create_adversary_ranger(create_monster(ecs, Color{0x00, 0x00, 0xee, 0xff}, "minotaur_tex", TEAM_ORCS, true));
  create_adversary_ranger(create_monster(ecs, Color{0x00, 0x00, 0xee, 0xff}, "minotaur_tex", TEAM_ORCS, true));

  // Hive
  create_hive_monster(create_monster(ecs, Color{0xee, 0x00, 0xee, 0xff}, "minotaur_tex", TEAM_HIVE));
  create_hive_monster(create_monster(ecs, Color{0xee, 0x00, 0xee, 0xff}, "minotaur_tex", TEAM_HIVE));
  create_hive_monster(create_monster(ecs, Color{0x11, 0x11, 0x11, 0xff}, "minotaur_tex", TEAM_HIVE));
  create_hive_monster(create_monster(ecs, Color{0x11, 0x11, 0x11, 0xff}, "minotaur_tex", TEAM_HIVE));
  create_hive(create_adversary_fleer(create_monster(ecs, Color{0, 255, 0, 255}, "minotaur_tex", TEAM_HIVE)));

  // Player
  create_map_explorer(create_player(ecs, "swordsman_tex"));

  ecs.entity("world").set(TurnCounter{}).set(ActionLog{});
}

void init_dungeon(flecs::world &ecs, char *tiles, size_t w, size_t h)
{
  flecs::entity wallTex = ecs.entity("wall_tex").set(Texture2D{LoadTexture("assets/wall.png")});
  flecs::entity floorTex = ecs.entity("floor_tex").set(Texture2D{LoadTexture("assets/floor.png")});

  std::vector<char> dungeonData;
  std::vector<bool> expData;
  dungeonData.resize(w * h);
  expData.resize(w * h);
  for (size_t y = 0; y < h; ++y)
    for (size_t x = 0; x < w; ++x)
    {
      dungeonData[y * w + x] = tiles[y * w + x];
      expData[y * w + x] = false;
    }
  ecs.entity("dungeon").set(DungeonData{dungeonData, w, h});
  ecs.entity("exploration").set(ExplorationMapData{std::move(expData)});

  for (size_t y = 0; y < h; ++y)
    for (size_t x = 0; x < w; ++x)
    {
      char tile = tiles[y * w + x];
      flecs::entity tileEntity = ecs.entity().add<BackgroundTile>().set(Position{int(x), int(y)}).set(Color{255, 255, 255, 255});
      if (tile == dungeon::wall)
        tileEntity.add<TextureSource>(wallTex);
      else if (tile == dungeon::floor)
        tileEntity.add<TextureSource>(floorTex);
    }
}


static bool is_player_acted(flecs::world &ecs)
{
  static auto processPlayer = ecs.query<const IsPlayer, const Action>();
  static auto processPlayerAutopilot = ecs.query<const IsPlayer, const Autopilot>();
  bool playerActed = false;
  processPlayer.each([&](const IsPlayer, const Action &a) { playerActed = a.action != EA_NOP; });
  processPlayerAutopilot.each([&](const IsPlayer, const Autopilot &a) {
    static int frames = 0;
    if (!playerActed && (++frames) % 12 == 0)
      playerActed = a.enabled;
  });
  return playerActed;
}

static bool upd_player_actions_count(flecs::world &ecs)
{
  static auto updPlayerActions = ecs.query<const IsPlayer, NumActions>();
  bool actionsReached = false;
  updPlayerActions.each([&](const IsPlayer, NumActions &na) {
    na.curActions = (na.curActions + 1) % na.numActions;
    actionsReached |= na.curActions == 0;
  });
  return actionsReached;
}

static Position move_pos(Position pos, int action)
{
  if (action == EA_MOVE_LEFT)
    pos.x--;
  else if (action == EA_MOVE_RIGHT)
    pos.x++;
  else if (action == EA_MOVE_UP)
    pos.y--;
  else if (action == EA_MOVE_DOWN)
    pos.y++;
  return pos;
}

static void push_to_log(flecs::world &ecs, const char *msg)
{
  static auto queryLog = ecs.query<ActionLog, const TurnCounter>();
  queryLog.each([&](ActionLog &l, const TurnCounter &c) {
    l.log.push_back(std::to_string(c.count) + ": " + msg);
    if (l.log.size() > l.capacity)
      l.log.erase(l.log.begin());
  });
}

static void process_actions(flecs::world &ecs)
{
  static auto processActions = ecs.query<Action, Position, MovePos, const MeleeDamage, const Team>();
  static auto processRanged = ecs.query<const Action, Position, const RangedDamage, const Team>();
  static auto processHeals = ecs.query<Action, Hitpoints>();
  static auto checkAttacks = ecs.query<const MovePos, Hitpoints, const Team>();
  // Process all actions
  ecs.defer([&] {
    processHeals.each([&](Action &a, Hitpoints &hp) {
      if (a.action != EA_HEAL_SELF)
        return;
      a.action = EA_NOP;
      push_to_log(ecs, "Monster healed itself");
      hp.hitpoints += 10.f;
    });
    processRanged.each([&](flecs::entity entity, const Action &a, Position &pos, const RangedDamage &dmg, const Team &team) {
      if (a.action != EA_NOP)
        return;
      checkAttacks.each([&](flecs::entity enemy, const MovePos &epos, Hitpoints &hp, const Team &enemy_team) {
        if (entity != enemy && team.team != enemy_team.team && dist(pos, epos) <= SHOT_DISTANCE)
        {
          push_to_log(ecs, "range damaged entity");
          hp.hitpoints -= dmg.damage;
        }
      });
    });
    processActions.each([&](flecs::entity entity, Action &a, Position &pos, MovePos &mpos, const MeleeDamage &dmg, const Team &team) {
      Position nextPos = move_pos(pos, a.action);
      bool blocked = !dungeon::is_tile_walkable(ecs, nextPos);
      checkAttacks.each([&](flecs::entity enemy, const MovePos &epos, Hitpoints &hp, const Team &enemy_team) {
        if (entity != enemy && epos == nextPos)
        {
          blocked = true;
          if (team.team != enemy_team.team)
          {
            push_to_log(ecs, "damaged entity");
            hp.hitpoints -= dmg.damage;
          }
        }
      });
      if (blocked)
        a.action = EA_NOP;
      else
        mpos = nextPos;
    });
    // now move
    processActions.each([&](Action &a, Position &pos, MovePos &mpos, const MeleeDamage &, const Team &) {
      pos = mpos;
      a.action = EA_NOP;
    });
  });

  static auto deleteAllDead = ecs.query<const Hitpoints>();
  ecs.defer([&] {
    deleteAllDead.each([&](flecs::entity entity, const Hitpoints &hp) {
      if (hp.hitpoints <= 0.f)
        entity.destruct();
    });
  });

  static auto playerPickup = ecs.query<const IsPlayer, const Position, Hitpoints, MeleeDamage>();
  static auto healPickup = ecs.query<const Position, const HealAmount>();
  static auto powerupPickup = ecs.query<const Position, const PowerupAmount>();
  ecs.defer([&] {
    playerPickup.each([&](const IsPlayer &, const Position &pos, Hitpoints &hp, MeleeDamage &dmg) {
      healPickup.each([&](flecs::entity entity, const Position &ppos, const HealAmount &amt) {
        if (pos == ppos)
        {
          hp.hitpoints += amt.amount;
          entity.destruct();
        }
      });
      powerupPickup.each([&](flecs::entity entity, const Position &ppos, const PowerupAmount &amt) {
        if (pos == ppos)
        {
          dmg.damage += amt.amount;
          entity.destruct();
        }
      });
    });
  });
}

template <typename T>
static void push_info_to_bb(Blackboard &bb, const char *name, const T &val)
{
  size_t idx = bb.regName<T>(name);
  bb.set(idx, val);
}

// sensors
static void gather_world_info(flecs::world &ecs)
{
  static auto gatherWorldInfo = ecs.query<Blackboard, const Position, const Hitpoints, const WorldInfoGatherer, const Team>();
  static auto alliesQuery = ecs.query<const Position, const Team>();
  gatherWorldInfo.each([&](Blackboard &bb, const Position &pos, const Hitpoints &hp, WorldInfoGatherer, const Team &team) {
    // first gather all needed names (without cache)
    push_info_to_bb(bb, "hp", hp.hitpoints);
    float numAllies = 0; // note float
    float closestEnemyDist = 100.f;
    alliesQuery.each([&](const Position &apos, const Team &ateam) {
      constexpr float limitDist = 5.f;
      if (team.team == ateam.team && dist_sq(pos, apos) < sqr(limitDist))
        numAllies += 1.f;
      if (team.team != ateam.team)
      {
        const float enemyDist = dist(pos, apos);
        if (enemyDist < closestEnemyDist)
          closestEnemyDist = enemyDist;
      }
    });
    push_info_to_bb(bb, "alliesNum", numAllies);
    push_info_to_bb(bb, "enemyDist", closestEnemyDist);
  });
}

void process_turn(flecs::world &ecs)
{
  static auto stateMachineAct = ecs.query<StateMachine>();
  static auto behTreeUpdate = ecs.query<BehaviourTree, Blackboard>();
  static auto turnIncrementer = ecs.query<TurnCounter>();

  if (is_player_acted(ecs))
  {
    if (upd_player_actions_count(ecs))
    {
      // Plan action for NPCs
      gather_world_info(ecs);
      ecs.defer([&] {
        stateMachineAct.each([&](flecs::entity e, StateMachine &sm) { sm.act(0.f, ecs, e); });
        behTreeUpdate.each([&](flecs::entity e, BehaviourTree &bt, Blackboard &bb) { bt.update(ecs, e, bb); });
        process_dmap_followers(ecs);
      });
      turnIncrementer.each([](TurnCounter &tc) { tc.count++; });
    }
    process_actions(ecs);

    static auto explorers = ecs.query<const MapExplorer, const Position>();
    static auto ddata = ecs.query<const DungeonData>();
    int w, h;
    ddata.each([&](const DungeonData &dd) {
      w = dd.width;
      h = dd.height;
    });
    static auto exp = ecs.query<ExplorationMapData>();
    exp.each([&](ExplorationMapData &data) {
      explorers.each([&](const MapExplorer &, const Position &pos) {
        for (int y = pos.y - ceilf(EXPLORATION_DIST); y < pos.y + ceilf(EXPLORATION_DIST); ++y)
          for (int x = pos.x - ceilf(EXPLORATION_DIST); x < pos.x + ceilf(EXPLORATION_DIST); ++x)
          {
            if (dist(pos, Position{x, y}) <= EXPLORATION_DIST && x >= 0 && y >= 0 && x <= w && y <= h)
              data.map[y * w + x] = true;
          }
      });
    });

    std::vector<float> expMap;
    dmaps::gen_exploration_map(ecs, expMap);
    ecs.entity("exploration_map").set(DijkstraMapData{expMap});

    for (GameTeam team : ENEMY_TEAMS)
    {
      std::vector<float> approachMap;
      dmaps::gen_adversary_approach_map(ecs, approachMap, team);
      ecs.entity(dmaps::gen_name("approach_map", team).c_str()).set(DijkstraMapData{approachMap});

      std::vector<float> fleeMap;
      dmaps::gen_adversary_flee_map(ecs, fleeMap, team);
      ecs.entity(dmaps::gen_name("flee_map", team).c_str()).set(DijkstraMapData{fleeMap});

      std::vector<float> rangeMap;
      dmaps::gen_adversary_go_to_range_map(ecs, rangeMap, team, SHOT_DISTANCE - FLT_EPSILON, SHOT_DISTANCE / 4 - FLT_EPSILON);
      ecs.entity(dmaps::gen_name("range_map", team).c_str()).set(DijkstraMapData{rangeMap});

      if (team == TEAM_HIVE)
      {
        std::vector<float> hiveMap;
        dmaps::gen_hive_pack_map(ecs, hiveMap);
        ecs.entity(dmaps::gen_name("hive_map", team).c_str()).set(DijkstraMapData{hiveMap});
      }
    }

    //ecs.entity("hive_follower_sum").set(DmapWeights{{{"exploration_map", {1.f, 1.f}}}}).add<VisualiseMap>();
  }
}

void print_stats(flecs::world &ecs)
{
  static auto playerStatsQuery = ecs.query<const IsPlayer, const Hitpoints, const MeleeDamage>();
  playerStatsQuery.each([&](const IsPlayer &, const Hitpoints &hp, const MeleeDamage &dmg) {
    DrawText(TextFormat("hp: %d", int(hp.hitpoints)), 20, 20, 20, WHITE);
    DrawText(TextFormat("power: %d", int(dmg.damage)), 20, 40, 20, WHITE);
  });

  static auto actionLogQuery = ecs.query<const ActionLog>();
  actionLogQuery.each([&](const ActionLog &l) {
    int yPos = GetRenderHeight() - 20;
    for (const std::string &msg : l.log)
    {
      DrawText(msg.c_str(), 20, yPos, 20, WHITE);
      yPos -= 20;
    }
  });

  static auto playerAutopilot = ecs.query<const IsPlayer, const Autopilot>();
  bool enabled;
  playerAutopilot.each([&](const IsPlayer, const Autopilot &a) { enabled = a.enabled; });
  DrawText(TextFormat("Autopilot is %s, A to switch", enabled ? "enabled" : "disabled"), 1500, 30, 20, WHITE);
}
