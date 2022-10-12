// Copyright (c) 2017 Object Computing, Inc.
// All rights reserved.
// See the file license.txt for licensing information.
#pragma once

#include "../book/depth_order_book.h"

#include "Order.h"

#include <string>
#include <vector>
#include <iostream>
#include <algorithm>
#include <map>
#include <memory>
#include <queue>

namespace orderentry
{
typedef liquibook::book::OrderBook<OrderPtr> OrderBook;
typedef std::shared_ptr<OrderBook> OrderBookPtr;
typedef std::pair<OrderBookPtr, std::string> NamedOrderBook;

class Market
    : public liquibook::book::OrderListener<OrderPtr>
    , public liquibook::book::TradeListener<OrderBook>
    , public liquibook::book::OrderBookListener<OrderBook>
{
    typedef std::map<std::string, OrderBookPtr> SymbolToBookMap;
public:
    Market(std::ostream * logFile = &std::cout);
    ~Market();

    NamedOrderBook createBook(std::string const &symbol);
    bool placeOrder(NamedOrderBook &namedOrderBook, std::queue<FilledOrder> &notifier, uint64_t req_id, bool buy_otherwise_sell, liquibook::book::Quantity qty, liquibook::book::Price price);

    void demo();

public:
    /////////////////////////////////////
    // Implement OrderListener interface

    /// @brief callback for an order accept
    virtual void on_accept(const OrderPtr& order);

    /// @brief callback for an order reject
    virtual void on_reject(const OrderPtr& order, const char* reason);

    /// @brief callback for an order fill
    /// @param order the inbound order
    /// @param matched_order the matched order
    /// @param fill_qty the quantity of this fill
    /// @param fill_cost the cost of this fill (qty * price)
    virtual void on_fill(const OrderPtr& order,
        const OrderPtr& matched_order,
        liquibook::book::Quantity fill_qty,
        liquibook::book::Cost fill_cost);

    /// @brief callback for an order cancellation
    virtual void on_cancel(const OrderPtr& order);

    /// @brief callback for an order cancel rejection
    virtual void on_cancel_reject(const OrderPtr& order, const char* reason);

    /// @brief callback for an order replace
    /// @param order the replaced order
    /// @param size_delta the change to order quantity
    /// @param new_price the updated order price
    virtual void on_replace(const OrderPtr& order,
        const int32_t& size_delta,
        liquibook::book::Price new_price);

    /// @brief callback for an order replace rejection
    virtual void on_replace_reject(const OrderPtr& order, const char* reason);

    ////////////////////////////////////
    // Implement TradeListener interface

    /// @brief callback for a trade
    /// @param book the order book of the fill (not defined whether this is before
    ///      or after fill)
    /// @param qty the quantity of this fill
    /// @param cost the cost of this fill (qty * price)
    virtual void on_trade(const OrderBook* book,
        liquibook::book::Quantity qty,
        liquibook::book::Cost cost);

    /////////////////////////////////////////
    // Implement OrderBookListener interface

    /// @brief callback for change anywhere in order book
    virtual void on_order_book_change(const OrderBook* book);

private:
    ////////////////////////
    // Order book interactions
    bool symbolIsDefined(const std::string & symbol);
    OrderBookPtr findBook(const std::string & symbol);
    OrderBookPtr addBook(const std::string & symbol);

    std::ostream & out()
    {
        return *logFile_;
    }
private:
    static uint32_t orderIdSeed_;

    std::ostream * logFile_;

    SymbolToBookMap books_;

};

} // namespace orderentry