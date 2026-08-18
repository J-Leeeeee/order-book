#pragma once

#include "order_book/events.hpp"
#include "order_book/level.hpp"
#include "order_book/order.hpp"
#include "order_book/types.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace order_book {

class OrderBook {
public:
    static constexpr std::size_t default_max_orders = 65536;
    static constexpr std::size_t default_max_levels = 4096;

    explicit OrderBook(std::size_t max_orders = default_max_orders,
                       std::size_t max_levels = default_max_levels);

    [[nodiscard]] CommandResult submit(OrderId id, Side side, OrderType type, Price price, Quantity quantity);
    [[nodiscard]] CommandResult cancel(OrderId id);
    [[nodiscard]] CommandResult modify(OrderId id, Price price, Quantity quantity);

    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    [[nodiscard]] std::optional<LevelSnapshot> bid_level(std::size_t rank) const;
    [[nodiscard]] std::optional<LevelSnapshot> ask_level(std::size_t rank) const;
    [[nodiscard]] std::optional<OrderSnapshot> active_order(OrderId id) const;

    [[nodiscard]] std::size_t order_count() const noexcept;
    [[nodiscard]] std::size_t bid_level_count() const noexcept;
    [[nodiscard]] std::size_t ask_level_count() const noexcept;
    [[nodiscard]] Quantity bid_quantity() const noexcept;
    [[nodiscard]] Quantity ask_quantity() const noexcept;
    [[nodiscard]] std::size_t max_orders() const noexcept;
    [[nodiscard]] std::size_t max_levels() const noexcept;
    [[nodiscard]] std::size_t storage_bytes() const noexcept;

    [[nodiscard]] bool invariants_hold() const;
    [[nodiscard]] std::uint64_t checksum() const;

private:
    using Index = std::uint32_t;
    static constexpr Index npos = std::numeric_limits<Index>::max();

    struct Slot {
        OrderId id{};
        Side side{};
        OrderType type{};
        Price price{};
        Quantity remaining{};
        Sequence sequence{};
        Index prev{npos};
        Index next{npos};
        Index level{npos};
    };

    struct Level {
        Price price{};
        Side side{};
        Quantity total{};
        Index head{npos};
        Index tail{npos};
        Index count{};
    };

    enum class Occupancy : std::uint8_t {
        Empty = 0,
        Occupied = 1,
        Tomb = 2,
    };

    struct HashEntry {
        OrderId id{};
        Index slot{npos};
        Occupancy occupancy{Occupancy::Empty};
    };

    struct MatchPlan {
        Quantity remaining{};
        std::size_t freed_orders{};
        std::size_t freed_levels{};
    };

    static std::uint64_t mix(std::uint64_t x) noexcept;
    static bool is_side(Side side) noexcept;
    static bool is_type(OrderType type) noexcept;
    static bool crosses(Side taker_side, Price taker_price, bool market, Price maker_price) noexcept;

    [[nodiscard]] CommandResult make_result(CommandStatus status, RejectReason reason, Quantity unfilled) const;

    [[nodiscard]] Index hash_find(OrderId id) const noexcept;
    void hash_insert(OrderId id, Index slot);
    void hash_erase(OrderId id);
    void hash_rehash();

    [[nodiscard]] Index alloc_slot();
    void release_slot(Index slot);
    [[nodiscard]] Index alloc_level();
    void release_level(Index level);

    [[nodiscard]] std::size_t bid_lower_bound(Price price) const;
    [[nodiscard]] std::size_t ask_lower_bound(Price price) const;
    [[nodiscard]] Index find_level(Side side, Price price) const;
    void insert_level_index(Side side, Index level);
    void erase_level_at(Side side, std::size_t position);

    void append_order(Index level, Index slot);
    void detach_from_level(Index slot);
    void rest(Index slot);
    void record_fill(OrderId maker_id, OrderId taker_id, Price price, Quantity quantity);

    [[nodiscard]] MatchPlan plan_match(Side side, Price price, bool market, Quantity quantity) const;
    void match(Side side, Price price, bool market, Quantity& remaining, OrderId taker_id);

    std::vector<Slot> slots_{};
    std::vector<Level> levels_{};
    std::vector<Index> bid_index_{};
    std::vector<Index> ask_index_{};
    std::vector<Index> free_slots_{};
    std::vector<Index> free_levels_{};
    std::vector<HashEntry> hash_{};
    std::vector<HashEntry> hash_scratch_{};
    std::vector<Execution> fills_{};

    std::size_t max_orders_{};
    std::size_t max_levels_{};
    std::size_t used_orders_{};
    std::size_t bid_count_{};
    std::size_t ask_count_{};
    std::size_t hash_mask_{};
    std::size_t hash_count_{};
    std::size_t hash_tombs_{};
    Quantity bid_qty_{};
    Quantity ask_qty_{};
    Sequence next_arrival_{1};
    Sequence next_execution_{1};
};

}  // namespace order_book
