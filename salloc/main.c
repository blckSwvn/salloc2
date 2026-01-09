#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <threads.h>
#include <stdbool.h>

#define DEBUG 1

struct header {
	struct header *next;
	struct header *prev;
};

struct page_header {
	struct page_header *next;
	struct page_header *prev;
	struct header *head;
	size_t size_index; //index except when size_index > size_freelist[BINS-1] then its raw size
	uint32_t blocks_used;
};

#define BINS 15

struct master {
	struct page_header *free_list[BINS];
	size_t used;
	size_t free;
};

struct global_master {
	struct page_header *free_page_list[BINS];
	struct page_header *used_page_list[BINS];
	mtx_t free_lock[BINS];
	mtx_t used_lock[BINS];
};

static const size_t size_freelist[BINS] = {
	16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048
};

struct global_master global_m = {
	.free_page_list = {NULL},
	.used_page_list = {NULL},
};

static once_flag once = ONCE_FLAG_INIT;
static void init(void){
	for(uint32_t i = 0; i < BINS; i++){
		mtx_init(&global_m.free_lock[i], mtx_plain);
		mtx_init(&global_m.used_lock[i], mtx_plain);
	}
}

thread_local struct master m = {
	.free_list = {NULL},
	.free = 0,
	.used = 0,
};

#define PAGE_SIZE 4096

size_t align(size_t len){
	if(len < sizeof(struct header)){
		len = sizeof(struct header);
		return len;
	}
	if(len < size_freelist[BINS-1]){
		uint32_t i = 0;
		while(i < BINS){
			if(size_freelist[i] >= len){
				return size_freelist[i];
			}
			i++;
		}
	}
	return (len + 15) & ~((size_t)15);
}

static inline struct page_header *get_header(void *ptr){
	uintptr_t header = (uintptr_t)ptr;
	header &= ~((uintptr_t)0xFFF);
	return (void *)header;
}

static void inline insert_page_to(struct page_header *page, struct page_header **head){
	page->prev = NULL;
	if(*head)(*head)->prev = page;
	page->next = *head;
	*head = page;
}

void pre_populate(struct page_header *page){
	uint32_t size = size_freelist[page->size_index];
	struct header *block = (struct header *)((char *)page + size + sizeof(struct page_header)); //must skip first block
	void *page_end = ((char *)page + PAGE_SIZE);

	struct header *head = NULL;

	while(((char *)block + size) <= ((char *)page_end)){
		block->prev = NULL;
		if(head){
			block->next = head;
			head->prev = block;
		} else block->next = NULL;

		head = block;
		block = (struct header *)((char *)block + size);
	}
	page->head = head;
	insert_page_to(page, &m.free_list[(page->size_index)]);
}

static void inline rm_from_free(struct header *free, struct page_header *page){
	if(free->prev)free->prev->next = free->next;
	if(free->next)free->next->prev = free->prev;
	if(page->head == free)page->head = free->next;
}

static void inline insert_free(struct header *free, struct page_header *page){
	if(page->head)page->head->prev = free;
	free->next = page->head;
	page->head = free;
	free->prev = NULL;
}


static void inline rm_page_from(struct page_header *page, struct page_header **head){
	if(page->prev)page->prev->next = page->next;
	if(page->next)page->next->prev = page->prev;
	if(*head == page)*head = page->next;
}


void *salloc(size_t len){
	call_once(&once, init);
	len = align(len);
	if(len > size_freelist[BINS-1]){
		struct page_header *curr = mmap(NULL, len + sizeof(struct page_header), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		curr->size_index = len;
		// curr->blocks_used = 1;
		return ((char*)curr + sizeof(struct page_header));
	}

	uint32_t z; //first page INDEX of big enough size regardless of NULL
	for(z = 0; z < BINS; z++)
		if(size_freelist[z] >= len)
			break;

	uint32_t i = z; //first valid block INDEX of big enough size
	struct header *free = NULL;
	while(i < BINS){
		if(m.free_list[i] && m.free_list[i]->head){
			free = m.free_list[i]->head;
			struct page_header *page = get_header(free);
			rm_from_free(free, page);
			page->blocks_used++;
			break;
		}
		i++;
	}
	if(free)return (void *)free;

	mtx_lock(&global_m.free_lock[z]);
	struct page_header *page = global_m.free_page_list[z];
	if(page) rm_page_from(page, &global_m.free_page_list[z]);
	mtx_unlock(&global_m.free_lock[z]);

	if(page){
	mtx_lock(&global_m.used_lock[z]);
	insert_page_to(page, &global_m.used_page_list[z]);
	mtx_unlock(&global_m.used_lock[z]);

	// page->size_class = z; shouldnt be changed its already set in
	page->blocks_used = 1;
	void *temp = page->head;
	insert_page_to(page, &m.free_list[z]);
	page->head = page->head->next;
	return temp;
	} else {
		struct page_header *new = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		mtx_lock(&global_m.used_lock[z]);
		insert_page_to(new, &global_m.used_page_list[z]);
		mtx_unlock(&global_m.used_lock[z]);
		if(!new)return NULL;
		new->size_index = z;
		new->blocks_used = 1;
		pre_populate(new);
		return (void *)((char *)new+sizeof(struct page_header));
	}
}


void sfree(void *ptr){
	struct header *block = ptr;
	struct page_header *page = get_header(ptr);

	if(page->size_index > size_freelist[BINS-1]){
		munmap(page, page->size_index + sizeof(struct page_header));
		return;
	}


	insert_free(block, page);

	page->blocks_used--;
	if(!page->blocks_used){
		rm_page_from(page, &m.free_list[page->size_index]);
		mtx_lock(&global_m.used_lock[page->size_index]);
		rm_page_from(page, &global_m.used_page_list[page->size_index]);
		mtx_unlock(&global_m.used_lock[page->size_index]);

		mtx_lock(&global_m.free_lock[page->size_index]);
		insert_page_to(page, &global_m.free_page_list[page->size_index]);
		mtx_unlock(&global_m.free_lock[page->size_index]);
		return;
	}

	return;
}

void *srealloc(void *ptr, size_t len){
	len = align(len);
	void *new = salloc(len);
	if(!new)return NULL;
	memcpy(new, ptr, size_freelist[get_header(ptr)->size_index]);
	return new;
}
