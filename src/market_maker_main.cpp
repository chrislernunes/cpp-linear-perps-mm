#include "mm/live.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

std::string flag(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name)
            return argv[i + 1];
    }
    return fallback;
}

bool has_flag(int argc, char** argv, const std::string& name) {
    for (int i = 1; i < argc; ++i)
        if (argv[i] == name)
            return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        mm::LiveOptions options;
        options.symbol = flag(argc, argv, "--symbol", "BTCUSDT");
        options.seconds = std::stoi(flag(argc, argv, "--seconds", "20"));
        options.check_only = has_flag(argc, argv, "--check");
        options.demo = has_flag(argc, argv, "--demo");
        options.quote.budget_usd = std::stod(flag(argc, argv, "--budget", "1000"));
        options.quote.min_spread_bps = std::stod(flag(argc, argv, "--min-spread-bps", "4"));
        options.quote.spread_factor = std::stod(flag(argc, argv, "--spread-factor", "1"));
        options.quote.max_net_qty = std::stod(flag(argc, argv, "--max-net-qty", "0.01"));
        options.quote.ladder_levels = std::stoi(flag(argc, argv, "--ladder", "1"));
        options.quote.fees.maker = std::stod(flag(argc, argv, "--maker-fee", "0.0002"));
        options.quote.fees.taker = std::stod(flag(argc, argv, "--taker-fee", "0.0005"));
        options.quote.filters.tick = 0.1;
        options.quote.filters.step = 0.001;
        options.quote.filters.min_qty = 0.001;
        options.quote.filters.min_notional = 50;
        if (options.demo && options.check_only) {
            std::cerr << "[mode] --check overrides --demo. No order request will be built.\n";
            options.demo = false;
        }
        std::cerr << "[mode] public market data on fapi.binance.com / fstream.binance.com\n";
        std::cerr << "[mode] demo orders " << (options.demo ? "ENABLED on demo-fapi.binance.com only" : "off")
                  << "\n";
        return mm::run_live(options);
    } catch (const std::exception& error) {
        std::cerr << "Startup failed: " << error.what() << "\n";
        return 1;
    }
}
