#include "mm/order_book.hpp"

#include <cmath>

namespace mm {

void OrderBook::reset() {
    bids_.clear();
    asks_.clear();
    last_u_ = 0;
    have_snapshot_ = false;
    synced_ = false;
}

std::int64_t OrderBook::key_of(double price) {
    return std::llround(price * 1e8);
}

double OrderBook::price_of(std::int64_t key) {
    return static_cast<double>(key) / 1e8;
}

template <typename Map>
void OrderBook::set_level(Map& side, double price, double qty) {
    const auto key = key_of(price);
    if (!(qty > 0.0))
        side.erase(key);
    else
        side[key] = qty;
}

void OrderBook::apply_snapshot(std::int64_t last_update_id,
                               const std::vector<BookLevel>& bids,
                               const std::vector<BookLevel>& asks) {
    bids_.clear();
    asks_.clear();
    for (const auto& level : bids)
        set_level(bids_, level.price, level.qty);
    for (const auto& level : asks)
        set_level(asks_, level.price, level.qty);
    last_u_ = last_update_id;
    have_snapshot_ = true;
    synced_ = false;
}

DiffResult OrderBook::apply_diff(const DepthEvent& event) {
    if (!have_snapshot_)
        return DiffResult::NeedResync;
    if (event.first_update_id <= 0 || event.final_update_id < event.first_update_id) {
        synced_ = false;
        have_snapshot_ = false;
        return DiffResult::NeedResync;
    }
    if (!synced_) {
        // Events ending before the snapshot are expected while bridging.
        if (event.final_update_id < last_u_)
            return DiffResult::IgnoredStale;
        // If the first remaining event starts after the snapshot ID, the
        // stream has already advanced past the bridge and a fresh snapshot is required.
        if (event.first_update_id > last_u_) {
            synced_ = false;
            have_snapshot_ = false;
            return DiffResult::NeedResync;
        }
        if (event.final_update_id < last_u_)
            return DiffResult::IgnoredStale;
    } else {
        if (event.final_update_id <= last_u_)
            return DiffResult::IgnoredStale;
        if (event.has_previous) {
            if (event.previous_update_id != last_u_) {
                synced_ = false;
                have_snapshot_ = false;
                return DiffResult::NeedResync;
            }
        } else if (event.first_update_id > last_u_ + 1) {
            synced_ = false;
            have_snapshot_ = false;
            return DiffResult::NeedResync;
        }
    }
    for (const auto& level : event.bids)
        set_level(bids_, level.price, level.qty);
    for (const auto& level : event.asks)
        set_level(asks_, level.price, level.qty);
    last_u_ = event.final_update_id;
    synced_ = true;
    return DiffResult::Applied;
}

std::optional<std::pair<double, double>> OrderBook::top() const {
    if (!synced_ || bids_.empty() || asks_.empty())
        return std::nullopt;
    const double bid = price_of(bids_.begin()->first);
    const double ask = price_of(asks_.begin()->first);
    if (!(bid > 0.0) || !(ask > bid))
        return std::nullopt;
    return std::make_pair(bid, ask);
}

double OrderBook::bid_qty() const {
    return bids_.empty() ? 0.0 : bids_.begin()->second;
}

double OrderBook::ask_qty() const {
    return asks_.empty() ? 0.0 : asks_.begin()->second;
}

}  // namespace mm
