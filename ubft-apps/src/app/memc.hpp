#pragma once

#include <cstdlib>
#include <chrono>
#include <algorithm>

#include <dory/rpc/basic-client.hpp>
#include <dory/shared/types.hpp>

#include "app.hpp"
#include "../kvstores.hpp"

class Memc : public Application {
public:
    Memc(bool server, std::string const &config_string) {
        parse_config(config_string);

        if (server) {
            int memc_port = 9998; // TODO: + local_id;
            kvstores::memcached::spawn_memc(memc_port);
            std::this_thread::sleep_for(std::chrono::seconds(2));

            memc_rpc.emplace("127.0.0.1", memc_port);
            if (!memc_rpc->connect()) {
                throw std::runtime_error("Failed to connect to the local memc instance");
            }
    
            // Warm up the server
            prepare_requests();
            for (auto const &r : prepared_requests) {
                memc_rpc->send(r.data(), r.size());
                memc_rpc->recv();
            }
        } else {
            prepare_requests();
        }
    }

    size_t maxRequestSize() const {
        auto const max_put_size = kvstores::memcached::put_max_buffer_size(key_size, value_size);
        auto const max_get_size = kvstores::memcached::get_max_buffer_size(key_size);
        return std::max(max_put_size, max_get_size);
    }

    size_t maxResponseSize() const {
        // TODO: This is not quite accurate but it works
        auto const max_put_size = kvstores::memcached::put_max_buffer_size(key_size, value_size);
        auto const max_get_size = kvstores::memcached::get_max_buffer_size(key_size);
        return std::max(max_put_size, max_get_size);
    }

    std::vector<uint8_t> const& randomRequest() const {
        if (rand() % 100 < get_percentage) {
            return prepared_requests[rand() % prepared_requests_cnt];
        }

        return prepared_requests[prepared_requests_cnt + rand() % prepared_requests_cnt];
    }

    void execute(uint8_t const *const request, size_t request_size, std::vector<uint8_t> &response) {
        // std::cout << "Request: " << kvstores::buff_repr(request, request + request_size) << std::endl;

        memc_rpc->send(request, request_size);
        auto received = memc_rpc->recv();

        if (received.size() > 0) {
            response.resize(received.size());
            std::copy(received.begin(), received.end(), response.begin());
            // std::cout << "Response: " << kvstores::buff_repr(response.begin(), response.end()) << std::endl;
        } else {
            throw std::runtime_error("Local memc instance failed to reply");
        }
    }

private:
    void parse_config(std::string const &config_string) {
        std::stringstream ss(config_string);

        std::vector<size_t> vec;
        for (size_t i; ss >> i;) {
            vec.push_back(i);    
            if (ss.peek() == ',') {
                ss.ignore();
            }
        }

        key_size = static_cast<int>(vec.at(0));
        value_size = static_cast<int>(vec.at(1));
        get_percentage = static_cast<int>(vec.at(2));
        get_success_percentage = static_cast<int>(vec.at(3));
        prepared_requests_cnt = vec.size() > 4 ? vec.at(4) : 1024;
    }

    void prepare_requests() {
        srand(1023);
        std::vector<std::vector<uint8_t>> keys;

        size_t unique_keys = prepared_requests_cnt + prepared_requests_cnt * (100 - get_success_percentage) / 100;
        keys.resize(unique_keys);

        for (size_t i = 0; i < unique_keys; i++) {
            keys[i].resize(key_size);
            kvstores::mkrndstr_ipa(key_size, keys[i].data());
        }

        size_t circular_index = 0;

        for (size_t i = 0; i < prepared_requests_cnt; i++) {
            std::vector<uint8_t> req;
            req.resize(kvstores::memcached::get_buffer_size(key_size));
            kvstores::memcached::get(req.data(), keys[circular_index % unique_keys]);
            prepared_requests.push_back(req);
            circular_index ++;
        }

        for (size_t i = 0; i < prepared_requests_cnt; i++) {
            std::vector<uint8_t> req;
            req.resize(kvstores::memcached::put_buffer_size(key_size, value_size));
            kvstores::memcached::put(req.data(), keys[circular_index % unique_keys], value_size);
            prepared_requests.push_back(req);
            circular_index ++;
        }

        // for (auto const &r : prepared_requests) {
        //     std::cout << kvstores::buff_repr(r.begin(), r.end()) << std::endl;
        // }
    }

    int key_size;
    int value_size;
    int get_percentage;
    int get_success_percentage;
    size_t prepared_requests_cnt;
    size_t get_end_index;

    dory::Delayed<dory::rpc::RpcBasicClient> memc_rpc;

    std::vector<std::vector<uint8_t>> prepared_requests;
};