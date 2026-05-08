#ifndef HW4_DISPATCHER_H
#define HW4_DISPATCHER_H

#include "common.h"

/* Dispatcher process: Region A'dan alir, level'a gore Region B'ye ve priority ise Region D'ye dagitir. */
void run_dispatcher_process(dispatcher_process_args_t *args);

#endif
