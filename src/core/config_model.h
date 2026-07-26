#pragma once
#include <string>
#include <map>
struct AppConfig {
  std::string anthropicKey, geoapifyKey, model, persona, theme;
  int maxTokens; int toolLoopCap; bool sounds; int brightness; int sleepSeconds; int kbBacklight;
};
AppConfig configDefaults();
std::map<std::string,std::string> configToKV(const AppConfig&);
AppConfig configFromKV(const std::map<std::string,std::string>&);
