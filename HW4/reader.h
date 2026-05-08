#ifndef HW4_READER_H
#define HW4_READER_H

#include "common.h"

/* Reader process: log dosyasini thread'lerle okuyup parse edilen entry'leri Region A'ya yollar. */
void run_reader_process(reader_process_args_t *args);

#endif
