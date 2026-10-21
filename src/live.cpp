#include "mm/live.hpp"

#include "mm/demo.hpp"
#include "mm/json_mini.hpp"
#include "mm/net.hpp"
#include "mm/order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace mm {
namespace {

std::string lower_symbol(std::string symbol) {
    for (char& c : symbol)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return symbol;
}

std::vector<BookLevel> levels_from(const Json& array) {
    std::vector<BookLevel> levels;
    if (array.type != Json::Type::Array)
        throw std::runtime_error("depth side is not an array");
    for (const auto& row : array.a) {
        BookLevel level;
        level.price = row.at(0).num();
        level.qty = row.at(1).num();
        levels.push_back(level);
    }
    return levels;
}

DepthEvent event_from_json(const Json& data) {
    DepthEvent event;
    event.first_update_id = data.at("U").i64();
    event.final_update_id = data.at("u").i64();
    if (const Json* pu = data.find("pu")) {
        event.previous_update_id = pu->i64();
        event.has_previous = true;
    }
    event.bids = levels_from(data.at("b"));
    event.asks = levels_from(data.at("a"));
    return event;
}

int status_code(const HttpResponse& response) {
    if (!response.completed)
        return 5;
    if (response.status == 451)
        return 3;
    if (response.status == 200)
        return 0;
    return 4;
}

void print_http(const char* name, const HttpResponse& response) {
    std::cerr << "[public] " << name << " status=" << response.status
              << " completed=" << (response.completed ? "yes" : "no");
    if (!response.error.empty())
        std::cerr << " error=" << response.error;
    std::cerr << "\n";
    if (!response.body.empty()) {
        const auto snippet = response.body.substr(0, 240);
        std::cerr << "[public] body " << snippet << "\n";
    }
}

HttpResponse execute_demo(HttpClient& http, const PreparedRequest& prepared) {
    assert_demo_order_url(prepared.url);
    HttpRequest request;
    request.method = prepared.method;
    request.url = prepared.url;
    request.headers = prepared.headers;
    request.timeout_ms = 10000;
    return http.request(request);
}

bool connect_public(WsClient& ws, const std::string& stream, int attempts) {
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        try {
            ws.connect(stream, 8000);
            if (attempt > 1)
                std::cerr << "[public] reconnected attempt " << attempt << "\n";
            return true;
        } catch (const std::exception& error) {
            std::cerr << "[public] websocket failed attempt " << attempt << ": " << error.what() << "\n";
            ws.close();
        }
    }
    return false;
}

}  // namespace

