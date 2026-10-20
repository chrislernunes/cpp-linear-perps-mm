#include "mm/demo.hpp"

#include "mm/decimal.hpp"
#include "mm/net.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace mm {

void assert_demo_order_url(const std::string& url) {
    const std::string prefix = std::string("https://") + kDemoRestHost + "/fapi/";
    if (url.rfind(prefix, 0) != 0)
        throw std::runtime_error("refusing order URL that is not https://demo-fapi.binance.com/fapi/: " + url);
    if (url.find(kProductionRestHost) != std::string::npos && url.find(kDemoRestHost) == std::string::npos)
        throw std::runtime_error("refusing production futures host");
}

namespace {

PreparedRequest finish(const std::string& api_key,
                       const std::string& api_secret,
                       const std::string& method,
                       const std::string& path,
                       std::string query) {
    query += "&signature=" + hmac_sha256_hex(api_secret, query);
    PreparedRequest prepared;
    prepared.method = method;
    prepared.url = std::string(kDemoRest) + path + "?" + query;
    prepared.signed_query = query;
    prepared.headers.push_back({"X-MBX-APIKEY", api_key});
    assert_demo_order_url(prepared.url);
    return prepared;
}

}  // namespace

PreparedRequest prepare_order(const std::string& api_key,
                              const std::string& api_secret,
                              const std::string& symbol,
                              const PlannedOrder& order,
                              const Filters& filters,
                              std::int64_t timestamp_ms) {
    if (api_key.empty() || api_secret.empty())
        throw std::runtime_error("demo order requires BINANCE_DEMO_API_KEY and BINANCE_DEMO_API_SECRET");
    if (order.type == "MARKET" && !order.reduce_only)
        throw std::runtime_error("market orders are only prepared as reduce-only demo exits");
    if (order.type == "LIMIT" && order.tif != "GTX")
        throw std::runtime_error("opening limits must be GTX post-only");
    std::ostringstream query;
    query << "symbol=" << symbol
          << "&side=" << order.side
          << "&type=" << order.type;
    if (order.type == "LIMIT") {
        query << "&timeInForce=" << order.tif
              << "&quantity=" << format_step(order.qty, filters.step)
              << "&price=" << format_step(order.price, filters.tick);
    } else {
        query << "&quantity=" << format_step(order.qty, filters.step);
    }
    if (order.reduce_only)
        query << "&reduceOnly=true";
    query << "&newClientOrderId=" << order.client_id
          << "&recvWindow=5000&timestamp=" << timestamp_ms;
    return finish(api_key, api_secret, "POST", "/fapi/v1/order", query.str());
}

PreparedRequest prepare_cancel_all(const std::string& api_key,
                                   const std::string& api_secret,
                                   const std::string& symbol,
                                   std::int64_t timestamp_ms) {
    if (api_key.empty() || api_secret.empty())
        throw std::runtime_error("demo cancel requires demo API credentials");
    std::ostringstream query;
    query << "symbol=" << symbol << "&recvWindow=5000&timestamp=" << timestamp_ms;
    return finish(api_key, api_secret, "DELETE", "/fapi/v1/allOpenOrders", query.str());
}

PreparedRequest prepare_cancel_client(const std::string& api_key,
                                      const std::string& api_secret,
                                      const std::string& symbol,
                                      const std::string& client_id,
                                      std::int64_t timestamp_ms) {
    if (api_key.empty() || api_secret.empty())
        throw std::runtime_error("demo cancel requires demo API credentials");
    if (client_id.empty())
        throw std::runtime_error("demo cancel requires a client id");
    std::ostringstream query;
    query << "symbol=" << symbol << "&origClientOrderId=" << client_id
          << "&recvWindow=5000&timestamp=" << timestamp_ms;
    return finish(api_key, api_secret, "DELETE", "/fapi/v1/order", query.str());
}

PreparedRequest prepare_position(const std::string& api_key,
                                 const std::string& api_secret,
                                 const std::string& symbol,
                                 std::int64_t timestamp_ms) {
    if (api_key.empty() || api_secret.empty())
        throw std::runtime_error("demo position query requires demo API credentials");
    std::ostringstream query;
    query << "symbol=" << symbol << "&recvWindow=5000&timestamp=" << timestamp_ms;
    return finish(api_key, api_secret, "GET", "/fapi/v2/positionRisk", query.str());
}

DemoDecision demo_send_decision(bool demo_requested,
                                bool check_only,
                                int demo_http_status,
                                bool have_key,
                                bool have_secret) {
    DemoDecision decision;
    if (check_only) {
        decision.reason = "check mode does not send orders";
        return decision;
    }
    if (!demo_requested) {
        decision.reason = "demo disabled";
        return decision;
    }
    if (demo_http_status != 200) {
        decision.reason = "demo REST status " + std::to_string(demo_http_status) + "; not sending";
        return decision;
    }
    if (!have_key || !have_secret) {
        decision.reason = "demo mode requires BINANCE_DEMO_API_KEY and BINANCE_DEMO_API_SECRET";
        return decision;
    }
    decision.send = true;
    decision.reason = "demo REST ping is HTTP 200 and demo credentials are set";
    return decision;
}

