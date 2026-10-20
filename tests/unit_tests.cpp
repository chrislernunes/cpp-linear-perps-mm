#include "mm/backtest.hpp"
#include "mm/decimal.hpp"
#include "mm/demo.hpp"
#include "mm/history.hpp"
#include "mm/json_mini.hpp"
#include "mm/net.hpp"
#include "mm/order_book.hpp"
#include "mm/quote.hpp"
#include "mm/zip_archive.hpp"

#include <zlib.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int g_failed = 0;

void check(bool condition, const std::string& name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++g_failed;
    }
}

template <typename Fn>
void expect_throw(Fn fn, const std::string& name) {
    try {
        fn();
        std::cerr << "FAIL " << name << " (no throw)\n";
        ++g_failed;
    } catch (const std::exception&) {
    }
}

mm::QuoteConfig base_config() {
    mm::QuoteConfig config;
    config.filters.tick = 0.1;
    config.filters.step = 0.001;
    config.filters.min_qty = 0.001;
    config.filters.min_notional = 5;
    config.budget_usd = 1000;
    config.min_spread_bps = 10;
    config.spread_factor = 1;
    config.max_net_qty = 1;
    config.gross_notional_cap = 5000;
    config.ladder_levels = 1;
    config.fees.maker = 0.0002;
    config.fees.taker = 0.0005;
    return config;
}

void test_decimal() {
    check(std::fabs(mm::floor_to_step(1.29, 0.1) - 1.2) < 1e-9, "floor step");
    check(std::fabs(mm::ceil_to_step(1.21, 0.1) - 1.3) < 1e-9, "ceil step");
    check(mm::format_step(1.2, 0.1) == "1.2", "format tenth");
    check(mm::format_step(0.001, 0.001) == "0.001", "format thousandth");
}

void test_json_and_api_error() {
    const mm::Json json = mm::parse_json("{\"code\":-1121,\"msg\":\"Invalid symbol.\",\"ok\":true,\"n\":1.5}");
    check(json.at("code").i64() == -1121, "json code");
    check(json.at("msg").str() == "Invalid symbol.", "json msg");
    check(json.at("ok").boolean(), "json bool");
    mm::ApiError error;
    const std::string body = "{\"code\":0,\"msg\":\"Service unavailable from a restricted location\"}";
    check(mm::parse_api_error(451, body, error), "parse 451");
    check(error.http_status == 451 && error.message.find("restricted") != std::string::npos, "451 message");
    check(!mm::parse_api_error(200, body, error), "200 is not an API error");
    bool threw = false;
    try {
        mm::parse_json("{");
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "bad json throws");
}

