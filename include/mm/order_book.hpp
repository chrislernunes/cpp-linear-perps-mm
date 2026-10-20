#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace mm {

struct BookLevel {
    double price{0.0};
    double qty{0.0};
};

struct DepthEvent {
    std::int64_t first_update_id{0};  // U
    std::int64_t final_update_id{0};  // u
    std::int64_t previous_update_id{0};  // pu
    bool has_previous{false};
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
};

enum class DiffResult { Applied, IgnoredStale, NeedResync };

// Local USD-M book per Binance's diff-depth rules:
// snapshot lastUpdateId, bridge the first event with U <= id <= u,
// then require pu == previous u.
class OrderBook {
public:
    void reset();
    void apply_snapshot(std::int64_t last_update_id,
                        const std::vector<BookLevel>& bids,
                        const std::vector<BookLevel>& asks);
    DiffResult apply_diff(const DepthEvent& event);
    bool synced() const { return synced_; }
    bool has_snapshot() const { return have_snapshot_; }
    std::int64_t last_update_id() const { return last_u_; }
    std::optional<std::pair<double, double>> top() const;
    double bid_qty() const;
    double ask_qty() const;
    std::size_t bid_levels() const { return bids_.size(); }
    std::size_t ask_levels() const { return asks_.size(); }

private:
    static std::int64_t key_of(double price);
    static double price_of(std::int64_t key);
    template <typename Map>
    static void set_level(Map& side, double price, double qty);

    std::map<std::int64_t, double, std::greater<std::int64_t>> bids_;
    std::map<std::int64_t, double> asks_;
    std::int64_t last_u_{0};
    bool have_snapshot_{false};
    bool synced_{false};
};

}  // namespace mm
