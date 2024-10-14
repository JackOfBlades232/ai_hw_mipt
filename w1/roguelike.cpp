#include "roguelike.h"
#include "ecsTypes.h"
#include "raylib.h"
#include "util.h"
#include "stateMachine.h"
#include "aiLibrary.h"
#include <cstdio>
#include <cfloat>
#include <functional>

template <class CompType>
static std::tuple<flecs::entity, Position, CompType> find_closest_of_type(
  flecs::world &ecs, flecs::entity entity, std::function<float(const CompType &comp)> scorer = [](const CompType &) { return 0.f; })
{
  static auto query = ecs.query<const Position, const CompType>();
  float closestScore = FLT_MAX;
  Position myPos;
  Position closestPos;
  CompType closestComp;
  flecs::entity closestEnt;
  entity.get([&](const Position &pos) { myPos = pos; });
  query.each([&](flecs::entity comp_ent, const Position &pos, const CompType &comp) {
    if (entity == comp_ent)
      return;
    float curDist = dist(myPos, pos);
    float score = curDist + scorer(comp);
    if (score < closestScore)
    {
      closestEnt = comp_ent;
      closestPos = pos;
      closestComp = comp;
      closestScore = score;
    }
  });
  return {closestEnt, closestPos, closestComp};
}

static void add_patrol_attack_flee_sm(flecs::entity entity)
{
  entity.get([](StateMachine &sm) {
    int patrol = sm.addState(create_patrol_state(3.f));
    int moveToEnemy = sm.addState(create_move_to_enemy_state());
    int fleeFromEnemy = sm.addState(create_flee_from_enemy_state());

    sm.addTransition(create_enemy_close_enough_transition(3.f), patrol, moveToEnemy);
    sm.addTransition(create_negate_transition(create_enemy_close_enough_transition(5.f)), moveToEnemy, patrol);

    sm.addTransition(create_and_transition(create_hitpoints_less_than_transition(60.f), create_enemy_close_enough_transition(5.f)),
      moveToEnemy, fleeFromEnemy);
    sm.addTransition(create_and_transition(create_hitpoints_less_than_transition(60.f), create_enemy_close_enough_transition(3.f)),
      patrol, fleeFromEnemy);

    sm.addTransition(create_negate_transition(create_enemy_close_enough_transition(7.f)), fleeFromEnemy, patrol);
  });
}

static void add_patrol_flee_sm(flecs::entity entity)
{
  entity.get([](StateMachine &sm) {
    int patrol = sm.addState(create_patrol_state(3.f));
    int fleeFromEnemy = sm.addState(create_flee_from_enemy_state());

    sm.addTransition(create_enemy_close_enough_transition(3.f), patrol, fleeFromEnemy);
    sm.addTransition(create_negate_transition(create_enemy_close_enough_transition(5.f)), fleeFromEnemy, patrol);
  });
}

static void add_attack_sm(flecs::entity entity)
{
  entity.get([](StateMachine &sm) { sm.addState(create_move_to_enemy_state()); });
}

static void add_slime_sm(flecs::entity entity, bool can_split)
{
  entity.insert([&](StateMachine &sm) {
    int moveToEnemy = sm.addState(create_move_to_enemy_state());
    int split = sm.addState(create_split_state());

    sm.addTransition(create_one_shot_transition(create_hitpoints_less_than_transition(80.f), !can_split), moveToEnemy, split);
    sm.addTransition(create_always_transition(), split, moveToEnemy);
  });
}

static void add_archer_sm(flecs::entity entity)
{
  entity.get([](StateMachine &sm) {
    int patrol = sm.addState(create_patrol_state(3.f));
    int shootEnemy = sm.addState(create_shoot_enemy_state());
    int fleeFromEnemy = sm.addState(create_flee_from_enemy_state());

    sm.addTransition(create_enemy_close_enough_transition(5.f), patrol, shootEnemy);
    sm.addTransition(create_negate_transition(create_enemy_close_enough_transition(5.f)), shootEnemy, patrol);

    sm.addTransition(create_enemy_close_enough_transition(3.f), shootEnemy, fleeFromEnemy);
    sm.addTransition(create_negate_transition(create_enemy_close_enough_transition(7.f)), fleeFromEnemy, patrol);
  });
}

