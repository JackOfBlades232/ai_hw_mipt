#pragma once

#include "stateMachine.h"
#include "ecsTypes.h"
#include <flecs.h>

// Sorry for using function
using AiCallback = std::function<void(flecs::world &, flecs::entity)>;
using AiDestinationProvider = std::function<DestinationPos(flecs::world &, flecs::entity)>;

// states
State *create_move_to_enemy_state();
State *create_move_to_player_state();
State *create_shoot_enemy_state();
State *create_flee_from_enemy_state();
State *create_patrol_state(float patrol_dist);
State *create_activity_state(float turns, AiCallback &&onDone);
State *create_wander_state();
State *create_goto_state(AiDestinationProvider &&dest_provider);
State *create_follow_player_state(float follow_dist);
State *create_split_state();
State *create_nop_state();
State *create_nested_sm_state(StateMachine *sm, AiCallback &&common_act = [](flecs::world &, flecs::entity) {});

// transitions
StateTransition *create_enemy_close_enough_transition(float dist);
StateTransition *create_player_far_enough_transition(float dist);
StateTransition *create_hitpoints_less_than_transition(float thres);
StateTransition *create_player_hitpoints_less_than_transition(float thres);
StateTransition *create_pouch_ready_transition();
StateTransition *create_crafter_need_transition(CrafterStateChecker &&checker);
StateTransition *create_arrived_transition();
StateTransition *create_negate_transition(StateTransition *in);
StateTransition *create_and_transition(StateTransition *lhs, StateTransition *rhs);
StateTransition *create_one_shot_transition(StateTransition *in, bool signaled = false);
StateTransition *create_always_transition();
