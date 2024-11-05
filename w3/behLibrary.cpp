#include "aiLibrary.h"
#include "behaviourTree.h"
#include "ecsTypes.h"
#include "aiUtils.h"
#include "math.h"
#include "raylib.h"
#include "blackboard.h"
#include <algorithm>
#include <utility>

class Utility
{
public:
  using UnderlyingFunc = utility_function;

  Utility(UnderlyingFunc &&base) : baseUtility(std::move(base)) {}
  virtual ~Utility() {}

  float operator()(Blackboard &bb) {
    return modifier() * baseUtility(bb);
  }

  virtual void enter() = 0;
  //virtual void exit() = 0; // Would be needed for "runtime"

  virtual float modifier() = 0;

private:
  UnderlyingFunc baseUtility;
};

class PureUtility
{
public:
  using UnderlyingFunc = pure_utility_function;

  PureUtility(UnderlyingFunc &&base) : baseUtility(std::move(base)) {}
  virtual ~PureUtility() {}

  float operator()(Blackboard &bb, const WorldEntSensorInfo &info) {
    return modifier() * baseUtility(bb, info);
  }

  virtual void enter() = 0;
  //virtual void exit() = 0; // Would be needed for "runtime"

  virtual float modifier() = 0;

private:
  UnderlyingFunc baseUtility;
};

template <class BaseUtility>
class SimpleUtilityGen : public BaseUtility
{
public:
  SimpleUtilityGen(typename BaseUtility::UnderlyingFunc &&base) : BaseUtility(std::move(base)) {}
  ~SimpleUtilityGen() override = default;
  void enter() override {}
  float modifier() override { return 1.f; }
};

template <class BaseUtility>
class CooldownUtilityGen : public BaseUtility
{
public:
  CooldownUtilityGen(typename BaseUtility::UnderlyingFunc &&base, int cd, float coeff, flecs::entity self) :
    BaseUtility(std::move(base)), selfRef(self), cd(cd), coeff(coeff)
  {}
  ~CooldownUtilityGen() override = default;

  void enter() override
  {
    selfRef.insert([this](Cooldown &cdHolder) { cdHolder.turnsLeft = cd; });
  }
  float modifier() override
  {
    int turnsLeft = 0;
    selfRef.get([&](const Cooldown &cdHolder) { turnsLeft = cdHolder.turnsLeft; });
    return float(turnsLeft + 1.f) * coeff;
  }

private:
  flecs::entity selfRef;
  int cd;
  float coeff;
};

using SimpleUtility = SimpleUtilityGen<Utility>;
using CooldownUtility = CooldownUtilityGen<Utility>;
using SimplePureUtility = SimpleUtilityGen<PureUtility>;
using CooldownPureUtility = CooldownUtilityGen<PureUtility>;

UtilityRef make_utility(utility_function &&f) { return new SimpleUtility{std::move(f)}; }
UtilityRef make_cd_utility(utility_function &&f, int cd, float coeff, flecs::entity ent)
{
  return new CooldownUtility{std::move(f), cd, coeff, ent};
}
PureUtilityRef make_pure_utility(pure_utility_function &&f) { return new SimplePureUtility{std::move(f)}; }
PureUtilityRef make_cd_pure_utility(pure_utility_function &&f, int cd, float coeff, flecs::entity ent)
{
  return new CooldownPureUtility{std::move(f), cd, coeff, ent};
}

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
  std::vector<std::pair<BehNode *, UtilityRef>> utilityNodes;

  BehResult update(flecs::world &ecs, flecs::entity entity, Blackboard &bb) override
  {
    std::vector<std::pair<float, size_t>> utilityScores;
    for (size_t i = 0; i < utilityNodes.size(); ++i)
    {
      const float utilityScore = (*utilityNodes[i].second)(bb);
      utilityScores.push_back(std::make_pair(utilityScore, i));
    }
    std::sort(utilityScores.begin(), utilityScores.end(), [](auto &lhs, auto &rhs) { return lhs.first > rhs.first; });
    for (const std::pair<float, size_t> &node : utilityScores)
    {
      size_t nodeIdx = node.second;
      BehResult res = utilityNodes[nodeIdx].first->update(ecs, entity, bb);
      if (res != BEH_FAIL)
      {
        utilityNodes[nodeIdx].second->enter();
        return res;
      }
    }
    return BEH_FAIL;
  }
};

struct PureUtilitySelector : public BehNode
{
  using NodeVector = std::vector<std::pair<BehNode *, PureUtilityRef>>;

  NodeVector utilityNodes;
  size_t allTargetsBb = size_t(-1);
  size_t chosenTargetBb = size_t(-1);

  PureUtilitySelector(flecs::entity entity, NodeVector &&nodes, const char *bb_all_targets_name,
    const char *bb_chosen_target_name) :
    utilityNodes(std::move(nodes))
  {
    allTargetsBb = reg_entity_blackboard_var<std::vector<WorldEntSensorInfo>>(entity, bb_all_targets_name);
    chosenTargetBb = reg_entity_blackboard_var<flecs::entity>(entity, bb_chosen_target_name);
  }

