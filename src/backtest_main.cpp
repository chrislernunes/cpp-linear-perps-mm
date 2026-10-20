#include "mm/backtest.hpp"
#include "mm/history.hpp"
#include "mm/report.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::string flag(int argc, char** argv, const std::string& name, const std::string& fallback = {}) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name)
            return argv[i + 1];
    }
    if (fallback.empty())
        throw std::runtime_error("missing " + name);
    return fallback;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        mm::BacktestConfig config;
        config.symbol = flag(argc, argv, "--symbol", "BTCUSDT");
        const std::string date = flag(argc, argv, "--date");
        const std::string data_dir = flag(argc, argv, "--data-dir", "data");
        const std::string out_dir = flag(argc, argv, "--out", "reports/latest");
        config.latency_ms = std::stoll(flag(argc, argv, "--latency-ms", "200"));
        config.quote.budget_usd = std::stod(flag(argc, argv, "--budget", "1000"));
        config.quote.min_spread_bps = std::stod(flag(argc, argv, "--min-spread-bps", "4"));
        config.quote.spread_factor = std::stod(flag(argc, argv, "--spread-factor", "1"));
        config.quote.max_net_qty = std::stod(flag(argc, argv, "--max-net-qty", "0.01"));
        config.quote.gross_notional_cap = std::stod(flag(argc, argv, "--gross-cap", "5000"));
        config.quote.ladder_levels = std::stoi(flag(argc, argv, "--ladder", "1"));
        config.quote.fees.maker = std::stod(flag(argc, argv, "--maker-fee", "0.0002"));
        config.quote.fees.taker = std::stod(flag(argc, argv, "--taker-fee", "0.0005"));
        config.quote.tp_safety_bps = std::stod(flag(argc, argv, "--tp-safety-bps", "1"));
        config.quote.stop_loss_bps = std::stod(flag(argc, argv, "--stop-bps", "-1"));
        config.quote.long_only = flag(argc, argv, "--long-only", "0") == "1";
        config.quote.filters.tick = std::stod(flag(argc, argv, "--tick"));
        config.quote.filters.step = std::stod(flag(argc, argv, "--step"));
        config.quote.filters.min_qty = std::stod(flag(argc, argv, "--min-qty"));
        config.quote.filters.min_notional = std::stod(flag(argc, argv, "--min-notional", "0"));
        config.quote.filters.max_qty = std::stod(flag(argc, argv, "--max-qty", "0"));
        config.filters_provenance =
            "operator-supplied on the command line; exchangeInfo was not used for this run";
        std::cerr << "[backtest] downloading official data.binance.vision files for " << config.symbol << " " << date
                  << "\n";
        const mm::HistoricalDay day = mm::load_historical_day(config.symbol, date, data_dir);
        std::cerr << "[backtest] trades " << day.trades.size() << " depth snaps " << day.depth.size()
                  << " funding rows " << day.funding.size() << "\n";
        const mm::BacktestResult result = mm::run_backtest(config, day.trades, day.depth, day.funding);
        mm::write_backtest_report(out_dir, config, day, result);
        std::cerr << "[backtest] net_pnl " << result.stats.net_pnl << " fills " << result.stats.fills
                  << " max_dd " << result.stats.max_drawdown << " report " << out_dir << "/report.md\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Backtest failed: " << error.what() << "\n";
        return 1;
    }
}
