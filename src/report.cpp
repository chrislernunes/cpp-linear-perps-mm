#include "mm/report.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace mm {
namespace {

void ensure_dir(const std::string& path) {
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0755);
#endif
}

std::string n(double value) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(6);
    out << value;
    return out.str();
}

}  // namespace

void write_backtest_report(const std::string& directory,
                           const BacktestConfig& config,
                           const HistoricalDay& day,
                           const BacktestResult& result) {
    ensure_dir(directory);
    {
        std::ofstream equity(directory + "/equity.csv");
        if (!equity)
            throw std::runtime_error("unable to write equity.csv");
        equity << "ts_ms,equity,cash,position,mid\n";
        equity.setf(std::ios::fixed);
        equity.precision(8);
        for (const auto& point : result.equity)
            equity << point.ts_ms << ',' << point.equity << ',' << point.cash << ',' << point.position << ','
                   << point.mid << '\n';
    }
    {
        std::ofstream fills(directory + "/fills.csv");
        if (!fills)
            throw std::runtime_error("unable to write fills.csv");
        fills << "ts_ms,trade_id,client_id,side,liquidity,price,qty,fee\n";
        fills.setf(std::ios::fixed);
        fills.precision(8);
        for (const auto& fill : result.fills)
            fills << fill.ts_ms << ',' << fill.trade_id << ',' << fill.client_id << ',' << fill.side << ','
                  << fill.liquidity << ',' << fill.price << ',' << fill.qty << ',' << fill.fee << '\n';
    }
    const auto& s = result.stats;
    std::ofstream summary(directory + "/summary.json");
    summary.setf(std::ios::fixed);
    summary.precision(8);
    summary << "{\n"
            << "  \"symbol\": \"" << day.symbol << "\",\n"
            << "  \"date\": \"" << day.date << "\",\n"
            << "  \"net_pnl\": " << s.net_pnl << ",\n"
            << "  \"realized\": " << s.realized << ",\n"
            << "  \"unrealized\": " << s.unrealized << ",\n"
            << "  \"fees\": " << s.fees << ",\n"
            << "  \"funding\": " << s.funding << ",\n"
            << "  \"max_drawdown\": " << s.max_drawdown << ",\n"
            << "  \"sharpe\": " << (s.sharpe_defined ? n(s.sharpe) : "null") << ",\n"
            << "  \"sharpe_note\": \"" << s.sharpe_note << "\",\n"
            << "  \"qty_fill_rate\": " << s.qty_fill_rate << ",\n"
            << "  \"order_fill_rate\": " << s.order_fill_rate << ",\n"
            << "  \"max_abs_inventory\": " << s.max_abs_inventory << ",\n"
            << "  \"end_inventory\": " << s.end_inventory << ",\n"
            << "  \"benchmark_pnl\": " << s.benchmark_pnl << ",\n"
            << "  \"fills\": " << s.fills << ",\n"
            << "  \"trades_seen\": " << s.trades_seen << ",\n"
            << "  \"funding_applied\": " << s.funding_applied << ",\n"
            << "  \"depth_used\": " << s.depth_used << ",\n"
            << "  \"depth_rejected\": " << s.depth_rejected << "\n"
            << "}\n";

    std::ofstream report(directory + "/report.md");
    if (!report)
        throw std::runtime_error("unable to write report.md");
    report.setf(std::ios::fixed);
    report.precision(4);
    report << "# USD-M backtest " << day.symbol << " " << day.date << "\n\n";
    report << "These fills are simulated from downloaded Binance public data. They are not exchange executions "
              "and no live order was sent.\n\n";
    report << "## Provenance\n\n";
    report << "| Kind | Present | SHA256 matches publisher | Rows | HTTP |\n|---|---|---|---|---|\n";
    for (const auto& file : day.files) {
        report << "| " << file.kind << " | " << (file.present ? "yes" : "no") << " | "
               << (file.checksum_matched ? "yes" : "no") << " | " << file.rows << " | " << file.http_status
               << " |\n";
    }
    report << "\nManifest: `" << day.manifest_path << "`\n\n";
    report << "## Result\n\n";
    report << "| Metric | Value |\n|---|---|\n";
    report << "| Net PnL (USDT, marked) | " << s.net_pnl << " |\n";
    report << "| Realized | " << s.realized << " |\n";
    report << "| Unrealized | " << s.unrealized << " |\n";
    report << "| Fees | " << s.fees << " |\n";
    report << "| Funding | " << s.funding << " |\n";
    report << "| Max drawdown | " << s.max_drawdown << " |\n";
    report << "| Sharpe | " << (s.sharpe_defined ? n(s.sharpe) : "undefined") << " |\n";
    report << "| Quantity fill rate | " << s.qty_fill_rate << " |\n";
    report << "| Orders placed | " << s.orders_placed << " |\n";
    report << "| Maker fills | " << s.fills << " |\n";
    report << "| Touches not filled | " << s.touches_not_filled << " |\n";
    report << "| Max |inventory| | " << s.max_abs_inventory << " |\n";
    report << "| End inventory | " << s.end_inventory << " |\n";
    report << "| Buy-and-hold benchmark | " << s.benchmark_pnl << " |\n";
    report << "| Mean 5s markout / fill | " << s.mean_markout_5s << " (n=" << s.markout_5s_n << ") |\n";
    report << "| Mean 30s markout / fill | " << s.mean_markout_30s << " (n=" << s.markout_30s_n << ") |\n";
    report << "\nSharpe note: " << s.sharpe_note << "\n\n";
    report << "Benchmark is the configured budget marked from the first usable bookDepth mid to the last, with no "
              "fees and no trades. It is not a traded account.\n\n";
    report << "## Parameters\n\n";
    report << "- Latency: " << config.latency_ms << " ms\n";
    report << "- Budget: " << config.quote.budget_usd << "\n";
    report << "- Min spread bps: " << config.quote.min_spread_bps << "\n";
    report << "- Spread factor: " << config.quote.spread_factor << "\n";
    report << "- Max net qty: " << config.quote.max_net_qty << "\n";
    report << "- Maker / taker fee: " << config.quote.fees.maker << " / " << config.quote.fees.taker << "\n";
    report << "- Tick / step: " << config.quote.filters.tick << " / " << config.quote.filters.step << "\n";
    report << "- Filter provenance: " << config.filters_provenance << "\n\n";
    report << "## Limitations\n\n";
    for (const auto& item : result.limitations)
        report << "- " << item << "\n";
    report << "\nEquity curve: `equity.csv`. Fills: `fills.csv`. Summary: `summary.json`.\n";
}

}  // namespace mm
