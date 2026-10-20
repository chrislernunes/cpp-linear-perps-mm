#pragma once

#include <string>
#include <vector>

namespace mm {

struct Filters {
    double tick{0.0};
    double step{0.0};
    double min_qty{0.0};
    double max_qty{0.0};
    double min_notional{0.0};
    double min_price{0.0};
    double max_price{0.0};
};

struct Fees {
    double maker{0.0};
    double taker{0.0};
};

struct QuoteConfig {
    Filters filters;
    Fees fees;
    double budget_usd{0.0};
    double min_spread_bps{0.0};
    double spread_factor{1.0};
    double max_net_qty{0.0};
    double gross_notional_cap{0.0};
    int ladder_levels{1};
    double tp_safety_bps{0.0};
    double stop_loss_bps{-1.0};
    bool long_only{false};
};

struct MarketTop {
    double bid{0.0};
    double ask{0.0};
    double mark{0.0};
};

struct Inventory {
    double net_qty{0.0};
    double avg_entry{0.0};
};

struct RestingOrder {
    std::string client_id;
    std::string side;
    double qty{0.0};
    double price{0.0};
    bool reduce_only{false};
};

struct PlannedOrder {
    std::string client_id;
    std::string side;
    std::string type{"LIMIT"};
    std::string tif{"GTX"};
    double qty{0.0};
    double price{0.0};
    bool reduce_only{false};
};

struct QuotePlan {
    std::vector<PlannedOrder> orders;
    bool stop_triggered{false};
    double mid{0.0};
    double half_spread{0.0};
};

struct OrderAction {
    enum class Type { Place, Cancel } type{Type::Place};
    PlannedOrder order;
    std::string cancel_id;
};

// Inventory-aware passive ladder. One-way net inventory replaces Bybit hedge indexes.
// Opening orders are post-only (GTX). Stops are reduce-only MARKET plans and are
// sent only to the demo host by the demo executor.
QuotePlan plan_quotes(const QuoteConfig& config,
                      const MarketTop& market,
                      const Inventory& inventory,
                      const std::vector<RestingOrder>& resting);

std::vector<OrderAction> diff_orders(const std::vector<RestingOrder>& live,
                                     const std::vector<PlannedOrder>& desired,
                                     const Filters& filters);

}  // namespace mm
