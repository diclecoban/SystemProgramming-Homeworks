#ifndef HW4_SHM_H
#define HW4_SHM_H

#include "common.h"

int init_shared_regions(shared_regions_t *shared, const program_options_t *opts);
void destroy_shared_regions(shared_regions_t *shared);

#endif
