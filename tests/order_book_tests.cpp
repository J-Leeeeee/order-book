#include "order_book/order_book.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <utility>
#include <vector>

using order_book::CommandResult;
using order_book::CommandStatus;
using order_book::Execution;
using order_book::OrderBook;
using order_book::OrderId;
using order_book::OrderType;
using order_book::Price;
using order_book::Quantity;
using order_book::RejectReason;
using order_book::Sequence;
using order_book::Side;

namespace {

int g_fails = 0;
int g_checks = 0;

struct RecordedFill {
    OrderId maker_id;
    OrderId taker_id;
    Price price;
    Quantity quantity;
    Sequence sequence;
};

void check(bool condition, const char* expression, const char* file, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "CHECK failed: " << expression << " (" << file << ":" << line << ")\n";
        ++g_fails;
    }
}

#define CHECK(condition) ::check(static_cast<bool>(condition), #condition, __FILE__, __LINE__)

std::vector<RecordedFill> copy_fills(const CommandResult& result) {
    std::vector<RecordedFill> out;
    out.reserve(result.executions.size());
    for (const Execution& fill : result.executions) {
        out.push_back(RecordedFill{fill.maker_id, fill.taker_id, fill.price, fill.quantity, fill.sequence});
    }
    return out;
}

Quantity filled_qty(const CommandResult& result) {
    Quantity total = 0;
    for (const Execution& fill : result.executions) {
        total += fill.quantity;
    }
    return total;
}

CommandResult submit_limit(OrderBook& book, OrderId id, Side side, Price price, Quantity quantity) {
    return book.submit(id, side, OrderType::Limit, price, quantity);
}

void test_validation() {
    OrderBook book;
    const std::uint64_t before = book.checksum();

    auto zero = submit_limit(book, 1, Side::Buy, 100, 0);
    CHECK(zero.status == CommandStatus::Rejected);
    CHECK(zero.reason == RejectReason::InvalidQuantity);
    CHECK(zero.executions.empty());
    CHECK(book.checksum() == before);

    auto bad_price = submit_limit(book, 1, Side::Buy, 0, 10);
    CHECK(bad_price.status == CommandStatus::Rejected);
    CHECK(bad_price.reason == RejectReason::InvalidPrice);
    CHECK(book.checksum() == before);

    auto negative = submit_limit(book, 1, Side::Sell, -5, 10);
    CHECK(negative.status == CommandStatus::Rejected);
    CHECK(negative.reason == RejectReason::InvalidPrice);
    CHECK(book.checksum() == before);

    auto bad_side = book.submit(1, static_cast<Side>(9), OrderType::Limit, 100, 10);
    CHECK(bad_side.status == CommandStatus::Rejected);
    CHECK(bad_side.reason == RejectReason::InvalidSide);
    CHECK(book.checksum() == before);

    auto bad_type = book.submit(1, Side::Buy, static_cast<OrderType>(9), 100, 10);
    CHECK(bad_type.status == CommandStatus::Rejected);
    CHECK(bad_type.reason == RejectReason::InvalidType);
    CHECK(book.checksum() == before);

    CHECK(submit_limit(book, 1, Side::Buy, 100, 10).status == CommandStatus::Accepted);
    auto duplicate = submit_limit(book, 1, Side::Sell, 110, 10);
    CHECK(duplicate.status == CommandStatus::Rejected);
    CHECK(duplicate.reason == RejectReason::DuplicateId);
    CHECK(book.order_count() == 1);

    auto unknown_cancel = book.cancel(99);
    CHECK(unknown_cancel.status == CommandStatus::Rejected);
    CHECK(unknown_cancel.reason == RejectReason::UnknownId);

    auto unknown_modify = book.modify(99, 100, 5);
    CHECK(unknown_modify.status == CommandStatus::Rejected);
    CHECK(unknown_modify.reason == RejectReason::UnknownId);

    auto zero_modify = book.modify(1, 100, 0);
    CHECK(zero_modify.status == CommandStatus::Rejected);
    CHECK(zero_modify.reason == RejectReason::InvalidQuantity);
    CHECK(book.active_order(1)->remaining == 10);

    auto bad_modify_price = book.modify(1, 0, 5);
    CHECK(bad_modify_price.status == CommandStatus::Rejected);
    CHECK(bad_modify_price.reason == RejectReason::InvalidPrice);
    CHECK(book.active_order(1)->remaining == 10);
    CHECK(book.invariants_hold());
}

