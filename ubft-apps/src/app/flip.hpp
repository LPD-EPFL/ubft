#pragma once

#include <cstdlib>

#include "app.hpp"

class Flip : public Application {
public:
    Flip(bool server, std::string const &config_string)
    {
        parse_config(config_string);
        if (!server) {
            prepare_requests();
        }
    }

    size_t maxRequestSize() const {
        return max_request_size;
    }

    size_t maxResponseSize() const {
        return max_request_size;
    }

    std::vector<uint8_t> const& randomRequest() const {
        return prepared_requests[rand() % prepared_requests.size()];
    }

    void execute(uint8_t const *const request, size_t request_size, std::vector<uint8_t> &response) {
        response.resize(request_size);
        std::copy(request, request + request_size, response.rbegin());
    }

private:
    std::vector<uint8_t> random_string(size_t min_length, size_t max_length) {
        const std::string CHARACTERS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

        std::random_device random_device;
        std::mt19937 generator(random_device());
        std::uniform_int_distribution<> distribution(0, static_cast<int>(CHARACTERS.size() - 1));

        std::vector<uint8_t> random_string;

        size_t length = min_length + rand() % (max_length - min_length);
        for (size_t i = 0; i < length; ++i) {
            random_string.push_back(CHARACTERS[distribution(generator)]);
        }

        return random_string;
    }

    void parse_config(std::string const &config_string) {
        std::stringstream ss(config_string);

        std::vector<size_t> vec;
        for (size_t i; ss >> i;) {
            vec.push_back(i);    
            if (ss.peek() == ',') {
                ss.ignore();
            }
        }

        min_request_size = vec.at(0);
        max_request_size = vec.at(1);
        prepared_requests_cnt = vec.size() > 2 ? vec.at(2) : 10 * 1024;
    }

    void prepare_requests() {
        for (size_t i = 0; i < prepared_requests_cnt; i++) {
            prepared_requests.push_back(random_string(min_request_size, max_request_size));
        }
    }

    size_t min_request_size;
    size_t max_request_size;
    size_t prepared_requests_cnt;
    std::vector<std::vector<uint8_t>> prepared_requests;
};