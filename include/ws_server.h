#include "../include/order_book.h"

void setup_ws_server(int port);
void broadcast_order_book(const OrderBook &book);
void broadcast_trade(const Trade &trade);
