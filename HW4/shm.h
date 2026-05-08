#ifndef HW4_SHM_H
#define HW4_SHM_H

#include "common.h"

/* Paylasimli region'lari mmap ile kurar; tum child process'ler bu adresleri fork sonrasinda kullanir. */
int init_shared_regions(shared_regions_t *shared, const program_options_t *opts);
/* Program sonunda mmap alanlarini ve process-shared senkronizasyon nesnelerini temizler. */
void destroy_shared_regions(shared_regions_t *shared);

#endif
