#ifndef CLAUDE_REQUEST_H
#define CLAUDE_REQUEST_H

#include <string>
#include "core/config_model.h"
#include "core/chat_history.h"

std::string buildRequestBody(const AppConfig& config, const ChatHistory& history);

#endif // CLAUDE_REQUEST_H
