#include "roguelike.h"
#include "ecsTypes.h"
#include "raylib.h"
#include "stateMachine.h"
#include "aiLibrary.h"
#include "blackboard.h"
#include "math.h"
#include <cassert>

static void create_fuzzy_monster_beh(flecs::entity e)
{
  BehNode *root = utility_selector({std::make_pair(sequence({find_enemy(e, 4.f, "flee_enemy"), flee(e, "flee_enemy")}),
                                      make_utility([](Blackboard &bb) {
                                        const float hp = bb.get<float>("hp");
                                        const float enemyDist = bb.get<float>("enemyDist");
                                        return (100.f - hp) * 5.f - 50.f * enemyDist;
                                      })),
    std::make_pair(sequence({find_enemy(e, 3.f, "attack_enemy"), move_to_entity(e, "attack_enemy")}), make_utility([](Blackboard &bb) {
      const float enemyDist = bb.get<float>("enemyDist");
      return 100.f - 10.f * enemyDist;
    })),
    std::make_pair(patrol(e, 2.f, "patrol_pos"), make_utility([](Blackboard &) { return 50.f; })),
    std::make_pair(patch_up(100.f), make_utility([](Blackboard &bb) {
      const float hp = bb.get<float>("hp");
      return 140.f - hp;
    }))});
  e.add<WorldInfoGatherer>();
  e.set(BehaviourTree{root});
}

static void create_gatherer_beh(flecs::entity e)
{
  BehNode *root = selector({sequence({find_heal_or_powerup(e, 10.f, "gather_pickup"), move_to_entity(e, "gather_pickup")}),
    sequence({move_to_entity(e, "spawn_point"), spawn_heals_and_powerups(20.f, 2)})});
  e.set(BehaviourTree{root});
}

static void create_guardsman_beh(flecs::entity e)
{
  BehNode *root = selector({sequence({find_enemy(e, 2.f, "attack_enemy"), move_to_entity(e, "attack_enemy")}),
    sequence({move_to_entity(e, "next_waypoint"), switch_wp(e, "next_waypoint")})});
  e.set(BehaviourTree{root});
}

static void create_explorer_monster_beh(flecs::entity e)
{
  BehNode *root = pure_utility_selector(e,
    {// flee
      std::make_pair(flee(e, "target"), make_pure_utility([](Blackboard &bb, const WorldEntSensorInfo &info) {
        const float hp = bb.get<float>("hp");
        return info.type == ENT_ENEMY ? (200.f - hp) * 5.f - 50.f * info.dist : -FLT_MAX;
      })),

      // attack
      std::make_pair(move_to_entity(e, "target"),
        make_cd_pure_utility(
          [](Blackboard &, const WorldEntSensorInfo &info) { return info.type == ENT_ENEMY ? 100.f - 30.f * info.dist : -FLT_MAX; }, 3.f,
          4.f, e)),

      // pickup hp
      std::make_pair(move_to_entity(e, "target"), make_pure_utility([](Blackboard &bb, const WorldEntSensorInfo &info) {
        const float hp = bb.get<float>("hp");
        return info.type == ENT_HEAL ? (300.f - hp) * info.hpOrAmount * 0.2f - 20.f * info.dist : -FLT_MAX;
      })),

      // pickup powerup
      std::make_pair(move_to_entity(e, "target"), make_pure_utility([](Blackboard &, const WorldEntSensorInfo &info) {
        return info.type == ENT_POWERUP ? info.hpOrAmount * 0.2f - 50.f * info.dist : -FLT_MAX;
      })),

      // heal ally
      std::make_pair(sequence({move_to_entity(e, "target"), heal_ally(e, 60.f, "target")}),
        make_pure_utility([](Blackboard &, const WorldEntSensorInfo &info) {
          return info.type == ENT_ALLY ? 25.f * (100.f - info.hpOrAmount) - 30.f * info.dist : -FLT_MAX;
        })),

      // follow ally
      std::make_pair(move_to_entity(e, "target"), make_pure_utility([](Blackboard &, const WorldEntSensorInfo &info) {
        return info.type == ENT_ALLY ? 50.f - 10.f * info.dist : -FLT_MAX;
      }))},
    "allTargets", "target");

  e.add<WorldPureInfoGatherer>();
  e.set(BehaviourTree{root});
}

