/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "lib/kernel/hash.h"
#include "threads/vaddr.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "threads/thread.h"
#include "userprog/process.h"

// 프레임 구조체를 관리하는 frame_table
struct list frame_table;
struct list_elem * clock_ref;
struct lock frame_table_lock;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes.W
 * 각 서브시스템의 초기화 코드를 호출하여 가상 메모리 서브시스템을 초기화합니다.
 */
void vm_init(void)
{
	vm_anon_init();
	vm_file_init();
#ifdef EFILESYS /* For project 4 */
	pagecache_init();
#endif
	register_inspect_intr();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	list_init(&frame_table);
	clock_ref = list_begin(&frame_table);
	lock_init(&frame_table_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
/* 페이지 유형을 가져옵니다.
 *  이 함수는 페이지가 초기화된 후 유형을 알고 싶을 때 유용합니다.
 * 이 함수는 현재 완전히 구현되어 있습니다. */
enum vm_type
page_get_type(struct page *page)
{
	int ty = VM_TYPE(page->operations->type);
	switch (ty)
	{
	case VM_UNINIT:
		return VM_TYPE(page->uninit.type);
	default:
		return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
/* 이니셜라이저를 사용해서 보류 중인 페이지 객체를 만듭니다.
 * 페이지를 만들고 싶다면 직접 만들지 말고 이 함수나 `vm_alloc_page`를 통해 만드세요.
 */

bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
									vm_initializer *init, void *aux)
{

	ASSERT(VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current()->spt;

	/* Check wheter the upage is already occupied or not. */
	/* 'upage'가 이미 사용중인지 여부를 확인한다.*/
	if (spt_find_page(spt, upage) == NULL)
	{
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */

		/* 페이지를 만들고 VM 유형에 따라 이니셜라이저를 가져와서
		 * uninit_new를 호출하여 "uninit" 페이지 구조체를 만듭니다.
		 * uninit_new를 호출한 후에 필드를 수정해야 합니다.
		 */

		struct page *page = (struct page *)malloc(sizeof(struct page));
		if (page == NULL)
		{
			return false;
		}
		page->va = upage;
		page_initializer new_initializer = NULL;
		switch (VM_TYPE(type))
		{
		case VM_ANON:
			new_initializer = anon_initializer;
			break;
		case VM_FILE:
			new_initializer = file_backed_initializer;
			break;
		}
		if (new_initializer == NULL)
		{
			free(page);
			return false;
		}
		uninit_new(page, upage, init, type, aux, new_initializer);
		page->writable = writable;

		/* TODO: Insert the page into the spt. */
		/* 페이지를 spt에 삽입합니다. */
		return spt_insert_page(spt, page);
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
/* spt로부터 VA를 찾고 페이지를 반환합니다. 에러인 경우 NULL을 반환합니다. */
struct page *
spt_find_page(struct supplemental_page_table *spt UNUSED, void *va UNUSED)
{
	struct page *page = malloc(sizeof(struct page));
	struct hash_elem *e;
	if (page == NULL)
	{
		return NULL;
	}

	page->va = pg_round_down(va);
	e = hash_find(&spt->hash_table, &page->hash_elem);

	if (e != NULL)
	{
		struct page *found_page = hash_entry(e, struct page, hash_elem);
		free(page);
		return found_page;
	}
	else
	{
		free(page);
		return NULL;
	}
}

/* Insert PAGE into spt with validation. */
/* 페이지를 유효성 검사를 거쳐 spt에 삽입합니다. */
bool spt_insert_page(struct supplemental_page_table *spt UNUSED, struct page *page UNUSED)
{
	int succ = false;

	if (is_user_vaddr(page->va))
	{
		if (spt_find_page(spt, page->va) == NULL)
		{
			hash_insert(&spt->hash_table, &page->hash_elem);
			succ = true;
		}
	}
	return succ;
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page)
{
	hash_delete(&spt->hash_table, &page->hash_elem);
	vm_dealloc_page(page);
	return true;
}

/* Get the struct frame, that will be evicted. */
/* 페이지를 교체할 프레임을 가져옵니다. */
static struct frame *
vm_get_victim(void)
{
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */
	struct thread* curr = thread_current();
	lock_acquire(&frame_table_lock);
	for (clock_ref; clock_ref != list_end(&frame_table); clock_ref = list_next(clock_ref)){
		victim = list_entry(clock_ref,struct frame,frame_elem);
		//bit가 1인 경우
		if(pml4_is_accessed(curr->pml4,victim->page->va)){
			pml4_set_accessed(curr->pml4,victim->page->va,0);
		}else{
			lock_release(&frame_table_lock);
			return victim;
		}
	}
	struct list_elem* start = list_begin(&frame_table);

	for (start; start != list_end(&frame_table); start = list_next(start)){
		victim = list_entry(start,struct frame,frame_elem);
		//bit가 1인 경우
		if(pml4_is_accessed(curr->pml4,victim->page->va)){
			pml4_set_accessed(curr->pml4,victim->page->va,0);
		}else{
			lock_release(&frame_table_lock);
			return victim;
		}
	}

	lock_release(&frame_table_lock);
	ASSERT(clock_ref != NULL);
	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
/* 페이지를 교체하고 해당 프레임을 반환합니다.
 * 에러인 경우 NULL을 반환합니다. */

static struct frame *
vm_evict_frame(void)
{
	struct frame *victim UNUSED = vm_get_victim();
	/* TODO: swap out the victim and return the evicted frame. */
	/* 희생자를 교체하고 교체된 프레임을 반환합니다. */
	swap_out(victim->page);
	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
/* palloc() 및 프레임을 가져옵니다. 사용 가능한 페이지가 없는 경우 페이지를
 * 교체하고 반환합니다. 이 함수는 항상 유효한 주소를 반환합니다. 즉, 사용자 풀
 * 메모리가 가득 찬 경우 이 함수는 사용 가능한 메모리 공간을 얻기 위해 프레임을
 * 교체합니다. */

static struct frame *
vm_get_frame(void)
{
	struct frame *frame = (struct frame*)malloc(sizeof(struct frame)); // user_pool 에서 frame 가져오고, kva return해서 frame에 넣어준다.
	/* TODO: Fill this function. */
	frame->kva = palloc_get_page(PAL_USER);
	
	if(frame->kva == NULL){ //frame에서 가용한 page가 없다면
		/* 해당 로직은 evict한 frame을 받아오기에 이미 Frame_Table 존재해서 list_push_back()할 필요 없음 */
		frame = vm_evict_frame(); // 쫓아냄
		frame->page = NULL;
		//free(frame);
		// PANIC("todo);
		return frame;
	}

	lock_acquire(&frame_table_lock);
	list_push_back(&frame_table,&frame->frame_elem);
	lock_release(&frame_table_lock);
	frame->page = NULL; //새 frame을 가져왔으니 page의 멤버를 초기화
	
	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}
/* 스택을 확장합니다. */
static void
vm_stack_growth(void *addr UNUSED)
{
	vm_alloc_page(VM_ANON | VM_MARKER_0, pg_round_down(addr), true);
}

/* Handle the fault on write_protected page */
/* 쓰기 보호된 페이지에 대한 처리 */
static bool
vm_handle_wp(struct page *page UNUSED)
{
	return false;
}

/* Return true on success */
/* 성공 시 true를 반환합니다. */
/*
 'f'
	- page fault 예외가 발생할 때 실행되던 context 정보가 담겨있는 interrupt frame이다.
 'addr'
	- page fault 예외가 발생할 때 접근한 virtual address이다. 즉, 이 virtual address에 접근했기 때문에 page fault가 발생한 것이다.
 'not_present'
	- true : addr에 매핑된 physical page가 존재하지 않는 경우에 해당한다.
	- false : read only page에 writing 작업을 하려는 시도에 해당한다.
 'write'
	- true : addr에 writing 작업을 시도한 경우에 해당한다.
	- false : addr에 read 작업을 시도한 경우에 해당한다.
 'user'
	- true : user에 의한 접근에 해당한다.
	- false : kernel에 의한 접근에 해당한다.
*/
bool vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr UNUSED,
						 bool user UNUSED, bool write UNUSED, bool not_present UNUSED)
{
	struct supplemental_page_table *spt UNUSED = &thread_current()->spt;
	struct page *page = NULL;
	if (addr == NULL)
		return false;

	if (is_kernel_vaddr(addr))
		return false;

	// true: addr에 매핑된 physical page가 존재하지 않는 경우에 해당한다.
	// false: read only page에 writing 작업을 하려는 시도에 해당한다.
	if (not_present) // 접근한 메모리의 physical page가 존재하지 않은 경우
	{
		/* TODO: Validate the fault */
		// 페이지 폴트가 스택 확장에 대한 유효한 경우인지를 확인한다.

		void *rsp = f->rsp; // user access인 경우 rsp는 유저 stack을 가리킨다.
		if (!user)			// kernel access인 경우 thread에서 rsp를 가져와야 한다.
			rsp = thread_current()->stack_rsp;

		// 스택 확장으로 처리할 수 있는 폴트인 경우, vm_stack_growth를 호출한다.
		// 1. addr이 rsp보다 위에있으면 안되고,
		// 2. stack_bottom보다 위에 있으면 안되고,
		// 3. addr이 USER_STACK- (1<<20) 보다 .아래에 있으면 안된다.
		// if (USER_STACK - (1 << 20) <= rsp - 8  && stack_bottom > addr && addr >= (USER_STACK - (1<<20)) && addr < rsp - 8 )
		// 	vm_stack_growth(addr);
		if (USER_STACK - (1 << 20) <= rsp - 8 && rsp - 8 <= addr && addr <= USER_STACK)
			vm_stack_growth(addr);

		page = spt_find_page(spt, addr);
		if (page == NULL)
			return false;
		if (write == 1 && page->writable == 0) // write 불가능한 페이지에 write 요청한 경우
			return false;
		return vm_do_claim_page(page);
	}
	return false;
}

/* Free the page.
/* DO NOT MODIFY THIS FUNCTION. */
/* 페이지를 해제합니다. */
/* 이 함수를 수정하지 마세요. */
void vm_dealloc_page(struct page *page)
{
	destroy(page);
	free(page);
}

/* Claim the page that allocate on VA. */
/* VA(페이지의 가상 메모리 주소)를 통해 페이지를 얻어온다. */
bool vm_claim_page(void *va UNUSED)
{
	struct page *page = NULL;

	/* 물리 프레임과 연결을 할 페이지를 SPT를 통해서 찾아준다.*/
	page = spt_find_page(&thread_current()->spt, va);

	if (page == NULL)
		return false;
	return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
/* 페이지를 청구하고 mmu를 설정합니다.
   실질적으로 frame과 인자로 받은 page를 연결해주는 역할 수행.
   -> 해당 페이지가 이미 어떠한 물리 주소(kva)와 미리 연결이 되어 있는지 확인해야 한다.
   -> 이후 미리 연결된 kva가 없을 경우, 해당 va를 kva에 set해준다. */
static bool
vm_do_claim_page(struct page *page)
{
	struct frame *frame = vm_get_frame();
	if (frame == NULL)
	{
		return false;
	}

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	/* 페이지 테이블 항목을 삽입하여 페이지의 VA를 프레임의 PA에 매핑합니다. */
	/*pml4_get_page는 가상주소를 넣어 해당 물리주소를 찾고 그에 해당하는
	커널 가상 주소를 반환한다.*/
	if (pml4_get_page(thread_current()->pml4, page->va) == NULL)
	{
		if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable))
		{
			vm_dealloc_page(page);
			return false;
		}
	}
	/* 해당 페이지를 물리 메모리에 올려준다.*/
	return swap_in(page, frame->kva);
}

/* Initialize new supplemental page table */
/* 새 보조 페이지 테이블을 초기화합니다. */
void supplemental_page_table_init(struct supplemental_page_table *spt UNUSED)
{
	hash_init(&spt->hash_table, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
/* src에서 dst로 보조 페이지 테이블을 복사합니다. */
// dst <- src가 직접적으로 이루어지지 않는 이유?
// 페이지의 모든 정보가 아니라 페이지에서 메타 정보만 뽑아서 로딩은 나중에 한다?
bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED,
								  struct supplemental_page_table *src UNUSED)
{

	struct hash_iterator i;
	hash_first(&i, &src->hash_table);

	while (hash_next(&i))
	{
		struct page *src_page = hash_entry(hash_cur(&i), struct page, hash_elem);

		enum vm_type vm_type = src_page->operations->type;
		void *va = src_page->va;
		bool writable = src_page->writable;

		/* 1) type이 uninit이면 */
		if (vm_type == VM_UNINIT)
		{ // uninit page 생성 & 초기화
			vm_initializer *init = src_page->uninit.init;
			void *aux = src_page->uninit.aux;
			vm_alloc_page_with_initializer(VM_ANON, va, writable, init, aux);
			continue;
		}

		// if(vm_type == VM_FILE)
		// {
		// 	//파일 로딩에 필요한 정보 저장
		// 	struct lazy_load_arg *file_aux =(struct lazy_load_arg *)malloc(sizeof(struct lazy_load_arg));
		// 	file_aux->file = src_page->file.file;
		// 	file_aux->ofs = src_page->file.ofs;
		// 	file_aux->read_bytes = src_page->file.read_bytes;
		// 	file_aux->zero_bytes = src_page->file.zero_bytes;

		// 	//file_aux 구조체를 인자로 전달하여 VM_FILE page 생성 & 초기화
	 	// 	if(!vm_alloc_page_with_initializer(vm_type, va, writable, NULL, file_aux))
		// 		return false;

		// 	//생성된 페이지 가져온다.
		// 	struct page *file_page = spt_find_page(dst, va);
		// 	//파일에서 데이터를 읽어와 페이지를 채운다.
		// 	file_backed_initializer(file_page, vm_type, va);

		// 	//생성된 페이지의 프레임을 원본 페이지의 프레임으로 설정한다.
		// 	file_page->frame = src_page->frame;

		// 	//페이지 테이블에 페이지 매핑
		// 	pml4_set_page(thread_current()->pml4, file_page->va, src_page->frame->kva, src_page->writable);
		// 	continue;
		// }
		else{

			/* 2) type이 uninit이 아니면 */
			if (!vm_alloc_page(vm_type, va, writable)) // uninit page 생성 & 초기화
				// init이랑 aux는 Lazy Loading에 필요함
				// 지금 만드는 페이지는 기다리지 않고 바로 내용을 넣어줄 것이므로 필요 없음
				return false;

			// vm_claim_page으로 요청해서 매핑 & 페이지 타입에 맞게 초기화
			if (!vm_claim_page(va))
				return false;

			// 매핑된 프레임에 내용 로딩
			struct page *dst_page = spt_find_page(dst, va);
			memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
		}
		
	}
	return true;
}

/* Free the resource hold by the supplemental page table */
/* 보조 페이지 테이블에 의해 보유된 리소스를 해제합니다. */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED)
{
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */

	/* 스레드에 의해 보유된 모든 보조 페이지 테이블을 파괴하고
	 * 변경된 모든 내용을 저장소에 기록하세요. */

	hash_clear(&spt->hash_table, page_destroy);
	// hash_destroy(&spt->hash_table, page_destroy);
}
