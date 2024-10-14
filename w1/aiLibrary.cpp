#include "aiLibrary.h"
#include "ecsTypes.h"
#include "raylib.h"
#include "stateMachine.h"
#include "util.h"
#include <cstdio>
#include <flecs.h>
#include <cfloat>

template <typename T, typename U>
static int move_towards(const T &from, const U &to)
{
  int deltaX = to.x - from.x;
  int deltaY = to.y - from.y;
  if (deltaX == deltaY && deltaY == 0)
    return EA_NOP;
  if (abs(deltaX) > abs(deltaY))
    return deltaX > 0 ? EA_MOVE_RIGHT : EA_MOVE_LEFT;
  return deltaY < 0 ? EA_MOVE_UP : EA_MOVE_DOWN;
}

static int inverse_move(int move)
{
  return move == EA_MOVE_LEFT    ? EA_MOVE_RIGHT
         : move == EA_MOVE_RIGHT ? EA_MOVE_LEFT
         : move == EA_MOVE_UP    ? EA_MOVE_DOWN
         : move == EA_MOVE_DOWN  ? EA_MOVE_UP
                                 : move;
}


template <typename Callable>
static void on_closest_enemy_pos(flecs::world &ecs, flecs::entity entity, Callable c)
{
  static auto enemiesQuery = ecs.query<const Position, const Team>();
  entity.insert([&](const Position &pos, const Team &t, Action &a) {
    flecs::entity closestEnemy;
    float closestDist = FLT_MAX;
    Position closestPos;
    enemiesQuery.each([&](flecs::entity enemy, const Position &epos, const Team &et) {
      if (t.team == et.team)
        return;
      float curDist = dist(epos, pos);
      if (curDist < closestDist)
      {
        closestDist = curDist;
        closestPos = epos;
        closestEnemy = enemy;
      }
    });
    if (ecs.is_valid(closestEnemy))
      c(a, pos, closestPos);
  });
}

class MoveToEnemyState : public State
{
public:
  void enter(flecs::world &, flecs::entity) const override {}
  void exit(flecs::world &, flecs::entity) const override {}
  void act(float, flecs::world &ecs, flecs::entity entity) const override
  {
    on_closest_enemy_pos(ecs, entity,
      [&](Action &a, const Position &pos, const Position &enemy_pos) { a.action = move_towards(pos, enemy_pos); });
  }
};

class MoveToPlayerState : public State
{
public:
  void enter(flecs::world &, flecs::entity) const override {}
  void exit(flecs::world &, flecs::entity) const override {}
  void act(float, flecs::world &ecs, flecs::entity entity) const override
  {
    static auto player = ecs.query<const IsPlayer, const Position>();
    entity.insert([&](const Position &pos, Action &a) {
      player.each([&](const IsPlayer, const Position &target_pos) { a.action = move_towards(pos, target_pos); });
    });
  }
};

class FleeFromEnemyState : public State
{
public:
  FleeFromEnemyState() {}
  void enter(flecs::world &, flecs::entity) const override {}
  void exit(flecs::world &, flecs::entity) const override {}
  void act(float, flecs::world &ecs, flecs::entity entity) const override
  {
    on_closest_enemy_pos(ecs, entity,
      [&](Action &a, const Position &pos, const Position &enemy_pos) { a.action = inverse_move(move_towards(pos, enemy_pos)); });
  }
};

class ShootEnemyState : public State
{
public:
  void enter(flecs::world &, flecs::entity) const override {}
  void exit(flecs::world &, flecs::entity) const override {}
  void act(float, flecs::world &ecs, flecs::entity entity) const override
  {
    int team;
    entity.get([&](const Team &shooter_team) { team = shooter_team.team; });

    on_closest_enemy_pos(ecs, entity, [&](Action &, const Position &pos, const Position &enemy_pos) {
      float distance = dist(pos, enemy_pos);
      float dir_x = (float)(enemy_pos.x - pos.x) / distance;
      float dir_y = (float)(enemy_pos.y - pos.y) / distance;

      // @TODO: unhack the dependency
      extern void shoot(flecs::world &, const Position &, const ShotDirection &, const Team &);
      shoot(ecs, pos, ShotDirection{dir_x, dir_y}, Team{team});
    });
  }
};

