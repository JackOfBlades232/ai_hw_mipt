#include "raylib.h"
#include <flecs.h>
#include <cstring>
#include "ecsTypes.h"
#include "roguelike.h"
#include <cassert>

static void update_camera(Camera2D &cam, flecs::world &ecs)
{
  static auto playerQuery = ecs.query<const Position, const IsPlayer>();

  playerQuery.each([&](const Position &pos, const IsPlayer &) {
    cam.target.x = pos.x * draw_scale;
    cam.target.y = pos.y * draw_scale;
  });
}

int main(int argc, char **argv)
{
  // @NOTE: no error handling, sorry
  for (int i = 1; i < argc; ++i) {
    if (strncmp(argv[i], "-drawScale", 11) == 0)
    {
      ++i;
      assert(i < argc);
      char *end;
      draw_scale = strtof(argv[i], &end);
    }
    else if (strncmp(argv[i], "-demo", 5) == 0)
    {
      const char *p = argv[i] + 5;
      if (strcmp(p, "NewEnemies") == 0)
        demo_type = DEMO_NEW_ENEMIES;
      else if (strcmp(p, "NewAlly") == 0)
        demo_type = DEMO_NEW_ALLY;
      else if (strcmp(p, "NewEnemiesAndAllies") == 0)
        demo_type = DEMO_NEW_ENEMIES_AND_ALLIES;
      else if (strcmp(p, "Initial") == 0)
        demo_type = DEMO_INTIAL;
      else if (strcmp(p, "AllCombat") == 0)
        demo_type = DEMO_ALL_COMBAT;
      else if (strcmp(p, "Crafter") == 0)
        demo_type = DEMO_CRAFTER;
    }
  }

  int width = 1920;
  int height = 1080;

  InitWindow(width, height, "w1 AI MIPT");

  const int scrWidth = GetMonitorWidth(0);
  const int scrHeight = GetMonitorHeight(0);
  if (scrWidth < width || scrHeight < height)
  {
    width = std::min(scrWidth, width);
    height = std::min(scrHeight, height);
    SetWindowSize(width, height);
  }

  flecs::world ecs;

  init_roguelike(ecs);

  Camera2D camera = {{0, 0}, {0, 0}, 0.f, 1.f};
  camera.target = Vector2{0.f, 0.f};
  camera.offset = Vector2{width * 0.5f, height * 0.5f};
  camera.rotation = 0.f;
  camera.zoom = 64.f;

  SetTargetFPS(60); // Set our game to run at 60 frames-per-second
  while (!WindowShouldClose())
  {
    process_turn(ecs);

    update_camera(camera, ecs);

    BeginDrawing();
    {
      ClearBackground(GetColor(0x052c46ff));
      BeginMode2D(camera);
      {
        ecs.progress();
      }
      EndMode2D();
      print_stats(ecs);
    }
    EndDrawing();
  }
  CloseWindow();
  return 0;
}
