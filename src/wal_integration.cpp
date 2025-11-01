// FILE: src/wal_integration.cpp
// Small integration glue to expose a global WAL instance used by server and orderbook
#include "../include/wal.h"

// instantiate global WAL
// Removed the flush interval and timeout, as WAL is now fully asynchronous
WAL global_wal("./data/wal.jsonl");
