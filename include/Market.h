#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>

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
    OrderBook book;
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
            instruments_[company.id] = std::make_unique<InstrumentState>(company);
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

private:
    std::vector<Company> companies_;
    std::vector<std::unique_ptr<InstrumentState>> instruments_;
};