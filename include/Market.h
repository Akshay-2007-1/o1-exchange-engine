#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include "OrderBook.h"
#include "OrderBookLegacy.h" // For legacy implementation
#include "IOrderBook.h"    // For the interface
#include "Metrics.h"     // For engine metrics

struct Company {
    uint16_t id;
    std::string symbol;
    std::string name;
    uint64_t total_shares;
};
struct InstrumentState {
    explicit InstrumentState(Company company_info)
        : company(std::move(company_info)) {}

    Company company;
    std::unique_ptr<IOrderBook> book;
};

enum EngineMode { CURRENT, LEGACY }; // Defined here to avoid circular dependency

class MarketState {
public:
    explicit MarketState(std::vector<Company> companies, EngineMode initial_mode = CURRENT)
        : companies_(std::move(companies)), current_engine_mode_(initial_mode) {
        
        uint16_t max_id = 0;
        for (const auto& company : companies_) {
            max_id = std::max(max_id, company.id);
        }

        instruments_.resize(max_id + 1);

        for (const auto& company : companies_) {
            instruments_[company.id] = create_instrument_state(company, current_engine_mode_);
        }

        if (companies_.empty()) {
            throw std::invalid_argument("MarketState requires at least one company");
        }
    }

    InstrumentState* find_instrument(uint16_t company_id) {
        if (company_id < instruments_.size() && instruments_[company_id]) { // Check bounds and if unique_ptr is valid
            return instruments_[company_id].get();
        }
        return nullptr;
    }

    const InstrumentState* find_instrument(uint16_t company_id) const {
        if (company_id < instruments_.size() && instruments_[company_id]) { // Check bounds and if unique_ptr is valid
            return instruments_[company_id].get();
        }
        return nullptr;
    }

    const std::vector<Company>& companies() const {
        return companies_;
    }

    uint16_t default_company_id() const {
        if (companies_.empty()) return 0; // Or throw an error, depending on desired behavior
        return companies_.front().id;
    }

    // New method to set engine mode and re-initialize order books
    void set_engine_mode(EngineMode mode) {
        if (current_engine_mode_ == mode) return; // No change needed
        current_engine_mode_ = mode;
        
        // Re-initialize all instrument order books with the new mode
        for (const auto& company : companies_) {
            if (instruments_[company.id]) { // Ensure instrument state exists
                instruments_[company.id] = create_instrument_state(company, current_engine_mode_);
            }
        }
        // Reset metrics for the newly active engine
        if (current_engine_mode_ == CURRENT) {
            current_engine_metrics_.reset();
        } else {
            legacy_engine_metrics_.reset();
        }
    }
    
    // Accessors for metrics
    const EngineMetrics& get_current_metrics() const { return current_engine_metrics_; }
    const EngineMetrics& get_legacy_metrics() const { return legacy_engine_metrics_; }
    
private:
    std::vector<Company> companies_;
    std::vector<std::unique_ptr<InstrumentState>> instruments_;
    EngineMode current_engine_mode_; // Track the current engine mode

    // Helper to create an InstrumentState with the correct OrderBook type
    std::unique_ptr<InstrumentState> create_instrument_state(Company company_info, EngineMode mode) {
        auto instrument_state = std::make_unique<InstrumentState>(std::move(company_info));
        if (mode == CURRENT) {
            instrument_state->book = std::make_unique<OrderBook>();
            instrument_state->book->set_metrics(&current_engine_metrics_);
        } else { // LEGACY
            instrument_state->book = std::make_unique<OrderBookLegacy>();
            instrument_state->book->set_metrics(&legacy_engine_metrics_);
        }
        return instrument_state;
    }

    EngineMetrics current_engine_metrics_;
    EngineMetrics legacy_engine_metrics_;
};