namespace {

double filter_number(const Json& object, const char* key) {
    const Json* value = object.find(key);
    if (!value)
        throw std::runtime_error(std::string("missing filter field ") + key);
    return value->num();
}

void erase_client(std::vector<RestingOrder>& resting, const std::string& client_id) {
    resting.erase(std::remove_if(resting.begin(), resting.end(),
                                 [&](const RestingOrder& order) { return order.client_id == client_id; }),
                  resting.end());
}

}  // namespace

bool parse_symbol_filters(const Json& exchange_info,
                          const std::string& symbol,
                          Filters& out,
                          std::string& error) {
    try {
        const Json* symbols = exchange_info.find("symbols");
        if (!symbols || symbols->type != Json::Type::Array) {
            error = "exchangeInfo has no symbols array";
            return false;
        }
        const Json* match = nullptr;
        for (const auto& row : symbols->a) {
            const Json* name = row.find("symbol");
            if (name && name->str() == symbol) {
                match = &row;
                break;
            }
        }
        if (!match) {
            error = "symbol " + symbol + " is not in exchangeInfo";
            return false;
        }
        const Json* filters = match->find("filters");
        if (!filters || filters->type != Json::Type::Array) {
            error = "symbol has no filters";
            return false;
        }
        bool have_price = false;
        bool have_lot = false;
        bool have_notional = false;
        Filters parsed;
        for (const auto& filter : filters->a) {
            const Json* kind = filter.find("filterType");
            if (!kind)
                continue;
            const std::string type = kind->str();
            if (type == "PRICE_FILTER") {
                parsed.tick = filter_number(filter, "tickSize");
                parsed.min_price = filter_number(filter, "minPrice");
                parsed.max_price = filter_number(filter, "maxPrice");
                have_price = parsed.tick > 0.0;
            } else if (type == "LOT_SIZE") {
                parsed.step = filter_number(filter, "stepSize");
                parsed.min_qty = filter_number(filter, "minQty");
                parsed.max_qty = filter_number(filter, "maxQty");
                have_lot = parsed.step > 0.0;
            } else if (type == "MIN_NOTIONAL") {
                if (filter.find("notional"))
                    parsed.min_notional = filter_number(filter, "notional");
                else
                    parsed.min_notional = filter_number(filter, "minNotional");
                have_notional = parsed.min_notional >= 0.0;
            }
        }
        if (!have_price || !have_lot || !have_notional) {
            error = "PRICE_FILTER, LOT_SIZE, or MIN_NOTIONAL missing";
            return false;
        }
        out = parsed;
        error.clear();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

bool parse_one_way_position(const std::string& body,
                            const std::string& symbol,
                            Inventory& inventory,
                            std::string& error) {
    try {
        const Json json = parse_json(body);
        if (json.type != Json::Type::Array) {
            error = "positionRisk is not an array";
            return false;
        }
        const Json* both = nullptr;
        for (const auto& row : json.a) {
            const Json* name = row.find("symbol");
            if (!name || name->str() != symbol)
                continue;
            const Json* side = row.find("positionSide");
            const std::string side_name = side ? side->str() : "BOTH";
            if (side_name == "LONG" || side_name == "SHORT") {
                error = "hedge mode position is not supported";
                return false;
            }
            if (side_name == "BOTH")
                both = &row;
        }
        if (!both) {
            error = "no one-way position for " + symbol;
            return false;
        }
        inventory.net_qty = both->at("positionAmt").num();
        inventory.avg_entry = both->at("entryPrice").num();
        if (std::fabs(inventory.net_qty) < 1e-12)
            inventory.avg_entry = 0.0;
        error.clear();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

bool apply_order_response(int http_status,
                          const std::string& body,
                          const PlannedOrder& sent,
                          std::vector<RestingOrder>& resting,
                          std::string& note) {
    ApiError api_error;
    if (http_status != 200) {
        if (parse_api_error(http_status, body, api_error))
            note = api_error.message;
        else
            note = "HTTP " + std::to_string(http_status);
        return false;
    }
    try {
        const Json json = parse_json(body);
        if (json.find("code") && json.find("msg")) {
            note = json.at("msg").str();
            return false;
        }
        const std::string status = json.at("status").str();
        const std::string client_id =
            json.find("clientOrderId") ? json.at("clientOrderId").str() : sent.client_id;
        if (status == "CANCELED" || status == "FILLED" || status == "EXPIRED" || status == "REJECTED") {
            erase_client(resting, client_id);
            note = status;
            return true;
        }
        if (status != "NEW" && status != "PARTIALLY_FILLED") {
            note = "unhandled order status " + status;
            return false;
        }
        RestingOrder row;
        row.client_id = client_id;
        row.side = json.find("side") ? json.at("side").str() : sent.side;
        row.price = json.find("price") ? json.at("price").num() : sent.price;
        const double orig = json.find("origQty") ? json.at("origQty").num() : sent.qty;
        const double executed = json.find("executedQty") ? json.at("executedQty").num() : 0.0;
        row.qty = std::max(0.0, orig - executed);
        row.reduce_only = sent.reduce_only;
        if (row.qty <= 1e-12) {
            erase_client(resting, client_id);
            note = status;
            return true;
        }
        erase_client(resting, client_id);
        resting.push_back(row);
        note = status;
        return true;
    } catch (const std::exception& ex) {
        note = ex.what();
        return false;
    }
}

}  // namespace mm
