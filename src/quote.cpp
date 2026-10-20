#include "mm/quote.hpp"

#include "mm/decimal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mm {
namespace {

void require_filters(const Filters& filters) {
    if (!(filters.tick > 0.0) || !(filters.step > 0.0))
        throw std::runtime_error("instrument tick and step must be positive");
    if (filters.min_qty < 0.0 || filters.max_qty < 0.0 || filters.min_notional < 0.0)
        throw std::runtime_error("instrument quantity filters must be non-negative");
}

double mark_of(const MarketTop& market, double mid) {
    return market.mark > 0.0 ? market.mark : mid;
}

}  // namespace

QuotePlan plan_quotes(const QuoteConfig& config,
                      const MarketTop& market,
                      const Inventory& inventory,
                      const std::vector<RestingOrder>& resting) {
    require_filters(config.filters);
    if (!(market.bid > 0.0) || !(market.ask > market.bid))
        throw std::runtime_error("market top must be a positive non-crossed book");
    if (config.ladder_levels < 1 || config.ladder_levels > 10)
        throw std::runtime_error("ladder levels must be in [1, 10]");
    if (!(config.budget_usd > 0.0) || !(config.spread_factor > 0.0) || config.min_spread_bps < 0.0)
        throw std::runtime_error("budget and spread settings are invalid");

    QuotePlan plan;
    plan.mid = (market.bid + market.ask) / 2.0;
    const double mark = mark_of(market, plan.mid);
    const double net = inventory.net_qty;
    const auto& filters = config.filters;

    auto push_market_exit = [&](const std::string& id, const std::string& side, double qty) {
        const double rounded = floor_to_step(qty, filters.step);
        if (rounded + 1e-12 < filters.min_qty)
            return;
        PlannedOrder order;
        order.client_id = id;
        order.side = side;
        order.type = "MARKET";
        order.tif = "IOC";
        order.qty = std::min(rounded, filters.max_qty > 0.0 ? filters.max_qty : rounded);
        order.reduce_only = true;
        plan.orders.push_back(order);
    };

    if (config.stop_loss_bps > 0.0 && inventory.avg_entry > 0.0 && std::fabs(net) >= filters.min_qty) {
        const double fraction = config.stop_loss_bps * 0.0001;
        const bool long_hit = net > 0.0 && mark <= inventory.avg_entry * (1.0 - fraction);
        const bool short_hit = net < 0.0 && mark >= inventory.avg_entry * (1.0 + fraction);
        if (long_hit || short_hit) {
            if (net > 0.0)
                push_market_exit("mm-stop", "SELL", net);
            else
                push_market_exit("mm-stop", "BUY", -net);
            plan.stop_triggered = !plan.orders.empty();
            return plan;
        }
    }

    const double live_spread_bps = (market.ask - market.bid) / plan.mid * 1e4;
    const double fee_floor_bps = 2.0 * std::max(0.0, config.fees.maker) * 1e4;
    const double full_spread_bps = std::max({live_spread_bps,
                                             live_spread_bps * config.spread_factor,
                                             config.min_spread_bps + fee_floor_bps});
    plan.half_spread = plan.mid * full_spread_bps * 0.0001 * 0.5;

    const int sides = config.long_only ? 1 : 2;
    const double budget_per_leg = config.budget_usd / (sides * config.ladder_levels);
    double bid_scale = 1.0;
    double ask_scale = 1.0;
    if (config.max_net_qty > 0.0) {
        if (net >= config.max_net_qty)
            bid_scale = 0.0;
        else if (net > 0.0)
            bid_scale = 1.0 - net / config.max_net_qty;
        if (net <= -config.max_net_qty)
            ask_scale = 0.0;
        else if (net < 0.0)
            ask_scale = 1.0 - std::fabs(net) / config.max_net_qty;
    }

    double gross = std::fabs(net) * mark;
    double buy_capacity = config.max_net_qty > 0.0 ? config.max_net_qty - net
                                                   : std::numeric_limits<double>::infinity();
    double sell_capacity = config.max_net_qty > 0.0 ? config.max_net_qty + net
                                                    : std::numeric_limits<double>::infinity();
    struct Exposure {
        double qty{0.0};
        double notional{0.0};
        std::string side;
        bool mixed{false};
    };
    std::vector<std::pair<std::string, Exposure>> existing;
    for (const auto& order : resting) {
        if (order.reduce_only)
            continue;
        gross += order.qty * order.price;
        auto it = std::find_if(existing.begin(), existing.end(),
                               [&](const auto& item) { return item.first == order.client_id; });
        if (it == existing.end()) {
            existing.push_back({order.client_id, {}});
            it = std::prev(existing.end());
            it->second.side = order.side;
        } else if (it->second.side != order.side) {
            it->second.mixed = true;
        }
        it->second.qty += order.qty;
        it->second.notional += order.qty * order.price;
        if (order.side == "BUY")
            buy_capacity -= order.qty;
        else if (order.side == "SELL")
            sell_capacity -= order.qty;
    }

    auto take_existing = [&](const std::string& id, const std::string& side) {
        Exposure found;
        auto it = std::find_if(existing.begin(), existing.end(),
                               [&](const auto& item) { return item.first == id; });
        if (it == existing.end())
            return found;
        found = it->second;
        existing.erase(it);
        if (found.mixed || found.side != side)
            return Exposure{};
        return found;
    };

    auto add_open = [&](const std::string& id, const std::string& side, double raw_qty, double price) {
        if (!(price > 0.0) || (filters.min_price > 0.0 && price < filters.min_price) ||
            (filters.max_price > 0.0 && price > filters.max_price))
            return;
        const Exposure reusable = take_existing(id, side);
        double& capacity = side == "BUY" ? buy_capacity : sell_capacity;
        double qty = raw_qty;
        if (std::isfinite(capacity))
            qty = std::min(qty, std::max(0.0, capacity + reusable.qty));
        if (config.gross_notional_cap > 0.0) {
            const double room = std::max(0.0, config.gross_notional_cap - gross + reusable.notional);
            qty = std::min(qty, room / price);
        }
        qty = floor_to_step(qty, filters.step);
        if (filters.max_qty > 0.0)
            qty = std::min(qty, floor_to_step(filters.max_qty, filters.step));
        if (qty + 1e-12 < filters.min_qty || qty * price + 1e-12 < filters.min_notional)
            return;
        PlannedOrder order;
        order.client_id = id;
        order.side = side;
        order.qty = qty;
        order.price = price;
        plan.orders.push_back(order);
        capacity -= std::max(0.0, qty - reusable.qty);
        gross += std::max(0.0, qty * price - reusable.notional);
    };

    for (int level = 1; level <= config.ladder_levels; ++level) {
        const double offset = plan.half_spread * level;
        const double bid_price = floor_to_step(plan.mid - offset, filters.tick);
        const double bid_qty = floor_to_step(budget_per_leg / bid_price * bid_scale, filters.step);
        add_open("mm-b" + std::to_string(level), "BUY", bid_qty, bid_price);
        if (!config.long_only) {
            const double ask_price = ceil_to_step(plan.mid + offset, filters.tick);
            const double ask_qty = floor_to_step(budget_per_leg / ask_price * ask_scale, filters.step);
            add_open("mm-s" + std::to_string(level), "SELL", ask_qty, ask_price);
        }
    }

    const double entry_fee = std::max(0.0, config.fees.taker);
    const double exit_fee = std::max(0.0, config.fees.maker);
    const double safety = std::max(0.0, config.tp_safety_bps) * 0.0001;
    if (exit_fee < 1.0 && inventory.avg_entry > 0.0 && std::fabs(net) >= filters.min_qty) {
        if (net > 0.0) {
            const double floor_px = inventory.avg_entry * (1.0 + entry_fee + safety) / (1.0 - exit_fee);
            PlannedOrder order;
            order.client_id = "mm-tp";
            order.side = "SELL";
            order.qty = floor_to_step(net, filters.step);
            order.price = ceil_to_step(std::max(market.ask, floor_px), filters.tick);
            order.reduce_only = true;
            if (order.qty >= filters.min_qty)
                plan.orders.push_back(order);
        } else {
            const double ceiling = inventory.avg_entry * (1.0 - entry_fee - safety) / (1.0 + exit_fee);
            PlannedOrder order;
            order.client_id = "mm-tp";
            order.side = "BUY";
            order.qty = floor_to_step(-net, filters.step);
            order.price = floor_to_step(std::min(market.bid, ceiling), filters.tick);
            order.reduce_only = true;
            if (order.qty >= filters.min_qty && order.price > 0.0)
                plan.orders.push_back(order);
        }
    }
    return plan;
}

std::vector<OrderAction> diff_orders(const std::vector<RestingOrder>& live,
                                     const std::vector<PlannedOrder>& desired,
                                     const Filters& filters) {
    require_filters(filters);
    std::vector<OrderAction> actions;
    std::vector<bool> keep(live.size(), false);
    for (const auto& want : desired) {
        const auto it = std::find_if(live.begin(), live.end(), [&](const RestingOrder& order) {
            return order.client_id == want.client_id && order.side == want.side &&
                   order.reduce_only == want.reduce_only &&
                   same_step(order.price, want.price, filters.tick) &&
                   same_step(order.qty, want.qty, filters.step);
        });
        if (it != live.end()) {
            keep[static_cast<std::size_t>(it - live.begin())] = true;
            continue;
        }
        actions.push_back(OrderAction{OrderAction::Type::Place, want, {}});
    }
    for (std::size_t i = 0; i < live.size(); ++i) {
        if (!keep[i])
            actions.push_back(OrderAction{OrderAction::Type::Cancel, {}, live[i].client_id});
    }
    return actions;
}

}  // namespace mm
