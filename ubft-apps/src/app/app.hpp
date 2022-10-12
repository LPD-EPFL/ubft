#pragma once

#include <vector>
#include <random>
#include <iterator>

#include <dory/ubft/rpc/server.hpp>

class Application {
public:
    virtual size_t maxRequestSize() const = 0;
    virtual size_t maxResponseSize() const = 0;
    virtual std::vector<uint8_t> const& randomRequest() const = 0;
    virtual void execute(uint8_t const *const request, size_t request_size, std::vector<uint8_t> &response) = 0;
};

template<typename Iter, typename RandomGenerator>
Iter select_randomly(Iter start, Iter end, RandomGenerator& g) {
    std::uniform_int_distribution<> dis(0, std::distance(start, end) - 1);
    std::advance(start, dis(g));
    return start;
}

template<typename Iter>
Iter select_randomly(Iter start, Iter end) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    return select_randomly(start, end, gen);
}