static void add_healer_sm(flecs::entity entity)
{
  entity.get([](StateMachine &sm) {
    int follow = sm.addState(create_follow_player_state(1.f));
    int moveToEnemy = sm.addState(create_move_to_enemy_state());
    int moveToPlayer = sm.addState(create_move_to_player_state());

    sm.addTransition(create_and_transition(create_player_hitpoints_less_than_transition(40.f), create_pouch_ready_transition()),
      follow, moveToPlayer);
    sm.addTransition(create_and_transition(create_player_hitpoints_less_than_transition(40.f), create_pouch_ready_transition()),
      moveToEnemy, moveToPlayer);
    sm.addTransition(create_negate_transition(
                       create_and_transition(create_player_hitpoints_less_than_transition(40.f), create_pouch_ready_transition())),
      moveToPlayer, follow);

    sm.addTransition(create_enemy_close_enough_transition(2.f), follow, moveToEnemy);
    sm.addTransition(create_player_hitpoints_less_than_transition(40.f), moveToEnemy, follow);
    sm.addTransition(create_player_far_enough_transition(4.f), moveToEnemy, follow);
  });
}

static void add_crafter_sm(flecs::entity entity)
{
  entity.insert([](StateMachine &sm) {
    StateMachine *sleepingSm = new StateMachine;
    StateMachine *craftingSm = new StateMachine;

    int gotoSleep = sleepingSm->addState(create_goto_state([](flecs::world &ecs, flecs::entity entity) {
      int money;
      entity.get([&](const CrafterState &crafter) { money = crafter.money; });
      auto [flophouse, flophousePos, flophouseComp] =
        find_closest_of_type<Flophouse>(ecs, entity, [&](const Flophouse &flophouse) { return -(money - flophouse.nightCost); });
      return DestinationPos{flophousePos.x, flophousePos.y};
    }));
    int sleep = sleepingSm->addState(create_activity_state(8, [](flecs::world &ecs, flecs::entity entity) {
      auto [flophouse, flophousePos, flophouseComp] = find_closest_of_type<Flophouse>(ecs, entity);
      entity.insert([&](CrafterState &crafter) {
        crafter.money -= flophouseComp.nightCost;
        crafter.sleepDeprivation -= 8.f;
        if (crafter.sleepDeprivation < 0.f)
          crafter.sleepDeprivation = 0.f;
        printf("Crafter %lu slept: money=%f, boredom=%f, sleep deprivation=%f\n", entity.id(), crafter.money, crafter.boredom,
          crafter.sleepDeprivation);
      });
    }));
    sleepingSm->addTransition(create_arrived_transition(), gotoSleep, sleep);

    int gotoCraft = craftingSm->addState(create_goto_state([](flecs::world &ecs, flecs::entity entity) {
      CrafterState crafter;
      entity.insert([&](const CrafterState &a_crafter) { crafter = a_crafter; });
      auto [station, stationPos, stationComp] =
        find_closest_of_type<CraftStation>(ecs, entity, [&](const CraftStation &station) { return -station.yield; });
      return DestinationPos{stationPos.x, stationPos.y};
    }));
    int craft = craftingSm->addState(create_activity_state(4, [](flecs::world &ecs, flecs::entity entity) {
      auto [station, stationPos, stationComp] = find_closest_of_type<CraftStation>(ecs, entity);
      entity.insert([&](CrafterState &crafter) {
        crafter.money += stationComp.yield;
        printf("Crafter %lu worked: money=%f, boredom=%f, sleep deprivation=%f\n", entity.id(), crafter.money, crafter.boredom,
          crafter.sleepDeprivation);
      });
    }));
    craftingSm->addTransition(create_arrived_transition(), gotoCraft, craft);

    int goWander = sm.addState(create_wander_state());
    int goSleep = sm.addState(create_nested_sm_state(sleepingSm));
    int goCraft = sm.addState(create_nested_sm_state(craftingSm, [](flecs::world &, flecs::entity entity) {
      entity.insert([&](CrafterState &state) {
        state.boredom += 2.f;
        state.sleepDeprivation += 0.5f;
        printf("Crafter %lu is working: money=%f, boredom=%f, sleep deprivation=%f\n", entity.id(), state.money, state.boredom,
          state.sleepDeprivation);
      });
    }));

    sm.addTransition(create_crafter_need_transition([](const CrafterState &state) { return state.money < 6.f; }), goWander, goCraft);
    sm.addTransition(
      create_crafter_need_transition([](const CrafterState &state) { return state.money >= 4.f && state.sleepDeprivation >= 10.f; }),
      goCraft, goSleep);
    sm.addTransition(
      create_crafter_need_transition([](const CrafterState &state) { return state.money >= 4.f && state.sleepDeprivation >= 10.f; }),
      goWander, goSleep);
    sm.addTransition(
      create_crafter_need_transition([](const CrafterState &state) { return state.money >= 4.f && state.boredom > 3.f; }), goCraft,
      goWander);
    sm.addTransition(create_crafter_need_transition([](const CrafterState &state) { return state.sleepDeprivation <= 0.f; }), goSleep,
      goCraft);
  });
}

