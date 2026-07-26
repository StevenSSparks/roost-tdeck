#include "core/config_model.h"
AppConfig configDefaults() {
  AppConfig c;
  c.model = "claude-haiku-4-5"; c.persona = "";
  c.maxTokens = 512; c.toolLoopCap = 4; c.sounds = true;
  c.theme = "roostos"; c.brightness = 80; c.sleepSeconds = 120; c.kbBacklight = 8;
  return c;
}

static int toInt(const std::map<std::string,std::string>& m, const char* k, int d){
  auto it=m.find(k); return it==m.end()?d:std::stoi(it->second);
}
static std::string toStr(const std::map<std::string,std::string>& m, const char* k, const std::string& d){
  auto it=m.find(k); return it==m.end()?d:it->second;
}
std::map<std::string,std::string> configToKV(const AppConfig& c){
  return {
    {"anthropicKey",c.anthropicKey},{"geoapifyKey",c.geoapifyKey},
    {"model",c.model},{"persona",c.persona},{"theme",c.theme},
    {"maxTokens",std::to_string(c.maxTokens)},{"toolLoopCap",std::to_string(c.toolLoopCap)},
    {"sounds",c.sounds?"1":"0"},{"brightness",std::to_string(c.brightness)},
    {"sleepSeconds",std::to_string(c.sleepSeconds)},{"kbBacklight",std::to_string(c.kbBacklight)},
  };
}
AppConfig configFromKV(const std::map<std::string,std::string>& m){
  AppConfig c = configDefaults();
  c.anthropicKey=toStr(m,"anthropicKey",c.anthropicKey);
  c.geoapifyKey=toStr(m,"geoapifyKey",c.geoapifyKey);
  c.model=toStr(m,"model",c.model); c.persona=toStr(m,"persona",c.persona);
  c.theme=toStr(m,"theme",c.theme);
  c.maxTokens=toInt(m,"maxTokens",c.maxTokens); c.toolLoopCap=toInt(m,"toolLoopCap",c.toolLoopCap);
  c.sounds=toStr(m,"sounds","1")=="1"; c.brightness=toInt(m,"brightness",c.brightness);
  c.sleepSeconds=toInt(m,"sleepSeconds",c.sleepSeconds); c.kbBacklight=toInt(m,"kbBacklight",c.kbBacklight);
  return c;
}
