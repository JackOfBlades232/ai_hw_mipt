#include "aiLibrary.h"
#include "behaviourTree.h"
#include "ecsTypes.h"
#include "aiUtils.h"
#include "math.h"
#include "raylib.h"
#include "blackboard.h"
#include <algorithm>
#include <utility>

struct CompoundNode : public BehNode
{
  std::vector<BehNode *> nodes;

  virtual ~CompoundNode()
  {
    for (BehNode *node : nodes)
      delete node;
    nodes.clear();
  }

  CompoundNode &pushNode(BehNode *node)
  {
    nodes.push_back(node);
    return *this;
  }
};

struct Sequence : public CompoundNode
{
  BehResult update(flecs::world &ecs, flecs::entity entity, Blackboard &bb) override
  {
    for (BehNode *node : nodes)
    {
      BehResult res = node->update(ecs, entity, bb);
      if (res != BEH_SUCCESS)
        return res;
    }
    return BEH_SUCCESS;
  }
};

struct Selector : public CompoundNode
{
  BehResult update(flecs::world &ecs, flecs::entity entity, Blackboard &bb) override
  {
    for (BehNode *node : nodes)
    {
      BehResult res = node->update(ecs, entity, bb);
      if (res != BEH_FAIL)
        return res;
    }
    return BEH_FAIL;
  }
};

struct Not : public BehNode
{
  BehNode *node;

  Not(BehNode *a_node) : node(a_node) {}
  ~Not() override { delete node; }

  BehResult update(flecs::world &ecs, flecs::entity entity, Blackboard &bb) override
  {
    BehResult res = node->update(ecs, entity, bb);
    return res == BEH_SUCCESS ? BEH_FAIL : (res == BEH_FAIL ? BEH_SUCCESS : BEH_RUNNING);
  }
};

struct Xor : public BehNode
{
  BehNode *node1, *node2;

  Xor(BehNode *a_node1, BehNode *a_node2) : node1(a_node1), node2(a_node2) {}
  ~Xor() override
  {
    delete node1;
    delete node2;
  }

  BehResult update(flecs::world &ecs, flecs::entity entity, Blackboard &bb) override
  {
    BehResult res1 = node1->update(ecs, entity, bb);
    BehResult res2 = node2->update(ecs, entity, bb);
    // @TODO: how does this work w/ running?
    return ((res1 == BEH_SUCCESS && res2 == BEH_FAIL) || (res1 == BEH_FAIL && res2 == BEH_SUCCESS)) ? BEH_SUCCESS : BEH_FAIL;
  }
};

struct Repeat : public BehNode
{
  BehNode *node;
  size_t n;

  Repeat(BehNode *a_node, size_t a_n) : node(a_node), n(a_n) {}
  ~Repeat() override { delete node; }

  BehResult update(flecs::world &ecs, flecs::entity entity, Blackboard &bb) override
  {
    for (size_t i = 0; i < n; ++i)
    {
      BehResult res = node->update(ecs, entity, bb);
      if (res != BEH_SUCCESS)
        return res;
    }
    return BEH_SUCCESS;
  }
};

struct UtilitySelector : public BehNode
{
  std::vector<std::pair<BehNode *, utility_function>> utilityNodes;

  BehResult update(flecs::world &ecs, flecs::entity entity, Blackboard &bb) override
  {
    std::vector<std::pair<float, size_t>> utilityScores;
    for (size_t i = 0; i < utilityNodes.size(); ++i)
    {
      const float utilityScore = utilityNodes[i].second(bb);
      utilityScores.push_back(std::make_pair(utilityScore, i));
    }
    std::sort(utilityScores.begin(), utilityScores.end(), [](auto &lhs, auto &rhs) { return lhs.first > rhs.first; });
    for (const std::pair<float, size_t> &node : utilityScores)
    {
      size_t nodeIdx = node.second;
      BehResult res = utilityNodes[nodeIdx].first->update(ecs, entity, bb);
      if (res != BEH_FAIL)
        return res;
    }
    return BEH_FAIL;
  }
};

struct MoveToEntity : public BehNode
{
  size_t entityBb = size_t(-1); // wraps to 0xff...
  MoveToEntity(flecs::entity entity, const char *bb_name) { entityBb = reg_entity_blackboard_var<flecs::entity>(entity, bb_name); }

  BehResult update(flecs::world &, flecs::entity entity, Blackboard &bb) override
  {
    BehResult res = BEH_RUNNING;
    entity.insert([&](Action &a, const Position &pos) {
      flecs::entity targetEntity = bb.get<flecs::entity>(entityBb);
      if (!targetEntity.is_alive())
      {
        res = BEH_FAIL;
        return;
      }
      targetEntity.get([&](const Position &target_pos) {
        if (pos != target_pos)
        {
          a.action = move_towards(pos, target_pos);
          res = BEH_RUNNING;
        }
        else
          res = BEH_SUCCESS;
      });
    });
    return res;
  }
};

