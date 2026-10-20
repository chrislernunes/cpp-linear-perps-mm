#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mm {

struct TradePrint {
    std::int64_t id{0};
    std::int64_t ts_ms{0};
    double price{0.0};
    double qty{0.0};
    bool buyer_is_maker{false};
};

struct DepthSnapshot {
    std::int64_t ts_ms{0};
    double tight_bid{0.0};
    double tight_ask{0.0};
    double tight_bid_qty{0.0};
    double tight_ask_qty{0.0};
    double tight_pct{0.0};
    bool usable{false};
    std::string reject_reason;
};

struct FundingPrint {
    std::int64_t ts_ms{0};
    double rate{0.0};
    int interval_hours{0};
};

struct FileProvenance {
    std::string kind;
    std::string url;
    std::string checksum_url;
    std::string sha256;
    std::string publisher_sha256;
    bool checksum_matched{false};
    bool present{false};
    int http_status{0};
    std::string csv_path;
    std::uint64_t rows{0};
    std::int64_t first_ts_ms{0};
    std::int64_t last_ts_ms{0};
    std::string note;
};

}  // namespace mm
