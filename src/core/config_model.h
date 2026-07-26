#pragma once
#include <string>
struct AppConfig {
  std::string anthropicKey, geoapifyKey, model, persona, theme;
  int maxTokens; int toolLoopCap; bool sounds; int brightness; int sleepSeconds;
};
AppConfig configDefaults();
