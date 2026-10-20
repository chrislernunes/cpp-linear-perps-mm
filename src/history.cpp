#include "mm/history.hpp"

#include "mm/net.hpp"
#include "mm/zip_archive.hpp"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace mm {
namespace {

void ensure_dir(const std::string& path) {
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0755);
#endif
}

std::string json_escape(const std::string& text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out.push_back(c);
        }
    }
    return out;
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    for (char c : line) {
        if (c == ',') {
            fields.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    fields.push_back(cur);
    return fields;
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty())
                lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        lines.push_back(cur);
    return lines;
}

void write_file(const std::string& path, const std::string& body) {
    std::ofstream output(path, std::ios::binary);
    if (!output)
        throw std::runtime_error("unable to write " + path);
    output << body;
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("unable to read " + path);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

bool file_exists(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return static_cast<bool>(input);
}

}  // namespace

std::int64_t parse_utc_millis(const std::string& text) {
    if (text.find('-') == std::string::npos && text.find(':') == std::string::npos) {
        std::size_t used = 0;
        const auto value = std::stoll(text, &used);
        if (used != text.size())
            throw std::runtime_error("timestamp is not an integer: " + text);
        return value;
    }
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
        throw std::runtime_error("unrecognized timestamp: " + text);
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
#ifdef _WIN32
    const auto seconds = _mkgmtime(&tm);
#else
    const auto seconds = timegm(&tm);
#endif
    if (seconds < 0)
        throw std::runtime_error("invalid calendar timestamp: " + text);
    return static_cast<std::int64_t>(seconds) * 1000;
}

std::vector<TradePrint> parse_agg_trades_csv(const std::string& text, FileProvenance& provenance) {
    const auto lines = lines_of(text);
    if (lines.empty())
        throw std::runtime_error("aggTrades CSV is empty");
    const auto header = split_csv(lines[0]);
    const std::vector<std::string> expect = {"agg_trade_id", "price", "quantity", "first_trade_id",
                                             "last_trade_id", "transact_time", "is_buyer_maker"};
    if (header != expect)
        throw std::runtime_error("aggTrades header does not match the published Binance schema");
    std::vector<TradePrint> trades;
    trades.reserve(lines.size() - 1);
    std::int64_t previous_ts = 0;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto fields = split_csv(lines[i]);
        if (fields.size() != expect.size())
            throw std::runtime_error("aggTrades column count mismatch at row " + std::to_string(i + 1));
        TradePrint trade;
        trade.id = std::stoll(fields[0]);
        trade.price = std::stod(fields[1]);
        trade.qty = std::stod(fields[2]);
        trade.ts_ms = std::stoll(fields[5]);
        if (!(trade.price > 0.0) || trade.qty < 0.0)
            throw std::runtime_error("aggTrades price/qty invalid at row " + std::to_string(i + 1));
        if (i > 1 && trade.ts_ms < previous_ts)
            throw std::runtime_error("aggTrades timestamps go backwards at row " + std::to_string(i + 1));
        if (fields[6] == "true")
            trade.buyer_is_maker = true;
        else if (fields[6] == "false")
            trade.buyer_is_maker = false;
        else
            throw std::runtime_error("aggTrades is_buyer_maker is not true/false");
        previous_ts = trade.ts_ms;
        trades.push_back(trade);
    }
    provenance.rows = trades.size();
    if (!trades.empty()) {
        provenance.first_ts_ms = trades.front().ts_ms;
        provenance.last_ts_ms = trades.back().ts_ms;
    }
    return trades;
}

