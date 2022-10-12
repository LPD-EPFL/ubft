// Copyright (c) 2017 Object Computing, Inc.
// All rights reserved.
// See the file license.txt for licensing information.
#include "Market.h"

#include <functional>
#include <cctype>
#include <locale>
#include <stdexcept>

namespace orderentry
{
uint32_t Market::orderIdSeed_ = 0;

Market::Market(std::ostream * out)
: logFile_(out)
{
}

Market::~Market()
{
}

NamedOrderBook Market::createBook(std::string const &symbol) {
    if (symbolIsDefined(symbol)) {
        throw std::runtime_error("Symbol already exists, cannot create book!");
    }

    return std::make_pair(this->addBook(symbol), std::string(symbol));
}

bool Market::placeOrder(NamedOrderBook &namedOrderBook, std::queue<FilledOrder> &notifier, uint64_t req_id, bool buy_otherwise_sell, liquibook::book::Quantity qty, liquibook::book::Price price) {
    std::string &symbol = namedOrderBook.second;
    OrderBookPtr &orderBook = namedOrderBook.first;

    // Fixed config
    liquibook::book::Price stopPrice = 0;
    bool aon = false;
    bool ioc = false;
    std::string orderId = std::to_string(++orderIdSeed_);

    // Config consts
    const liquibook::book::OrderConditions AON(liquibook::book::oc_all_or_none);
    const liquibook::book::OrderConditions IOC(liquibook::book::oc_immediate_or_cancel);
    const liquibook::book::OrderConditions NOC(liquibook::book::oc_no_conditions);

    OrderPtr order = std::make_shared<Order>(orderId, notifier, req_id, buy_otherwise_sell, qty, symbol, price, stopPrice, aon, ioc);

    const liquibook::book::OrderConditions conditions =
        (aon ? AON : NOC) | (ioc ? IOC : NOC);

    order->onSubmitted();
    // out() << "ADDING order:  " << *order << std::endl;

    return orderBook->add(order, conditions);
}

/////////////////////////////
// Order book interactions

bool
Market::symbolIsDefined(const std::string & symbol)
{
    auto book = books_.find(symbol);
    return book != books_.end();
}

OrderBookPtr
Market::addBook(const std::string & symbol)
{
    OrderBookPtr result;
    out() << "Create new order book for " << symbol << std::endl;
    result = std::make_shared<OrderBook>(symbol);
    result->set_order_listener(this);
    // result->set_trade_listener(this);
    // result->set_order_book_listener(this);
    books_[symbol] = result;
    return result;
}

OrderBookPtr
Market::findBook(const std::string & symbol)
{
    OrderBookPtr result;
    auto entry = books_.find(symbol);
    if(entry != books_.end())
    {
        result = entry->second;
    }
    return result;
}

/////////////////////////////////////
// Implement OrderListener interface

void
Market::on_accept(const OrderPtr& order)
{
    order->onAccepted();
    // out() << "\tAccepted: " <<*order<< std::endl;
}

void
Market::on_reject(const OrderPtr& order, const char* reason)
{
    // This is a terminal state
    order->onRejected(reason);
    out() << "\tRejected: " <<*order<< ' ' << reason << std::endl;

}

void
Market::on_fill(const OrderPtr& order,
    const OrderPtr& matched_order,
    liquibook::book::Quantity fill_qty,
    liquibook::book::Cost fill_cost)
{
    // This is a terminal state
    order->onFilled(fill_qty, fill_cost);
    matched_order->onFilled(fill_qty, fill_cost);

    order->notifier().emplace(order, fill_qty, fill_cost);
    matched_order->notifier().emplace(matched_order, fill_qty, fill_cost);

//     out() << (order->is_buy() ? "\tBought: " : "\tSold: ")
//         << fill_qty << " Shares for " << fill_cost << ' ' <<*order<< std::endl;
//     out() << (matched_order->is_buy() ? "\tBought: " : "\tSold: ")
//         << fill_qty << " Shares for " << fill_cost << ' ' << *matched_order << std::endl;
// }
}

void
Market::on_cancel(const OrderPtr& order)
{
    // This is a terminal state
    order->onCancelled();
    out() << "\tCanceled: " << *order<< std::endl;
}

void Market::on_cancel_reject(const OrderPtr& order, const char* reason)
{
    order->onCancelRejected(reason);
    out() << "\tCancel Reject: " <<*order<< ' ' << reason << std::endl;
}

void Market::on_replace(const OrderPtr& order,
    const int32_t& size_delta,
    liquibook::book::Price new_price)
{
    order->onReplaced(size_delta, new_price);
    out() << "\tModify " ;
    if(size_delta != liquibook::book::SIZE_UNCHANGED)
    {
        out() << " QUANTITY  += " << size_delta;
    }
    if(new_price != liquibook::book::PRICE_UNCHANGED)
    {
        out() << " PRICE " << new_price;
    }
    out() <<*order<< std::endl;
}

void
Market::on_replace_reject(const OrderPtr& order, const char* reason)
{
    order->onReplaceRejected(reason);
    out() << "\tReplace Reject: " <<*order<< ' ' << reason << std::endl;
}

////////////////////////////////////
// Implement TradeListener interface

void
Market::on_trade(const OrderBook* book,
    liquibook::book::Quantity qty,
    liquibook::book::Cost cost)
{
    out() << "\tTrade: " << qty <<  ' ' << book->symbol() << " Cost "  << cost  << std::endl;
}

/////////////////////////////////////////
// Implement OrderBookListener interface

void
Market::on_order_book_change(const OrderBook* book)
{
    out() << "\tBook Change: " << ' ' << book->symbol() << std::endl;
}

}  // namespace orderentry
