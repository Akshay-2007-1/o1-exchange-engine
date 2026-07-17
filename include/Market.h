#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include "IOrderBook.h"
#include "OrderBook.h"
#include "OrderBookLegacy.h"

struct Company {
    uint16_t id;
    std::string symbol;
    std::string name;
    uint64_t total_shares;
};

enum class EngineMode { CURRENT, LEGACY };

inline std::unique_ptr<IOrderBook> make_order_book(EngineMode mode) {
    if (mode == EngineMode::LEGACY)
        return std::make_unique<OrderBookLegacy>();
    return std::make_unique<OrderBook>();
}

struct InstrumentState {
    explicit InstrumentState(Company company_info, EngineMode mode = EngineMode::CURRENT)
        : company(std::move(company_info)), book(make_order_book(mode)) {}

    Company company;
    std::unique_ptr<IOrderBook> book;
    // Mutex removed: The dedicated matching engine thread now has exclusive ownership
};

class MarketState {
public:
    explicit MarketState(std::vector<Company> companies) {
        companies_ = companies;

        uint16_t max_id = 0;
        for (const auto& company : companies_) {
            max_id = std::max(max_id, company.id);
        }

        // Direct-mapped array lookup based on company ID
        instruments_.resize(max_id + 1);

        for (const auto& company : companies_) {
            instruments_[company.id] = std::make_unique<InstrumentState>(company, engine_mode_);
        }

        if (companies_.empty()) {
            throw std::invalid_argument("MarketState requires at least one company");
        }
    }

    InstrumentState* find_instrument(uint16_t company_id) {
        if (company_id < 0 || company_id >= instruments_.size()) return nullptr;
        return instruments_[company_id].get();
    }

    const InstrumentState* find_instrument(uint16_t company_id) const {
        if (company_id < 0 || company_id >= instruments_.size()) return nullptr;
        return instruments_[company_id].get();
    }

    const std::vector<Company>& companies() const {
        return companies_;
    }

    uint16_t default_company_id() const {
        return companies_.front().id;
    }

    EngineMode engine_mode() const { return engine_mode_; }

    // Re-initializes every instrument with a fresh, empty book of the given
    // engine. All resting orders across the market are discarded - this is a
    // stress-test/benchmark control, not something used during normal trading.
    // Caller must only invoke this from the single-writer engine thread with
    // its task queue drained, since it replaces IOrderBook instances other
    // threads may be reading via find_instrument().
    void set_engine_mode(EngineMode mode) {
        if (mode == engine_mode_) return;
        engine_mode_ = mode;
        for (auto& inst : instruments_) {
            if (inst) inst->book = make_order_book(mode);
        }
    }

private:
    std::vector<Company> companies_;
    std::vector<std::unique_ptr<InstrumentState>> instruments_;
    EngineMode engine_mode_ = EngineMode::CURRENT;
};