# Design

The engine is an in-memory limit order book for one instrument. Construction allocates every buffer the hot path needs. Event processing only mutates those buffers.

## Order storage

Resting orders live in a contiguous slot pool sized to `max_orders`. Free slots are a stack of indices. Each occupied slot holds id, side, type, price, remaining quantity, arrival sequence, and intrusive `prev` / `next` indices for the FIFO at its price.

A price level is another pool slot: price, side, aggregate quantity, head, tail, and order count. Bid and ask indexes are arrays of level indices kept best-first (bids descending, asks ascending). Lookup and insert use binary search; matching always starts at index 0.

## Order-id index

An open-addressed hash table maps `OrderId` to slot index. Table size is a power of two at least twice the order capacity. Probing is linear. Deletes leave tombstones; when occupied plus tombstone entries exceed half the table, the engine rebuilds into a second preallocated buffer. Average lookup, insert, and erase are O(1) with no per-order allocation.

## Matching

`submit` and aggressive `modify` walk the opposite side while prices cross. Each fill is recorded at the maker's price into a preallocated execution vector, which is cleared at the start of the next command. Fully filled makers are unlinked, removed from the hash table, and returned to the slot pool. A level whose FIFO becomes empty is removed from its index array and returned to the level pool.

Before mutating, the engine forecasts consumed orders and levels. If a limit remainder would need a slot or a new level that capacity cannot supply, the command is rejected and the book is left unchanged.

## Sequences and determinism

`next_arrival_` increments when a limit rests or when a modify loses time priority. `next_execution_` increments once per fill. Checksums mix arrival and execution counters with each level and each resting order in FIFO order, so two engines that see the same commands agree on both trades and book state.

## Concurrency and I/O

There is no internal locking, networking, persistence, or CLI. The library and the optional benchmark are the only binaries.
