// FILE: src/wal_integration.cpp
// Small integration glue to expose a global WAL instance used by server and orderbook
#include "../include/wal.h"

// instantiate global WAL
WAL global_wal("./data/wal.jsonl");