class WanderState : public State
{
public:
  void enter(flecs::world &, flecs::entity) const override {}
  void exit(flecs::world &, flecs::entity) const override {}
  void act(float, flecs::world &, flecs::entity entity) const override
  {
    entity.insert([&](Action &a) { a.action = GetRandomValue(EA_MOVE_START, EA_MOVE_END - 1); });
    entity.insert([&](CrafterState &crafter) {
      crafter.boredom -= 1.f;
      crafter.sleepDeprivation += 0.3f;
      printf("Crafter %lu wandering: money=%f, boredom=%f, sleep deprivation=%f\n", entity.id(), crafter.money, crafter.boredom,
        crafter.sleepDeprivation);
    });
  }
};

class GoToState : public State
{
  AiDestinationProvider destProvider;

public:
  GoToState(AiDestinationProvider &&dest_provider) : destProvider(std::move(dest_provider)) {}
  void enter(flecs::world &ecs, flecs::entity entity) const override { entity.set(destProvider(ecs, entity)); }
  void exit(flecs::world &, flecs::entity) const override {}
  void act(float, flecs::world &, flecs::entity entity) const override
  {
    entity.insert([&](const Position &pos, const DestinationPos &ppos, Action &a) { a.action = move_towards(pos, ppos); });
  }
};


class PatrolState : public State
{
  float patrolDist;

public:
  PatrolState(float dist) : patrolDist(dist) {}
  void enter(flecs::world &, flecs::entity) const override {}
  void exit(flecs::world &, flecs::entity) const override {}
  void act(float, flecs::world &, flecs::entity entity) const override
  {
    entity.insert([&](const Position &pos, const DestinationPos &ppos, Action &a) {
      if (dist(pos, ppos) > patrolDist)
        a.action = move_towards(pos, ppos); // do a recovery walk
      else
      {
        // do a random walk
        a.action = GetRandomValue(EA_MOVE_START, EA_MOVE_END - 1);
      }
    });
  }
};

class FollowPlayerState : public State
{
  const float followDist;

public:
  FollowPlayerState(float follow_dist) : followDist(follow_dist) {}

  void enter(flecs::world &, flecs::entity) const override {}
  void exit(flecs::world &, flecs::entity) const override {}
  void act(float, flecs::world &ecs, flecs::entity entity) const override
  {
    static auto player = ecs.query<const IsPlayer, const Position>();
    entity.insert([&](const Position &pos, Action &a) {
      player.each([&](const IsPlayer, const Position &ppos) {
        if (dist(pos, ppos) >= followDist)
          a.action = move_towards(pos, ppos);
      });
    });
  }
};

class SplitState : public State
{
public:
  void enter(flecs::world &, flecs::entity) const override {}
  void exit(flecs::world &, flecs::entity) const override {}
  void act(float, flecs::world &ecs, flecs::entity entity) const override
  {
    // @TODO: unhack the dependency
    extern void create_clone(flecs::world &, flecs::entity);
    create_clone(ecs, entity);
  }
};

class AcitvityState : public State
{
  const int turns;
  mutable bool justEntered = true;
  AiCallback onDone;

public:
  AcitvityState(int in_turns, AiCallback &&on_done) : turns(in_turns), onDone(on_done) {}

  void enter(flecs::world &, flecs::entity) const override {}
  void exit(flecs::world &, flecs::entity) const override { justEntered = true; }
  void act(float, flecs::world &ecs, flecs::entity entity) const override
  {
    entity.insert([&](Activity &activity) {
      if (activity.turnsLeft == 0)
      {
        activity.turnsLeft = turns;
        if (justEntered)
          justEntered = false;
        else
          onDone(ecs, entity);
      }

      --activity.turnsLeft;
    });
  }
};

class NopState : public State
{
public:
  void enter(flecs::world &, flecs::entity) const override {}
  void exit(flecs::world &, flecs::entity) const override {}
  void act(float, flecs::world &, flecs::entity) const override {}
};

