#include "salloc/salloc.h"
#include <stdint.h>
#include <stdio.h>

int main(){
	void *ptr = salloc(16);
	void *ptr2 = salloc(16);
	sfree(ptr);
	sfree(ptr2);
	void *ptr3 = salloc(16);
	return 0;
}
