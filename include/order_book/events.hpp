#pragma once

#include "order_book/types.hpp"

#include <span>

namespace order_book {

struct Execution {
    OrderId maker_id{};
    OrderId taker_id{};
    Price price{};
    Quantity quantity{};
    Sequence sequence{};
};

struct CommandResult {
    CommandStatus status{CommandStatus::Rejected};
    RejectReason reason{RejectReason::None};
    Quantity unfilled_quantity{};
    std::span<const Execution> executions{};
};

}  // namespace order_book