void test_empty_queries() {
    OrderBook book;
    CHECK(!book.best_bid().has_value());
    CHECK(!book.best_ask().has_value());
    CHECK(!book.bid_level(0).has_value());
    CHECK(!book.ask_level(0).has_value());
    CHECK(!book.active_order(1).has_value());
    CHECK(book.order_count() == 0);
    CHECK(book.bid_level_count() == 0);
    CHECK(book.ask_level_count() == 0);
    CHECK(book.bid_quantity() == 0);
    CHECK(book.ask_quantity() == 0);
    CHECK(book.max_orders() == OrderBook::default_max_orders);
    CHECK(book.max_levels() == OrderBook::default_max_levels);
    CHECK(book.invariants_hold());
}

void test_insertion() {
    OrderBook book;
    CHECK(submit_limit(book, 1, Side::Buy, 100, 10).status == CommandStatus::Accepted);
    CHECK(submit_limit(book, 2, Side::Buy, 101, 4).status == CommandStatus::Accepted);
    CHECK(submit_limit(book, 3, Side::Sell, 105, 7).status == CommandStatus::Accepted);

    CHECK(book.best_bid() == 101);
    CHECK(book.best_ask() == 105);
    CHECK(book.bid_level(0)->quantity == 4);
    CHECK(book.bid_level(1)->price == 100);
    CHECK(book.bid_level(1)->quantity == 10);
    CHECK(book.ask_level(0)->quantity == 7);
    CHECK(!book.bid_level(2).has_value());
    CHECK(book.order_count() == 3);
    CHECK(book.bid_quantity() == 14);
    CHECK(book.ask_quantity() == 7);
    CHECK(book.active_order(1)->remaining == 10);
    CHECK(book.invariants_hold());
}

void test_cancellation() {
    OrderBook book;
    CHECK(submit_limit(book, 1, Side::Buy, 100, 10).status == CommandStatus::Accepted);
    CHECK(submit_limit(book, 2, Side::Buy, 100, 6).status == CommandStatus::Accepted);
    CHECK(book.cancel(1).status == CommandStatus::Accepted);
    CHECK(!book.active_order(1).has_value());
    CHECK(book.best_bid() == 100);
    CHECK(book.bid_level(0)->quantity == 6);
    CHECK(book.order_count() == 1);
    CHECK(book.cancel(2).status == CommandStatus::Accepted);
    CHECK(!book.best_bid().has_value());
    CHECK(book.order_count() == 0);
    CHECK(book.invariants_hold());
}

void test_id_reuse() {
    OrderBook book;
    CHECK(submit_limit(book, 7, Side::Buy, 100, 5).status == CommandStatus::Accepted);
    CHECK(book.cancel(7).status == CommandStatus::Accepted);
    CHECK(submit_limit(book, 7, Side::Sell, 110, 8).status == CommandStatus::Accepted);
    CHECK(book.active_order(7)->side == Side::Sell);
    CHECK(book.active_order(7)->remaining == 8);

    CHECK(submit_limit(book, 8, Side::Buy, 110, 8).status == CommandStatus::Accepted);
    CHECK(!book.active_order(7).has_value());
    CHECK(submit_limit(book, 7, Side::Buy, 90, 1).status == CommandStatus::Accepted);
    CHECK(book.active_order(7)->price == 90);
    CHECK(book.invariants_hold());
}

