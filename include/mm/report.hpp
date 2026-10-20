#pragma once

#include "mm/backtest.hpp"
#include "mm/history.hpp"

#include <string>

namespace mm {

void write_backtest_report(const std::string& directory,
                           const BacktestConfig& config,
                           const HistoricalDay& day,
                           const BacktestResult& result);

}  // namespace mm