static flecs::entity create_monster(flecs::world &ecs, int x, int y, Color color, float hp = 100.f)
{
  return ecs.entity()
    .set(Position{x, y})
    .set(MovePos{x, y})
    .set(DestinationPos{x, y})
    .set(Hitpoints{hp})
    .set(Action{EA_NOP})
    .set(Color{color})
    .set(StateMachine{})
    .set(Team{1})
    .set(NumActions{1, 0})
    .set(MeleeDamage{20.f});
}

static flecs::entity create_player(flecs::world &ecs, int x, int y)
{
  return ecs.entity("player")
    .set(Position{x, y})
    .set(MovePos{x, y})
    .set(Hitpoints{100.f})
    .set(GetColor(0xeeeeeeff))
    .set(Action{EA_NOP})
    .add<IsPlayer>()
    .set(Team{0})
    .set(PlayerInput{})
    .set(NumActions{2, 0})
    .set(MeleeDamage{50.f});
}

static flecs::entity create_healer(flecs::world &ecs, int x, int y, float amount, int cooldown)
{
  return ecs.entity("healer")
    .set(Position{x, y})
    .set(MovePos{x, y})
    .set(Hitpoints{100.f})
    .set(GetColor(0x0055ffff))
    .set(Action{EA_NOP})
    .set(StateMachine{})
    .set(Team{0})
    .set(NumActions{1, 0})
    .set(MeleeDamage{25.f})
    .set(HealerPouch{amount, 0, cooldown});
}

static flecs::entity create_crafter(flecs::world &ecs, int x, int y, Color color)
{
  return ecs.entity()
    .set(Position{x, y})
    .set(MovePos{x, y})
    .set(DestinationPos{x, y})
    .set(Hitpoints{10000000.f})
    .set(Action{EA_NOP})
    .set(Color{color})
    .set(StateMachine{})
    .set(Team{2})
    .set(NumActions{1, 0})
    .set(MeleeDamage{0.f})
    .set(CrafterState{0, 6, 0, 4});
}

static void create_heal(flecs::world &ecs, int x, int y, float amount)
{
  ecs.entity().set(Position{x, y}).set(HealAmount{amount}).set(GetColor(0x44ff44ff));
}

static void create_powerup(flecs::world &ecs, int x, int y, float amount)
{
  ecs.entity().set(Position{x, y}).set(PowerupAmount{amount}).set(Color{255, 255, 0, 255});
}

static void create_craft_station(flecs::world &ecs, int x, int y, float yield)
{
  ecs.entity().set(Position{x, y}).set(CraftStation{yield});
}

