#pragma once

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace mm {

inline double floor_to_step(double value, double step) {
    if (!(step > 0.0) || !std::isfinite(step) || !std::isfinite(value))
        throw std::runtime_error("floor_to_step requires a finite value and a positive step");
    const auto n = static_cast<long long>(std::floor(value / step + 1e-9));
    return static_cast<double>(n) * step;
}

inline double ceil_to_step(double value, double step) {
    if (!(step > 0.0) || !std::isfinite(step) || !std::isfinite(value))
        throw std::runtime_error("ceil_to_step requires a finite value and a positive step");
    const auto n = static_cast<long long>(std::ceil(value / step - 1e-9));
    return static_cast<double>(n) * step;
}

inline int step_decimals(double step) {
    int decimals = 0;
    double scaled = step;
    while (decimals < 12 && std::fabs(scaled - std::round(scaled)) > 1e-8) {
        scaled *= 10.0;
        ++decimals;
    }
    return decimals;
}

inline std::string format_step(double value, double step) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", step_decimals(step), value);
    return buffer;
}

inline bool same_step(double a, double b, double step) {
    return std::fabs(a - b) <= step * 0.5 + 1e-12;
}

}  // namespace mm