void test_order_book() {
    mm::OrderBook book;
    mm::DepthEvent early;
    early.first_update_id = 1;
    early.final_update_id = 2;
    early.has_previous = true;
    early.previous_update_id = 0;
    check(book.apply_diff(early) == mm::DiffResult::NeedResync, "diff before snapshot");
    book.apply_snapshot(100, {{100.0, 2.0}, {99.0, 1.0}}, {{101.0, 3.0}});
    mm::DepthEvent stale;
    stale.first_update_id = 90;
    stale.final_update_id = 99;
    stale.has_previous = true;
    stale.previous_update_id = 89;
    check(book.apply_diff(stale) == mm::DiffResult::IgnoredStale, "stale before bridge");
    mm::DepthEvent bridge;
    bridge.first_update_id = 100;
    bridge.final_update_id = 105;
    bridge.has_previous = true;
    bridge.previous_update_id = 99;
    bridge.bids = {{100.0, 0.0}, {100.5, 4.0}};
    bridge.asks = {{101.0, 1.5}};
    check(book.apply_diff(bridge) == mm::DiffResult::Applied, "bridge applied");
    const auto top = book.top();
    check(top && std::fabs(top->first - 100.5) < 1e-6 && std::fabs(top->second - 101.0) < 1e-6, "top after delete");
    check(std::fabs(book.ask_qty() - 1.5) < 1e-9, "ask qty");
    mm::DepthEvent gap;
    gap.first_update_id = 110;
    gap.final_update_id = 111;
    gap.has_previous = true;
    gap.previous_update_id = 106;
    check(book.apply_diff(gap) == mm::DiffResult::NeedResync, "pu gap");
    check(!book.synced(), "unsynced after gap");

    // A stream that has advanced beyond the snapshot without a bridging event
    // must trigger a resnapshot instead of remaining indefinitely unsynchronized.
    mm::OrderBook no_bridge;
    no_bridge.apply_snapshot(500, {{100.0, 1.0}}, {{101.0, 1.0}});
    mm::DepthEvent missed_bridge;
    missed_bridge.first_update_id = 501;
    missed_bridge.final_update_id = 503;
    missed_bridge.has_previous = true;
    missed_bridge.previous_update_id = 500;
    check(no_bridge.apply_diff(missed_bridge) == mm::DiffResult::NeedResync,
          "event after snapshot without bridge requires resync");
    check(!no_bridge.synced() && !no_bridge.has_snapshot(),
          "no bridge never reports synced");

    // Verify multiple buffered/replayed updates can be applied sequentially.
    mm::OrderBook replay;
    replay.apply_snapshot(1000, {{100.0, 2.0}}, {{101.0, 2.0}});
    mm::DepthEvent old_buffered;
    old_buffered.first_update_id = 990;
    old_buffered.final_update_id = 999;
    check(replay.apply_diff(old_buffered) == mm::DiffResult::IgnoredStale,
          "buffered event older than snapshot discarded");
    mm::DepthEvent first_buffered;
    first_buffered.first_update_id = 998;
    first_buffered.final_update_id = 1002;
    first_buffered.has_previous = true;
    first_buffered.previous_update_id = 997;
    first_buffered.bids = {{100.0, 3.0}};
    check(replay.apply_diff(first_buffered) == mm::DiffResult::Applied,
          "first buffered event bridges snapshot");
    mm::DepthEvent second_buffered;
    second_buffered.first_update_id = 1003;
    second_buffered.final_update_id = 1005;
    second_buffered.has_previous = true;
    second_buffered.previous_update_id = 1002;
    second_buffered.asks = {{101.0, 0.0}, {101.1, 1.5}};
    check(replay.apply_diff(second_buffered) == mm::DiffResult::Applied,
          "second buffered event follows pu chain");
    check(replay.synced() && replay.last_update_id() == 1005,
          "replayed update chain remains synchronized");

    mm::DepthEvent bad_sequence;
    bad_sequence.first_update_id = 1006;
    bad_sequence.final_update_id = 1007;
    bad_sequence.has_previous = true;
    bad_sequence.previous_update_id = 1004;
    check(replay.apply_diff(bad_sequence) == mm::DiffResult::NeedResync,
          "broken pu chain requires resync");
    check(!replay.synced() && !replay.has_snapshot(),
          "broken pu chain invalidates snapshot");
}

void test_quotes() {
    const auto config = base_config();
    mm::MarketTop market{100.0, 100.2, 100.1};
    const mm::QuotePlan flat = mm::plan_quotes(config, market, {}, {});
    check(flat.orders.size() == 2, "two sided quotes");
    check(flat.orders[0].side == "BUY" && flat.orders[0].tif == "GTX", "post only bid");
    check(flat.orders[0].price < market.bid || flat.orders[0].price <= 100.0, "bid not through");
    check(flat.orders[1].price >= market.ask, "ask not through");
    mm::Inventory long_inventory{1.0, 100.0};
    const mm::QuotePlan blocked = mm::plan_quotes(config, market, long_inventory, {});
    bool has_bid = false;
    for (const auto& order : blocked.orders)
        if (order.side == "BUY" && !order.reduce_only)
            has_bid = true;
    check(!has_bid, "inventory cap removes bid");
    mm::QuoteConfig rich = config;
    rich.filters.min_notional = 100000;
    const mm::QuotePlan none = mm::plan_quotes(rich, market, {}, {});
    check(none.orders.empty(), "min notional blocks");
    std::vector<mm::RestingOrder> live = {{"mm-b1", "BUY", flat.orders[0].qty, flat.orders[0].price, false}};
    const auto actions = mm::diff_orders(live, flat.orders, config.filters);
    int cancels = 0;
    int places = 0;
    for (const auto& action : actions) {
        if (action.type == mm::OrderAction::Type::Cancel)
            ++cancels;
        if (action.type == mm::OrderAction::Type::Place)
            ++places;
    }
    check(cancels == 0 && places == 1, "diff keeps unchanged bid");
}

