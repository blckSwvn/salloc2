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
	uint32_t size_class;
	uint32_t blocks_used;
	struct page_header *next;
	struct page_header *prev;
};

#define BINS 8

struct master {
	struct header *free_list[BINS];
	size_t used;
	size_t free;
};

struct global_master {
	struct page_header *free_page_list[BINS];
	mtx_t free_lock[BINS];
	struct page_header *used_page_list[BINS];
	mtx_t used_lock[BINS];
};

static const uint32_t size_freelist[BINS] = {
	16, 32, 64, 128, 256, 512, 1024, 2048
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

	while(((char *)tail + size) <= ((char *)page + PAGE_SIZE)){
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

static void inline insert_page_to(struct page_header *page, struct page_header **head){
	page->prev = NULL;
	if(*head)(*head)->prev = page;
	page->next = *head;
	*head = page;
}

static void inline rm_page_from(struct page_header *page, struct page_header **head){
	if(page->prev)page->prev->next = page->next;
	if(page->next)page->next->prev = page->prev;
	if(*head == page)*head = page->next;
}

void *salloc(size_t len){
	call_once(&once, init);
	len = align(len);
	if(len >= size_freelist[BINS-1]){
		struct page_header *curr = mmap(NULL, len + sizeof(struct page_header), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		curr->size_class = len;
		// curr->blocks_used = 1;
		return ((char*)curr + sizeof(struct page_header));
	}

	uint32_t z; //first page INDEX of big enough size regardless of NULL
	for(z = 0; z < BINS; z++)
		if(size_freelist[z] >= len)
			break;

	uint32_t i = z; //first valid page INDEX of big enough size
	struct header *free = NULL;
	while(i < BINS){
		free = m.free_list[i];
		if(free){
			rm_from_free(free, i);
			get_header(free)->blocks_used++;
			break;
		}
		i++;
	}
	if(free)return (void *)free;

	if(global_m.free_page_list[z]){
		struct page_header *page = global_m.free_page_list[z];

		mtx_lock(&global_m.free_lock[z]);
		rm_page_from(page, &global_m.free_page_list[z]);
		mtx_unlock(&global_m.free_lock[z]);

		mtx_lock(&global_m.used_lock[z]);
		insert_page_to(page, &global_m.used_page_list[z]);
		mtx_unlock(&global_m.used_lock[z]);

		page->size_class = z;
		page->blocks_used = 1;
		pre_populate(page);
		return (void *)((char *)page + sizeof(struct page_header));
	} else {
		struct page_header *new = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		mtx_lock(&global_m.used_lock[z]);
		insert_page_to(new, &global_m.used_page_list[z]);
		mtx_unlock(&global_m.used_lock[z]);
		if(!new)return NULL;
		new->size_class = z;
		new->blocks_used = 1;
		pre_populate(new);
		return (void *)((char *)new+sizeof(struct page_header));
	}
}

void sfree(void *ptr){
	struct header *block = ptr;
	struct page_header *page = get_header(ptr);

	if(page->size_class >= size_freelist[BINS-1]){
		munmap(page, page->size_class + sizeof(struct page_header));
		return;
	}

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
		mtx_lock(&global_m.used_lock[page->size_class]);
		rm_page_from(page, &global_m.used_page_list[page->size_class]);
		mtx_unlock(&global_m.used_lock[page->size_class]);

		mtx_lock(&global_m.free_lock[page->size_class]);
		insert_page_to(page, &global_m.free_page_list[page->size_class]);
		mtx_unlock(&global_m.free_lock[page->size_class]);
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
	memcpy(new, ptr, size_freelist[get_header(ptr)->size_class]);
	return new;
}
