#ifndef HW4_WATCHDOG_H
#define HW4_WATCHDOG_H

#include "common.h"

/* Parent icindeki watchdog thread: reader heartbeat pipe'larini izler ve progress raporu basar. */
void *watchdog_thread_main(void *arg);

#endif
