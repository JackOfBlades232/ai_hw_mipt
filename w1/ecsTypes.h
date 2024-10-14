#pragma once

#include <functional>

struct Position;
struct MovePos;

struct MovePos
{
  int x = 0;
  int y = 0;

  MovePos &operator=(const Position &rhs);
};

struct Position
{
  int x = 0;
  int y = 0;

  Position &operator=(const MovePos &rhs);
};

inline Position &Position::operator=(const MovePos &rhs)
{
  x = rhs.x;
  y = rhs.y;
  return *this;
}

inline MovePos &MovePos::operator=(const Position &rhs)
{
  x = rhs.x;
  y = rhs.y;
  return *this;
}

inline bool operator==(const Position &lhs, const Position &rhs) { return lhs.x == rhs.x && lhs.y == rhs.y; }
inline bool operator==(const Position &lhs, const MovePos &rhs) { return lhs.x == rhs.x && lhs.y == rhs.y; }
inline bool operator==(const MovePos &lhs, const MovePos &rhs) { return lhs.x == rhs.x && lhs.y == rhs.y; }
inline bool operator==(const MovePos &lhs, const Position &rhs) { return lhs.x == rhs.x && lhs.y == rhs.y; }


struct DestinationPos
{
  int x = 0;
  int y = 0;
};

struct ShotDirection
{
  float x = 0.f;
  float y = 0.f;
};

struct BulletPos
{
  float x = 0.f;
  float y = 0.f;
};

struct Hitpoints
{
  float hitpoints = 10.f;
};

enum Actions
{
  EA_NOP = 0,
  EA_MOVE_START,
  EA_MOVE_LEFT = EA_MOVE_START,
  EA_MOVE_RIGHT,
  EA_MOVE_DOWN,
  EA_MOVE_UP,
  EA_MOVE_END,
  EA_ATTACK = EA_MOVE_END,
  EA_SHOOT,
  EA_NUM
};

struct Action
{
  int action = 0;
};

struct NumActions
{
  int numActions = 1;
  int curActions = 0;
};

struct MeleeDamage
{
  float damage = 2.f;
};

struct HealAmount
{
  float amount = 0.f;
};

struct PowerupAmount
{
  float amount = 0.f;
};

struct HealerPouch // Sorry
{
  float amount = 0.f;
  int cooldown = 0;
  int maxCooldown = 0;
};

struct PlayerInput
{
  bool left = false;
  bool right = false;
  bool up = false;
  bool down = false;
};

struct Symbol
{
  char symb;
};

struct IsPlayer
{};

struct Team
{
  int team = 0;
};

struct TextureSource
{};

struct Activity
{
  int turnsLeft = 0;
};

struct CrafterState // Sorry
{
  float money = 0;
  float craftingSkill = 0;
  float boredom = 0;
  float sleepDeprivation = 0;
};

// Again, sorry for function
using CrafterStateChecker = std::function<bool(const CrafterState &)>;

struct CraftStation
{
  float yield = 0;
};

struct Flophouse
{
  float nightCost = 0;
};
