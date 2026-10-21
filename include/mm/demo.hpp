#pragma once

#include "mm/json_mini.hpp"
#include "mm/quote.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mm {

inline constexpr const char* kDemoRestHost = "demo-fapi.binance.com";
inline constexpr const char* kProductionRestHost = "fapi.binance.com";
inline constexpr const char* kDemoRest = "https://demo-fapi.binance.com";
inline constexpr const char* kProductionRest = "https://fapi.binance.com";
inline constexpr const char* kPublicWs = "wss://fstream.binance.com/public/stream";
inline constexpr const char* kDemoWs = "wss://demo-fstream.binance.com/public/stream";

struct PreparedRequest {
    std::string method;
    std::string url;
    std::string signed_query;
    std::vector<std::pair<std::string, std::string>> headers;
};

// Throws if the URL is not exactly the official demo REST host. Production is refused.
void assert_demo_order_url(const std::string& url);

PreparedRequest prepare_order(const std::string& api_key,
                              const std::string& api_secret,
                              const std::string& symbol,
                              const PlannedOrder& order,
                              const Filters& filters,
                              std::int64_t timestamp_ms);

PreparedRequest prepare_cancel_all(const std::string& api_key,
                                   const std::string& api_secret,
                                   const std::string& symbol,
                                   std::int64_t timestamp_ms);

PreparedRequest prepare_cancel_client(const std::string& api_key,
                                      const std::string& api_secret,
                                      const std::string& symbol,
                                      const std::string& client_id,
                                      std::int64_t timestamp_ms);

PreparedRequest prepare_position(const std::string& api_key,
                                 const std::string& api_secret,
                                 const std::string& symbol,
                                 std::int64_t timestamp_ms);

PreparedRequest prepare_open_orders(const std::string& api_key,
                                    const std::string& api_secret,
                                    const std::string& symbol,
                                    std::int64_t timestamp_ms);

struct DemoDecision {
    bool send{false};
    std::string reason;
};

// Orders are sent only when demo was requested, this is not a check run,
// the demo REST ping is HTTP 200, and both demo credentials are present.
// Any other state returns send=false. Production is never selected.
DemoDecision demo_send_decision(bool demo_requested,
                                bool check_only,
                                int demo_http_status,
                                bool have_key,
                                bool have_secret);

// PRICE_FILTER, LOT_SIZE, and MIN_NOTIONAL for one USD-M symbol.
// Returns false without modifying out when the symbol or a filter is absent.
bool parse_symbol_filters(const Json& exchange_info,
                          const std::string& symbol,
                          Filters& out,
                          std::string& error);

// One-way positionAmt. Hedge-mode LONG/SHORT rows are rejected.
bool parse_one_way_position(const std::string& body,
                            const std::string& symbol,
                            Inventory& inventory,
                            std::string& error);

// Updates the local working set from a demo order response.
// HTTP errors leave the set unchanged and return false.
bool apply_order_response(int http_status,
                          const std::string& body,
                          const PlannedOrder& sent,
                          std::vector<RestingOrder>& resting,
                          std::string& note);

}  // namespace mm
