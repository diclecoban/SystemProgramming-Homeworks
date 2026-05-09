#include "order_queue.h"
#include <stdlib.h>
#include <string.h>

void queue_init(OrderQueue *queue)
{
    queue->head = NULL;
    queue->size = 0;
}


static int order_has_higher_priority(const Order *new_order, const Order *queued_order)
{
    if (new_order->priority != queued_order->priority) {
        return new_order->priority < queued_order->priority;
    }
    return new_order->id < queued_order->id;
}


int queue_push(OrderQueue *queue, const Order *order)
{
    OrderNode *new_node = malloc(sizeof(OrderNode));
    if (new_node == NULL) {
        return -1;
    }

    new_node->order = *order;
    new_node->next = NULL;

    if (queue->head == NULL || order_has_higher_priority(order, &queue->head->order)) {
        new_node->next = queue->head;
        queue->head = new_node;
    } else {
        OrderNode *scan_node = queue->head;

        while (scan_node->next != NULL &&
               !order_has_higher_priority(order, &scan_node->next->order)) {
            scan_node = scan_node->next;
        }

        new_node->next = scan_node->next;
        scan_node->next = new_node;
    }

    queue->size++;
    return 0;
}


int queue_pop(OrderQueue *queue, Order *removed_order)
{
    OrderNode *first_node;

    if (queue->head == NULL) {
        return 0;
    }

    first_node = queue->head;
    queue->head = first_node->next;
    queue->size--;
    *removed_order = first_node->order;
    free(first_node);
    return 1;
}


size_t queue_size(const OrderQueue *queue)
{
    return queue->size;
}


void queue_clear(OrderQueue *queue)
{
    Order ignored;

    while (queue_pop(queue, &ignored)) {
    }
}


const char *priority_to_string(Priority priority)
{
    switch (priority) {
    case PRIORITY_EXPRESS:
        return "EXPRESS";
    case PRIORITY_STANDARD:
        return "STANDARD";
    case PRIORITY_ECONOMY:
        return "ECONOMY";
    default:
        return "UNKNOWN";
    }
}


int parse_priority(const char *text, Priority *priority)
{
    if (strcmp(text, "EXPRESS") == 0) {
        *priority = PRIORITY_EXPRESS;
        return 1;
    }
    if (strcmp(text, "STANDARD") == 0) {
        *priority = PRIORITY_STANDARD;
        return 1;
    }
    if (strcmp(text, "ECONOMY") == 0) {
        *priority = PRIORITY_ECONOMY;
        return 1;
    }
    return 0;
}
