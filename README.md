# High-Performance C++ Matching Engine

This repository contains a **high-performance cryptocurrency matching engine** built in C++. It implements core trading functionalities based on **REG NMS-inspired principles** of price-time priority and internal order protection.

The system is designed for **high throughput and low latency** by decoupling order matching from slower I/O operations (like disk writing and WebSocket broadcasting) through asynchronous queues and dedicated writer threads.

The project includes the core matching engine, a persistent **Write-Ahead Log (WAL)** for state recovery, a **REST API** for order management, a real-time **WebSocket API** for market data, and a functional frontend UI to visualize the system in action.

## 🚀 Features

- **Core Matching Logic**: Implements strict price-time priority (FIFO) for all orders.

- **Order Type Handling**:
  - **Standard**: Market, Limit
  - **Time-in-Force**: Immediate-Or-Cancel (IOC), Fill-Or-Kill (FOK)
  - **Advanced Order Types (Bonus)**:
    - Stop-Loss (Market)
    - Stop-Limit

- **High-Performance Architecture**:
  - Asynchronous, thread-pooled broadcast queue for non-blocking WebSocket updates.
  - Asynchronous Write-Ahead Log (WAL) for persistent logging without blocking the request thread.
  - Fine-grained locking using `std::shared_mutex` for concurrent reads of the order book.

- **Persistence & Recovery (Bonus)**: All orders, trades, and cancels are written to a WAL. The engine replays this log on startup to fully restore the order book state.

- **Real-time Data APIs**:
  - **REST API** (via cpp-httplib) for order submission, cancellation, and system stats.
  - **WebSocket API** for streaming L2 order book depth and live trade executions.

- **Fee Model (Bonus)**: Implements a simple maker-taker fee model, with fees calculated and included in trade reports.

- **Testing**: Includes unit tests for core matching logic and a high-load benchmark client.

- **Frontend**: A simple HTML/JS frontend is provided to visualize the order book, trade feed, and submit orders.

## 📊 Performance

The engine is designed for **high throughput**. Benchmarking was performed using the included benchmark tool (`tests/benchmark.cpp`).

**Benchmark Results**:

```
========================================
  Benchmark Results
========================================
Total Orders:      128000
Successful:        128000
Failed:            0
Success Rate:      100%
----------------------------------------
Duration:          85.876 seconds
Throughput:        1490.52 orders/sec
----------------------------------------
Avg Latency:       21.1665 ms
Min Latency:       0.2273 ms
Max Latency:       3077.38 ms
========================================
```
## 🏗️ System Architecture

### 1. Data Structures

The core of the engine is the `OrderBook` class. It uses two `std::map`s to manage bids and asks, ensuring price priority.

```cpp
std::map<long long, std::deque<Order>, std::greater<long long>> bids_
std::map<long long, std::deque<Order>> asks_
```

This design choice provides:

- **Price Priority**: `std::map` automatically sorts orders by price (key). `std::greater` is used for bids to sort them from highest to lowest.
- **Time Priority**: `std::deque<Order>` at each price level acts as a FIFO queue, ensuring orders are matched based on their arrival time.

### 2. Concurrency & Asynchronous I/O

To achieve high performance, the HTTP request thread (which handles order submission) is decoupled from all slow I/O operations.

**Order Submission** (`/orders`):

1. An HTTP request is received by `server.cpp`.
2. The order is placed in the `global_wal` queue (very fast, in-memory push).
3. The order is given to the `OrderBook` for matching. This is a blocking, in-memory operation.
4. Any resulting trades are also pushed to the `global_wal` queue and the `g_broadcast_queue`.
5. The HTTP response is sent immediately to the client.

**Asynchronous WAL**:

- A dedicated thread in the `WAL` class consumes the `queue_`.
- It batches writes and flushes them to the `data/wal.jsonl` file, ensuring persistence without impacting the order matching latency.

**Asynchronous Broadcasting**:

- A thread pool in `BroadcastQueue` consumes the message queue (`queue_`).
- Worker threads format the JSON for trades and order book updates.
- The `WebSocketServer` then broadcasts this data to all connected clients.

### 3. Persistence & Recovery

On startup, `main.cpp` calls `global_wal.replay()`. This function reads the entire `wal.jsonl` file, reconstructs the state of all open orders (handling creations, trades, and cancels), and repopulates the `g_order_books` map before the server starts accepting connections.

## 🔌 API Specification

The server runs two services: a **REST API** (default port 8080) and a **WebSocket API** (default port 9002).

### REST API (Port 8080)

---

#### • **POST** `/orders`

Submits a new order.

- **Body (JSON)**:

```json
{
  "symbol": "BTC-USDT",
  "order_type": "limit", // "limit", "market", "ioc", "fok"
  "side": "buy",
  "quantity": 1.5,
  "price": 50000.0 // Required for limit, ioc, fok
}
```

- **Success Response (200 OK)**:

```json
{
  "order": {
    "order_id": "ORD-123",
    "status": "open", // "filled", "partially_filled", "cancelled"
    /* ... other order details ... */
  },
  "trades": [ /* Array of trade execution reports, if any */ ],
  "filled_quantity": 0,
  "remaining_quantity": 1500000
}
```

---

#### • **POST** `/orders/stop`

Submits a new stop order.

- **Body (JSON)**:

```json
{
  "symbol": "BTC-USDT",
  "stop_type": "stop_limit", // "stop_loss" or "stop_limit"
  "side": "sell",
  "quantity": 0.5,
  "trigger_price": 49000.0,
  "limit_price": 48950.0 // Required for "stop_limit"
}
```

- **Success Response (200 OK)**:

```json
{
  "status": "accepted",
  "stop_order_id": "STO-124",
  "order": { /* ... stop order details ... */ }
}
```

---

#### • **DELETE** `/orders/<order_id>`

Cancels an active limit or stop order by its ID.

- **Success Response (200 OK)**:

```json
{
  "cancelled": true,
  "order_id": "ORD-123",
  "symbol": "BTC-USDT",
  "timestamp": "..."
}
```

---

#### • **GET** `/orderbook/<symbol>`

Retrieves a snapshot of the order book depth.

- **Example**: `/orderbook/BTC-USDT`

- **Success Response (200 OK)**:

```json
{
  "symbol": "BTC-USDT",
  "bids": [ {"price": 49999.0, "quantity": 2.5, "total": 124997.5}, ... ],
  "asks": [ {"price": 50000.0, "quantity": 1.5, "total": 75000.0}, ... ],
  "timestamp": "..."
}
```

---

#### • **GET** `/trades/<symbol>`

Retrieves recent trades for a symbol (by replaying WAL).

- **Example**: `/trades/BTC-USDT`

---

#### • **GET** `/stats`

Retrieves global engine statistics.

---

#### • **GET** `/health`

Simple health check endpoint.

---

### WebSocket API (Port 9002)

Connect to `ws://localhost:9002`. The server pushes two types of messages:

---

#### • **Trade Execution Report**

Sent whenever a trade occurs.

- **Format**:

```json
{
  "type": "trade",
  "data": {
    "trade_id": "T-100",
    "symbol": "BTC-USDT",
    "price": 5000000, // Integer price
    "quantity": 500000, // Integer quantity
    "aggressor_side": "buy",
    "maker_order_id": "ORD-123",
    "taker_order_id": "ORD-124",
    "maker_fee": 5000,
    "taker_fee": 10000,
    "timestamp": "..."
  }
}
```

---

#### • **L2 Order Book Update**

Sent after any event that changes the order book (new order, cancel, trade).

Sent after any event that changes the order book (new order, cancel, trade).

- **Format**:

```json
{
  "type": "orderbook",
  "data": {
    "symbol": "BTC-USDT",
    "bids": [ {"price": 4999900, "quantity": 2500000}, ... ],
    "asks": [ {"price": 5000000, "quantity": 1500000}, ... ],
    "timestamp": 1678886400000
  }
}
```

---

## 🔧 Build and Run

The project uses **CMake**.

**Prerequisites** (Linux/macOS):

- `cmake >= 3.10`
- A C++17 compliant compiler (e.g., `g++`)

### 1. Download Dependencies

The required headers (`httplib.h`, `json.hpp`, `catch.hpp`) are included as vendor files.

```bash
# Create vendor directory and download headers
mkdir -p vendor data
cd vendor
curl -L -o httplib.h https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h
curl -L -o json.hpp https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp
curl -L -o catch.hpp https://raw.githubusercontent.com/catchorg/Catch2/devel/single_include/catch2/catch.hpp
cd ..
```

### 2. Build

```bash

```bash
mkdir -p build && cd build
cmake ..
cmake --build . -- -j4
```

### 3. Run Server

This starts the HTTP (8080) and WebSocket (9002) servers.

```bash
# Run from the 'build' directory
./matching_engine 8080 9002
```

### 4. Run Tests

```bash
# Run from the 'build' directory
./tests
```

or

```bash
ctest --output-on-failure
```

### 5. Run Benchmark

```bash
# Run from the 'build' directory
# Usage: ./benchmark [threads] [orders_per_thread]
./benchmark 8 10000
```

### 6. View Frontend

Open `frontend/index.html` in a web browser.