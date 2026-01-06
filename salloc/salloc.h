#include <stddef.h>
#define SALLOC_H
#ifdef SALLOC_H

void *salloc(size_t len);
void *sfree(void *ptr);

#endif
