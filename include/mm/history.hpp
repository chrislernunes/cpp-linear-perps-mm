#pragma once

#include "mm/events.hpp"

#include <string>
#include <vector>

namespace mm {

struct HistoricalDay {
    std::string symbol;
    std::string date;
    std::vector<TradePrint> trades;
    std::vector<DepthSnapshot> depth;
    std::vector<FundingPrint> funding;
    std::vector<FileProvenance> files;
    std::string manifest_path;
    bool funding_available{false};
};

// Downloads official data.binance.vision USD-M files, checks publisher SHA256,
// extracts CSV, validates schema and time order, and writes a manifest.
// A missing funding file is recorded and does not invent rates.
// A missing aggTrades or bookDepth file throws.
HistoricalDay load_historical_day(const std::string& symbol,
                                  const std::string& date,
                                  const std::string& data_dir);

std::vector<TradePrint> parse_agg_trades_csv(const std::string& text, FileProvenance& provenance);
std::vector<DepthSnapshot> parse_book_depth_csv(const std::string& text, FileProvenance& provenance);
std::vector<FundingPrint> parse_funding_csv(const std::string& text, FileProvenance& provenance);
std::int64_t parse_utc_millis(const std::string& text);

}  // namespace mm