std::vector<DepthSnapshot> parse_book_depth_csv(const std::string& text, FileProvenance& provenance) {
    const auto lines = lines_of(text);
    if (lines.empty())
        throw std::runtime_error("bookDepth CSV is empty");
    const auto header = split_csv(lines[0]);
    const std::vector<std::string> expect = {"timestamp", "percentage", "depth", "notional"};
    if (header != expect)
        throw std::runtime_error("bookDepth header does not match the published Binance schema");
    struct Row {
        double pct;
        double depth;
        double notional;
    };
    std::map<std::int64_t, std::vector<Row>> grouped;
    std::int64_t previous = 0;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto fields = split_csv(lines[i]);
        if (fields.size() != 4)
            throw std::runtime_error("bookDepth column count mismatch");
        const auto ts = parse_utc_millis(fields[0]);
        if (i > 1 && ts < previous)
            throw std::runtime_error("bookDepth timestamps go backwards");
        previous = ts;
        Row row;
        row.pct = std::stod(fields[1]);
        row.depth = std::stod(fields[2]);
        row.notional = std::stod(fields[3]);
        if (!(row.depth > 0.0) || !(row.notional > 0.0))
            throw std::runtime_error("bookDepth depth/notional must be positive");
        grouped[ts].push_back(row);
    }
    std::vector<DepthSnapshot> snaps;
    snaps.reserve(grouped.size());
    for (const auto& item : grouped) {
        DepthSnapshot snap;
        snap.ts_ms = item.first;
        double best_bid_pct = -1e9;
        double best_ask_pct = 1e9;
        bool have_bid = false;
        bool have_ask = false;
        for (const auto& row : item.second) {
            if (row.pct < 0.0 && row.pct > best_bid_pct) {
                best_bid_pct = row.pct;
                snap.tight_bid = row.notional / row.depth;
                snap.tight_bid_qty = row.depth;
                have_bid = true;
            }
            if (row.pct > 0.0 && row.pct < best_ask_pct) {
                best_ask_pct = row.pct;
                snap.tight_ask = row.notional / row.depth;
                snap.tight_ask_qty = row.depth;
                have_ask = true;
            }
        }
        if (!have_bid || !have_ask) {
            snap.usable = false;
            snap.reject_reason = "missing tight bid or ask bucket";
        } else if (!(snap.tight_ask > snap.tight_bid)) {
            snap.usable = false;
            snap.reject_reason = "tight buckets are crossed or empty";
        } else {
            const double mid = (snap.tight_bid + snap.tight_ask) / 2.0;
            const double width = (snap.tight_ask - snap.tight_bid) / mid;
            snap.tight_pct = std::max(std::fabs(best_bid_pct), std::fabs(best_ask_pct));
            // Percentage buckets are not a BBO. Reject only pathological width.
            if (width > 0.02) {
                snap.usable = false;
                snap.reject_reason = "tightest buckets are more than 2% apart";
            } else {
                snap.usable = true;
            }
        }
        snaps.push_back(snap);
    }
    provenance.rows = lines.size() > 0 ? lines.size() - 1 : 0;
    if (!snaps.empty()) {
        provenance.first_ts_ms = snaps.front().ts_ms;
        provenance.last_ts_ms = snaps.back().ts_ms;
    }
    return snaps;
}

std::vector<FundingPrint> parse_funding_csv(const std::string& text, FileProvenance& provenance) {
    const auto lines = lines_of(text);
    if (lines.empty())
        throw std::runtime_error("fundingRate CSV is empty");
    const auto header = split_csv(lines[0]);
    const std::vector<std::string> expect = {"calc_time", "funding_interval_hours", "last_funding_rate"};
    if (header != expect)
        throw std::runtime_error("fundingRate header does not match the published Binance schema");
    std::vector<FundingPrint> rows;
    std::int64_t previous = 0;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto fields = split_csv(lines[i]);
        if (fields.size() != 3)
            throw std::runtime_error("fundingRate column count mismatch");
        FundingPrint row;
        row.ts_ms = std::stoll(fields[0]);
        row.interval_hours = std::stoi(fields[1]);
        row.rate = std::stod(fields[2]);
        if (!std::isfinite(row.rate) || row.interval_hours <= 0)
            throw std::runtime_error("fundingRate row is invalid");
        if (i > 1 && row.ts_ms < previous)
            throw std::runtime_error("fundingRate timestamps go backwards");
        previous = row.ts_ms;
        rows.push_back(row);
    }
    provenance.rows = rows.size();
    if (!rows.empty()) {
        provenance.first_ts_ms = rows.front().ts_ms;
        provenance.last_ts_ms = rows.back().ts_ms;
    }
    return rows;
}

