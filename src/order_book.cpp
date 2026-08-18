#include "order_book/order_book.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace order_book {

namespace {

std::size_t hash_table_size(std::size_t max_orders) {
    std::size_t size = 8;
    const std::size_t needed = (max_orders > (std::numeric_limits<std::size_t>::max() / 2))
                                   ? std::numeric_limits<std::size_t>::max()
                                   : max_orders * 2;
    while (size < needed) {
        if (size > (std::numeric_limits<std::size_t>::max() / 2)) {
            break;
        }
        size *= 2;
    }
    return size;
}

}  // namespace

OrderBook::OrderBook(std::size_t max_orders, std::size_t max_levels)
    : max_orders_(max_orders), max_levels_(max_levels) {
    if (max_orders > std::numeric_limits<Index>::max()) {
        throw std::invalid_argument("max_orders exceeds index range");
    }
    if (max_levels > std::numeric_limits<Index>::max()) {
        throw std::invalid_argument("max_levels exceeds index range");
    }

    slots_.resize(max_orders);
    levels_.resize(max_levels);
    bid_index_.resize(max_levels);
    ask_index_.resize(max_levels);

    free_slots_.reserve(max_orders);
    for (std::size_t i = 0; i < max_orders; ++i) {
        free_slots_.push_back(static_cast<Index>(i));
    }
    free_levels_.reserve(max_levels);
    for (std::size_t i = 0; i < max_levels; ++i) {
        free_levels_.push_back(static_cast<Index>(i));
    }

    const std::size_t table_size = hash_table_size(max_orders);
    hash_.assign(table_size, HashEntry{});
    hash_scratch_.assign(table_size, HashEntry{});
    hash_mask_ = table_size == 0 ? 0 : table_size - 1;

    fills_.reserve(max_orders == 0 ? 1 : max_orders);
}

