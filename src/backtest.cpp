#include "mm/backtest.hpp"

#include "mm/decimal.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace mm {
namespace {

struct Account {
    double position{0.0};
    double avg{0.0};
    double cash{0.0};
    double realized{0.0};
    double fees{0.0};
    double funding{0.0};

    void apply_funding(double mid, double rate) {
        const double payment = -position * mid * rate;
        cash += payment;
        funding += payment;
    }

    void apply_fill(double signed_qty, double price, double fee) {
        cash -= fee;
        fees += fee;
        if (std::fabs(position) < 1e-12 || (position > 0.0 && signed_qty > 0.0) ||
            (position < 0.0 && signed_qty < 0.0)) {
            const double new_pos = position + signed_qty;
            avg = (std::fabs(position) * avg + std::fabs(signed_qty) * price) / std::fabs(new_pos);
            position = new_pos;
            return;
        }
        const double closing = std::min(std::fabs(signed_qty), std::fabs(position));
        const double pnl = (position > 0.0 ? (price - avg) : (avg - price)) * closing;
        cash += pnl;
        realized += pnl;
        const double new_pos = position + signed_qty;
        if (std::fabs(new_pos) < 1e-12) {
            position = 0.0;
            avg = 0.0;
        } else if ((position > 0.0 && new_pos < 0.0) || (position < 0.0 && new_pos > 0.0)) {
            avg = price;
            position = new_pos;
        } else {
            position = new_pos;
        }
    }

    double equity(double mid) const {
        if (std::fabs(position) < 1e-12)
            return cash;
        return cash + position * (mid - avg);
    }
};

struct LiveOrder {
    std::string id;
    std::string side;
    double price{0.0};
    double qty{0.0};
    double filled{0.0};
    std::int64_t active_ms{0};
    bool reduce_only{false};
    bool any_fill{false};
};

double mid_of(const DepthSnapshot& snap) {
    return (snap.tight_bid + snap.tight_ask) / 2.0;
}

}  // namespace