int run_live(const LiveOptions& options) {
    if (options.symbol.empty())
        throw std::runtime_error("symbol is required");
    HttpClient http;
    const std::string production_ping = std::string(kProductionRest) + "/fapi/v1/ping";
    const std::string demo_ping = std::string(kDemoRest) + "/fapi/v1/ping";
    const HttpResponse prod = http.get(production_ping, 10000);
    const HttpResponse demo = http.get(demo_ping, 10000);
    print_http("fapi.binance.com/fapi/v1/ping", prod);
    print_http("demo-fapi.binance.com/fapi/v1/ping", demo);

    const int prod_code = status_code(prod);
    bool rest_ok = prod.status == 200;
    QuoteConfig quote = options.quote;
    if (rest_ok) {
        try {
            const HttpResponse info = http.get(
                std::string(kProductionRest) + "/fapi/v1/exchangeInfo?symbol=" + options.symbol, 10000);
            std::string filter_error;
            Filters parsed;
            if (info.status == 200 &&
                parse_symbol_filters(parse_json(info.body), options.symbol, parsed, filter_error)) {
                quote.filters = parsed;
                std::cerr << "[public] exchangeInfo filters tick " << parsed.tick << " step " << parsed.step
                          << " min_qty " << parsed.min_qty << " min_notional " << parsed.min_notional << "\n";
            } else {
                std::cerr << "[public] exchangeInfo not applied";
                if (!filter_error.empty())
                    std::cerr << ": " << filter_error;
                else
                    std::cerr << " HTTP " << info.status;
                std::cerr << ". Using operator filter defaults.\n";
            }
        } catch (const std::exception& error) {
            std::cerr << "[public] exchangeInfo failed: " << error.what()
                      << ". Using operator filter defaults.\n";
        }
    } else {
        std::cerr << "[public] REST is not HTTP 200. Diff-depth snapshots and exchangeInfo will not be requested. "
                     "No alternate REST host will be used. Operator filter defaults remain in effect.\n";
    }

    const char* key = std::getenv("BINANCE_DEMO_API_KEY");
    const char* secret = std::getenv("BINANCE_DEMO_API_SECRET");
    const DemoDecision decision = demo_send_decision(options.demo, options.check_only, demo.status, key && key[0],
                                                     secret && secret[0]);
    std::cerr << "[demo] " << decision.reason << "\n";

    const std::string stream = std::string(kPublicWs) + "?streams=" + lower_symbol(options.symbol) +
                               "@depth@100ms/" + lower_symbol(options.symbol) + "@bookTicker";
    std::cerr << "[public] websocket " << stream << "\n";
    WsClient ws;
    if (!connect_public(ws, stream, 3))
        return prod_code == 0 ? 5 : prod_code;

    OrderBook book;
    bool snapshot_requested = false;
    int book_tickers = 0;
    bool depth_warning = false;
    int reconnects = 0;
    std::vector<RestingOrder> resting;
    Inventory inventory;
    bool demo_halted = !decision.send;
    if (decision.send) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        const PreparedRequest position = prepare_position(key, secret, options.symbol, now);
        const HttpResponse response = execute_demo(http, position);
        std::string error;
        if (response.status != 200 || !parse_one_way_position(response.body, options.symbol, inventory, error)) {
            std::cerr << "[demo] position query failed status=" << response.status;
            if (!error.empty())
                std::cerr << " " << error;
            std::cerr << ". Not sending orders.\n";
            demo_halted = true;
        } else {
            std::cerr << "[demo] position " << inventory.net_qty << " entry " << inventory.avg_entry << "\n";
        }
    }

    const int budget_seconds = options.seconds;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() <
           budget_seconds) {
        if (!ws.connected()) {
            if (reconnects >= 3)
                break;
            ++reconnects;
            std::cerr << "[public] websocket closed, reconnect " << reconnects << "\n";
            book.reset();
            snapshot_requested = false;
            // Local resting set may no longer match exchange-side state after a reconnect.
            resting.clear();
            if (!connect_public(ws, stream, 1))
                break;
            continue;
        }
        WsMessage message;
        if (!ws.read(message, 2000)) {
            ws.close();
            continue;
        }
        if (message.type != WsMessage::Type::Text)
            continue;
        Json root;
        try {
            root = parse_json(message.data);
        } catch (const std::exception& error) {
            std::cerr << "[public] ignored malformed frame: " << error.what() << "\n";
            continue;
        }
        Json payload = root;
        if (const Json* data = root.find("data"))
            payload = *data;
        const Json* event = payload.find("e");
        if (!event)
            continue;
        if (event->str() == "bookTicker") {
            const Json* bid = payload.find("b");
            const Json* ask = payload.find("a");
            if (!bid || !ask) {
                std::cerr << "[public] bookTicker missing b/a: " << message.data.substr(0, 220) << "\n";
                continue;
            }
            MarketTop market;
            market.bid = bid->num();
            market.ask = ask->num();
            market.mark = (market.bid + market.ask) / 2.0;
            ++book_tickers;
            if (!(market.ask > market.bid))
                continue;
            QuotePlan plan;
            try {
                plan = plan_quotes(quote, market, inventory, resting);
            } catch (const std::exception& error) {
                std::cerr << "[public] quote skipped: " << error.what() << "\n";
                if (options.check_only)
                    continue;
                continue;
            }
            if (!decision.send || demo_halted) {
                std::cerr << "[public] bookTicker bid " << market.bid << " ask " << market.ask
                          << " dry orders " << plan.orders.size() << " (not sent)\n";
                continue;
            }
            const auto actions = diff_orders(resting, plan.orders, quote.filters);
            std::cerr << "[demo] bookTicker bid " << market.bid << " ask " << market.ask
                      << " actions " << actions.size() << "\n";
            for (const auto& action : actions) {
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                PreparedRequest prepared = action.type == OrderAction::Type::Cancel
                                               ? prepare_cancel_client(key, secret, options.symbol, action.cancel_id, now)
                                               : prepare_order(key, secret, options.symbol, action.order, quote.filters, now);
                const HttpResponse response = execute_demo(http, prepared);
                std::string note;
                const PlannedOrder sent = action.type == OrderAction::Type::Cancel ? PlannedOrder{} : action.order;
                if (action.type == OrderAction::Type::Cancel) {
                    PlannedOrder cancel_as;
                    cancel_as.client_id = action.cancel_id;
                    if (!apply_order_response(response.status, response.body, cancel_as, resting, note)) {
                        std::cerr << "[demo] cancel failed " << action.cancel_id << " " << note << "\n";
                        if (response.status == 451 || response.status == 401 || response.status == 403 ||
                            !response.completed)
                            demo_halted = true;
                    }
                } else if (!apply_order_response(response.status, response.body, sent, resting, note)) {
                    std::cerr << "[demo] place failed " << action.order.client_id << " " << note << "\n";
                    if (response.status == 451 || response.status == 401 || response.status == 403 ||
                        !response.completed)
                        demo_halted = true;
                } else {
                    std::cerr << "[demo] " << action.order.client_id << " " << note << "\n";
                }
                if (demo_halted)
                    break;
            }
        } else if (event->str() == "depthUpdate") {
            if (!rest_ok) {
                if (!depth_warning) {
                    std::cerr << "[public] depthUpdate ignored; snapshot REST is unavailable\n";
                    depth_warning = true;
                }
                continue;
            }
            const DepthEvent diff = event_from_json(payload);
            if (!snapshot_requested) {
                snapshot_requested = true;
                const HttpResponse depth = http.get(
                    std::string(kProductionRest) + "/fapi/v1/depth?symbol=" + options.symbol + "&limit=1000", 15000);
                if (depth.status != 200) {
                    std::cerr << "[public] depth snapshot HTTP " << depth.status << "; book not synced\n";
                    rest_ok = false;
                    continue;
                }
                const Json snap = parse_json(depth.body);
                book.apply_snapshot(snap.at("lastUpdateId").i64(), levels_from(snap.at("bids")),
                                    levels_from(snap.at("asks")));
            }
            if (book.has_snapshot() && book.apply_diff(diff) == DiffResult::NeedResync) {
                book.reset();
                snapshot_requested = false;
                std::cerr << "[public] depth gap, resync required\n";
            }
        }
    }
    ws.close();
    if (book_tickers == 0) {
        std::cerr << "[public] no bookTicker received\n";
        return rest_ok ? 4 : prod_code;
    }
    std::cerr << "[public] bookTicker messages " << book_tickers
              << (book.synced() ? " depth book synced\n" : " depth book NOT synced\n");
    std::cerr << "[public] websocket reconnects " << reconnects << "\n";

    // Demo-order cleanup: cancel any outstanding orders this process may have created,
    // then reconcile via openOrders before reporting. Local resting is not trusted
    // after errors, reconnects, or API failures.
    if (decision.send && key && secret && key[0] && secret[0]) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        bool cleanup_ok = true;
        try {
            const PreparedRequest cancel_all = prepare_cancel_all(key, secret, options.symbol, now);
            const HttpResponse cancel_resp = execute_demo(http, cancel_all);
            if (cancel_resp.status != 200 || !cancel_resp.completed) {
                std::cerr << "[demo] cancel-all failed status=" << cancel_resp.status
                          << "; exchange-side orders may remain\n";
                cleanup_ok = false;
            } else {
                std::cerr << "[demo] cancel-all requested on " << kDemoRestHost << "\n";
            }
        } catch (const std::exception& error) {
            std::cerr << "[demo] cancel-all error: " << error.what()
                      << "; exchange-side orders may remain\n";
            cleanup_ok = false;
        }

        int open_count = -1;
        try {
            const PreparedRequest query = prepare_open_orders(key, secret, options.symbol, now + 1);
            const HttpResponse query_resp = execute_demo(http, query);
            if (query_resp.status == 200 && query_resp.completed && !query_resp.body.empty()) {
                const Json opens = parse_json(query_resp.body);
                if (opens.type == Json::Type::Array)
                    open_count = static_cast<int>(opens.a.size());
                else
                    cleanup_ok = false;
            } else {
                cleanup_ok = false;
            }
        } catch (const std::exception& error) {
            std::cerr << "[demo] openOrders query error: " << error.what() << "\n";
            cleanup_ok = false;
        }

        if (cleanup_ok && open_count == 0) {
            std::cerr << "[demo] confirmed no open orders remain on " << kDemoRestHost << "\n";
        } else if (cleanup_ok && open_count > 0) {
            std::cerr << "[demo] warning: " << open_count
                      << " open order(s) still reported by demo exchange after cancel-all\n";
        } else {
            std::cerr << "[demo] warning: cleanup confirmation failed; exchange-side demo orders may remain\n";
        }
        resting.clear();
    } else {
        std::cerr << "[demo] no demo orders were sent by this process\n";
    }
    return 0;
}

}  // namespace mm