namespace {

std::string vision_url(const std::string& path) {
    return "https://data.binance.vision/" + path;
}

FileProvenance fetch_zip(HttpClient& http,
                         const std::string& kind,
                         const std::string& path,
                         const std::string& directory,
                         bool required) {
    FileProvenance provenance;
    provenance.kind = kind;
    provenance.url = vision_url(path);
    provenance.checksum_url = provenance.url + ".CHECKSUM";
    const auto slash = path.find_last_of('/');
    const std::string filename = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::string zip_path = directory + "/" + filename;
    std::string csv_name = filename.size() >= 4 ? filename.substr(0, filename.size() - 4) : filename;
    if (csv_name.size() < 4 || csv_name.substr(csv_name.size() - 4) != ".csv")
        csv_name += ".csv";
    provenance.csv_path = directory + "/" + csv_name;
    const HttpResponse checksum = http.get(provenance.checksum_url, 20000);
    if (!checksum.completed) {
        provenance.note = "checksum request failed: " + checksum.error;
        provenance.http_status = 0;
        if (required)
            throw std::runtime_error(provenance.note + " url " + provenance.checksum_url);
        return provenance;
    }
    if (checksum.status == 200) {
        std::istringstream in(checksum.body);
        in >> provenance.publisher_sha256;
        if (file_exists(zip_path)) {
            provenance.sha256 = sha256_hex_file(zip_path);
            if (lower_ascii(provenance.publisher_sha256) == provenance.sha256) {
                provenance.present = true;
                provenance.checksum_matched = true;
                provenance.http_status = 200;
                provenance.note = "reused local zip; publisher checksum matched";
            }
        }
    }
    if (!provenance.present) {
        const HttpResponse body = http.get(provenance.url, 120000);
        provenance.http_status = body.status;
        if (!body.completed) {
            provenance.note = "download failed: " + body.error;
            if (required)
                throw std::runtime_error(provenance.note);
            return provenance;
        }
        if (body.status == 404) {
            provenance.note = "HTTP 404 not published";
            if (required)
                throw std::runtime_error(kind + " not published at " + provenance.url);
            return provenance;
        }
        if (body.status != 200) {
            provenance.note = "HTTP " + std::to_string(body.status);
            if (required)
                throw std::runtime_error(kind + " download " + provenance.note);
            return provenance;
        }
        write_file(zip_path, body.body);
        provenance.sha256 = sha256_hex_file(zip_path);
        provenance.present = true;
        if (checksum.status != 200 || provenance.publisher_sha256.empty()) {
            provenance.note = "publisher checksum unavailable HTTP " + std::to_string(checksum.status);
            throw std::runtime_error(provenance.note);
        }
        provenance.checksum_matched = lower_ascii(provenance.publisher_sha256) == provenance.sha256;
        if (!provenance.checksum_matched)
            throw std::runtime_error(kind + " SHA256 does not match publisher checksum");
        provenance.note = "downloaded; publisher checksum matched";
    }
    if (!file_exists(provenance.csv_path)) {
        const auto files = unzip_file(zip_path);
        if (files.size() != 1)
            throw std::runtime_error(kind + " zip does not contain exactly one file");
        write_file(provenance.csv_path, std::string(files[0].bytes.begin(), files[0].bytes.end()));
    }
    return provenance;
}

}  // namespace

HistoricalDay load_historical_day(const std::string& symbol, const std::string& date, const std::string& data_dir) {
    if (symbol.empty() || symbol.find('/') != std::string::npos)
        throw std::runtime_error("symbol is empty or contains a path separator");
    if (date.size() != 10 || date[4] != '-' || date[7] != '-')
        throw std::runtime_error("date must be YYYY-MM-DD");
    ensure_dir(data_dir);
    const std::string month = date.substr(0, 7);
    HttpClient http;
    HistoricalDay day;
    day.symbol = symbol;
    day.date = date;
    FileProvenance trades = fetch_zip(
        http, "aggTrades",
        "data/futures/um/daily/aggTrades/" + symbol + "/" + symbol + "-aggTrades-" + date + ".zip",
        data_dir, true);
    FileProvenance depth = fetch_zip(
        http, "bookDepth",
        "data/futures/um/daily/bookDepth/" + symbol + "/" + symbol + "-bookDepth-" + date + ".zip",
        data_dir, true);
    FileProvenance funding = fetch_zip(
        http, "fundingRate",
        "data/futures/um/monthly/fundingRate/" + symbol + "/" + symbol + "-fundingRate-" + month + ".zip",
        data_dir, false);
    day.trades = parse_agg_trades_csv(read_file(trades.csv_path), trades);
    day.depth = parse_book_depth_csv(read_file(depth.csv_path), depth);
    if (funding.present) {
        day.funding = parse_funding_csv(read_file(funding.csv_path), funding);
        day.funding_available = true;
    }
    day.files = {trades, depth, funding};
    day.manifest_path = data_dir + "/" + symbol + "-" + date + "-manifest.json";
    std::ostringstream manifest;
    manifest << "{\n  \"symbol\": \"" << json_escape(symbol) << "\",\n  \"date\": \"" << date
             << "\",\n  \"source\": \"data.binance.vision USD-M futures\",\n  \"files\": [\n";
    for (std::size_t i = 0; i < day.files.size(); ++i) {
        const auto& file = day.files[i];
        if (i)
            manifest << ",\n";
        manifest << "    {\"kind\": \"" << file.kind << "\", \"url\": \"" << json_escape(file.url)
                 << "\", \"sha256\": \"" << file.sha256 << "\", \"publisher_sha256\": \""
                 << file.publisher_sha256 << "\", \"checksum_matched\": "
                 << (file.checksum_matched ? "true" : "false") << ", \"present\": "
                 << (file.present ? "true" : "false") << ", \"http_status\": " << file.http_status
                 << ", \"rows\": " << file.rows << ", \"first_ts_ms\": " << file.first_ts_ms
                 << ", \"last_ts_ms\": " << file.last_ts_ms << ", \"csv\": \""
                 << json_escape(file.csv_path) << "\", \"note\": \"" << json_escape(file.note) << "\"}";
    }
    manifest << "\n  ]\n}\n";
    write_file(day.manifest_path, manifest.str());
    return day;
}

}  // namespace mm