static void create_flophouse(flecs::world &ecs, int x, int y, float cost)
{
  ecs.entity().set(Position{x, y}).set(Flophouse{cost});
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

  // @TODO: make turn based?
  ecs.system<BulletPos, const ShotDirection, const Team>().each(
    [&](flecs::entity entity, BulletPos &pos, const ShotDirection &dir, const Team &bullet_team) {
      // @HACK: using hard set 60fps
      pos.x += dir.x * 2.5f / 60.f;
      pos.y += dir.y * 2.5f / 60.f;

      static auto processTragets = ecs.query<const Position, Hitpoints, const Team>();
      bool hit = false;
      processTragets.each([&](const Position &target_pos, Hitpoints &target_hp, const Team &team) {
        if (hit || team.team == bullet_team.team)
          return;

        float dx = (float)target_pos.x - pos.x;
        float dy = (float)target_pos.y - pos.y;
        // @TODO: make damage component
        if (dx < 0.1f && dx > -1.f && dy < 0.1f && dy > -1.f)
        {
          target_hp.hitpoints -= 10.f;
          entity.destruct();
          hit = true;
        }
      });
    });

  ecs.system<const Position, const CraftStation>().each([&](const Position &pos, const CraftStation) {
    const Rectangle rect = {(float(pos.x) - 0.2f) * draw_scale, (float(pos.y) - 0.2f) * draw_scale, 1.4f * draw_scale, 1.4f * draw_scale};
    DrawRectangleRounded(rect, 0.2f, 4, GetColor(0x444444ff));
  });
  ecs.system<const Position, const Flophouse>().each([&](const Position &pos, const Flophouse) {
    DrawCircleV({(float(pos.x) + 0.5f) * draw_scale, (float(pos.y) + 0.5f) * draw_scale}, 0.7f * draw_scale, GetColor(0x444444ff));
  });

  ecs.system<const BulletPos>().each([&](const BulletPos &pos) {
    // @TODO: make size and color components
    const Rectangle rect = {pos.x * draw_scale, pos.y * draw_scale, 0.1f * draw_scale, 0.1f * draw_scale};
    DrawRectangleRec(rect, GetColor(0xffff00ff));
  });
  ecs.system<const Position, const Color>().without<TextureSource>(flecs::Wildcard).each([&](const Position &pos, const Color color) {
    const Rectangle rect = {float(pos.x) * draw_scale, float(pos.y) * draw_scale, 1.f * draw_scale, 1.f * draw_scale};
    DrawRectangleRec(rect, color);
  });
  ecs.system<const Position, const Color>()
    .with<TextureSource>(flecs::Wildcard)
    .each([&](flecs::entity e, const Position &pos, const Color color) {
      const auto textureSrc = e.target<TextureSource>();
      DrawTextureQuad(*textureSrc.get<Texture2D>(), Vector2{1, 1}, Vector2{0, 0},
        Rectangle{float(pos.x) * draw_scale, float(pos.y) * draw_scale, 1 * draw_scale, 1 * draw_scale}, color);
    });
}


