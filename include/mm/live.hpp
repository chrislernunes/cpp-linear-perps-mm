#pragma once

#include "mm/quote.hpp"

#include <string>

namespace mm {

struct LiveOptions {
    std::string symbol{"BTCUSDT"};
    int seconds{20};
    bool check_only{false};
    bool demo{false};
    QuoteConfig quote;
};

// Public market data unless demo execution is explicitly allowed.
// Demo orders go only to https://demo-fapi.binance.com.
// HTTP 451 is reported and not bypassed. Public bookTicker still runs.
// Depth snapshots are requested only after a production REST HTTP 200.
// --check never sends an order. There is no production-order fallback.
int run_live(const LiveOptions& options);

}  // namespace mm