void test_maker_price() {
    OrderBook book;
    CHECK(submit_limit(book, 1, Side::Buy, 100, 10).status == CommandStatus::Accepted);
    auto sell = submit_limit(book, 2, Side::Sell, 90, 4);
    CHECK(sell.status == CommandStatus::Accepted);
    CHECK(sell.executions.size() == 1);
    CHECK(sell.executions[0].price == 100);
    CHECK(sell.executions[0].maker_id == 1);
    CHECK(sell.executions[0].taker_id == 2);
    CHECK(sell.executions[0].quantity == 4);
    CHECK(book.active_order(1)->remaining == 6);

    CHECK(submit_limit(book, 3, Side::Sell, 120, 5).status == CommandStatus::Accepted);
    auto buy = submit_limit(book, 4, Side::Buy, 130, 3);
    CHECK(buy.executions.size() == 1);
    CHECK(buy.executions[0].price == 120);
    CHECK(buy.executions[0].maker_id == 3);
    CHECK(buy.executions[0].taker_id == 4);
    CHECK(book.invariants_hold());
}

void test_fifo() {
    OrderBook book;
    CHECK(submit_limit(book, 1, Side::Sell, 100, 5).status == CommandStatus::Accepted);
    CHECK(submit_limit(book, 2, Side::Sell, 100, 5).status == CommandStatus::Accepted);
    auto buy = submit_limit(book, 3, Side::Buy, 100, 5);
    CHECK(buy.executions.size() == 1);
    CHECK(buy.executions[0].maker_id == 1);
    CHECK(!book.active_order(1).has_value());
    CHECK(book.active_order(2)->remaining == 5);
    CHECK(book.invariants_hold());
}

void test_partial_fill() {
    OrderBook book;
    CHECK(submit_limit(book, 1, Side::Sell, 100, 10).status == CommandStatus::Accepted);
    const Sequence seq = book.active_order(1)->sequence;
    auto buy = submit_limit(book, 2, Side::Buy, 100, 4);
    CHECK(buy.executions.size() == 1);
    CHECK(buy.executions[0].quantity == 4);
    CHECK(buy.unfilled_quantity == 0);
    CHECK(book.active_order(1)->remaining == 6);
    CHECK(book.active_order(1)->sequence == seq);
    CHECK(book.ask_quantity() == 6);
    CHECK(book.invariants_hold());
}

void test_complete_fill() {
    OrderBook book;
    CHECK(submit_limit(book, 1, Side::Buy, 50, 3).status == CommandStatus::Accepted);
    auto sell = submit_limit(book, 2, Side::Sell, 50, 3);
    CHECK(sell.status == CommandStatus::Accepted);
    CHECK(sell.unfilled_quantity == 0);
    CHECK(sell.executions.size() == 1);
    CHECK(sell.executions[0].quantity == 3);
    CHECK(!book.active_order(1).has_value());
    CHECK(!book.active_order(2).has_value());
    CHECK(book.order_count() == 0);
    CHECK(!book.best_bid().has_value());
    CHECK(!book.best_ask().has_value());
    CHECK(book.invariants_hold());
}

void test_multi_level_crossing() {
    OrderBook book;
    CHECK(submit_limit(book, 1, Side::Sell, 100, 5).status == CommandStatus::Accepted);
    CHECK(submit_limit(book, 2, Side::Sell, 101, 5).status == CommandStatus::Accepted);
    CHECK(submit_limit(book, 3, Side::Sell, 102, 5).status == CommandStatus::Accepted);
    auto buy = submit_limit(book, 4, Side::Buy, 102, 12);
    CHECK(buy.status == CommandStatus::Accepted);
    CHECK(buy.executions.size() == 3);
    CHECK(buy.executions[0].price == 100);
    CHECK(buy.executions[0].quantity == 5);
    CHECK(buy.executions[1].price == 101);
    CHECK(buy.executions[1].quantity == 5);
    CHECK(buy.executions[2].price == 102);
    CHECK(buy.executions[2].quantity == 2);
    CHECK(buy.unfilled_quantity == 0);
    CHECK(book.active_order(3)->remaining == 3);
    CHECK(book.ask_level_count() == 1);
    CHECK(book.invariants_hold());
}

