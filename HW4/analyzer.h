#ifndef HW4_ANALYZER_H
#define HW4_ANALYZER_H

#include "common.h"

/* Analyzer process: tek bir log level'inin Region B kuyrugunu worker thread'lerle analiz eder. */
void run_analyzer_process(analyzer_process_args_t *args);

#endif
