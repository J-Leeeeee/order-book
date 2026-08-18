#pragma once

#include "order_book/types.hpp"

#include <cstdint>

namespace order_book {

struct LevelSnapshot {
    Price price{};
    Quantity quantity{};
    std::uint32_t order_count{};
};

}  // namespace order_book
