#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <threads.h>
#include <stdio.h>
#include <stdbool.h>

#define DEBUG 1

struct header {
	struct header *next;
	struct header *prev;
};

struct page_header {
	uint32_t size_class;
	uint32_t blocks_used;
};

#define BINS 8

struct master {
	struct header *free_list[BINS];
	size_t used;
	size_t free;
};

static const uint32_t size_freelist[BINS] = {
	16, 32, 64, 128, 256, 512, 1024, 2048
};

struct master m = {
	.free_list = {NULL},
	.free = 0,
	.used = 0
};

#define PAGE_SIZE 4096

size_t align(size_t len){
	if(len < sizeof(struct header))
		len = sizeof(struct header);
	return (len + 15) & ~((size_t)15);
}

static inline struct page_header *get_header(void *ptr){
	uintptr_t header = (uintptr_t)ptr;
	header &= ~((uintptr_t)0xFFF);
	return (void *)header;
}

void pre_populate(struct page_header *page){
	uint32_t size = size_freelist[page->size_class];
	struct header *tail = (struct header *)((char *)page + sizeof(struct page_header) + size);
	void *page_end = ((char *)page + PAGE_SIZE);

	while((void *)((char *)tail + size) <= page_end){
		if(m.free_list[page->size_class]){
			m.free_list[page->size_class]->prev = tail;
			tail->next = m.free_list[page->size_class];
		} else tail->next = NULL;
		m.free_list[page->size_class] = tail; 
		tail = (struct header *)((char *)tail + size);
	}
}

static inline void dump_freelist(){
	uint32_t i = 0;
	while(i < BINS){
		printf("size:%u\n",size_freelist[i]);
		struct header *curr = m.free_list[i];
		uint32_t count = 0;
		while(curr){
			printf("%p, next:%p, prev:%p\n", curr, curr->next, curr->prev);
			curr = curr->next;
			count++;
		}
		printf("count:%u\n", count);
		count = 0;
		printf("\n");
		i++;
	}
}

static void inline rm_from_free(struct header *free, uint32_t i){
	if(free->prev)free->prev->next = free->next;
	if(free->next)free->next->prev = free->prev;
	if(m.free_list[i] == free)m.free_list[i] = free->next;
}

void *salloc(size_t len){
	len = align(len);
	if(len >= size_freelist[BINS-1]){
		struct page_header *curr = mmap(NULL, len + sizeof(struct page_header), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		curr->size_class = len;
		// curr->blocks_used = 1;
		return ((char*)curr + sizeof(struct page_header));
	}

	uint32_t i = 0;
	uint32_t y = 0;
	struct header *free = NULL;
	while(i < BINS){
		free = m.free_list[i];
		while(free){
			if(size_freelist[get_header(free)->size_class]*2 >= len){
				rm_from_free(free, i);
				get_header(free)->blocks_used++;
				y = i;
				i = BINS;
				break;
			} else {
				free = free->next;
			}
		}
		i++;
	}
	if(free)return (void *)free;
	else {
		struct page_header *new = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if(!new)return NULL;
		new->size_class = y;
		new->blocks_used = 1;
		pre_populate(new);
		return (void *)((char *)new+sizeof(struct page_header));
	}
}

void sfree(void *ptr){
	struct header *block = ptr;
	struct page_header *page = get_header(ptr);
	page->blocks_used--;
	if(!page->blocks_used){
		uint32_t i = 0;
		void *tail = (void *)((char *)page + sizeof(struct page_header));
		void *page_end = (void *)((char *)page + PAGE_SIZE);
		size_t size = size_freelist[page->size_class];
		while((void *)((char *)tail + size) <= page_end){
			rm_from_free(tail, page->size_class);
			tail = (void *)((char *)tail + size);
			i++;
		}
		printf("%u\n",i);
		munmap((void *)page, PAGE_SIZE);
		return;
	}

	if(m.free_list[page->size_class])
		m.free_list[page->size_class]->prev = block;

	block->next = m.free_list[page->size_class];
	m.free_list[page->size_class] = block;
	return;
}

void *srealloc(void *ptr, size_t len){
	len = align(len);
	void *new = salloc(len);
	if(!new)return NULL;
	memcpy(ptr, new, size_freelist[get_header(ptr)->size_class]);
	return new;
}
