#include "salloc/salloc.h"
#include "sys/types.h"
#include <stdio.h>
#include <unistd.h>

int main(){
	void *ptr = salloc(12);
	void *ptr2 = salloc(12);
	sfree(ptr);
	sfree(ptr2);
	void *ptr3 = salloc(12);
	sfree(ptr3);
	return 0;
}