  BehResult update(flecs::world &ecs, flecs::entity entity, Blackboard &bb) override
  {
    struct ChoiceIndex
    {
      size_t actionId;
      size_t targetId;
    };
    std::vector<std::pair<float, ChoiceIndex>> utilityScores;
    const std::vector<WorldEntSensorInfo> &targets = bb.getCref<std::vector<WorldEntSensorInfo>>(allTargetsBb);

    printf("======================================\n");
    for (size_t i = 0; i < utilityNodes.size(); ++i)
    {
      printf("Node %lu\n", i);
      for (size_t j = 0; j < targets.size(); ++j)
      {
        const float utilityScore = (*utilityNodes[i].second)(bb, targets[j]);
        utilityScores.push_back(std::make_pair(utilityScore, ChoiceIndex{i, j}));
        printf("ut(type=%d, dist=%f, hp/amt=%f, name=%s, id=%lu) = %f\n", targets[j].type, targets[j].dist, targets[j].hpOrAmount,
          targets[j].entTag.name().c_str(), targets[j].entTag.id(), utilityScore);
      }
    }
    std::sort(utilityScores.begin(), utilityScores.end(), [](auto &lhs, auto &rhs) { return lhs.first > rhs.first; });

    for (const auto &node : utilityScores)
    {
      bb.set<flecs::entity>(chosenTargetBb, targets[node.second.targetId].entTag);
      BehResult res = utilityNodes[node.second.actionId].first->update(ecs, entity, bb);
      if (res != BEH_FAIL)
      {
        utilityNodes[node.second.actionId].second->enter();
        printf("\nChosen for Node %d:  ut(type=%d, dist=%f, hp/amt=%f, name=%s, id=%lu) = %f\n", node.second.actionId,
          targets[node.second.targetId].type, targets[node.second.targetId].dist, targets[node.second.targetId].hpOrAmount,
          targets[node.second.targetId].entTag.name().c_str(), targets[node.second.targetId].entTag.id(), node.first);
        return res;
      }
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

template <bool ENEMY>
struct FindActor : public BehNode
{
  size_t entityBb = size_t(-1);
  float distance = 0;
  FindActor(flecs::entity entity, float in_dist, const char *bb_name) : distance(in_dist)
  {
    entityBb = reg_entity_blackboard_var<flecs::entity>(entity, bb_name);
  }
  BehResult update(flecs::world &ecs, flecs::entity entity, Blackboard &bb) override
  {
    BehResult res = BEH_FAIL;
    static auto actorsQuery = ecs.query<const Position, const Team>();
    entity.insert([&](const Position &pos, const Team &t) {
      flecs::entity closestActor;
      float closestDist = FLT_MAX;
      Position closestPos;
      actorsQuery.each([&](flecs::entity enemy, const Position &apos, const Team &at) {
        if ((ENEMY && t.team == at.team) || (!ENEMY && t.team != at.team))
          return;
        float curDist = dist(apos, pos);
        if (curDist < closestDist)
        {
          closestDist = curDist;
          closestPos = apos;
          closestActor = enemy;
        }
      });
      if (ecs.is_valid(closestActor) && closestDist <= distance)
      {
        bb.set<flecs::entity>(entityBb, closestActor);
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

  BehResult update(flecs::world &, flecs::entity entity, Blackboard &bb) override
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

struct HealAlly : public BehNode
{
  size_t allyBb = size_t(-1);
  float hpThreshold = 100.f;
  HealAlly(flecs::entity entity, float threshold, const char *bb_name) : hpThreshold(threshold)
  {
    allyBb = reg_entity_blackboard_var<flecs::entity>(entity, bb_name);
  }

  BehResult update(flecs::world &, flecs::entity, Blackboard &bb) override
  {
    BehResult res = BEH_SUCCESS;
    bb.get<flecs::entity>(allyBb).insert([&](Action &a, Hitpoints &hp) {
      if (hp.hitpoints >= hpThreshold)
        return;
      res = BEH_RUNNING;
      a.action = EA_HEAL_ALLY;
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

BehNode *utility_selector(std::vector<std::pair<BehNode *, UtilityRef>> &&nodes)
{
  UtilitySelector *usel = new UtilitySelector;
  usel->utilityNodes = std::move(nodes);
  return usel;
}

BehNode *pure_utility_selector(flecs::entity entity, std::vector<std::pair<BehNode *, PureUtilityRef>> &&nodes,
  const char *bb_all_targets_name, const char *bb_chosen_target_name)
{
  PureUtilitySelector *usel = new PureUtilitySelector(entity, std::move(nodes), bb_all_targets_name, bb_chosen_target_name);
  return usel;
}

BehNode *move_to_entity(flecs::entity entity, const char *bb_name) { return new MoveToEntity(entity, bb_name); }

BehNode *is_low_hp(float thres) { return new IsLowHp(thres); }

BehNode *find_enemy(flecs::entity entity, float dist, const char *bb_name) { return new FindActor<true>(entity, dist, bb_name); }
BehNode *find_ally(flecs::entity entity, float dist, const char *bb_name) { return new FindActor<false>(entity, dist, bb_name); }
BehNode *find_heal_or_powerup(flecs::entity entity, float dist, const char *bb_name)
{
  return new FindHealOrPowerup(entity, dist, bb_name);
}

BehNode *flee(flecs::entity entity, const char *bb_name) { return new Flee(entity, bb_name); }

BehNode *patrol(flecs::entity entity, float patrol_dist, const char *bb_name) { return new Patrol(entity, patrol_dist, bb_name); }
BehNode *switch_wp(flecs::entity entity, const char *bb_name) { return new SwitchWaypoint(entity, bb_name); }

BehNode *patch_up(float thres) { return new PatchUp(thres); }
BehNode *heal_ally(flecs::entity entity, float thres, const char *bb_name) { return new HealAlly(entity, thres, bb_name); }
BehNode *spawn_heals_and_powerups(float dist, int coeff) { return new SpawnHealsAndPowerups(dist, coeff); }
