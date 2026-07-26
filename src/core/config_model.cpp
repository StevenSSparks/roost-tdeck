#include "core/config_model.h"
AppConfig configDefaults() {
  AppConfig c;
  c.model = "claude-haiku-4-5"; c.persona = "";
  c.maxTokens = 512; c.toolLoopCap = 4; c.sounds = true;
  c.theme = "roostos"; c.brightness = 80; c.sleepSeconds = 120;
  return c;
}