void test_hmac_and_demo_guard() {
    const std::string mac = mm::hmac_sha256_hex("key", "The quick brown fox jumps over the lazy dog");
    check(mac == "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8", "hmac vector");
    bool refused = false;
    try {
        mm::assert_demo_order_url("https://fapi.binance.com/fapi/v1/order");
    } catch (const std::exception&) {
        refused = true;
    }
    check(refused, "production order URL refused");
    mm::assert_demo_order_url("https://demo-fapi.binance.com/fapi/v1/order?symbol=BTCUSDT");
    mm::PlannedOrder order;
    order.client_id = "mm-b1";
    order.side = "BUY";
    order.type = "LIMIT";
    order.tif = "GTX";
    order.qty = 0.002;
    order.price = 100.1;
    const auto prepared = mm::prepare_order("demo-key", "demo-secret", "BTCUSDT", order, base_config().filters, 1591702613943);
    check(prepared.url.rfind("https://demo-fapi.binance.com/fapi/v1/order?", 0) == 0, "demo host only");
    check(prepared.url.find("fapi.binance.com/fapi") == std::string::npos ||
              prepared.url.find("demo-fapi.binance.com") != std::string::npos,
          "no production host");
    check(prepared.signed_query.find("signature=") != std::string::npos, "signature present");
    bool market_refused = false;
    try {
        order.type = "MARKET";
        order.reduce_only = false;
        mm::prepare_order("demo-key", "demo-secret", "BTCUSDT", order, base_config().filters, 1);
    } catch (const std::exception&) {
        market_refused = true;
    }
    check(market_refused, "non-reduce market refused");
}

void test_parsers() {
    mm::FileProvenance provenance;
    const std::string trades =
        "agg_trade_id,price,quantity,first_trade_id,last_trade_id,transact_time,is_buyer_maker\n"
        "10,100.5,0.2,1,2,1792368000004,true\n"
        "11,100.4,0.1,3,3,1792368000100,false\n";
    const auto parsed = mm::parse_agg_trades_csv(trades, provenance);
    check(parsed.size() == 2 && parsed[0].buyer_is_maker && !parsed[1].buyer_is_maker, "agg parser");
    check(parsed[0].ts_ms == 1792368000004, "agg timestamp");
    bool bad = false;
    try {
        mm::parse_agg_trades_csv("a,b\n1,2\n", provenance);
    } catch (const std::exception&) {
        bad = true;
    }
    check(bad, "bad agg header");
    const std::string depth =
        "timestamp,percentage,depth,notional\n"
        "2026-10-19 00:00:01,-0.20,10,1000\n"
        "2026-10-19 00:00:01,0.20,10,1002\n";
    const auto snaps = mm::parse_book_depth_csv(depth, provenance);
    check(snaps.size() == 1 && snaps[0].usable, "depth parser usable");
    check(std::fabs(snaps[0].tight_bid - 100.0) < 1e-9, "tight bid average");
    check(mm::parse_utc_millis("1792368000004") == 1792368000004, "millis timestamp");
    const auto midnight = mm::parse_utc_millis("2026-10-19 00:00:00");
    check(midnight == 1792368000000LL, "calendar timestamp utc");
    const std::string funding =
        "calc_time,funding_interval_hours,last_funding_rate\n"
        "1792368000000,8,0.0001\n";
    const auto rates = mm::parse_funding_csv(funding, provenance);
    check(rates.size() == 1 && std::fabs(rates[0].rate - 0.0001) < 1e-12, "funding parser");
}

