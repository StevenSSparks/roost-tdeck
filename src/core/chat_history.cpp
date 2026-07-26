#include "chat_history.h"

void ChatHistory::addUser(std::string content) {
  _turns.push_back({"user", content});
}

void ChatHistory::addAssistantRaw(std::string jsonContent) {
  _turns.push_back({"assistant", jsonContent});
}

const std::vector<Turn>& ChatHistory::turns() const {
  return _turns;
}

void ChatHistory::clear() {
  _turns.clear();
}

void ChatHistory::trimToApproxTokens(int budget) {
  // Erase oldest turns from front while more than 1 turn remains AND total chars/4 > budget
  while (_turns.size() > 1) {
    // Calculate total characters across all turns
    int totalChars = 0;
    for (const auto& turn : _turns) {
      totalChars += turn.role.size() + turn.content.size();
    }

    int approxTokens = totalChars / 4;
    if (approxTokens <= budget) {
      break;
    }

    // Erase the first (oldest) turn
    _turns.erase(_turns.begin());
  }
}