struct IsLowHp : public BehNode
{
  float threshold = 0.f;
  IsLowHp(float thres) : threshold(thres) {}

  BehResult update(flecs::world &, flecs::entity entity, Blackboard &) override
  {
    BehResult res = BEH_SUCCESS;
    entity.get([&](const Hitpoints &hp) { res = hp.hitpoints < threshold ? BEH_SUCCESS : BEH_FAIL; });
    return res;
  }
};

struct FindEnemy : public BehNode
{
  size_t entityBb = size_t(-1);
  float distance = 0;
  FindEnemy(flecs::entity entity, float in_dist, const char *bb_name) : distance(in_dist)
  {
    entityBb = reg_entity_blackboard_var<flecs::entity>(entity, bb_name);
  }
  BehResult update(flecs::world &ecs, flecs::entity entity, Blackboard &bb) override
  {
    BehResult res = BEH_FAIL;
    static auto enemiesQuery = ecs.query<const Position, const Team>();
    entity.insert([&](const Position &pos, const Team &t) {
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
      if (ecs.is_valid(closestEnemy) && closestDist <= distance)
      {
        bb.set<flecs::entity>(entityBb, closestEnemy);
        res = BEH_SUCCESS;
      }
    });
    return res;
  }
};

struct FindHealOrPowerup : public BehNode
{
  size_t entityBb = size_t(-1);
  float distance = 0;

  FindHealOrPowerup(flecs::entity entity, float in_dist, const char *bb_name) : distance(in_dist)
  {
    entityBb = reg_entity_blackboard_var<flecs::entity>(entity, bb_name);
  }

  BehResult update(flecs::world &ecs, flecs::entity entity, Blackboard &bb) override
  {
    BehResult res = BEH_FAIL;
    static auto healsQuery = ecs.query<const Position, const HealAmount>();
    static auto powerupsQuery = ecs.query<const Position, const PowerupAmount>();
    entity.insert([&](const Position &pos, const Team &t) {
      flecs::entity closestPickup;
      float closestDist = FLT_MAX;
      Position closestPos;
      auto updateClosest = [&](flecs::entity pickup, const Position &ppos) {
        float curDist = dist(ppos, pos);
        if (curDist < closestDist)
        {
          closestDist = curDist;
          closestPos = ppos;
          closestPickup = pickup;
        }
      };
      healsQuery.each([&](flecs::entity pickup, const Position &ppos, const HealAmount &) { updateClosest(pickup, ppos); });
      powerupsQuery.each([&](flecs::entity pickup, const Position &ppos, const PowerupAmount &) { updateClosest(pickup, ppos); });
      if (ecs.is_valid(closestPickup) && closestDist <= distance)
      {
        bb.set<flecs::entity>(entityBb, closestPickup);
        res = BEH_SUCCESS;
      }
    });
    return res;
  }
};

struct Flee : public BehNode
{
  size_t entityBb = size_t(-1);
  Flee(flecs::entity entity, const char *bb_name) { entityBb = reg_entity_blackboard_var<flecs::entity>(entity, bb_name); }

  BehResult update(flecs::world &, flecs::entity entity, Blackboard &bb) override
  {
    BehResult res = BEH_RUNNING;
    entity.insert([&](Action &a, const Position &pos) {
      flecs::entity targetEntity = bb.get<flecs::entity>(entityBb);
      if (!targetEntity.is_alive())
      {
        res = BEH_FAIL;
        return;
      }
      targetEntity.get([&](const Position &target_pos) { a.action = inverse_move(move_towards(pos, target_pos)); });
    });
    return res;
  }
};

struct Patrol : public BehNode
{
  size_t pposBb = size_t(-1);
  float patrolDist = 1.f;
  Patrol(flecs::entity entity, float patrol_dist, const char *bb_name) : patrolDist(patrol_dist)
  {
    pposBb = reg_entity_blackboard_var<Position>(entity, bb_name);
    entity.insert([&](Blackboard &bb, const Position &pos) { bb.set<Position>(pposBb, pos); });
  }

  BehResult update(flecs::world &, flecs::entity entity, Blackboard &bb) override
  {
    BehResult res = BEH_RUNNING;
    entity.insert([&](Action &a, const Position &pos) {
      Position patrolPos = bb.get<Position>(pposBb);
      if (dist(pos, patrolPos) > patrolDist)
        a.action = move_towards(pos, patrolPos);
      else
        a.action = GetRandomValue(EA_MOVE_START, EA_MOVE_END - 1); // do a random walk
    });
    return res;
  }
};