void test_backtest_fills_fees_funding() {
    mm::BacktestConfig config;
    config.symbol = "TEST";
    config.latency_ms = 0;
    config.quote = base_config();
    config.quote.budget_usd = 1000;
    config.quote.min_spread_bps = 0;
    config.quote.spread_factor = 1;
    config.quote.max_net_qty = 5;
    config.quote.filters.min_notional = 1;
    config.filters_provenance = "unit fixture, not market data";
    mm::DepthSnapshot snap;
    snap.ts_ms = 1'000;
    snap.tight_bid = 100.0;
    snap.tight_ask = 100.2;
    snap.usable = true;
    std::vector<mm::TradePrint> trades = {
        {1, 1'500, 100.2, 1.0, false},
        {2, 1'600, 99.0, 0.01, true},
        {3, 1'700, 101.0, 0.02, false},
    };
    std::vector<mm::FundingPrint> funding = {{1'650, 0.001, 8}};
    const mm::BacktestResult result = mm::run_backtest(config, trades, {snap}, funding);
    check(result.stats.trades_seen == 3, "trades counted");
    check(result.stats.touches_not_filled >= 1, "touch not filled");
    bool saw_through = false;
    double max_fill = 0.0;
    for (const auto& fill : result.fills) {
        max_fill = std::max(max_fill, fill.qty);
        if (fill.trade_id == 3 && fill.liquidity == "maker")
            saw_through = true;
        check(fill.qty <= 0.02 + 1e-9 || fill.trade_id != 3, "fill not larger than print");
        if (fill.liquidity == "maker")
            check(fill.fee > 0.0, "maker fee charged");
    }
    check(saw_through, "strict through sell fill");
    check(max_fill <= 0.02 + 1e-9, "qty cap");
    check(result.stats.funding_applied == 1, "funding print applied");
    check(result.stats.funding < 0.0, "long inventory pays positive funding");
    config.latency_ms = 10'000;
    const mm::BacktestResult delayed = mm::run_backtest(config, trades, {snap}, funding);
    check(delayed.stats.fills == 0, "latency blocks fills");
    bool empty = false;
    try {
        mm::run_backtest(config, {}, {snap}, {});
    } catch (const std::exception&) {
        empty = true;
    }
    check(empty, "empty trades refused");
}

void test_filters_orders_and_shutdown() {
    const char* info =
        "{\"symbols\":[{\"symbol\":\"BTCUSDT\",\"filters\":["
        "{\"filterType\":\"PRICE_FILTER\",\"tickSize\":\"0.10\",\"minPrice\":\"1\",\"maxPrice\":\"1000000\"},"
        "{\"filterType\":\"LOT_SIZE\",\"stepSize\":\"0.001\",\"minQty\":\"0.001\",\"maxQty\":\"100\"},"
        "{\"filterType\":\"MIN_NOTIONAL\",\"notional\":\"50\"}]}]}";
    mm::Filters filters;
    std::string error;
    check(mm::parse_symbol_filters(mm::parse_json(info), "BTCUSDT", filters, error), "exchangeInfo filters");
    check(std::fabs(filters.tick - 0.1) < 1e-12 && std::fabs(filters.step - 0.001) < 1e-12, "tick and step");
    check(std::fabs(filters.min_notional - 50.0) < 1e-12, "min notional");
    check(!mm::parse_symbol_filters(mm::parse_json(info), "ETHUSDT", filters, error), "missing symbol");
    const auto blocked = mm::demo_send_decision(true, false, 451, true, true);
    check(!blocked.send && blocked.reason.find("451") != std::string::npos, "451 does not send");
    const auto check_only = mm::demo_send_decision(true, true, 200, true, true);
    check(!check_only.send, "check mode does not send");
    const auto allowed = mm::demo_send_decision(true, false, 200, true, true);
    check(allowed.send, "demo ping 200 can send");
    expect_throw([] { mm::assert_demo_order_url("https://fapi.binance.com/fapi/v1/order"); },
                 "production URL throws");
    mm::PlannedOrder order;
    order.client_id = "mm-b1";
    order.side = "BUY";
    order.qty = 0.002;
    order.price = 100.1;
    std::vector<mm::RestingOrder> resting;
    std::string note;
    const std::string ack =
        "{\"orderId\":7,\"clientOrderId\":\"mm-b1\",\"status\":\"NEW\",\"price\":\"100.1\",\"origQty\":\"0.002\","
        "\"executedQty\":\"0\",\"side\":\"BUY\"}";
    check(mm::apply_order_response(200, ack, order, resting, note), "ack new order");
    check(resting.size() == 1 && resting[0].client_id == "mm-b1", "working order stored");
    const std::string reject = "{\"code\":-2019,\"msg\":\"Margin is insufficient.\"}";
    check(!mm::apply_order_response(400, reject, order, resting, note), "api error rejected");
    check(resting.size() == 1 && note.find("Margin") != std::string::npos, "reject leaves working order");
    const std::string canceled =
        "{\"orderId\":7,\"clientOrderId\":\"mm-b1\",\"status\":\"CANCELED\",\"price\":\"100.1\",\"origQty\":\"0.002\","
        "\"executedQty\":\"0\",\"side\":\"BUY\"}";
    check(mm::apply_order_response(200, canceled, order, resting, note), "cancel applied");
    check(resting.empty(), "cancel removes working order");
    mm::Inventory inventory;
    const std::string position =
        "[{\"symbol\":\"BTCUSDT\",\"positionAmt\":\"0.010\",\"entryPrice\":\"82000\",\"positionSide\":\"BOTH\"}]";
    check(mm::parse_one_way_position(position, "BTCUSDT", inventory, error), "one-way position");
    check(std::fabs(inventory.net_qty - 0.01) < 1e-12, "position qty");
    const std::string hedge =
        "[{\"symbol\":\"BTCUSDT\",\"positionAmt\":\"0.010\",\"entryPrice\":\"82000\",\"positionSide\":\"LONG\"}]";
    check(!mm::parse_one_way_position(hedge, "BTCUSDT", inventory, error), "hedge mode rejected");
    mm::WsClient ws;
    ws.close();
    check(!ws.connected(), "shutdown of unopened socket");
}

void test_zip_roundtrip() {
    const std::string payload = "timestamp,percentage,depth,notional\n2026-10-19 00:00:01,-0.20,1,100\n";
    z_stream stream{};
    deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(payload.data()));
    stream.avail_in = static_cast<uInt>(payload.size());
    std::vector<unsigned char> comp(payload.size() + 64);
    stream.next_out = comp.data();
    stream.avail_out = static_cast<uInt>(comp.size());
    const int rc = deflate(&stream, Z_FINISH);
    check(rc == Z_STREAM_END, "deflate test payload");
    comp.resize(stream.total_out);
    deflateEnd(&stream);
    const auto crc = crc32(0L, reinterpret_cast<const Bytef*>(payload.data()), static_cast<uInt>(payload.size()));
    std::vector<std::uint8_t> zip;
    auto append_u16 = [&](unsigned value) {
        zip.push_back(static_cast<std::uint8_t>(value & 0xff));
        zip.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    };
    auto append_u32 = [&](unsigned long value) {
        for (int i = 0; i < 4; ++i)
            zip.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
    };
    const std::string name = "sample.csv";
    const std::size_t local = zip.size();
    append_u32(0x04034b50);
    append_u16(20);
    append_u16(0);
    append_u16(8);
    append_u16(0);
    append_u16(0);
    append_u32(crc);
    append_u32(comp.size());
    append_u32(payload.size());
    append_u16(name.size());
    append_u16(0);
    zip.insert(zip.end(), name.begin(), name.end());
    zip.insert(zip.end(), comp.begin(), comp.end());
    const std::size_t central = zip.size();
    append_u32(0x02014b50);
    append_u16(20);
    append_u16(20);
    append_u16(0);
    append_u16(8);
    append_u16(0);
    append_u16(0);
    append_u32(crc);
    append_u32(comp.size());
    append_u32(payload.size());
    append_u16(name.size());
    append_u16(0);
    append_u16(0);
    append_u16(0);
    append_u16(0);
    append_u32(0);
    append_u32(local);
    zip.insert(zip.end(), name.begin(), name.end());
    const std::size_t central_size = zip.size() - central;
    append_u32(0x06054b50);
    append_u16(0);
    append_u16(0);
    append_u16(1);
    append_u16(1);
    append_u32(central_size);
    append_u32(central);
    append_u16(0);
    const auto files = mm::unzip(zip.data(), zip.size());
    check(files.size() == 1 && std::string(files[0].bytes.begin(), files[0].bytes.end()) == payload, "zip roundtrip");
}

}  // namespace

int main() {
    test_decimal();
    test_json_and_api_error();
    test_order_book();
    test_quotes();
    test_hmac_and_demo_guard();
    test_parsers();
    test_backtest_fills_fees_funding();
    test_filters_orders_and_shutdown();
    test_zip_roundtrip();
    if (g_failed) {
        std::cerr << g_failed << " checks failed\n";
        return 1;
    }
    std::cout << "all unit checks passed\n";
    return 0;
}
