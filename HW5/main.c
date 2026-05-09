#define _POSIX_C_SOURCE 200809L

#include "order_queue.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DELIVERY_UNIT_MS 500

typedef struct {
    int delivered_order_count;
    long delivered_time_ms;
} CourierStats;


typedef struct {
    int courier_id;
} CourierArg;


static OrderQueue pending_order_queue;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t courier_wakeup_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t shift_finished_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t stdout_lock = PTHREAD_MUTEX_INITIALIZER;

static _Atomic int total_completed_orders = 0;
static _Atomic int total_cancelled_orders = 0;
static _Atomic long total_completed_delivery_time = 0;

static volatile sig_atomic_t sigint_flag = 0;
static int shift_stopping = 0;
static int active_delivery_count = 0;
static int valid_order_count = 0;
static CourierStats *courier_stats_list = NULL;


static void print_log_line(const char *fmt, ...)
{
    va_list args;

    pthread_mutex_lock(&stdout_lock);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
    pthread_mutex_unlock(&stdout_lock);
}


static void sigint_handler(int signo)
{
    (void)signo;
    sigint_flag = 1;
}


static void print_usage(const char *program)
{
    fprintf(stderr, "Usage: %s -n <num_couriers> -i <orders.txt> -s <stats.txt>\n", program);
}


static int parse_positive_int(const char *text, int *value)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0) {
        return 0;
    }
    if (end == text || *end != '\0') {
        return 0;
    }
    if (parsed < 1 || parsed > 2147483647L) {
        return 0;
    }

    *value = (int)parsed;
    return 1;
}


static int is_valid_recipient_name(const char *name)
{
    size_t i;

    if (name[0] == '\0') {
        return 0;
    }

    for (i = 0; name[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)name[i];
        int is_upper_case = (ch >= 'A' && ch <= 'Z');
        int is_lower_case = (ch >= 'a' && ch <= 'z');
        int is_number = (ch >= '0' && ch <= '9');
        int is_underscore = (ch == '_');

        if (!is_upper_case && !is_lower_case && !is_number && !is_underscore) {
            return 0;
        }
    }

    return 1;
}


static int load_orders_from_file(const char *path)
{
    FILE *file = fopen(path, "r");
    char line[256];

    if (file == NULL) {
        perror(path);
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        Order order;
        char priority_text[PRIORITY_MAX_LEN + 1];
        char extra;
        int fields;

        fields = sscanf(line, " %d %32s %8s %d %c",
                        &order.id, order.recipient, priority_text, &order.duration_units, &extra);
        if (fields != 4) {
            continue;
        }
        if (order.id < 1 || order.duration_units < 1) {
            continue;
        }
        if (!is_valid_recipient_name(order.recipient)) {
            continue;
        }
        if (!parse_priority(priority_text, &order.priority)) {
            continue;
        }
        if (queue_push(&pending_order_queue, &order) != 0) {
            fclose(file);
            fprintf(stderr, "Memory error: order queue\n");
            return -1;
        }
        valid_order_count++;
    }

    fclose(file);
    return 0;
}


static void print_queued_orders(void)
{
    OrderNode *current_node;

    for (current_node = pending_order_queue.head; current_node != NULL; current_node = current_node->next) {
        print_log_line("[CARGOGTU] ORDER_QUEUED id=%d recipient=%s priority=%s duration=%d\n",
                       current_node->order.id, current_node->order.recipient,
                       priority_to_string(current_node->order.priority),
                       current_node->order.duration_units);
    }
}


static void simulate_delivery_time(int duration_units)
{
    struct timespec remaining_sleep_time;

    remaining_sleep_time.tv_sec = (duration_units * DELIVERY_UNIT_MS) / 1000;
    remaining_sleep_time.tv_nsec = (long)((duration_units * DELIVERY_UNIT_MS) % 1000) * 1000000L;

    while (nanosleep(&remaining_sleep_time, &remaining_sleep_time) == -1 && errno == EINTR) {
    }
}


