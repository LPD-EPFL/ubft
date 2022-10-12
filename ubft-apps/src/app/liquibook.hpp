#pragma once

#include <cstdlib>
#include <algorithm>

#include "app.hpp"
#include "internal/liquibook/server.h"

class Liquibook : public Application {
public:
    Liquibook(bool server, std::string const &config_string)
    {
        parse_config(config_string);
        if (server) {
            market = orderentry::Market(&std::cout);
            named_book = market.createBook("AAPL");

            for (size_t i = 0; i < max_traders_cnt; i++) {
                traders.emplace_back(&market, named_book);
            }
        }
    }

    size_t maxRequestSize() const {
        return sizeof(ClientRequest);
    }

    size_t maxResponseSize() const {
        return cli_resp_offset + max_num_cli_resp * sizeof(ClientResponse);
    }

    std::vector<uint8_t> const& randomRequest() const {
        auto &r = prepared_requests[rand() % prepared_requests.size()];
        auto *r_ptr = reinterpret_cast<ClientRequest*>(r.data());
        r_ptr->req_id = req_id++;
        return r;
    }

    void execute(uint8_t const *const request, size_t request_size, std::vector<uint8_t> &response) {
        auto *req = reinterpret_cast<ClientRequest const*>(request);
        auto &trader = traders.at(req->client_id);
        auto immediately_filled = trader.placeOrder(req->req_id, req->is_buy, req->price, req->qty);

        // Predict the size of the response
        size_t client_responses = 0;
        if (trader.previousResponsesNum() > 0) {
            client_responses = std::min(trader.previousResponsesNum(), max_num_cli_resp);
        }

        response.resize(cli_resp_offset + client_responses * sizeof(ClientResponse));

        auto *resp = reinterpret_cast<ReplicationResponse *>(response.data());
        resp->kind = ReplicationResponse::OK;
        resp->v.commit_ret = immediately_filled;
        resp->cli_resp.offset = cli_resp_offset;
        resp->cli_resp.num = 0;

        if (trader.previousResponsesNum() > 0) {
            auto *cli_resp = reinterpret_cast<ClientResponse *>(reinterpret_cast<uintptr_t>(resp) + cli_resp_offset);
            resp->cli_resp.num = trader.copyPreviousResponses(static_cast<int>(client_responses), cli_resp);
        }
    }

    void setClientId(int id) {
        client_id = id;
        prepare_requests();
    }

    template <class InputIt>
    static std::string resp_buff_repr(InputIt first, InputIt last) {
        auto *r = reinterpret_cast<ReplicationResponse const*>(&(*first));

        std::stringstream ss;
        ss << "["
            << "OK: " << (r->kind == ReplicationResponse::OK) << ", "
            << "CommitRet: " << r->v.commit_ret << ", "
            << "ClientResponse: " << r->cli_resp.num << ", ";
        
        ss << "[";
        auto *cli_resp = reinterpret_cast<ClientResponse *>(reinterpret_cast<uintptr_t>(r) + cli_resp_offset);
        for (int i = 0; i < r->cli_resp.num; i++, cli_resp++) {
            ss << "["
                << "ReqId: " << cli_resp->req_id << ", "
                << "Quantity: " << cli_resp->fill_qty << ", "
                << "Cost: " << cli_resp->fill_cost
                << "]";
        }
        ss << "]";
        ss << "]";
        
        return ss.str();
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

        buy_percentage = static_cast<int>(vec.at(0));
        prepared_requests_cnt = vec.size() > 1 ? vec.at(1) : 10 * 1024;
    }

    void prepare_requests() {
        for (size_t i = 0; i < prepared_requests_cnt; i++) {
            ClientRequest r;
            r.client_id = client_id;
            r.is_buy = rand() % 100 < buy_percentage;
            uint32_t delta = r.is_buy ? 1880 : 1884;
            r.price = (rand() % 10) + delta;
            r.qty = ((rand() % 10) + 1) * 100;

            uint8_t *r_raw = reinterpret_cast<uint8_t *>(&r);
            std::vector<uint8_t> r_bytes(sizeof(ClientRequest));
            std::copy(r_raw, r_raw + sizeof(ClientRequest), r_bytes.begin());

            prepared_requests.push_back(r_bytes);
        }
    }

    int client_id;
    mutable uint64_t req_id = 1;

    int buy_percentage;
    size_t prepared_requests_cnt;
    mutable std::vector<std::vector<uint8_t>> prepared_requests;

    orderentry::Market market;
    orderentry::NamedOrderBook named_book;
    std::vector<TraderContext> traders;

    static size_t constexpr max_traders_cnt = 1024;
};