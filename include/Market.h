#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "OrderBook.h"

struct Company {
    int id;
    std::string symbol;
    std::string name;
    uint64_t total_shares;
};

struct InstrumentState {
    explicit InstrumentState(Company company_info)
        : company(std::move(company_info)) {}

    Company company;
    OrderBook book;
    mutable std::mutex mutex;
};

class MarketState {
public:
    explicit MarketState(std::vector<Company> companies) {
        companies_.reserve(companies.size());
        instruments_.reserve(companies.size());

        for (const auto& company : companies) {
            companies_.push_back(company);
            instruments_.emplace(company.id, std::make_unique<InstrumentState>(company));
        }

        if (companies_.empty()) {
            throw std::invalid_argument("MarketState requires at least one company");
        }
    }

    InstrumentState* find_instrument(int company_id) {
        auto it = instruments_.find(company_id);
        return it == instruments_.end() ? nullptr : it->second.get();
    }

    const InstrumentState* find_instrument(int company_id) const {
        auto it = instruments_.find(company_id);
        return it == instruments_.end() ? nullptr : it->second.get();
    }

    const std::vector<Company>& companies() const {
        return companies_;
    }

    int default_company_id() const {
        return companies_.front().id;
    }

private:
    std::vector<Company> companies_;
    std::unordered_map<int, std::unique_ptr<InstrumentState>> instruments_;
};