static flecs::entity create_monster(flecs::world &ecs, int x, int y, Color col, const char *texture_src, float hp = 100.f)
{
  flecs::entity textureSrc = ecs.entity(texture_src);
  return ecs.entity()
    .set(Position{x, y})
    .set(MovePos{x, y})
    .set(Hitpoints{hp})
    .set(Action{EA_NOP})
    .set(Color{col})
    .add<TextureSource>(textureSrc)
    .set(StateMachine{})
    .set(Team{1})
    .set(NumActions{1, 0})
    .set(MeleeDamage{20.f})
    .set(Blackboard{});
}

static flecs::entity create_explorer_monster(flecs::world &ecs, int x, int y, Color col, const char *texture_src)
{
  return create_monster(ecs, x, y, col, texture_src, 300.f).add<PickupUser>();
}

static flecs::entity create_gatherer(flecs::world &ecs, int x, int y, Color col, const char *texture_src)
{
  flecs::entity textureSrc = ecs.entity(texture_src);
  flecs::entity spawn = ecs.entity().set(Position{x, y}).set(Color{0x33, 0x33, 0x33, 0xff});
  flecs::entity gatherer = ecs.entity()
    .set(Position{x, y})
    .set(MovePos{x, y})
    .set(Hitpoints{150.f})
    .set(Action{EA_NOP})
    .set(Color{col})
    .add<TextureSource>(textureSrc)
    .set(StateMachine{})
    .set(Team{2})
    .set(NumActions{1, 0})
    .set(MeleeDamage{20.f})
    .set(Blackboard{})
    .set(HealsCollected{0})
    .set(PowerupsCollected{0});
  gatherer.insert([&](Blackboard &bb) {
    size_t id = bb.regName<flecs::entity>("spawn_point");
    bb.set(id, spawn);
  });
  return gatherer;
}

static flecs::entity create_guardsman(flecs::world &ecs, flecs::entity first_wp, int x, int y, Color col, const char *texture_src)
{
  flecs::entity textureSrc = ecs.entity(texture_src);
  flecs::entity guardsman = ecs.entity()
    .set(Position{x, y})
    .set(MovePos{x, y})
    .set(Hitpoints{300.f})
    .set(Action{EA_NOP})
    .set(Color{col})
    .add<TextureSource>(textureSrc)
    .set(StateMachine{})
    .set(Team{2})
    .set(NumActions{1, 0})
    .set(MeleeDamage{20.f})
    .set(Blackboard{});
  guardsman.insert([&](Blackboard &bb) {
    size_t id = bb.regName<flecs::entity>("next_waypoint");
    bb.set(id, first_wp);
  });
  return guardsman;
}

static flecs::entity create_waypoint_loop(flecs::world &ecs, const std::vector<Position> &points)
{
  assert(points.size() > 0);
  flecs::entity first = ecs.entity().set(points[0]).set(Color{0x44, 0x44, 0x44, 0x44});
  flecs::entity prev = first;
  for (size_t i = 1; i < points.size(); ++i) {
    flecs::entity next = ecs.entity().set(points[i]).set(Color{0x44, 0x44, 0x44, 0x44});
    prev.set(Waypoint{next});
    prev = next;
  }
  if (points.size() > 1)
    prev.set(Waypoint{first});
  return first;
}

static void create_player(flecs::world &ecs, int x, int y, const char *texture_src)
{
  flecs::entity textureSrc = ecs.entity(texture_src);
  ecs.entity("player")
    .set(Position{x, y})
    .set(MovePos{x, y})
    .set(Hitpoints{100.f})
    .set(Action{EA_NOP})
    .add<IsPlayer>()
    .add<PickupUser>()
    .set(Team{0})
    .set(PlayerInput{})
    .set(NumActions{2, 0})
    .set(Color{255, 255, 255, 255})
    .add<TextureSource>(textureSrc)
    .set(MeleeDamage{50.f});
}

static void create_heal(flecs::world &ecs, int x, int y, float amount)
{
  ecs.entity().set(Position{x, y}).set(HealAmount{amount}).set(Color{0xff, 0x44, 0x44, 0xff});
}

static void create_powerup(flecs::world &ecs, int x, int y, float amount)
{
  ecs.entity().set(Position{x, y}).set(PowerupAmount{amount}).set(Color{0xff, 0xff, 0x00, 0xff});
}

