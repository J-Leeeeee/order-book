#pragma once

#include "order_book/types.hpp"

namespace order_book {

struct OrderSnapshot {
    OrderId id{};
    Side side{};
    OrderType type{};
    Price price{};
    Quantity remaining{};
    Sequence sequence{};
};

}  // namespace order_book
