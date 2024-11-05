#pragma once

#include <cfloat>
#include <functional>
#include "stateMachine.h"
#include "behaviourTree.h"
#include "ecsTypes.h"

// states
State *create_attack_enemy_state();
State *create_move_to_enemy_state();
State *create_flee_from_enemy_state();
State *create_patrol_state(float patrol_dist);
State *create_nop_state();
State *create_nested_sm_state(StateMachine *sm);

// transitions
StateTransition *create_enemy_available_transition(float dist);
StateTransition *create_enemy_reachable_transition();
StateTransition *create_hitpoints_less_than_transition(float thres);
StateTransition *create_negate_transition(StateTransition *in);
StateTransition *create_and_transition(StateTransition *lhs, StateTransition *rhs);

using utility_function = std::function<float(Blackboard &)>;
using pure_utility_function = std::function<float(Blackboard &, const WorldEntSensorInfo &)>;

class Utility;
class PureUtility;

// Dangling pointers to not care about moving and copying around. Should be properly dealt with though.
using UtilityRef = Utility *;
using PureUtilityRef = PureUtility *;

UtilityRef make_utility(utility_function &&f);
UtilityRef make_cd_utility(utility_function &&f, int cd, float coeff, flecs::entity ent);
PureUtilityRef make_pure_utility(pure_utility_function &&f);
PureUtilityRef make_cd_pure_utility(pure_utility_function &&f, int cd, float coeff, flecs::entity ent);

BehNode *sequence(const std::vector<BehNode *> &nodes);
BehNode *selector(const std::vector<BehNode *> &nodes);
BehNode *inverter(BehNode *node);
BehNode *xorer(BehNode *node1, BehNode *node2);
BehNode *repeatn(BehNode *node, size_t n);
BehNode *utility_selector(std::vector<std::pair<BehNode *, UtilityRef>> &&nodes);
BehNode *pure_utility_selector(flecs::entity entity, std::vector<std::pair<BehNode *, PureUtilityRef>> &&nodes,
  const char *bb_all_targets_name, const char *bb_chosen_target_name);

BehNode *move_to_entity(flecs::entity entity, const char *bb_name);
BehNode *is_low_hp(float thres);
BehNode *find_enemy(flecs::entity entity, float dist, const char *bb_name);
BehNode *find_ally(flecs::entity entity, float dist, const char *bb_name);
BehNode *find_heal_or_powerup(flecs::entity entity, float dist, const char *bb_name);
BehNode *flee(flecs::entity entity, const char *bb_name);
BehNode *patrol(flecs::entity entity, float patrol_dist, const char *bb_name);
BehNode *switch_wp(flecs::entity entity, const char *bb_name);
BehNode *patch_up(float thres);
BehNode *heal_ally(flecs::entity entity, float thres, const char *bb_name);
BehNode *spawn_heals_and_powerups(float dist, int coeff);