static void register_roguelike_systems(flecs::world &ecs)
{
  ecs.system<PlayerInput, Action, const IsPlayer>().each([&](PlayerInput &inp, Action &a, const IsPlayer) {
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
  });
  ecs.system<const Position, const Color>().without<TextureSource>(flecs::Wildcard).each([&](const Position &pos, const Color color) {
    const Rectangle rect = {float(pos.x), float(pos.y), 1, 1};
    DrawRectangleRec(rect, color);
  });
  ecs.system<const Position, const Color>()
    .with<TextureSource>(flecs::Wildcard)
    .each([&](flecs::entity e, const Position &pos, const Color color) {
      const auto textureSrc = e.target<TextureSource>();
      DrawTextureQuad(*textureSrc.get<Texture2D>(), Vector2{1, 1}, Vector2{0, 0}, Rectangle{float(pos.x), float(pos.y), 1, 1}, color);
    });
  ecs.system<const Position, const Hitpoints>().each([&](const Position &pos, const Hitpoints &hp) {
    constexpr float hpPadding = 0.05f;
    const float hpWidth = 1.f - 2.f * hpPadding;
    const Rectangle underRect = {float(pos.x + hpPadding), float(pos.y - 0.25f), hpWidth, 0.1f};
    DrawRectangleRec(underRect, BLACK);
    const Rectangle hpRect = {float(pos.x + hpPadding), float(pos.y - 0.25f), hp.hitpoints / 100.f * hpWidth, 0.1f};
    DrawRectangleRec(hpRect, RED);
  });
}


void init_roguelike(flecs::world &ecs)
{
  register_roguelike_systems(ecs);

  ecs.entity("swordsman_tex").set(Texture2D{LoadTexture("assets/swordsman.png")});
  ecs.entity("minotaur_tex").set(Texture2D{LoadTexture("assets/minotaur.png")});

  ecs.observer<Texture2D>().event(flecs::OnRemove).each([](Texture2D texture) { UnloadTexture(texture); });

  create_fuzzy_monster_beh(create_monster(ecs, 5, 5, Color{0xee, 0x00, 0xee, 0xff}, "minotaur_tex"));
  create_fuzzy_monster_beh(create_monster(ecs, 10, -5, Color{0xee, 0x00, 0xee, 0xff}, "minotaur_tex"));
  create_fuzzy_monster_beh(create_monster(ecs, -5, -5, Color{0x11, 0x11, 0x11, 0xff}, "minotaur_tex"));

  /*
  create_gatherer_beh(create_gatherer(ecs, -5, 5, Color{0, 255, 0, 255}, "minotaur_tex"));

  create_guardsman_beh(create_guardsman(ecs, create_waypoint_loop(ecs, {{6, 6}, {-6, 6}, {-6, -6}, {6, -6}}), 6, -6,
    Color{0, 0, 255, 255}, "minotaur_tex"));
    */

  create_explorer_monster_beh(create_explorer_monster(ecs, 7, 7, Color{0xf1, 0xf1, 0xf1, 0xff}, "minotaur_tex"));

  create_player(ecs, 0, 0, "swordsman_tex");

  create_powerup(ecs, -5, -3, 10.f);
  create_powerup(ecs, -5, 2, 10.f);

  create_powerup(ecs, 7, 7, 10.f);
  create_powerup(ecs, 10, -6, 10.f);
  create_powerup(ecs, 10, -4, 10.f);

  create_heal(ecs, -3, -7, 50.f);
  create_heal(ecs, -3, 2, 50.f);

  create_heal(ecs, -5, -5, 50.f);
  create_heal(ecs, -5, 5, 50.f);

  ecs.entity("world").set(TurnCounter{}).set(ActionLog{});
}

static bool is_player_acted(flecs::world &ecs)
{
  static auto processPlayer = ecs.query<const IsPlayer, const Action>();
  bool playerActed = false;
  processPlayer.each([&](const IsPlayer, const Action &a) { playerActed = a.action != EA_NOP; });
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
  printf("pushing to log %s\n", msg);
  queryLog.each([&](ActionLog &l, const TurnCounter &c) {
    l.log.push_back(std::to_string(c.count) + ": " + msg);
    printf("pushed to log %s\n", msg);
    if (l.log.size() > l.capacity)
      l.log.erase(l.log.begin());
  });
}