void init_roguelike(flecs::world &ecs)
{
  register_roguelike_systems(ecs);

  if (demo_type & DEMO_INTIAL)
  {
    add_patrol_attack_flee_sm(create_monster(ecs, 5, 5, GetColor(0xee00eeff)));
    add_patrol_attack_flee_sm(create_monster(ecs, 10, -5, GetColor(0xee00eeff)));
    add_patrol_flee_sm(create_monster(ecs, -5, -5, GetColor(0x111111ff)));
    add_attack_sm(create_monster(ecs, -5, 5, GetColor(0x880000ff)));

    create_powerup(ecs, 7, 7, 10.f);
    create_powerup(ecs, 10, -6, 10.f);
    create_powerup(ecs, 10, -4, 10.f);

    create_heal(ecs, -5, -5, 50.f);
    create_heal(ecs, -5, 5, 50.f);
  }

  if (demo_type & DEMO_NEW_ENEMIES)
  {
    add_slime_sm(create_monster(ecs, 0, -8, GetColor(0x00ff00ff)), true);
    add_slime_sm(create_monster(ecs, -7, -5, GetColor(0x00ff00ff)), true);
    add_slime_sm(create_monster(ecs, 6, 1, GetColor(0x00ff00ff)), true);

    add_archer_sm(create_monster(ecs, -3, 7, GetColor(0xffdd11ff)));
    add_archer_sm(create_monster(ecs, 0, 7, GetColor(0xffdd11ff)));
    add_archer_sm(create_monster(ecs, 3, 7, GetColor(0xffdd11ff)));
  }

  if (demo_type & DEMO_NEW_ALLY)
    add_healer_sm(create_healer(ecs, 0, 2, 30.f, 10));
  
  if (demo_type & DEMO_CRAFTER)
  {
    create_craft_station(ecs, -5, 20, 5.f);
    create_craft_station(ecs, -30, 20, 7.5f);
    create_flophouse(ecs, -15, 10, 12.5f);

    add_crafter_sm(create_crafter(ecs, -30, 15, GetColor(0x00ffffff)));
  }

  create_player(ecs, 0, 0);
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

void create_clone(flecs::world &ecs, flecs::entity entity)
{
  entity.insert([&](const Position &pos, const Hitpoints &hp, const Color &color, Action &a) {
    add_slime_sm(create_monster(ecs, pos.x, pos.y - 1 /* offset */, color, hp.hitpoints), false);
    a.action = EA_MOVE_LEFT; // offset
  });
}

void shoot(flecs::world &ecs, const Position &pos, const ShotDirection &shot, const Team &team)
{
  ecs.entity().set(BulletPos{(float)pos.x, (float)pos.y}).set(shot).set(team);
}

static void process_actions(flecs::world &ecs)
{
  static auto healers = ecs.query<HealerPouch>();

  // Can't be static anymore cause slimes are spawning.
  static auto processActions = ecs.query<Action, Position, MovePos, const MeleeDamage, const Team, HealerPouch *>();
  static auto checkCollisions = ecs.query<const MovePos, Hitpoints, const Team>();

  // Process all actions
  ecs.defer([&] {
    processActions.each([&](flecs::entity entity, Action &a, Position &pos, MovePos &mpos, const MeleeDamage &dmg, const Team &team,
                          HealerPouch *healer_pouch) {
      Position nextPos = move_pos(pos, a.action);
      bool blocked = false;
      checkCollisions.each([&](flecs::entity other, const MovePos &epos, Hitpoints &hp, const Team &other_team) {
        if (entity != other && epos == nextPos)
        {
          blocked = true;
          if (team.team != other_team.team)
            hp.hitpoints -= dmg.damage;
          else if (healer_pouch && healer_pouch->cooldown == 0)
          {
            hp.hitpoints += healer_pouch->amount;
            healer_pouch->cooldown = healer_pouch->maxCooldown + 1; // One will be taken off right away
          }
        }
      });
      if (blocked)
        a.action = EA_NOP;
      else
        mpos = nextPos;
    });
    // now move
    processActions.each([&](flecs::entity, Action &a, Position &pos, MovePos &mpos, const MeleeDamage &, const Team &, HealerPouch *) {
      pos = mpos;
      a.action = EA_NOP;
    });
    // @NOTE: should be just a cooldown component, but whatever
    healers.each([](HealerPouch &pouch) {
      if (pouch.cooldown > 0)
        --pouch.cooldown;
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

void process_turn(flecs::world &ecs)
{
  static auto stateMachineAct = ecs.query<StateMachine>();
  if (is_player_acted(ecs))
  {
    if (upd_player_actions_count(ecs))
    {
      // Plan action for NPCs
      ecs.defer([&] { stateMachineAct.each([&](flecs::entity e, StateMachine &sm) { sm.act(0.f, ecs, e); }); });
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
}