static void *courier_thread(void *data)
{
    CourierArg *courier_arg = data;
    int courier_index = courier_arg->courier_id - 1;

    for (;;) {
        Order selected_order;

        pthread_mutex_lock(&queue_lock);
        while (queue_size(&pending_order_queue) == 0 && !shift_stopping && active_delivery_count > 0) {
            print_log_line("[COURIER-%d] WAITING\n", courier_arg->courier_id);
            pthread_cond_wait(&courier_wakeup_cond, &queue_lock);
        }

        if (shift_stopping || !queue_pop(&pending_order_queue, &selected_order)) {
            if (!shift_stopping && active_delivery_count == 0) {
                shift_stopping = 1;
                pthread_cond_broadcast(&courier_wakeup_cond);
                pthread_cond_signal(&shift_finished_cond);
            }
            pthread_mutex_unlock(&queue_lock);
            print_log_line("[COURIER-%d] SHIFT_OVER\n", courier_arg->courier_id);
            return NULL;
        }

        active_delivery_count++;
        print_log_line("[COURIER-%d] DELIVERY_START id=%d recipient=%s priority=%s\n",
                       courier_arg->courier_id, selected_order.id, selected_order.recipient,
                       priority_to_string(selected_order.priority));
        pthread_mutex_unlock(&queue_lock);

        simulate_delivery_time(selected_order.duration_units);

        courier_stats_list[courier_index].delivered_order_count++;
        courier_stats_list[courier_index].delivered_time_ms +=
            (long)selected_order.duration_units * DELIVERY_UNIT_MS;
        atomic_fetch_add(&total_completed_orders, 1);
        atomic_fetch_add(&total_completed_delivery_time,
                         (long)selected_order.duration_units * DELIVERY_UNIT_MS);

        print_log_line("[COURIER-%d] DELIVERY_COMPLETE id=%d recipient=%s duration=%ldms\n",
                       courier_arg->courier_id, selected_order.id, selected_order.recipient,
                       (long)selected_order.duration_units * DELIVERY_UNIT_MS);

        pthread_mutex_lock(&queue_lock);
        active_delivery_count--;
        if ((queue_size(&pending_order_queue) == 0 && active_delivery_count == 0) || shift_stopping) {
            shift_stopping = 1;
            pthread_cond_broadcast(&courier_wakeup_cond);
            pthread_cond_signal(&shift_finished_cond);
        } else if (queue_size(&pending_order_queue) > 0) {
            pthread_cond_broadcast(&courier_wakeup_cond);
        }
        pthread_mutex_unlock(&queue_lock);
    }
}


static void cancel_pending_orders_after_sigint(void)
{
    Order cancelled_order;
    size_t pending_order_count;

    pthread_mutex_lock(&queue_lock);
    if (shift_stopping) {
        pthread_mutex_unlock(&queue_lock);
        return;
    }

    pending_order_count = queue_size(&pending_order_queue);
    shift_stopping = 1;
    print_log_line("[CARGOGTU] SIGINT_RECEIVED pending_orders=%zu\n", pending_order_count);
    while (queue_pop(&pending_order_queue, &cancelled_order)) {
        atomic_fetch_add(&total_cancelled_orders, 1);
        print_log_line("[CARGOGTU] ORDER_CANCELLED id=%d recipient=%s priority=%s\n",
                       cancelled_order.id, cancelled_order.recipient,
                       priority_to_string(cancelled_order.priority));
    }

    pthread_cond_broadcast(&courier_wakeup_cond);
    if (active_delivery_count == 0) {
        pthread_cond_signal(&shift_finished_cond);
    }
    pthread_mutex_unlock(&queue_lock);
}


static void wait_for_shift_to_finish(void)
{
    pthread_mutex_lock(&queue_lock);
    while (!shift_stopping || active_delivery_count > 0) {
        struct timespec wait_timeout;

        if (sigint_flag) {
            pthread_mutex_unlock(&queue_lock);
            cancel_pending_orders_after_sigint();
            pthread_mutex_lock(&queue_lock);
            continue;
        }

        clock_gettime(CLOCK_REALTIME, &wait_timeout);
        wait_timeout.tv_nsec += 100000000L;
        if (wait_timeout.tv_nsec >= 1000000000L) {
            wait_timeout.tv_sec++;
            wait_timeout.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&shift_finished_cond, &queue_lock, &wait_timeout);
    }
    pthread_mutex_unlock(&queue_lock);
}