void test_market_exhaustion() {
    OrderBook book;
    CHECK(submit_limit(book, 1, Side::Sell, 100, 4).status == CommandStatus::Accepted);
    CHECK(submit_limit(book, 2, Side::Sell, 101, 3).status == CommandStatus::Accepted);
    auto market = book.submit(3, Side::Buy, OrderType::Market, 0, 20);
    CHECK(market.status == CommandStatus::Accepted);
    CHECK(market.unfilled_quantity == 13);
    CHECK(filled_qty(market) == 7);
    CHECK(!book.active_order(3).has_value());
    CHECK(book.order_count() == 0);
    CHECK(!book.best_ask().has_value());

    auto empty_market = book.submit(4, Side::Sell, OrderType::Market, 0, 5);
    CHECK(empty_market.status == CommandStatus::Accepted);
    CHECK(empty_market.unfilled_quantity == 5);
    CHECK(empty_market.executions.empty());
    CHECK(book.invariants_hold());
}

void test_quantity_reduction_keeps_priority() {
    OrderBook book;
    CHECK(submit_limit(book, 1, Side::Sell, 100, 10).status == CommandStatus::Accepted);
    CHECK(submit_limit(book, 2, Side::Sell, 100, 10).status == CommandStatus::Accepted);
    const Sequence seq = book.active_order(1)->sequence;
    auto reduced = book.modify(1, 100, 3);
    CHECK(reduced.status == CommandStatus::Accepted);
    CHECK(reduced.executions.empty());
    CHECK(book.active_order(1)->remaining == 3);
    CHECK(book.active_order(1)->sequence == seq);
    auto buy = submit_limit(book, 3, Side::Buy, 100, 3);
    CHECK(buy.executions.size() == 1);
    CHECK(buy.executions[0].maker_id == 1);
    CHECK(!book.active_order(1).has_value());
    CHECK(book.active_order(2)->remaining == 10);
    CHECK(book.invariants_hold());
}

void test_quantity_increase_loses_priority() {
    OrderBook book;
    CHECK(submit_limit(book, 1, Side::Sell, 100, 5).status == CommandStatus::Accepted);
    CHECK(submit_limit(book, 2, Side::Sell, 100, 5).status == CommandStatus::Accepted);
    const Sequence before = book.active_order(1)->sequence;
    auto increased = book.modify(1, 100, 9);
    CHECK(increased.status == CommandStatus::Accepted);
    CHECK(book.active_order(1)->remaining == 9);
    CHECK(book.active_order(1)->sequence != before);
    auto buy = submit_limit(book, 3, Side::Buy, 100, 5);
    CHECK(buy.executions.size() == 1);
    CHECK(buy.executions[0].maker_id == 2);
    CHECK(book.active_order(1)->remaining == 9);
    CHECK(book.invariants_hold());
}

void test_reprice_loses_priority() {
    OrderBook book;
    CHECK(submit_limit(book, 1, Side::Sell, 100, 5).status == CommandStatus::Accepted);
    CHECK(submit_limit(book, 2, Side::Sell, 100, 5).status == CommandStatus::Accepted);
    const Sequence before = book.active_order(1)->sequence;
    auto repriced = book.modify(1, 101, 5);
    CHECK(repriced.status == CommandStatus::Accepted);
    CHECK(book.active_order(1)->price == 101);
    CHECK(book.active_order(1)->sequence != before);
    auto buy = submit_limit(book, 3, Side::Buy, 100, 5);
    CHECK(buy.executions.size() == 1);
    CHECK(buy.executions[0].maker_id == 2);
    CHECK(book.active_order(1)->remaining == 5);
    CHECK(book.best_ask() == 101);
    CHECK(book.invariants_hold());
}

void test_aggressive_modification() {
    OrderBook book;
    CHECK(submit_limit(book, 1, Side::Buy, 100, 10).status == CommandStatus::Accepted);
    CHECK(submit_limit(book, 2, Side::Sell, 110, 6).status == CommandStatus::Accepted);
    auto aggress = book.modify(1, 110, 10);
    CHECK(aggress.status == CommandStatus::Accepted);
    CHECK(aggress.executions.size() == 1);
    CHECK(aggress.executions[0].maker_id == 2);
    CHECK(aggress.executions[0].taker_id == 1);
    CHECK(aggress.executions[0].price == 110);
    CHECK(aggress.executions[0].quantity == 6);
    CHECK(aggress.unfilled_quantity == 4);
    CHECK(book.active_order(1)->price == 110);
    CHECK(book.active_order(1)->remaining == 4);
    CHECK(!book.active_order(2).has_value());
    CHECK(book.best_bid() == 110);
    CHECK(!book.best_ask().has_value());
    CHECK(book.invariants_hold());
}

