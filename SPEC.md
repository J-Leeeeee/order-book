# Matching engine specification

Single instrument, one thread. Callers own synchronization. Orders have no participant identity, so self-trade prevention is out of scope.

## Types

- `Price`: signed integer ticks. Limit prices must be positive. Market orders ignore price.
- `Quantity`: unsigned integer size. Must be positive on submit and modify.
- `OrderId`: unique among **active** orders. After a fill or cancel, the id may be reused.
- `Sequence`: monotonic arrival or execution counter owned by the engine.
- `Side`: `Buy` or `Sell`.
- `OrderType`: `Limit` or `Market`.

Snapshots:

- `OrderSnapshot`: id, side, type, price, remaining quantity, arrival sequence.
- `LevelSnapshot`: price, aggregate remaining quantity, order count.

## API

```text
OrderBook(max_orders = 65536, max_levels = 4096)

submit(id, side, type, price, quantity) -> CommandResult
cancel(id) -> CommandResult
modify(id, price, quantity) -> CommandResult

best_bid() / best_ask()                  optional best price
bid_level(rank) / ask_level(rank)        rank 0 is best
active_order(id)                         optional snapshot
order_count(), bid_level_count(), ask_level_count()
bid_quantity(), ask_quantity()
max_orders(), max_levels()
invariants_hold(), checksum()
```

`CommandResult` contains `status`, `reason`, `unfilled_quantity`, and `executions`. The execution span is borrowed from engine storage and is valid until the next command on that `OrderBook`.

Modify quantity is the **new remaining quantity**, not a delta.

## Matching rules

1. Highest bid and lowest ask match first (price priority).
2. Orders at one price match in arrival sequence (time priority).
3. A trade executes at the **resting** order's price.
4. A market order takes available liquidity and never rests; leftover quantity expires and is reported as `unfilled_quantity`.
5. A limit remainder rests at its limit price.
6. Quantity reduction at the same price keeps arrival sequence.
7. Quantity increase or any price change assigns a new arrival sequence and is processed as an incoming limit order.
8. Zero quantity and non-positive limit prices are rejected with no book change.
9. Empty price levels and completed ids are removed immediately.
10. Identical command streams produce identical executions, sequences, and checksums.

## Rejections

Rejected commands do not mutate the book.

| Reason | When |
| --- | --- |
| `InvalidQuantity` | quantity is zero |
| `InvalidPrice` | limit or modify price is not positive |
| `InvalidSide` / `InvalidType` | enumerator is not a defined value |
| `DuplicateId` | submit id is already active |
| `UnknownId` | cancel or modify of an id that is not active |
| `OrderCapacity` | a limit remainder would need a slot and none is available |
| `LevelCapacity` | a remainder would need a new price level and none is available |
| `QuantityOverflow` | a resting remainder would overflow the aggregate quantity on its side |

Market orders never require an incoming slot or level. Fully filled limits likewise do not.
Aggregate quantities are exact: a command that would make `bid_quantity()` or `ask_quantity()` exceed
the `Quantity` range is rejected transactionally with no executions or book mutation.

## Queries

Depth rank 0 is the best price on that side. Missing ranks and unknown ids return empty optionals. After every accepted command the book is uncrossed: if both sides are present, best bid is strictly less than best ask.