class NestedSMStage : public State
{
  StateMachine *sm; // owning
  AiCallback commonAct;

public:
  NestedSMStage(StateMachine *in_sm, AiCallback &&in_common_act) : sm(in_sm), commonAct(in_common_act) {}
  ~NestedSMStage() override { delete sm; }

  void enter(flecs::world &, flecs::entity) const override { sm->curStateIdx = INT_MAX; }
  void exit(flecs::world &ecs, flecs::entity entity) const override
  {
    if (sm->curStateIdx < sm->states.size())
      sm->states[sm->curStateIdx]->exit(ecs, entity);
  }
  void act(float dt, flecs::world &ecs, flecs::entity entity) const override
  {
    commonAct(ecs, entity);
    sm->act(dt, ecs, entity);
  }
};

class EnemyCloseEnoughTransition : public StateTransition
{
  float triggerDist;

public:
  EnemyCloseEnoughTransition(float in_dist) : triggerDist(in_dist) {}
  bool isAvailable(flecs::world &ecs, flecs::entity entity) const override
  {
    static auto enemiesQuery = ecs.query<const Position, const Team>();
    bool enemiesFound = false;
    entity.get([&](const Position &pos, const Team &t) {
      enemiesQuery.each([&](flecs::entity, const Position &epos, const Team &et) {
        if (t.team == et.team)
          return;
        float curDist = dist(epos, pos);
        enemiesFound |= curDist <= triggerDist;
      });
    });
    return enemiesFound;
  }
};

class PlayerFarEnoughTransition : public StateTransition
{
  float triggerDist;

public:
  PlayerFarEnoughTransition(float in_dist) : triggerDist(in_dist) {}
  bool isAvailable(flecs::world &ecs, flecs::entity entity) const override
  {
    static auto player = ecs.query<const IsPlayer, const Position>();
    bool playerIsFar = false;
    entity.get([&](const Position &pos) {
      player.each([&](const IsPlayer, const Position &ppos) { playerIsFar |= dist(ppos, pos) >= triggerDist; });
    });
    return playerIsFar;
  }
};

class HitpointsLessThanTransition : public StateTransition
{
  float threshold;

public:
  HitpointsLessThanTransition(float in_thres) : threshold(in_thres) {}
  bool isAvailable(flecs::world &, flecs::entity entity) const override
  {
    bool hitpointsThresholdReached = false;
    entity.get([&](const Hitpoints &hp) { hitpointsThresholdReached |= hp.hitpoints < threshold; });
    return hitpointsThresholdReached;
  }
};

class PlayerHitpointsLessThanTransition : public StateTransition
{
  float threshold;

public:
  PlayerHitpointsLessThanTransition(float in_thres) : threshold(in_thres) {}
  bool isAvailable(flecs::world &ecs, flecs::entity) const override
  {
    static auto player = ecs.query<const IsPlayer, const Hitpoints>();
    bool hitpointsThresholdReached = false;
    player.each([&](const IsPlayer, const Hitpoints &hp) { hitpointsThresholdReached |= hp.hitpoints < threshold; });
    return hitpointsThresholdReached;
  }
};

class HealerPouchReadyTransition : public StateTransition
{
public:
  bool isAvailable(flecs::world &, flecs::entity entity) const override
  {
    bool pouchReady = false;
    entity.get([&](const HealerPouch &pouch) { pouchReady |= pouch.cooldown == 0; });
    return pouchReady;
  }
};

class CrafterNeedTransition : public StateTransition
{
  CrafterStateChecker checker;

public:
  CrafterNeedTransition(CrafterStateChecker &&in_checker) : checker(in_checker) {}

  bool isAvailable(flecs::world &, flecs::entity entity) const override
  {
    bool need = false;
    entity.insert([&](const CrafterState &crafter) { need = checker(crafter); });
    return need;
  }
};

