#ifndef CHAT_HISTORY_H
#define CHAT_HISTORY_H

#include <string>
#include <vector>

struct Turn {
  std::string role;
  std::string content;
};

class ChatHistory {
public:
  void addUser(std::string content);
  void addAssistantRaw(std::string jsonContent);
  const std::vector<Turn>& turns() const;
  void clear();
  void trimToApproxTokens(int budget);

private:
  std::vector<Turn> _turns;
};

#endif // CHAT_HISTORY_H