void test_transactional_capacity() {
    OrderBook orders(2, 8);
    CHECK(submit_limit(orders, 1, Side::Buy, 90, 10).status == CommandStatus::Accepted);
    CHECK(submit_limit(orders, 2, Side::Sell, 110, 10).status == CommandStatus::Accepted);
    const std::uint64_t full_orders = orders.checksum();
    auto rejected = submit_limit(orders, 3, Side::Buy, 100, 4);
    CHECK(rejected.status == CommandStatus::Rejected);
    CHECK(rejected.reason == RejectReason::OrderCapacity);
    CHECK(orders.checksum() == full_orders);
    CHECK(orders.order_count() == 2);

    auto crossing = submit_limit(orders, 3, Side::Buy, 110, 10);
    CHECK(crossing.status == CommandStatus::Accepted);
    CHECK(crossing.unfilled_quantity == 0);
    CHECK(!orders.active_order(2).has_value());
    CHECK(orders.order_count() == 1);

    OrderBook levels(8, 1);
    CHECK(submit_limit(levels, 1, Side::Sell, 100, 10).status == CommandStatus::Accepted);
    const std::uint64_t full_levels = levels.checksum();
    auto new_price = submit_limit(levels, 2, Side::Buy, 90, 5);
    CHECK(new_price.status == CommandStatus::Rejected);
    CHECK(new_price.reason == RejectReason::LevelCapacity);
    CHECK(levels.checksum() == full_levels);
    CHECK(levels.active_order(1)->remaining == 10);

    auto reuse_level = submit_limit(levels, 2, Side::Buy, 100, 15);
    CHECK(reuse_level.status == CommandStatus::Accepted);
    CHECK(reuse_level.unfilled_quantity == 5);
    CHECK(!levels.active_order(1).has_value());
    CHECK(levels.active_order(2)->price == 100);
    CHECK(levels.bid_level_count() == 1);
    CHECK(levels.ask_level_count() == 0);

    OrderBook tight(4, 2);
    CHECK(submit_limit(tight, 1, Side::Buy, 100, 5).status == CommandStatus::Accepted);
    CHECK(submit_limit(tight, 2, Side::Buy, 100, 5).status == CommandStatus::Accepted);
    CHECK(submit_limit(tight, 3, Side::Buy, 99, 5).status == CommandStatus::Accepted);
    const std::uint64_t two_levels = tight.checksum();
    auto third_level = tight.modify(1, 98, 5);
    CHECK(third_level.status == CommandStatus::Rejected);
    CHECK(third_level.reason == RejectReason::LevelCapacity);
    CHECK(tight.checksum() == two_levels);
    CHECK(tight.active_order(1)->price == 100);
    CHECK(tight.invariants_hold());
}

