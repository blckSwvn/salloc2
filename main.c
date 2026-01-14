#include "salloc/salloc.h"
#include <stdio.h>
#include <string.h>

struct foo {
	int id;
	char *string;
	struct foo *next;
	struct foo *prev;
};

int main(){
	printf("sizeof foo:%zu\n",sizeof(struct foo));
	struct foo *node1 = salloc(sizeof(struct foo));
	memset(&node1->id, 1, sizeof(int));
	node1->string = salloc(8);
	node1->string = "i am 1!";

	struct foo *node2 = salloc(sizeof(struct foo));
	memset(&node2->id, 2, sizeof(int));
	node2->string = salloc(8);
	node2->string = "i am 2!";


	printf("%u, %s\n", node1->id, node1->string);
	printf("%u, %s\n", node2->id, node2->string);
}
