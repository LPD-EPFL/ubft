#pragma once

#include <cstdlib>
#include <chrono>
#include <algorithm>

#include <dory/rpc/basic-client.hpp>
#include <dory/shared/types.hpp>

#include "app.hpp"
#include "../kvstores.hpp"

class Redis : public Application {
public:
    Redis(bool server, std::string const &config_string) {
        parse_config(config_string);

        if (server) {
            int redis_port = 9998; // TODO: + local_id;
            kvstores::redis::spawn_redis(redis_port);
            std::this_thread::sleep_for(std::chrono::seconds(2));

            redis_rpc.emplace("127.0.0.1", redis_port);
            if (!redis_rpc->connect()) {
                throw std::runtime_error("Failed to connect to the local redis instance");
            }
    
            // Warm up the server
            prepare_requests();
            for (auto const &r : prepared_requests) {
                redis_rpc->send(r.data(), r.size());
                redis_rpc->recv();
            }
        } else {
            prepare_requests();
        }
    }

    size_t maxRequestSize() const {
        auto const max_put_size = kvstores::redis::put_max_buffer_size(key_size, value_size);
        auto const max_get_size = kvstores::redis::get_max_buffer_size(key_size);
        return std::max(max_put_size, max_get_size);
    }

    size_t maxResponseSize() const {
        // TODO: This is not quite accurate but it works
        auto const max_put_size = kvstores::redis::put_max_buffer_size(key_size, value_size);
        auto const max_get_size = kvstores::redis::get_max_buffer_size(key_size);
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

        redis_rpc->send(request, request_size);
        auto received = redis_rpc->recv();

        if (received.size() > 0) {
            response.resize(received.size());
            std::copy(received.begin(), received.end(), response.begin());
            // std::cout << "Response: " << kvstores::buff_repr(response.begin(), response.end()) << std::endl;
        } else {
            throw std::runtime_error("Local redis instance failed to reply");
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
            req.resize(kvstores::redis::get_buffer_size(key_size));
            kvstores::redis::get(req.data(), keys[circular_index % unique_keys]);
            prepared_requests.push_back(req);
            circular_index ++;
        }

        for (size_t i = 0; i < prepared_requests_cnt; i++) {
            std::vector<uint8_t> req;
            req.resize(kvstores::redis::put_buffer_size(key_size, value_size));
            kvstores::redis::put(req.data(), keys[circular_index % unique_keys], value_size);
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

    dory::Delayed<dory::rpc::RpcBasicClient> redis_rpc;

    std::vector<std::vector<uint8_t>> prepared_requests;
};