BacktestResult run_backtest(const BacktestConfig& config,
                            const std::vector<TradePrint>& trades,
                            const std::vector<DepthSnapshot>& depth,
                            const std::vector<FundingPrint>& funding) {
    if (config.latency_ms < 0)
        throw std::runtime_error("latency must be non-negative");
    BacktestResult result;
    result.limitations = {
        "Simulated fills are not exchange executions.",
        "Maker fills require an actual aggTrade to print strictly through the quote after the order is active. Equal-price touches are not filled because queue position is unknown.",
        "Fill quantity never exceeds the real print quantity.",
        "bookDepth is percentage-bucket depth from data.binance.vision, not a synchronized tick order book. Quotes are anchored to the average price of the tightest published bucket.",
        "bookTicker was not used. If that file is unpublished, this run does not invent a best bid/ask.",
        "Cancel is immediate and the replacement is delayed by the configured latency.",
        "Stop orders, if enabled, consume subsequent real prints at the print price and pay the taker fee. They are not filled at a synthetic price.",
        "Fees are the configured rates, not an account commissionRate response.",
        config.filters_provenance,
    };
    if (trades.empty() || depth.empty())
        throw std::runtime_error("backtest requires real trades and bookDepth snapshots");

    Account account;
    std::vector<LiveOrder> orders;
    DepthSnapshot reference{};
    bool have_reference = false;
    bool stop_latched = false;
    double quoted_qty = 0.0;
    std::int64_t last_equity_second = -1;
    double peak = 0.0;
    bool have_peak = false;

    auto sample_equity = [&](std::int64_t ts, double mid) {
        const std::int64_t second = ts / 1000;
        const double equity = account.equity(mid);
        if (!have_peak || equity > peak)
            peak = equity;
        have_peak = true;
        result.stats.max_drawdown = std::max(result.stats.max_drawdown, peak - equity);
        if (second == last_equity_second && !result.equity.empty()) {
            result.equity.back() = EquityPoint{ts, equity, account.cash, account.position, mid};
            return;
        }
        last_equity_second = second;
        result.equity.push_back(EquityPoint{ts, equity, account.cash, account.position, mid});
    };

    auto place_from_plan = [&](std::int64_t ts) {
        if (!have_reference || !reference.usable)
            return;
        MarketTop top;
        top.bid = reference.tight_bid;
        top.ask = reference.tight_ask;
        top.mark = mid_of(reference);
        Inventory inventory{account.position, account.avg};
        std::vector<RestingOrder> resting;
        for (const auto& order : orders) {
            resting.push_back(RestingOrder{order.id, order.side, order.qty - order.filled, order.price, order.reduce_only});
        }
        QuotePlan plan;
        try {
            plan = plan_quotes(config.quote, top, inventory, resting);
        } catch (const std::exception&) {
            return;
        }
        if (plan.stop_triggered) {
            stop_latched = true;
            orders.clear();
            return;
        }
        const auto actions = diff_orders(resting, plan.orders, config.quote.filters);
        std::unordered_set<std::string> cancel;
        for (const auto& action : actions) {
            if (action.type == OrderAction::Type::Cancel)
                cancel.insert(action.cancel_id);
        }
        if (!cancel.empty()) {
            orders.erase(std::remove_if(orders.begin(), orders.end(),
                                        [&](const LiveOrder& order) { return cancel.count(order.id) != 0; }),
                         orders.end());
        }
        for (const auto& action : actions) {
            if (action.type != OrderAction::Type::Place || action.order.type != "LIMIT")
                continue;
            LiveOrder order;
            order.id = action.order.client_id;
            order.side = action.order.side;
            order.price = action.order.price;
            order.qty = action.order.qty;
            order.active_ms = ts + config.latency_ms;
            order.reduce_only = action.order.reduce_only;
            quoted_qty += order.qty;
            ++result.stats.orders_placed;
            orders.push_back(order);
        }
    };

    std::size_t depth_i = 0;
    std::size_t fund_i = 0;
    std::int64_t window_start = 0;
    std::int64_t window_end = 0;
    if (!trades.empty()) {
        window_start = trades.front().ts_ms;
        window_end = trades.back().ts_ms;
    }

    for (const auto& trade : trades) {
        ++result.stats.trades_seen;
        while (depth_i < depth.size() && depth[depth_i].ts_ms <= trade.ts_ms) {
            reference = depth[depth_i];
            have_reference = true;
            if (reference.usable)
                ++result.stats.depth_used;
            else
                ++result.stats.depth_rejected;
            if (result.stats.first_mid == 0.0 && reference.usable)
                result.stats.first_mid = mid_of(reference);
            place_from_plan(reference.ts_ms);
            ++depth_i;
        }
        while (fund_i < funding.size() && funding[fund_i].ts_ms <= trade.ts_ms) {
            if (funding[fund_i].ts_ms >= window_start && funding[fund_i].ts_ms <= window_end && have_reference &&
                reference.usable) {
                account.apply_funding(mid_of(reference), funding[fund_i].rate);
                ++result.stats.funding_applied;
                result.stats.funding_covered = true;
            }
            ++fund_i;
        }
        if (!have_reference || !reference.usable)
            continue;
        const double mid = mid_of(reference);
        if (stop_latched && std::fabs(account.position) > 1e-12) {
            const double qty = std::min(trade.qty, std::fabs(account.position));
            if (qty > 0.0) {
                const double signed_qty = account.position > 0.0 ? -qty : qty;
                const double fee = qty * trade.price * config.quote.fees.taker;
                account.apply_fill(signed_qty, trade.price, fee);
                result.fills.push_back(SimFill{trade.ts_ms, trade.id, "mm-stop", signed_qty < 0.0 ? "SELL" : "BUY",
                                               "taker", trade.price, qty, fee});
                ++result.stats.fills;
            }
            if (std::fabs(account.position) < config.quote.filters.min_qty)
                stop_latched = false;
            sample_equity(trade.ts_ms, mid);
            result.stats.max_abs_inventory = std::max(result.stats.max_abs_inventory, std::fabs(account.position));
            continue;
        }

        std::vector<std::size_t> crossed;
        for (std::size_t i = 0; i < orders.size(); ++i) {
            const auto& order = orders[i];
            if (trade.ts_ms < order.active_ms)
                continue;
            const double remaining = order.qty - order.filled;
            if (remaining <= 1e-12)
                continue;
            if (order.side == "BUY" && trade.buyer_is_maker && trade.price < order.price)
                crossed.push_back(i);
            else if (order.side == "SELL" && !trade.buyer_is_maker && trade.price > order.price)
                crossed.push_back(i);
            else if (order.side == "BUY" && trade.buyer_is_maker && same_step(trade.price, order.price, config.quote.filters.tick))
                ++result.stats.touches_not_filled;
            else if (order.side == "SELL" && !trade.buyer_is_maker && same_step(trade.price, order.price, config.quote.filters.tick))
                ++result.stats.touches_not_filled;
        }
        std::sort(crossed.begin(), crossed.end(), [&](std::size_t a, std::size_t b) {
            if (orders[a].side != orders[b].side)
                return orders[a].side == "BUY";
            if (orders[a].side == "BUY")
                return orders[a].price > orders[b].price;
            return orders[a].price < orders[b].price;
        });
        double available = trade.qty;
        for (std::size_t index : crossed) {
            if (available <= 1e-12)
                break;
            auto& order = orders[index];
            double room = order.qty - order.filled;
            if (order.reduce_only)
                room = std::min(room, std::fabs(account.position));
            const double qty = std::min(room, available);
            if (qty <= 1e-12)
                continue;
            const double signed_qty = order.side == "BUY" ? qty : -qty;
            if (order.reduce_only && account.position * signed_qty > 0.0)
                continue;
            const double fee = qty * order.price * config.quote.fees.maker;
            account.apply_fill(signed_qty, order.price, fee);
            order.filled += qty;
            order.any_fill = true;
            available -= qty;
            result.fills.push_back(SimFill{trade.ts_ms, trade.id, order.id, order.side, "maker", order.price, qty, fee});
            ++result.stats.fills;
        }
        orders.erase(std::remove_if(orders.begin(), orders.end(),
                                    [](const LiveOrder& order) { return order.qty - order.filled <= 1e-12; }),
                     orders.end());
        sample_equity(trade.ts_ms, mid);
        result.stats.max_abs_inventory = std::max(result.stats.max_abs_inventory, std::fabs(account.position));
        result.stats.last_mid = mid;
    }

    if (result.stats.first_mid > 0.0 && result.stats.last_mid > 0.0)
        result.stats.benchmark_pnl = config.quote.budget_usd * (result.stats.last_mid / result.stats.first_mid - 1.0);
    result.stats.realized = account.realized;
    result.stats.fees = account.fees;
    result.stats.funding = account.funding;
    result.stats.unrealized = std::fabs(account.position) < 1e-12 || result.stats.last_mid <= 0.0
                                  ? 0.0
                                  : account.position * (result.stats.last_mid - account.avg);
    result.stats.net_pnl = account.equity(result.stats.last_mid > 0.0 ? result.stats.last_mid : result.stats.first_mid);
    result.stats.end_inventory = account.position;
    result.stats.end_avg_entry = account.avg;
    if (config.quote.budget_usd > 0.0)
        result.stats.max_drawdown_over_budget = result.stats.max_drawdown / config.quote.budget_usd;
    if (quoted_qty > 0.0) {
        double filled_qty = 0.0;
        for (const auto& fill : result.fills)
            if (fill.liquidity == "maker")
                filled_qty += fill.qty;
        result.stats.qty_fill_rate = filled_qty / quoted_qty;
    }
    std::unordered_set<std::string> maker_ids;
    for (const auto& fill : result.fills)
        if (fill.liquidity == "maker")
            maker_ids.insert(fill.client_id);
    result.stats.orders_filled = static_cast<int>(maker_ids.size());
    if (result.stats.orders_placed > 0)
        result.stats.order_fill_rate = static_cast<double>(result.stats.orders_filled) /
                                       static_cast<double>(result.stats.orders_placed);

    auto markout = [&](std::int64_t horizon) {
        double sum = 0.0;
        int n = 0;
        for (const auto& fill : result.fills) {
            if (fill.liquidity != "maker")
                continue;
            const auto it = std::find_if(depth.begin(), depth.end(), [&](const DepthSnapshot& snap) {
                return snap.usable && snap.ts_ms >= fill.ts_ms + horizon;
            });
            if (it == depth.end())
                continue;
            const double future = mid_of(*it);
            const double sign = fill.side == "BUY" ? 1.0 : -1.0;
            sum += sign * (future - fill.price) * fill.qty;
            ++n;
        }
        return std::make_pair(n ? sum / n : 0.0, n);
    };
    const auto m5 = markout(5000);
    const auto m30 = markout(30000);
    result.stats.mean_markout_5s = m5.first;
    result.stats.markout_5s_n = m5.second;
    result.stats.mean_markout_30s = m30.first;
    result.stats.markout_30s_n = m30.second;

    std::vector<double> minute_equity;
    std::int64_t last_minute = -1;
    for (const auto& point : result.equity) {
        const auto minute = point.ts_ms / 60000;
        if (minute != last_minute) {
            minute_equity.push_back(point.equity);
            last_minute = minute;
        } else {
            minute_equity.back() = point.equity;
        }
    }
    if (minute_equity.size() < 31) {
        result.stats.sharpe_note = "undefined: fewer than 30 one-minute equity samples";
    } else {
        std::vector<double> returns;
        returns.reserve(minute_equity.size() - 1);
        for (std::size_t i = 1; i < minute_equity.size(); ++i)
            returns.push_back((minute_equity[i] - minute_equity[i - 1]) / config.quote.budget_usd);
        double mean = 0.0;
        for (double r : returns)
            mean += r;
        mean /= static_cast<double>(returns.size());
        double var = 0.0;
        for (double r : returns)
            var += (r - mean) * (r - mean);
        var /= static_cast<double>(returns.size());
        const double sd = std::sqrt(var);
        if (!(sd > 0.0)) {
            result.stats.sharpe_note = "undefined: zero variance of one-minute returns";
        } else {
            result.stats.sharpe = mean / sd * std::sqrt(365.25 * 24.0 * 60.0);
            result.stats.sharpe_defined = true;
            result.stats.sharpe_note = "one-minute equity returns, annualized with 365.25*24*60; not a guarantee";
        }
    }
    if (!result.stats.funding_covered)
        result.limitations.push_back(
            "No funding print fell inside this replay window. Funding PnL is zero because the monthly file was absent or did not overlap; rates were not extrapolated.");
    return result;
}

}  // namespace mm
