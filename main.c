#include "salloc/salloc.h"
#include "sys/types.h"
#include <stdio.h>
#include <unistd.h>

int main(){
	void *ptr = salloc(32);
	void *ptr2 = salloc(32);
	sfree(ptr);
	sfree(ptr2);
	void *ptr3 = salloc(32);
	sfree(ptr3);
	return 0;
}