class ArrivedTransition : public StateTransition
{
public:
  bool isAvailable(flecs::world &, flecs::entity entity) const override
  {
    bool arrived = false;
    entity.get([&](const Position &pos, const DestinationPos &ppos) { arrived = dist(ppos, pos) < 0.5f; });
    return arrived;
  }
};


class NegateTransition : public StateTransition
{
  const StateTransition *transition; // we own it
public:
  NegateTransition(const StateTransition *in_trans) : transition(in_trans) {}
  ~NegateTransition() override { delete transition; }

  bool isAvailable(flecs::world &ecs, flecs::entity entity) const override { return !transition->isAvailable(ecs, entity); }
};

class AndTransition : public StateTransition
{
  const StateTransition *lhs; // we own it
  const StateTransition *rhs; // we own it
public:
  AndTransition(const StateTransition *in_lhs, const StateTransition *in_rhs) : lhs(in_lhs), rhs(in_rhs) {}
  ~AndTransition() override
  {
    delete lhs;
    delete rhs;
  }

  bool isAvailable(flecs::world &ecs, flecs::entity entity) const override
  {
    return lhs->isAvailable(ecs, entity) && rhs->isAvailable(ecs, entity);
  }
};

class OneShotTransition : public StateTransition
{
  const StateTransition *transition; // we own it
  mutable bool alreadyHappened = false;

public:
  OneShotTransition(const StateTransition *in_trans, bool signaled) : transition(in_trans), alreadyHappened(signaled) {}
  ~OneShotTransition() override { delete transition; }

  bool isAvailable(flecs::world &ecs, flecs::entity entity) const override
  {
    if (alreadyHappened)
      return false;

    alreadyHappened = transition->isAvailable(ecs, entity);
    return alreadyHappened;
  }
};

class AlwaysTransition : public StateTransition
{
public:
  bool isAvailable(flecs::world &, flecs::entity) const override { return true; }
};


// states
State *create_move_to_enemy_state() { return new MoveToEnemyState(); }
State *create_shoot_enemy_state() { return new ShootEnemyState(); }
State *create_flee_from_enemy_state() { return new FleeFromEnemyState(); }
State *create_patrol_state(float patrol_dist) { return new PatrolState(patrol_dist); }

State *create_activity_state(float turns, AiCallback &&onDone) { return new AcitvityState(turns, std::move(onDone)); }

State *create_wander_state() { return new WanderState(); }
State *create_goto_state(AiDestinationProvider &&dest_provider) { return new GoToState(std::move(dest_provider)); }

State *create_follow_player_state(float follow_dist) { return new FollowPlayerState(follow_dist); }
State *create_move_to_player_state() { return new MoveToPlayerState(); }

State *create_split_state() { return new SplitState(); }

State *create_nop_state() { return new NopState(); }

State *create_nested_sm_state(StateMachine *sm, AiCallback &&common_act) { return new NestedSMStage(sm, std::move(common_act)); }

// transitions
StateTransition *create_enemy_close_enough_transition(float dist) { return new EnemyCloseEnoughTransition(dist); }
StateTransition *create_player_far_enough_transition(float dist) { return new PlayerFarEnoughTransition(dist); }


StateTransition *create_hitpoints_less_than_transition(float thres) { return new HitpointsLessThanTransition(thres); }
StateTransition *create_player_hitpoints_less_than_transition(float thres) { return new PlayerHitpointsLessThanTransition(thres); }

StateTransition *create_pouch_ready_transition() { return new HealerPouchReadyTransition(); }

StateTransition *create_crafter_need_transition(CrafterStateChecker &&checker)
{
  return new CrafterNeedTransition(std::move(checker));
}

StateTransition *create_arrived_transition() { return new ArrivedTransition(); }

StateTransition *create_negate_transition(StateTransition *in) { return new NegateTransition(in); }
StateTransition *create_and_transition(StateTransition *lhs, StateTransition *rhs) { return new AndTransition(lhs, rhs); }
StateTransition *create_one_shot_transition(StateTransition *in, bool signaled) { return new OneShotTransition(in, signaled); }

StateTransition *create_always_transition() { return new AlwaysTransition(); }