void test_quantity_overflow() {
    constexpr Quantity max_quantity = std::numeric_limits<Quantity>::max();

    OrderBook submit_book(4, 4);
    CHECK(submit_limit(submit_book, 1, Side::Buy, 100, max_quantity).status ==
          CommandStatus::Accepted);
    const std::uint64_t full_submit = submit_book.checksum();
    const auto submit_overflow = submit_limit(submit_book, 2, Side::Buy, 100, 1);
    CHECK(submit_overflow.status == CommandStatus::Rejected);
    CHECK(submit_overflow.reason == RejectReason::QuantityOverflow);
    CHECK(submit_overflow.unfilled_quantity == 1);
    CHECK(submit_overflow.executions.empty());
    CHECK(submit_book.checksum() == full_submit);
    CHECK(submit_book.bid_quantity() == max_quantity);
    CHECK(submit_book.bid_level(0)->quantity == max_quantity);
    CHECK(submit_book.invariants_hold());

    OrderBook modify_book(4, 4);
    CHECK(submit_limit(modify_book, 1, Side::Sell, 100, max_quantity - 5).status ==
          CommandStatus::Accepted);
    CHECK(submit_limit(modify_book, 2, Side::Sell, 100, 5).status == CommandStatus::Accepted);
    const std::uint64_t full_modify = modify_book.checksum();
    const auto modify_overflow = modify_book.modify(2, 100, 6);
    CHECK(modify_overflow.status == CommandStatus::Rejected);
    CHECK(modify_overflow.reason == RejectReason::QuantityOverflow);
    CHECK(modify_overflow.unfilled_quantity == 5);
    CHECK(modify_overflow.executions.empty());
    CHECK(modify_book.checksum() == full_modify);
    CHECK(modify_book.ask_quantity() == max_quantity);
    CHECK(modify_book.active_order(2)->remaining == 5);
    CHECK(modify_book.invariants_hold());

    CHECK(modify_book.cancel(1).status == CommandStatus::Accepted);
    CHECK(modify_book.modify(2, 100, 6).status == CommandStatus::Accepted);
    CHECK(modify_book.ask_quantity() == 6);
    CHECK(modify_book.invariants_hold());
}

void test_deterministic_replay() {
    struct Command {
        int kind;
        OrderId id;
        Side side;
        OrderType type;
        Price price;
        Quantity quantity;
    };

    const Command commands[] = {
        {0, 1, Side::Buy, OrderType::Limit, 100, 10},
        {0, 2, Side::Buy, OrderType::Limit, 101, 4},
        {0, 3, Side::Sell, OrderType::Limit, 105, 6},
        {0, 4, Side::Sell, OrderType::Limit, 100, 7},
        {2, 2, Side::Buy, OrderType::Limit, 101, 2},
        {0, 5, Side::Buy, OrderType::Market, 0, 8},
        {2, 3, Side::Sell, OrderType::Limit, 99, 6},
        {1, 1, Side::Buy, OrderType::Limit, 0, 0},
        {0, 6, Side::Sell, OrderType::Limit, 102, 3},
        {2, 6, Side::Sell, OrderType::Limit, 102, 9},
    };

    auto run = [&](OrderBook& book) {
        std::vector<RecordedFill> fills;
        std::vector<std::uint64_t> checksums;
        for (const Command& command : commands) {
            CommandResult result;
            if (command.kind == 0) {
                result = book.submit(command.id, command.side, command.type, command.price, command.quantity);
            } else if (command.kind == 1) {
                result = book.cancel(command.id);
            } else {
                result = book.modify(command.id, command.price, command.quantity);
            }
            auto recorded = copy_fills(result);
            fills.insert(fills.end(), recorded.begin(), recorded.end());
            checksums.push_back(book.checksum());
            CHECK(book.invariants_hold());
        }
        return std::make_pair(std::move(fills), std::move(checksums));
    };

    OrderBook first;
    OrderBook second;
    const auto a = run(first);
    const auto b = run(second);
    CHECK(a.first.size() == b.first.size());
    for (std::size_t i = 0; i < a.first.size(); ++i) {
        CHECK(a.first[i].maker_id == b.first[i].maker_id);
        CHECK(a.first[i].taker_id == b.first[i].taker_id);
        CHECK(a.first[i].price == b.first[i].price);
        CHECK(a.first[i].quantity == b.first[i].quantity);
        CHECK(a.first[i].sequence == b.first[i].sequence);
    }
    CHECK(a.second == b.second);
    CHECK(first.checksum() == second.checksum());
}

