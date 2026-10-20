#pragma once

#include "mm/events.hpp"
#include "mm/quote.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mm {

struct BacktestConfig {
    std::string symbol{"BTCUSDT"};
    QuoteConfig quote;
    std::int64_t latency_ms{200};
    std::string filters_provenance{"operator-supplied; not loaded from exchangeInfo"};
};

struct SimFill {
    std::int64_t ts_ms{0};
    std::int64_t trade_id{0};
    std::string client_id;
    std::string side;
    std::string liquidity;  // maker or taker
    double price{0.0};
    double qty{0.0};
    double fee{0.0};
};

struct EquityPoint {
    std::int64_t ts_ms{0};
    double equity{0.0};
    double cash{0.0};
    double position{0.0};
    double mid{0.0};
};

struct BacktestStats {
    double net_pnl{0.0};
    double realized{0.0};
    double unrealized{0.0};
    double fees{0.0};
    double funding{0.0};
    double max_drawdown{0.0};
    double max_drawdown_over_budget{0.0};
    double sharpe{0.0};
    bool sharpe_defined{false};
    std::string sharpe_note;
    double qty_fill_rate{0.0};
    double order_fill_rate{0.0};
    double max_abs_inventory{0.0};
    double end_inventory{0.0};
    double end_avg_entry{0.0};
    double benchmark_pnl{0.0};
    double first_mid{0.0};
    double last_mid{0.0};
    double mean_markout_5s{0.0};
    double mean_markout_30s{0.0};
    int markout_5s_n{0};
    int markout_30s_n{0};
    int fills{0};
    int orders_placed{0};
    int orders_filled{0};
    int trades_seen{0};
    int touches_not_filled{0};
    int depth_used{0};
    int depth_rejected{0};
    int funding_applied{0};
    bool funding_covered{false};
};

struct BacktestResult {
    BacktestStats stats;
    std::vector<SimFill> fills;
    std::vector<EquityPoint> equity;
    std::vector<std::string> limitations;
};

// Replay is chronological. Maker fills happen only when a real aggTrade prints
// strictly through the simulated price after the order is active. Touching the
// price does not fill, because queue position is not in the data. No fill is
// larger than the print. bookDepth percentage buckets are a reference, not a
// tick book.
BacktestResult run_backtest(const BacktestConfig& config,
                            const std::vector<TradePrint>& trades,
                            const std::vector<DepthSnapshot>& depth,
                            const std::vector<FundingPrint>& funding);

}  // namespace mm
