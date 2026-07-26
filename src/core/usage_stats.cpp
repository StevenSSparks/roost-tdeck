#include "usage_stats.h"

void accumulate(UsageStats& stats, int in, int out) {
  stats.inTokens += in;
  stats.outTokens += out;
  stats.lastIn = in;
  stats.lastOut = out;
}

double estimateCostUSD(const UsageStats& stats, const std::string& model) {
  if (model == "claude-haiku-4-5") {
    return stats.inTokens / 1e6 * 1.0 + stats.outTokens / 1e6 * 5.0;
  }
  return 0.0;
}