struct SwitchWaypoint : public BehNode
{
  size_t wpBb = size_t(-1);

  SwitchWaypoint(flecs::entity entity, const char *bb_name) { wpBb = reg_entity_blackboard_var<flecs::entity>(entity, bb_name); }

  BehResult update(flecs::world &, flecs::entity, Blackboard &bb) override
  {
    bb.get<flecs::entity>(wpBb).get([&](const Waypoint &newtWp) { bb.set(wpBb, newtWp.nextWaypoint); });
    return BEH_SUCCESS;
  }
};

struct PatchUp : public BehNode
{
  float hpThreshold = 100.f;
  PatchUp(float threshold) : hpThreshold(threshold) {}

  BehResult update(flecs::world &, flecs::entity entity, Blackboard &) override
  {
    BehResult res = BEH_SUCCESS;
    entity.insert([&](Action &a, Hitpoints &hp) {
      if (hp.hitpoints >= hpThreshold)
        return;
      res = BEH_RUNNING;
      a.action = EA_HEAL_SELF;
    });
    return res;
  }
};

struct SpawnHealsAndPowerups : public BehNode
{
  float dist = 20.f;
  int coeff = 2;
  SpawnHealsAndPowerups(int a_dist, int a_coeff) : dist(a_dist), coeff(a_coeff) {}

  BehResult update(flecs::world &ecs, flecs::entity entity, Blackboard &) override
  {
    int pickedHeals;
    int pickedPowerups;
    int posX, posY;
    entity.insert([&](HealsCollected &heals) { pickedHeals = std::exchange(heals.count, 0); });
    entity.insert([&](PowerupsCollected &powerups) { pickedPowerups = std::exchange(powerups.count, 0); });
    entity.get([&](const Position &pos) {
      posX = pos.x;
      posY = pos.y;
    });

    auto randPos = [&] {
      // @TODO: floating, truncate dist properly
      return Position{posX + randint(-int(dist), int(dist)), posY + randint(-int(dist), int(dist))};
    };
    // @NOTE: data about heal/powerup amouns should be carried w/ the gatherer
    // @NOTE: powerups should not be spawned into the same places
    for (int i = 0; i < pickedHeals * coeff; ++i)
      ecs.entity().set(randPos()).set(HealAmount{50.f}).set(Color{0xff, 0x44, 0x44, 0xff});
    for (int i = 0; i < pickedPowerups * coeff; ++i)
      ecs.entity().set(randPos()).set(PowerupAmount{10.f}).set(Color{0xff, 0xff, 0x00, 0xff});

    return BEH_SUCCESS;
  }
};


BehNode *sequence(const std::vector<BehNode *> &nodes)
{
  Sequence *seq = new Sequence;
  for (BehNode *node : nodes)
    seq->pushNode(node);
  return seq;
}

BehNode *selector(const std::vector<BehNode *> &nodes)
{
  Selector *sel = new Selector;
  for (BehNode *node : nodes)
    sel->pushNode(node);
  return sel;
}

BehNode *inverter(BehNode *node) { return new Not(node); }
BehNode *xorer(BehNode *node1, BehNode *node2) { return new Xor(node1, node2); }
BehNode *repeatn(BehNode *node, size_t n) { return new Repeat(node, n); }

BehNode *utility_selector(const std::vector<std::pair<BehNode *, utility_function>> &nodes)
{
  UtilitySelector *usel = new UtilitySelector;
  usel->utilityNodes = std::move(nodes);
  return usel;
}

BehNode *move_to_entity(flecs::entity entity, const char *bb_name) { return new MoveToEntity(entity, bb_name); }

BehNode *is_low_hp(float thres) { return new IsLowHp(thres); }

BehNode *find_enemy(flecs::entity entity, float dist, const char *bb_name) { return new FindEnemy(entity, dist, bb_name); }
BehNode *find_heal_or_powerup(flecs::entity entity, float dist, const char *bb_name) { return new FindHealOrPowerup(entity, dist, bb_name); }

BehNode *flee(flecs::entity entity, const char *bb_name) { return new Flee(entity, bb_name); }

BehNode *patrol(flecs::entity entity, float patrol_dist, const char *bb_name) { return new Patrol(entity, patrol_dist, bb_name); }
BehNode *switch_wp(flecs::entity entity, const char *bb_name) { return new SwitchWaypoint(entity, bb_name); }

BehNode *patch_up(float thres) { return new PatchUp(thres); }
BehNode *spawn_heals_and_powerups(float dist, int coeff) { return new SpawnHealsAndPowerups(dist, coeff); }
