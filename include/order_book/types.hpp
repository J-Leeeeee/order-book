#pragma once

#include <cstdint>

namespace order_book {

using Price = std::int64_t;
using Quantity = std::uint64_t;
using OrderId = std::uint64_t;
using Sequence = std::uint64_t;

enum class Side : std::uint8_t {
    Buy = 0,
    Sell = 1,
};

enum class OrderType : std::uint8_t {
    Limit = 0,
    Market = 1,
};

enum class CommandStatus : std::uint8_t {
    Accepted = 0,
    Rejected = 1,
};

enum class RejectReason : std::uint8_t {
    None = 0,
    InvalidQuantity = 1,
    InvalidPrice = 2,
    InvalidSide = 3,
    InvalidType = 4,
    DuplicateId = 5,
    UnknownId = 6,
    OrderCapacity = 7,
    LevelCapacity = 8,
    QuantityOverflow = 9,
};

}  // namespace order_book