void test_stress() {
    constexpr int events = 100000;
    constexpr std::uint64_t seed = 20260818;

    auto run = [&](std::uint64_t& out_checksum) {
        OrderBook book;
        std::mt19937_64 rng(seed);
        std::vector<OrderId> live;
        std::vector<OrderId> recycled;
        live.reserve(8192);
        recycled.reserve(8192);
        OrderId next_id = 1;
        Quantity resting = 0;

        const auto alloc_id = [&] {
            if (!recycled.empty()) {
                const OrderId id = recycled.back();
                recycled.pop_back();
                return id;
            }
            return next_id++;
        };

        const auto remove_live_at = [&](std::size_t index) {
            const OrderId id = live[index];
            live[index] = live.back();
            live.pop_back();
            recycled.push_back(id);
        };

        for (int i = 0; i < events; ++i) {
            const int pick = static_cast<int>(rng() % 100);
            const bool crowded = live.size() > 4096;

            if (live.empty() || (!crowded && pick < 50)) {
                const OrderId id = alloc_id();
                const Side side = (rng() & 1U) == 0 ? Side::Buy : Side::Sell;
                const OrderType type = (rng() % 12 == 0) ? OrderType::Market : OrderType::Limit;
                const Price price = static_cast<Price>(1 + static_cast<int>(rng() % 80));
                const Quantity quantity = 1 + static_cast<Quantity>(rng() % 12);
                const CommandResult result = book.submit(id, side, type, price, quantity);
                if (result.status == CommandStatus::Accepted) {
                    resting -= filled_qty(result);
                    if (type == OrderType::Limit && result.unfilled_quantity > 0) {
                        resting += result.unfilled_quantity;
                        live.push_back(id);
                    } else {
                        recycled.push_back(id);
                    }
                } else {
                    recycled.push_back(id);
                }
            } else if (pick < 70 || crowded) {
                const std::size_t index = static_cast<std::size_t>(rng() % live.size());
                const OrderId id = live[index];
                const auto snapshot = book.active_order(id);
                const CommandResult result = book.cancel(id);
                if (result.status == CommandStatus::Accepted) {
                    resting -= snapshot->remaining;
                }
                remove_live_at(index);
            } else {
                const std::size_t index = static_cast<std::size_t>(rng() % live.size());
                const OrderId id = live[index];
                const auto snapshot = book.active_order(id);
                if (!snapshot.has_value()) {
                    remove_live_at(index);
                } else {
                    const Price price = static_cast<Price>(1 + static_cast<int>(rng() % 80));
                    const Quantity quantity = 1 + static_cast<Quantity>(rng() % 16);
                    const CommandResult result = book.modify(id, price, quantity);
                    if (result.status == CommandStatus::Rejected && result.reason == RejectReason::UnknownId) {
                        remove_live_at(index);
                    } else if (result.status == CommandStatus::Accepted) {
                        if (price == snapshot->price && quantity < snapshot->remaining) {
                            resting -= (snapshot->remaining - quantity);
                        } else {
                            resting -= snapshot->remaining;
                            resting -= filled_qty(result);
                            resting += result.unfilled_quantity;
                        }
                        if (result.unfilled_quantity == 0) {
                            remove_live_at(index);
                        }
                    }
                }
            }

            CHECK(book.bid_quantity() + book.ask_quantity() == resting);
            CHECK(book.invariants_hold());
        }

        out_checksum = book.checksum();
        return resting;
    };

    std::uint64_t first_checksum = 0;
    std::uint64_t second_checksum = 0;
    const Quantity first_resting = run(first_checksum);
    const Quantity second_resting = run(second_checksum);
    CHECK(first_checksum == second_checksum);
    CHECK(first_resting == second_resting);
}

}  // namespace

int main() {
    test_validation();
    test_empty_queries();
    test_insertion();
    test_cancellation();
    test_id_reuse();
    test_maker_price();
    test_fifo();
    test_partial_fill();
    test_complete_fill();
    test_multi_level_crossing();
    test_market_exhaustion();
    test_quantity_reduction_keeps_priority();
    test_quantity_increase_loses_priority();
    test_reprice_loses_priority();
    test_aggressive_modification();
    test_transactional_capacity();
    test_quantity_overflow();
    test_deterministic_replay();
    test_stress();

    if (g_fails != 0) {
        std::cerr << g_fails << " checks failed out of " << g_checks << "\n";
        return 1;
    }
    std::cout << g_checks << " checks passed\n";
    return 0;
}
