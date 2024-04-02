/* file.c: Implementation of memory backed file object (mmaped object). */
/* file.c: memory baked file 객체 구현 (mmaped object).*/

#include "vm/vm.h"
#include "userprog/process.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "devices/disk.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);
void do_munmap (void *addr);
void* do_mmap (void *addr, size_t length, int writable, struct file *file, off_t offset);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
/* file vm 초기화*/
void
vm_file_init (void) {
}

/* Initialize the file backed page */
/* file baked page 초기화*/
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;
}

/* Swap in the page by read contents from the file. */
/* 파일로 부터 contents를 읽어서 page를 Swap-In 해라*/
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
/* 파일에 contents를 다시 작성하여 page를 Swap-Out하라 */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
/* file backed page를 파괴하라. page는 호출자에 의해서 해제 될 것이다.*/
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	
	struct file *_file = file_reopen(file);
	void *start_addr = addr;	//매핑 성공 시 파일이 매핑된 가상 주소 반환하는 데 사용
	
	size_t read_bytes = file_length(_file) < length ? file_length(_file) : length;
	size_t zero_bytes = PGSIZE - read_bytes % PGSIZE;

	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (addr) == 0);
	ASSERT (offset % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0){
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		struct lazy_load_arg *lazy_load_arg = (struct lazy_load_arg *)malloc(sizeof(struct lazy_load_arg));
		lazy_load_arg->file = _file;
		lazy_load_arg->ofs = offset;
		lazy_load_arg->read_bytes = page_read_bytes;
		lazy_load_arg->zero_bytes = page_zero_bytes;

		if(!vm_alloc_page_with_initializer(VM_FILE, addr, writable, lazy_load_segment, lazy_load_arg))
			return false;
		
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		addr += PGSIZE;
		offset += page_read_bytes;
	}
	return start_addr;
}

/* Do the munmap */
/*연결된 물리프레임과의 연결을 끊어준다.*/
void
do_munmap (void *addr) {

	while(true){
		struct thread *t = thread_current();
		struct page *find_page = spt_find_page(&t->spt, addr);
		
		if(find_page == NULL){
			return NULL;
		}

		struct lazy_load_arg *page_aux = (struct lazy_load_arg *)find_page->uninit.aux;
			
		if(pml4_is_dirty(t->pml4, find_page->va)){			
			file_write_at(page_aux->file, addr, page_aux->read_bytes, page_aux->ofs);
			pml4_set_dirty(t->pml4, find_page->va, 0);
		}
		else{	
			pml4_clear_page(t->pml4, find_page->va);
			addr += PGSIZE;
		}
	}
}