std::uint64_t OrderBook::mix(std::uint64_t x) noexcept {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

bool OrderBook::is_side(Side side) noexcept {
    return side == Side::Buy || side == Side::Sell;
}

bool OrderBook::is_type(OrderType type) noexcept {
    return type == OrderType::Limit || type == OrderType::Market;
}

bool OrderBook::crosses(Side taker_side, Price taker_price, bool market, Price maker_price) noexcept {
    if (market) {
        return true;
    }
    if (taker_side == Side::Buy) {
        return taker_price >= maker_price;
    }
    return taker_price <= maker_price;
}

CommandResult OrderBook::make_result(CommandStatus status, RejectReason reason, Quantity unfilled) const {
    return CommandResult{
        status,
        reason,
        unfilled,
        std::span<const Execution>{fills_.data(), fills_.size()},
    };
}

OrderBook::Index OrderBook::hash_find(OrderId id) const noexcept {
    if (hash_.empty()) {
        return npos;
    }
    std::size_t i = static_cast<std::size_t>(mix(id) & hash_mask_);
    const std::size_t start = i;
    do {
        const HashEntry& entry = hash_[i];
        if (entry.occupancy == Occupancy::Empty) {
            return npos;
        }
        if (entry.occupancy == Occupancy::Occupied && entry.id == id) {
            return entry.slot;
        }
        i = (i + 1) & hash_mask_;
    } while (i != start);
    return npos;
}

void OrderBook::hash_insert(OrderId id, Index slot) {
    if (hash_.empty()) {
        return;
    }
    if (hash_count_ + hash_tombs_ > (hash_.size() / 2)) {
        hash_rehash();
    }
    std::size_t i = static_cast<std::size_t>(mix(id) & hash_mask_);
    std::size_t tomb = hash_.size();
    for (;;) {
        HashEntry& entry = hash_[i];
        if (entry.occupancy == Occupancy::Occupied && entry.id == id) {
            entry.slot = slot;
            return;
        }
        if (entry.occupancy == Occupancy::Tomb && tomb == hash_.size()) {
            tomb = i;
        }
        if (entry.occupancy == Occupancy::Empty) {
            const std::size_t target = (tomb != hash_.size()) ? tomb : i;
            if (target == tomb) {
                --hash_tombs_;
            }
            hash_[target] = HashEntry{id, slot, Occupancy::Occupied};
            ++hash_count_;
            return;
        }
        i = (i + 1) & hash_mask_;
    }
}

void OrderBook::hash_erase(OrderId id) {
    if (hash_.empty()) {
        return;
    }
    std::size_t i = static_cast<std::size_t>(mix(id) & hash_mask_);
    const std::size_t start = i;
    do {
        HashEntry& entry = hash_[i];
        if (entry.occupancy == Occupancy::Empty) {
            return;
        }
        if (entry.occupancy == Occupancy::Occupied && entry.id == id) {
            entry.occupancy = Occupancy::Tomb;
            --hash_count_;
            ++hash_tombs_;
            return;
        }
        i = (i + 1) & hash_mask_;
    } while (i != start);
}

void OrderBook::hash_rehash() {
    std::fill(hash_scratch_.begin(), hash_scratch_.end(), HashEntry{});
    for (const HashEntry& entry : hash_) {
        if (entry.occupancy != Occupancy::Occupied) {
            continue;
        }
        std::size_t i = static_cast<std::size_t>(mix(entry.id) & hash_mask_);
        while (hash_scratch_[i].occupancy == Occupancy::Occupied) {
            i = (i + 1) & hash_mask_;
        }
        hash_scratch_[i] = entry;
    }
    hash_.swap(hash_scratch_);
    hash_tombs_ = 0;
}

OrderBook::Index OrderBook::alloc_slot() {
    const Index slot = free_slots_.back();
    free_slots_.pop_back();
    ++used_orders_;
    return slot;
}

void OrderBook::release_slot(Index slot) {
    free_slots_.push_back(slot);
    --used_orders_;
}

OrderBook::Index OrderBook::alloc_level() {
    const Index level = free_levels_.back();
    free_levels_.pop_back();
    return level;
}

void OrderBook::release_level(Index level) {
    free_levels_.push_back(level);
}

std::size_t OrderBook::bid_lower_bound(Price price) const {
    std::size_t lo = 0;
    std::size_t hi = bid_count_;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (levels_[bid_index_[mid]].price > price) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

std::size_t OrderBook::ask_lower_bound(Price price) const {
    std::size_t lo = 0;
    std::size_t hi = ask_count_;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (levels_[ask_index_[mid]].price < price) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

OrderBook::Index OrderBook::find_level(Side side, Price price) const {
    if (side == Side::Buy) {
        const std::size_t pos = bid_lower_bound(price);
        if (pos < bid_count_ && levels_[bid_index_[pos]].price == price) {
            return bid_index_[pos];
        }
        return npos;
    }
    const std::size_t pos = ask_lower_bound(price);
    if (pos < ask_count_ && levels_[ask_index_[pos]].price == price) {
        return ask_index_[pos];
    }
    return npos;
}

void OrderBook::insert_level_index(Side side, Index level) {
    const Price price = levels_[level].price;
    if (side == Side::Buy) {
        const std::size_t pos = bid_lower_bound(price);
        for (std::size_t i = bid_count_; i > pos; --i) {
            bid_index_[i] = bid_index_[i - 1];
        }
        bid_index_[pos] = level;
        ++bid_count_;
        return;
    }
    const std::size_t pos = ask_lower_bound(price);
    for (std::size_t i = ask_count_; i > pos; --i) {
        ask_index_[i] = ask_index_[i - 1];
    }
    ask_index_[pos] = level;
    ++ask_count_;
}

void OrderBook::erase_level_at(Side side, std::size_t position) {
    if (side == Side::Buy) {
        for (std::size_t i = position + 1; i < bid_count_; ++i) {
            bid_index_[i - 1] = bid_index_[i];
        }
        --bid_count_;
        return;
    }
    for (std::size_t i = position + 1; i < ask_count_; ++i) {
        ask_index_[i - 1] = ask_index_[i];
    }
    --ask_count_;
}

void OrderBook::append_order(Index level, Index slot) {
    Slot& order = slots_[slot];
    Level& lvl = levels_[level];
    order.level = level;
    order.prev = lvl.tail;
    order.next = npos;
    if (lvl.tail != npos) {
        slots_[lvl.tail].next = slot;
    } else {
        lvl.head = slot;
    }
    lvl.tail = slot;
    ++lvl.count;
    lvl.total += order.remaining;
}

void OrderBook::detach_from_level(Index slot) {
    Slot& order = slots_[slot];
    Level& lvl = levels_[order.level];
    if (order.prev != npos) {
        slots_[order.prev].next = order.next;
    } else {
        lvl.head = order.next;
    }
    if (order.next != npos) {
        slots_[order.next].prev = order.prev;
    } else {
        lvl.tail = order.prev;
    }
    --lvl.count;
    order.prev = npos;
    order.next = npos;
    order.level = npos;
}

void OrderBook::rest(Index slot) {
    Slot& order = slots_[slot];
    Index level = find_level(order.side, order.price);
    if (level == npos) {
        level = alloc_level();
        Level& lvl = levels_[level];
        lvl.price = order.price;
        lvl.side = order.side;
        lvl.total = 0;
        lvl.head = npos;
        lvl.tail = npos;
        lvl.count = 0;
        insert_level_index(order.side, level);
    }
    append_order(level, slot);
    if (order.side == Side::Buy) {
        bid_qty_ += order.remaining;
    } else {
        ask_qty_ += order.remaining;
    }
}

void OrderBook::record_fill(OrderId maker_id, OrderId taker_id, Price price, Quantity quantity) {
    fills_.push_back(Execution{maker_id, taker_id, price, quantity, next_execution_++});
}

OrderBook::MatchPlan OrderBook::plan_match(Side side, Price price, bool market, Quantity quantity) const {
    MatchPlan plan;
    plan.remaining = quantity;
    const std::vector<Index>& index = (side == Side::Buy) ? ask_index_ : bid_index_;
    const std::size_t count = (side == Side::Buy) ? ask_count_ : bid_count_;
    for (std::size_t i = 0; i < count && plan.remaining > 0; ++i) {
        const Level& level = levels_[index[i]];
        if (!crosses(side, price, market, level.price)) {
            break;
        }
        if (plan.remaining >= level.total) {
            plan.remaining -= level.total;
            plan.freed_orders += level.count;
            ++plan.freed_levels;
            continue;
        }
        Index order = level.head;
        while (order != npos && plan.remaining > 0) {
            const Slot& maker = slots_[order];
            if (plan.remaining >= maker.remaining) {
                plan.remaining -= maker.remaining;
                ++plan.freed_orders;
            } else {
                plan.remaining = 0;
            }
            order = maker.next;
        }
        break;
    }
    return plan;
}

void OrderBook::match(Side side, Price price, bool market, Quantity& remaining, OrderId taker_id) {
    std::vector<Index>& index = (side == Side::Buy) ? ask_index_ : bid_index_;
    std::size_t& count = (side == Side::Buy) ? ask_count_ : bid_count_;
    Quantity& opposite_qty = (side == Side::Buy) ? ask_qty_ : bid_qty_;

    while (remaining > 0 && count > 0) {
        const Index level_idx = index[0];
        Level& level = levels_[level_idx];
        if (!crosses(side, price, market, level.price)) {
            break;
        }
        while (remaining > 0 && level.head != npos) {
            const Index maker_idx = level.head;
            Slot& maker = slots_[maker_idx];
            const Quantity fill = std::min(remaining, maker.remaining);
            record_fill(maker.id, taker_id, maker.price, fill);
            remaining -= fill;
            maker.remaining -= fill;
            level.total -= fill;
            opposite_qty -= fill;
            if (maker.remaining == 0) {
                const OrderId maker_id = maker.id;
                detach_from_level(maker_idx);
                hash_erase(maker_id);
                release_slot(maker_idx);
            }
        }
        if (level.head == npos) {
            erase_level_at(side == Side::Buy ? Side::Sell : Side::Buy, 0);
            release_level(level_idx);
        }
    }
}

CommandResult OrderBook::submit(OrderId id, Side side, OrderType type, Price price, Quantity quantity) {
    fills_.clear();
    if (!is_side(side)) {
        return make_result(CommandStatus::Rejected, RejectReason::InvalidSide, quantity);
    }
    if (!is_type(type)) {
        return make_result(CommandStatus::Rejected, RejectReason::InvalidType, quantity);
    }
    if (quantity == 0) {
        return make_result(CommandStatus::Rejected, RejectReason::InvalidQuantity, quantity);
    }
    if (type == OrderType::Limit && price <= 0) {
        return make_result(CommandStatus::Rejected, RejectReason::InvalidPrice, quantity);
    }
    if (hash_find(id) != npos) {
        return make_result(CommandStatus::Rejected, RejectReason::DuplicateId, quantity);
    }

    const bool market = type == OrderType::Market;
    const MatchPlan plan = plan_match(side, price, market, quantity);
    if (!market && plan.remaining > 0) {
        const Quantity side_quantity = (side == Side::Buy) ? bid_qty_ : ask_qty_;
        if (plan.remaining > std::numeric_limits<Quantity>::max() - side_quantity) {
            return make_result(CommandStatus::Rejected, RejectReason::QuantityOverflow, quantity);
        }
        if (free_slots_.size() + plan.freed_orders < 1) {
            return make_result(CommandStatus::Rejected, RejectReason::OrderCapacity, quantity);
        }
        if (find_level(side, price) == npos && free_levels_.size() + plan.freed_levels < 1) {
            return make_result(CommandStatus::Rejected, RejectReason::LevelCapacity, quantity);
        }
    }

    Quantity remaining = quantity;
    match(side, price, market, remaining, id);

    if (remaining > 0 && !market) {
        const Index slot = alloc_slot();
        Slot& order = slots_[slot];
        order.id = id;
        order.side = side;
        order.type = OrderType::Limit;
        order.price = price;
        order.remaining = remaining;
        order.sequence = next_arrival_++;
        order.prev = npos;
        order.next = npos;
        order.level = npos;
        hash_insert(id, slot);
        rest(slot);
    }

    return make_result(CommandStatus::Accepted, RejectReason::None, remaining);
}

CommandResult OrderBook::cancel(OrderId id) {
    fills_.clear();
    const Index slot = hash_find(id);
    if (slot == npos) {
        return make_result(CommandStatus::Rejected, RejectReason::UnknownId, 0);
    }

    Slot& order = slots_[slot];
    const Side side = order.side;
    const Price price = order.price;
    const Index level_idx = order.level;
    if (side == Side::Buy) {
        bid_qty_ -= order.remaining;
    } else {
        ask_qty_ -= order.remaining;
    }
    levels_[level_idx].total -= order.remaining;
    detach_from_level(slot);
    if (levels_[level_idx].head == npos) {
        const std::size_t pos = (side == Side::Buy) ? bid_lower_bound(price) : ask_lower_bound(price);
        erase_level_at(side, pos);
        release_level(level_idx);
    }
    hash_erase(id);
    release_slot(slot);
    return make_result(CommandStatus::Accepted, RejectReason::None, 0);
}

CommandResult OrderBook::modify(OrderId id, Price price, Quantity quantity) {
    fills_.clear();
    const Index slot = hash_find(id);
    if (slot == npos) {
        return make_result(CommandStatus::Rejected, RejectReason::UnknownId, 0);
    }
    const Quantity old_qty = slots_[slot].remaining;
    if (quantity == 0) {
        return make_result(CommandStatus::Rejected, RejectReason::InvalidQuantity, old_qty);
    }
    if (price <= 0) {
        return make_result(CommandStatus::Rejected, RejectReason::InvalidPrice, old_qty);
    }

    Slot& order = slots_[slot];
    const Price old_price = order.price;
    const Side side = order.side;

    if (price == old_price && quantity == old_qty) {
        return make_result(CommandStatus::Accepted, RejectReason::None, old_qty);
    }

    if (price == old_price && quantity < old_qty) {
        const Quantity delta = old_qty - quantity;
        order.remaining = quantity;
        levels_[order.level].total -= delta;
        if (side == Side::Buy) {
            bid_qty_ -= delta;
        } else {
            ask_qty_ -= delta;
        }
        return make_result(CommandStatus::Accepted, RejectReason::None, quantity);
    }

    const Index level_idx = order.level;
    const bool old_level_empties = levels_[level_idx].count == 1;
    const MatchPlan plan = plan_match(side, price, false, quantity);
    const Quantity side_quantity = (side == Side::Buy) ? bid_qty_ : ask_qty_;
    const Quantity quantity_without_order = side_quantity - old_qty;
    if (plan.remaining > std::numeric_limits<Quantity>::max() - quantity_without_order) {
        return make_result(CommandStatus::Rejected, RejectReason::QuantityOverflow, old_qty);
    }
    const bool new_level_exists =
        (price == old_price) ? !old_level_empties : find_level(side, price) != npos;
    if (plan.remaining > 0 && !new_level_exists) {
        const std::size_t extra = old_level_empties ? 1 : 0;
        if (free_levels_.size() + plan.freed_levels + extra < 1) {
            return make_result(CommandStatus::Rejected, RejectReason::LevelCapacity, old_qty);
        }
    }

    if (side == Side::Buy) {
        bid_qty_ -= old_qty;
    } else {
        ask_qty_ -= old_qty;
    }
    levels_[level_idx].total -= old_qty;
    detach_from_level(slot);
    if (levels_[level_idx].head == npos) {
        const std::size_t pos = (side == Side::Buy) ? bid_lower_bound(old_price) : ask_lower_bound(old_price);
        erase_level_at(side, pos);
        release_level(level_idx);
    }

    order.price = price;
    order.remaining = quantity;
    order.sequence = next_arrival_++;
    order.type = OrderType::Limit;

    Quantity remaining = quantity;
    match(side, price, false, remaining, id);

    if (remaining > 0) {
        order.remaining = remaining;
        rest(slot);
    } else {
        hash_erase(id);
        release_slot(slot);
    }

    return make_result(CommandStatus::Accepted, RejectReason::None, remaining);
}

std::optional<Price> OrderBook::best_bid() const {
    if (bid_count_ == 0) {
        return std::nullopt;
    }
    return levels_[bid_index_[0]].price;
}

std::optional<Price> OrderBook::best_ask() const {
    if (ask_count_ == 0) {
        return std::nullopt;
    }
    return levels_[ask_index_[0]].price;
}

std::optional<LevelSnapshot> OrderBook::bid_level(std::size_t rank) const {
    if (rank >= bid_count_) {
        return std::nullopt;
    }
    const Level& level = levels_[bid_index_[rank]];
    return LevelSnapshot{level.price, level.total, level.count};
}

std::optional<LevelSnapshot> OrderBook::ask_level(std::size_t rank) const {
    if (rank >= ask_count_) {
        return std::nullopt;
    }
    const Level& level = levels_[ask_index_[rank]];
    return LevelSnapshot{level.price, level.total, level.count};
}

std::optional<OrderSnapshot> OrderBook::active_order(OrderId id) const {
    const Index slot = hash_find(id);
    if (slot == npos) {
        return std::nullopt;
    }
    const Slot& order = slots_[slot];
    return OrderSnapshot{order.id, order.side, order.type, order.price, order.remaining, order.sequence};
}

std::size_t OrderBook::order_count() const noexcept {
    return used_orders_;
}

std::size_t OrderBook::bid_level_count() const noexcept {
    return bid_count_;
}

std::size_t OrderBook::ask_level_count() const noexcept {
    return ask_count_;
}

Quantity OrderBook::bid_quantity() const noexcept {
    return bid_qty_;
}

Quantity OrderBook::ask_quantity() const noexcept {
    return ask_qty_;
}

std::size_t OrderBook::max_orders() const noexcept {
    return max_orders_;
}

std::size_t OrderBook::max_levels() const noexcept {
    return max_levels_;
}

std::size_t OrderBook::storage_bytes() const noexcept {
    const auto bytes = [](const auto& container) {
        using Value = typename std::decay_t<decltype(container)>::value_type;
        return container.capacity() * sizeof(Value);
    };
    return bytes(slots_) + bytes(levels_) + bytes(bid_index_) + bytes(ask_index_) + bytes(free_slots_) +
           bytes(free_levels_) + bytes(hash_) + bytes(hash_scratch_) + bytes(fills_);
}

bool OrderBook::invariants_hold() const {
    if (free_slots_.size() + used_orders_ != max_orders_) {
        return false;
    }
    if (free_levels_.size() + bid_count_ + ask_count_ != max_levels_) {
        return false;
    }
    if (hash_count_ != used_orders_) {
        return false;
    }

    std::size_t walked = 0;
    Quantity bid_sum = 0;
    Price previous_bid = 0;
    for (std::size_t i = 0; i < bid_count_; ++i) {
        const Index level_idx = bid_index_[i];
        const Level& level = levels_[level_idx];
        if (level.side != Side::Buy || level.price <= 0 || level.count == 0 || level.head == npos ||
            level.tail == npos) {
            return false;
        }
        if (i > 0 && previous_bid <= level.price) {
            return false;
        }
        previous_bid = level.price;

        Quantity sum = 0;
        Index prev = npos;
        Index cur = level.head;
        Index seen = 0;
        while (cur != npos) {
            const Slot& order = slots_[cur];
            if (order.prev != prev || order.side != Side::Buy || order.price != level.price ||
                order.remaining == 0 || order.level != level_idx || hash_find(order.id) != cur) {
                return false;
            }
            sum += order.remaining;
            prev = cur;
            cur = order.next;
            ++seen;
            ++walked;
            if (seen > level.count) {
                return false;
            }
        }
        if (seen != level.count || prev != level.tail || sum != level.total) {
            return false;
        }
        bid_sum += sum;
    }
    if (bid_sum != bid_qty_) {
        return false;
    }

    Quantity ask_sum = 0;
    Price previous_ask = 0;
    for (std::size_t i = 0; i < ask_count_; ++i) {
        const Index level_idx = ask_index_[i];
        const Level& level = levels_[level_idx];
        if (level.side != Side::Sell || level.price <= 0 || level.count == 0 || level.head == npos ||
            level.tail == npos) {
            return false;
        }
        if (i > 0 && previous_ask >= level.price) {
            return false;
        }
        previous_ask = level.price;

        Quantity sum = 0;
        Index prev = npos;
        Index cur = level.head;
        Index seen = 0;
        while (cur != npos) {
            const Slot& order = slots_[cur];
            if (order.prev != prev || order.side != Side::Sell || order.price != level.price ||
                order.remaining == 0 || order.level != level_idx || hash_find(order.id) != cur) {
                return false;
            }
            sum += order.remaining;
            prev = cur;
            cur = order.next;
            ++seen;
            ++walked;
            if (seen > level.count) {
                return false;
            }
        }
        if (seen != level.count || prev != level.tail || sum != level.total) {
            return false;
        }
        ask_sum += sum;
    }
    if (ask_sum != ask_qty_) {
        return false;
    }
    if (walked != used_orders_) {
        return false;
    }
    if (bid_count_ > 0 && ask_count_ > 0 && levels_[bid_index_[0]].price >= levels_[ask_index_[0]].price) {
        return false;
    }
    return true;
}

std::uint64_t OrderBook::checksum() const {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    const auto mix_value = [&](std::uint64_t value) {
        hash ^= value;
        hash *= 0x100000001b3ULL;
    };
    mix_value(next_arrival_);
    mix_value(next_execution_);
    mix_value(used_orders_);
    mix_value(bid_qty_);
    mix_value(ask_qty_);

    const auto walk_side = [&](const std::vector<Index>& index, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            const Level& level = levels_[index[i]];
            mix_value(static_cast<std::uint64_t>(level.price));
            mix_value(level.total);
            mix_value(level.count);
            for (Index cur = level.head; cur != npos; cur = slots_[cur].next) {
                const Slot& order = slots_[cur];
                mix_value(order.id);
                mix_value(order.remaining);
                mix_value(order.sequence);
            }
        }
    };
    walk_side(bid_index_, bid_count_);
    walk_side(ask_index_, ask_count_);
    return hash;
}

}  // namespace order_book
