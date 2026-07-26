#ifndef USAGE_STATS_H
#define USAGE_STATS_H

#include <string>

struct UsageStats {
  long inTokens = 0;
  long outTokens = 0;
  long lastIn = 0;
  long lastOut = 0;
};

void accumulate(UsageStats& stats, int in, int out);
double estimateCostUSD(const UsageStats& stats, const std::string& model);

#endif // USAGE_STATS_H