static int write_summary_file(const char *path, int num_couriers)
{
    FILE *file = fopen(path, "w");
    int completed_order_count = atomic_load(&total_completed_orders);
    int cancelled_order_count = atomic_load(&total_cancelled_orders);
    long completed_time_sum = atomic_load(&total_completed_delivery_time);
    long average_time_per_order;
    int courier_index;

    if (file == NULL) {
        perror(path);
        return -1;
    }

    if (completed_order_count == 0) {
        average_time_per_order = 0;
    } else {
        average_time_per_order = completed_time_sum / completed_order_count;
    }

    fprintf(file, "SHIFT_SUMMARY\n");
    fprintf(file, "Total orders    : %d\n", valid_order_count);
    fprintf(file, "Completed       : %d\n", completed_order_count);
    fprintf(file, "Cancelled       : %d\n", cancelled_order_count);
    fprintf(file, "Total time      : %ldms\n", completed_time_sum);
    fprintf(file, "Avg per order   : %ldms\n\n", average_time_per_order);
    fprintf(file, "COURIER_STATS\n");
    for (courier_index = 0; courier_index < num_couriers; courier_index++) {
        fprintf(file, "Courier-%d  completed=%d  total_time=%ldms\n",
                courier_index + 1, courier_stats_list[courier_index].delivered_order_count,
                courier_stats_list[courier_index].delivered_time_ms);
    }

    fclose(file);
    return 0;
}


int main(int argc, char **argv)
{
    int command_option;
    int num_couriers = 0;
    const char *input_path = NULL;
    const char *stats_path = NULL;
    pthread_t *courier_threads = NULL;
    CourierArg *courier_args = NULL;
    struct sigaction sigint_action;
    int exit_code = EXIT_SUCCESS;
    int courier_index;

    while ((command_option = getopt(argc, argv, "n:i:s:")) != -1) {
        switch (command_option) {
        case 'n':
            if (!parse_positive_int(optarg, &num_couriers)) {
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            break;
        case 'i':
            input_path = optarg;
            break;
        case 's':
            stats_path = optarg;
            break;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (num_couriers < 1 || input_path == NULL || stats_path == NULL || optind != argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    memset(&sigint_action, 0, sizeof(sigint_action));
    sigint_action.sa_handler = sigint_handler;
    sigemptyset(&sigint_action.sa_mask);
    if (sigaction(SIGINT, &sigint_action, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    queue_init(&pending_order_queue);
    if (load_orders_from_file(input_path) != 0) {
        queue_clear(&pending_order_queue);
        return EXIT_FAILURE;
    }
    print_log_line("[CARGOGTU] SHIFT_START couriers=%d orders=%d\n", num_couriers, valid_order_count);
    print_queued_orders();

    courier_threads = calloc((size_t)num_couriers, sizeof(pthread_t));
    courier_args = calloc((size_t)num_couriers, sizeof(CourierArg));
    courier_stats_list = calloc((size_t)num_couriers, sizeof(CourierStats));
    if (courier_threads == NULL || courier_args == NULL || courier_stats_list == NULL) {
        fprintf(stderr, "Memory error: courier data\n");
        free(courier_threads);
        free(courier_args);
        free(courier_stats_list);
        queue_clear(&pending_order_queue);
        return EXIT_FAILURE;
    }

    for (courier_index = 0; courier_index < num_couriers; courier_index++) {
        courier_args[courier_index].courier_id = courier_index + 1;
        if (pthread_create(&courier_threads[courier_index], NULL, courier_thread,
                           &courier_args[courier_index]) != 0) {
            perror("pthread_create");
            cancel_pending_orders_after_sigint();
            num_couriers = courier_index;
            exit_code = EXIT_FAILURE;
            break;
        }
    }

    wait_for_shift_to_finish();

    for (courier_index = 0; courier_index < num_couriers; courier_index++) {
        pthread_join(courier_threads[courier_index], NULL);
    }

    print_log_line("[CARGOGTU] SHIFT_END completed=%d cancelled=%d total_time=%ldms\n",
                   atomic_load(&total_completed_orders), atomic_load(&total_cancelled_orders),
                   atomic_load(&total_completed_delivery_time));

    if (write_summary_file(stats_path, num_couriers) != 0) {
        exit_code = EXIT_FAILURE;
    }

    print_log_line("[CARGOGTU] SHUTDOWN_COMPLETE\n");

    queue_clear(&pending_order_queue);
    free(courier_threads);
    free(courier_args);
    free(courier_stats_list);
    return exit_code;
}
