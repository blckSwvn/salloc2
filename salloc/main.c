#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <threads.h>

#define DEBUG 1

struct header {
	struct header *next;
	struct header *prev;
};

#define BINS 128

struct page_header {
	struct page_header *next;
	struct page_header *prev;
	struct header *remote_head; //while page in use
	void *owner;
	struct header *head;
	size_t size_index; //index except when size_index > size_freelist[BINS-1] then its raw size
	uint32_t blocks_used;
	alignas(64) mtx_t remote_lock;
	_Atomic uint32_t remote_frees;
};

struct global_master {
	struct page_header *free_page_list[BINS];
	mtx_t free_lock[BINS];
};


static const size_t max_slab = 2048;
// static const size_t size_freelist[BINS] = {
// 	16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048
// };

thread_local struct page_header *freelist[BINS];

struct global_master global_m = {
	.free_page_list = {NULL},
};

static once_flag once = ONCE_FLAG_INIT;
static void init(void){
	for(uint32_t i = 0; i < BINS; i++){
		mtx_init(&global_m.free_lock[i], mtx_plain);
	}
}

#define PAGE_SIZE 4096

size_t align(size_t len){
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
	uint32_t size = (page->size_index+1)<<4;
	struct header *block = (struct header *)((char *)page + size + sizeof(struct page_header)); //skips first block since its returned imediately
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
	insert_page_to(page, &freelist[(page->size_index)]);
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
	if(len > max_slab){
		struct page_header *curr = mmap(NULL, len + sizeof(struct page_header), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		curr->size_index = len;
		// curr->blocks_used = 1;
		curr->owner = *freelist;
		return ((char*)curr + sizeof(struct page_header));
	}

	uint32_t z = (len/16)-1; //first page INDEX of big enough size regardless of NULL

	uint32_t i = z; //first valid block INDEX of big enough size
	struct header *free = NULL;
	while(i < BINS){
		if(freelist[i]){
			if(freelist[i]->head){
				free = freelist[i]->head;
				rm_from_free(free, freelist[i]);
				freelist[i]->blocks_used++;
				break;
			} else { 
				if(freelist[i]->remote_head){
					mtx_lock(&freelist[i]->remote_lock);
					struct header *remote = freelist[i]->remote_head;
					if(remote){
						free = remote;
						remote = remote->next;
						while(remote){
							struct header *next = remote->next;
							insert_free(remote, freelist[i]);
							remote = next;
						}
						freelist[i]->remote_head = NULL;
						atomic_store(&freelist[i]->remote_frees, 0);
						freelist[i]->blocks_used++;
						mtx_unlock(&freelist[i]->remote_lock);
						break;
					}
					mtx_unlock(&freelist[i]->remote_lock);
				}
			}
		}
		i++;
	}
	if(free)return (void *)free;

	

	mtx_lock(&global_m.free_lock[z]);
	struct page_header *page = global_m.free_page_list[z];
	if(page) rm_page_from(page, &global_m.free_page_list[z]);
	mtx_unlock(&global_m.free_lock[z]);

	if(page){
		// page->size_index = z; shouldnt be changed its already set in
		page->owner = *freelist;
		page->remote_frees = 0;
		page->remote_head = NULL;
		page->blocks_used = 1;
		void *temp = page->head;
		insert_page_to(page, &freelist[z]);
		page->head = page->head->next;
		return temp;
	} else {
		struct page_header *new = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		madvise(new, PAGE_SIZE, MADV_NOHUGEPAGE);
		if(!new)return NULL;

		new->owner = *freelist;
		new->remote_frees = 0;
		new->remote_head = NULL;
		new->blocks_used = 1;
		new->size_index = z;
		insert_page_to(new, &freelist[z]);
		pre_populate(new);
		mtx_init(&new->remote_lock, mtx_plain);
		return (void *)((char *)new+sizeof(struct page_header));
	}
}


void sfree(void *ptr){
	struct header *block = ptr;
	struct page_header *page = get_header(ptr);
	if(page->owner != *freelist){ //remote free
#ifdef DEBUG
		printf("remote free!\n");
#endif
		mtx_lock(&page->remote_lock);
		if(page->size_index > max_slab){
			mtx_destroy(&page->remote_lock);
			munmap(page, page->size_index + sizeof(struct page_header));
			return;
		}
		if(page->remote_head)page->remote_head->prev = block;
		block->next = page->remote_head;
		block->prev = NULL;
		page->remote_head = block;
		atomic_fetch_add(&page->remote_frees, +1);
		mtx_unlock(&page->remote_lock);
		return;
	} 

	if(page->size_index > max_slab){
		mtx_destroy(&page->remote_lock);
		munmap(page, page->size_index + sizeof(struct page_header));
		return;
	}

	page->blocks_used--;
	insert_free(block, page);

	if(!page->blocks_used || page->blocks_used - atomic_load(&page->remote_frees) == 0){
		struct header *remote = page->remote_head;
		while(remote){
			struct header *next = remote->next;
			insert_free(remote, page);
			remote = next;
		}
		rm_page_from(page, &freelist[page->size_index]);

		mtx_lock(&global_m.free_lock[page->size_index]);
		insert_page_to(page, &global_m.free_page_list[page->size_index]);
		mtx_unlock(&global_m.free_lock[page->size_index]);
		return;
	}

	return;
}

void *srealloc(void *ptr, size_t len){
	len = align(len);
	struct page_header *page = get_header(ptr);
	if((page->size_index+15)/16-1 >= len)
		return ptr;
	void *new = salloc(len);
	if(!new)return NULL;
	memcpy(new, ptr, (page->size_index+15)/16-1);
	return new;
}