static void process_actions(flecs::world &ecs)
{
  static auto processActions = ecs.query<Action, Position, MovePos, const MeleeDamage, const Team>();
  static auto processHealActions = ecs.query<Action, const Position, const HealAmount, const Team>();
  static auto processHeals = ecs.query<Action, Hitpoints>();
  static auto checkAttacks = ecs.query<const MovePos, Hitpoints, const Team>();
  static auto checkHealTargets = ecs.query<const Position, Hitpoints, const Team>();
  // Process all actions
  ecs.defer([&] {
    processHeals.each([&](Action &a, Hitpoints &hp) {
      if (a.action != EA_HEAL_SELF)
        return;
      a.action = EA_NOP;
      push_to_log(ecs, "Monster healed itself");
      hp.hitpoints += 10.f;
    });
    processActions.each([&](flecs::entity entity, Action &a, Position &pos, MovePos &mpos, const MeleeDamage &dmg, const Team &team) {
      Position nextPos = move_pos(pos, a.action);
      bool blocked = false;
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
    processHealActions.each([&](flecs::entity entity, Action &a, const Position &pos, const HealAmount &amt, const Team &team) {
      if (a.action != EA_HEAL_ALLY)
        return;
      bool done = false;
      checkHealTargets.each([&](flecs::entity ally, const Position &apos, Hitpoints &hp, const Team &ateam) {
        if (done || ally == entity || ateam.team != team.team || dist(pos, apos) > 2.2f) // Random
          return;
        hp.hitpoints += amt.amount;
        done = true;
      });
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

  static auto actorPickup = ecs.query<const PickupUser, const Position, Hitpoints, MeleeDamage>();
  static auto gathererPickup = ecs.query<const Position, HealsCollected, PowerupsCollected>();
  static auto healPickup = ecs.query<const Position, const HealAmount>();
  static auto powerupPickup = ecs.query<const Position, const PowerupAmount>();
  ecs.defer([&] {
    actorPickup.each([&](const PickupUser &, const Position &pos, Hitpoints &hp, MeleeDamage &dmg) {
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
    gathererPickup.each([&](const Position &pos, HealsCollected &heals, PowerupsCollected &powerups) {
      healPickup.each([&](flecs::entity entity, const Position &ppos, const HealAmount &) {
        if (entity.is_alive() && pos == ppos)
        {
          ++heals.count;
          entity.destruct();
        }
      });
      powerupPickup.each([&](flecs::entity entity, const Position &ppos, const PowerupAmount &) {
        if (entity.is_alive() && pos == ppos)
        {
          ++powerups.count;
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
  static auto gatherWorldPureInfo = ecs.query<Blackboard, const Position, const Hitpoints, const WorldPureInfoGatherer, const Team>();
  static auto actorsQuery = ecs.query<const Position, const Hitpoints, const Team>();
  static auto healPickupQuery = ecs.query<const Position, const HealAmount>();
  static auto powerupPickupQuery = ecs.query<const Position, const PowerupAmount>();

  gatherWorldInfo.each([&](Blackboard &bb, const Position &pos, const Hitpoints &hp, WorldInfoGatherer, const Team &team) {
    // first gather all needed names (without cache)
    push_info_to_bb(bb, "hp", hp.hitpoints);
    float numAllies = 0; // note float
    float closestEnemyDist = 100.f;
    actorsQuery.each([&](const Position &apos, Hitpoints, const Team &ateam) {
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

  gatherWorldPureInfo.each([&](flecs::entity ent, Blackboard &bb, const Position &pos, const Hitpoints &hp, WorldPureInfoGatherer,
                             const Team &team) {
    std::vector<WorldEntSensorInfo> entInfos{};

    actorsQuery.each([&](flecs::entity aent, const Position &apos, const Hitpoints &ahp, const Team &ateam) {
      if (aent == ent)
        return;

      entInfos.emplace_back(ateam.team == team.team ? ENT_ALLY : ENT_ENEMY, dist(pos, apos), ahp.hitpoints, aent);
    });
    healPickupQuery.each([&](flecs::entity hent, const Position &hpos, const HealAmount &amt) {
      entInfos.emplace_back(ENT_HEAL, dist(pos, hpos), amt.amount, hent);
    });
    powerupPickupQuery.each([&](flecs::entity pent, const Position &ppos, const PowerupAmount &amt) {
      entInfos.emplace_back(ENT_POWERUP, dist(pos, ppos), amt.amount, pent);
    });

    // By copy cause I don't want to rewrite bb stuff for rvals
    push_info_to_bb(bb, "hp", hp.hitpoints);
    push_info_to_bb(bb, "allTargets", entInfos);
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
      });
      turnIncrementer.each([](TurnCounter &tc) { tc.count++; });
    }
    process_actions(ecs);
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
}
