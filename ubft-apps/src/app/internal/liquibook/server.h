#pragma once

#include <queue>
#include <utility>

#include <dory/third-party/liquibook/market/Market.h>

#include "common.h"

class TraderContext {
public:
  TraderContext(orderentry::Market *market, orderentry::NamedOrderBook &namedBook) : market_(market), namedBook_(namedBook) {
  }

  bool placeOrder(uint64_t req_id, bool buy_otherwise_sell, liquibook::book::Quantity qty, liquibook::book::Price price) {
    return market_->placeOrder(namedBook_, notifier, req_id, buy_otherwise_sell, qty, price);
  }

  size_t previousResponsesNum() {
    return notifier.size();
  }

  int copyPreviousResponses(int num, ClientResponse *resp) {
    int i = 0;
    while (!notifier.empty() && num > 0) {
      auto &front = notifier.front();
      resp[i].req_id = front.order->req_id();
      resp[i].fill_qty = front.fill_qty;
      resp[i].fill_cost = front.fill_cost;

      notifier.pop();

      i += 1;
      num -= 1;
    }

    return i;
  }

  void deleteResponses() {
    std::queue<orderentry::FilledOrder> empty;
    std::swap(notifier, empty);
  }

private:
    orderentry::Market *market_;
    orderentry::NamedOrderBook &namedBook_;
    std::queue<orderentry::FilledOrder> notifier;

};