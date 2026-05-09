#ifndef ORDER_QUEUE_H
#define ORDER_QUEUE_H

#include <stddef.h>

#define NAME_MAX_LEN 32
#define PRIORITY_MAX_LEN 8

typedef enum {
    PRIORITY_EXPRESS = 1,
    PRIORITY_STANDARD = 2,
    PRIORITY_ECONOMY = 3
} Priority;

typedef struct {
    int id;
    char recipient[NAME_MAX_LEN + 1];
    Priority priority;
    int duration_units;
} Order;

typedef struct OrderNode {
    Order order;
    struct OrderNode *next;
} OrderNode;

typedef struct {
    OrderNode *head;
    size_t size;
} OrderQueue;

void queue_init(OrderQueue *queue);
int queue_push(OrderQueue *queue, const Order *order);
int queue_pop(OrderQueue *queue, Order *removed_order);
size_t queue_size(const OrderQueue *queue);
void queue_clear(OrderQueue *queue);
const char *priority_to_string(Priority priority);
int parse_priority(const char *text, Priority *priority);

#endif
