#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "intrinsic.h"
#include "lib/kernel/hash.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);
void set_userstack(char **argv, int argc, struct intr_frame *if_);
struct thread *get_child_process(tid_t pid);
static struct thread *main_thread; // tid 1 thread

/* General process initializer for initd and other process. */
static void process_init (void) {
}

/* process_create_initd - FILE_NAME에서 로드된 "initd"라는 첫 번째 userland 프로그램을 시작한다.
 *
 * process_create_initd()가 반환되기 전에 새 스레드가 스케줄될 수 있으며 종료될 수도 있다.
 * initd의 스레드 ID를 반환하거나 스레드를 생성할 수 없는 경우 TID_ERROR를 반환한다.
 *
 * 이 함수는 한 번만 호출되어야 한다.
 */
tid_t process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;
	main_thread = thread_current();

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* Project 2: COmmand Line Parsing */
	char *save_ptr;
	strtok_r(file_name, " ",  &save_ptr);
	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	sema_down(&main_thread->load_sema);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* initd - 첫 번째 사용자 프로세스를 시작하는 스레드 함수
 * 이 다음 프로세스부터는 fork()를 사용하여 생성한다.
 */
static void initd(void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init();
	
	if (process_exec(f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}


/* process_fork - 현재 프로세스를 'name'으로 복제한다.
 * 새 프로세스의 tid를 반환하거나 스레드를 생성할 수 없는 경우 TID_ERROR를 반환한다.
 */
tid_t process_fork(const char *name, struct intr_frame *if_) {
	// 현재 스레드의 실행 컨텍스트를 복사
	struct thread *cur = thread_current();
	memcpy(&cur->parent_if, if_, sizeof(struct intr_frame));

	// 현재 스레드를 새 스레드로 복제
	tid_t tid = thread_create(name, PRI_DEFAULT, __do_fork, cur);
	if (tid == TID_ERROR)
		return TID_ERROR;

	struct thread *child = get_child_process(tid);
	if (child == NULL)
		return TID_ERROR;

	sema_down(&child->load_sema);
	return tid;
}

#ifndef VM
/* duplicate_pte - 이 함수를 pml4_for_each에 전달하여 부모의 주소 공간을 복제한다.
 * 이 함수는 project 2 전용이다.
 */
static bool duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	/* 부모 페이지가 커널 페이지인 경우 즉시 반환합니다. */
	if (is_kernel_vaddr (va))
		return true;

	/* 2. Resolve VA from the parent's page map level 4. */
	/* 부모의 페이지 맵 레벨 4에서 VA를 해결합니다.*/
	parent_page = pml4_get_page (parent->pml4, va);
	if(parent_page == NULL){
		return false;
	}

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	/* 해석: 자식을 위해 새로운 PAL_USER 페이지를 할당하고 결과를 NEWPAGE에 설정합니다.*/
	newpage = palloc_get_page (PAL_USER);
	if(newpage == NULL){
		return false;
	}
	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	/* 해석: 부모의 페이지를 새 페이지로 복제하고 부모의 페이지가 쓰기 가능한지 여부를 확인하십시오 (결과에 따라 WRITABLE을 설정하십시오). */
	memcpy (newpage, parent_page, PGSIZE);
	writable = is_writable (pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE permission. */
	/* 해석: 새 페이지를 VA 주소에 WRITABLE 권한으로 자식의 페이지 테이블에 추가하십시오. */

	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		/* 해석: 페이지 삽입에 실패하면 오류 처리를 수행하십시오. */
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/* __do_fork - 부모의 실행 컨텍스트를 복사하는 스레드 함수이다.
 * 힌트: parent->tf는 프로세스의 userland 컨텍스트를 보유하지 않는다.
 * 즉, 이 함수에 process_fork()의 두 번째 인수를 전달해야 한다.
 */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *)aux;
	struct thread *current = thread_current();
	struct intr_frame *parent_if = &parent->parent_if;
	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent)) {
		goto error;
	}
#endif

	/* TODO: Your code goes here.
	 * 파일 객체를 복제하려면 include/filesys/file.h의 file_duplicate를 사용하십시오.
	 * 이 함수가 부모의 자원을 성공적으로 복제할 때까지 
	 * 부모는 fork()에서 반환되지 않아야 한다.
	 */
	int idx = 2;
	current->fdt[0] = parent->fdt[0];
	current->fdt[1] = parent->fdt[1];
	for (int idx = 2; idx < FDT_SIZE; idx++) {
		struct file *f = parent->fdt[idx];
		if (f != NULL) {
			current->fdt[idx] = file_duplicate(f);
		}
	}
	if_.R.rax = 0; // 자식 프로세스의 반환 값은 0
	sema_up(&current->load_sema);
	process_init();
	/* Finally, switch to the newly created process. */
	if (succ)
		do_iret (&if_);
error:
	sema_up(&current->load_sema);
	// thread_exit ();
	exit(TID_ERROR);
}

/* process_exec - 현재 실행 컨텍스트를 f_name으로 전환한다.
 * 실패 시 -1을 반환한다.
 */
int process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* 스레드 구조체에서는 intr_frame을 사용할 수 없다.
	 * 현재 스레드가 재스케줄 될 때 실행 정보를 멤버에 저장하기 때문이다.
	 */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* 먼저 현재 컨텍스트를 죽인다. */
	process_cleanup ();

	/* Project 2: Command to Word */
	char *argv[64];
	char *token, *save_ptr;
	int argc = 0;
	for (token = strtok_r (file_name, " ", &save_ptr); token != NULL; token = strtok_r (NULL, " ", &save_ptr))
		argv[argc++] = token;

	/* 그리고 바이너리를 불러온다. */
	success = load(file_name, &_if);
	if (!success)
		return -1;

	/* Project 2: Argument Passing*/
	set_userstack(argv, argc, &_if);
	_if.R.rdi = argc;
	_if.R.rsi = _if.rsp + 8;
	// hex_dump(_if.rsp, _if.rsp, USER_STACK - (uint64_t)_if.rsp, true);

	/* 로드에 실패하면 종료한다. */
	palloc_free_page (file_name);
	if (!success)
		return -1;
	sema_up(&main_thread->load_sema);
	/* 전환된 사용자 프로세스를 시작한다. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* 스레드 tid가 종료될 때까지 기다렸다가 종료 상태를 반환한다.
 * 커널에 의해 종료된 경우 (즉, 예외로 인해 종료된 경우) -1을 반환한다.
 *
 * tid가 유효하지 않거나 호출 프로세스의 자식이 아닌 경우,
 * 또는 지정된 tid에 대해 process_wait()가 이미 성공적으로 호출된 경우
 * 즉시 -1을 반환하고 기다리지 않는다.
 */
int process_wait (tid_t child_tid) {
	struct thread *cur = thread_current();
	struct thread *child = get_child_process(child_tid);
	if (child == NULL)
		return -1;

	sema_down(&child->wait_sema);
	int ret = child->exit_status;
	list_remove(&child->child_elem);
	sema_up(&child->exit_sema);
	return ret;
}

/* process_exit - 현재 프로세스를 종료한다.
 * 이 함수는 thread_exit()에 의해 호출된다.
 */
void process_exit (void) {
	struct thread *t = thread_current();

	/* TODO: Your code goes here.
	 * 여기서 프로세스 자원 정리를 구현하는 것을 권장한다.
	 * 프로세스가 종료되는 경우 모든 파일을 암시적으로 닫는다.
	 */

	// 스레드의 파일 닫기
	for (int fd = 2; fd < FDT_SIZE; fd++) {
		if (t->fdt[fd] != NULL) {
			close(fd);
		}
	}
	file_close(t->self_file);
	palloc_free_multiple(t->fdt, FDT_PAGES);
	process_cleanup ();
	hash_destroy(&t->spt.hash_table , NULL);	//NULL-> h->buckest만 해제, hash_clear로 인해 해시는 이미 해제되어있음.
	sema_up(&t->wait_sema);
	sema_down(&t->exit_sema);
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* 여기서 올바른 순서가 중요하다.
		 * 타이머 인터럽트가 프로세스 페이지 디렉토리로 다시 전환되지 않도록
		 * 페이지 디렉토리를 전환하기 전에 cur->pagedir를 NULL로 설정해야 한다.
		 * 프로세스 페이지 디렉토리를 파괴하기 전에 기본 페이지 디렉토리를 활성화해야 한다.
		 * 그렇지 않으면 활성 페이지 디렉토리는 해제(및 지워짐)된 페이지 디렉토리가 될 것이다.
		 */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* FILE_NAME에서 현재 스레드로 ELF 실행 파일을 로드한다.
 * 실행 파일의 진입점을 *RIP에, 초기 스택 포인터를 *RSP에 저장한다.
 * 성공하면 true, 실패하면 false를 반환한다.
 */
static bool load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Open executable file. */
	file = filesys_open (file_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		return false;
	}

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	t->self_file = file;
	file_deny_write(file);
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	//file_close (file);
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
	/* 1. 파일에서 세그먼트를 로드합니다.
	   2. 주소 VA에서 첫 번째 페이지 오류가 발생하면 호출됩니다. 
	   3. 이 함수를 호출할 때 VA를 사용할 수 있다.*/

	// void **aux_ = (void **)aux;
	// struct file *file = ((struct file **)aux_)[0];
	// size_t *page_read_bytes = ((size_t *)aux_)[1];
	// size_t *page_zero_bytes = ((size_t *)aux_)[2];
	// off_t *ofs = ((off_t *)aux_)[3];
	
	struct lazy_load_arg *lazy_load_arg = (struct lazy_load_arg *)aux;
	file_seek(lazy_load_arg->file, lazy_load_arg->ofs);

	if(file_read(lazy_load_arg->file, page->frame->kva, lazy_load_arg->read_bytes)!= (int)(lazy_load_arg->read_bytes)){
			palloc_free_page(page->frame->kva);
			return false;
	}

	memset(page->frame->kva + lazy_load_arg->read_bytes , 0 , lazy_load_arg->zero_bytes);

	return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */

/* FILE의 오프셋 OFS에서 시작하는 세그먼트를 주소 UPAGE에서 로드합니다.  
   다음과 같이 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리가 초기화됩니다 
   
   -UPAGE의 READ_BYTES 바이트는 오프셋 OFS에서 시작하는 FILE에서 읽어야 합니다.
   
   -UPAGE + READ_BYTES의 ZERO_BYTES 바이트는 0이 되어야 합니다.
   
   이 함수에 의해 초기화된 페이지는 WRITABLE이 참이면 사용자 프로세스에서 쓰기 가능해야 하고,
   그렇지 않으면 읽기 전용이어아 합니다.
   
   성공하면 참을 반환하고, 메모리 할당 오류 또는 디스크 읽기 오류*/

static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		/* 이 페이지를 채우는 방법을 계산하세요.
		   FILE에서 PAGE_READ_BYTES 바이트를 읽고
		   최종 PAGE_ZERO_BYTES 바이트를 0으로 합니다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		/* lazy_load_segment에 정보를 전달하도록 aux를 설정합니다.*/
		//void **aux = (file, &page_read_bytes, &page_zero_bytes, &ofs);
		
		struct lazy_load_arg *lazy_load_arg = (struct lazy_load_arg *)malloc(sizeof(struct lazy_load_arg));
		lazy_load_arg->file = file;
		lazy_load_arg->ofs = ofs;
		lazy_load_arg->read_bytes = page_read_bytes;
		lazy_load_arg->zero_bytes = page_zero_bytes;

		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, lazy_load_arg))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		ofs += page_read_bytes;
	}
	return true;
}

/* setup_stack - USER_STACK에 스택의 PAGE를 생성한다. 성공하면 true를 반환한다.
 */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);
	thread_current()->stack_rsp = stack_bottom;
	// printf("setup_stack \n");

	/* 스택을 stack_bottom에 매핑하고 즉시 페이지를 요구한다.
	 * 성공하면 그에 따라 rsp를 설정한다.
	 * 페이지가 스택임을 표시해야 한다.
	 */

	if(vm_alloc_page(VM_ANON | VM_MARKER_0, stack_bottom, true)){
		success = vm_claim_page(stack_bottom);
		if(success){
			if_->rsp = USER_STACK;
		}
	}
	
	return success;
}
#endif /* VM */

void set_userstack(char **argv, int argc, struct intr_frame *if_) {
	char *addrs[64];
	int size;
	// 2. 단어를 스택의 맨위에 넣는다.
	for (int i = argc - 1; i >= 0; i--)
	{
		size = strlen(argv[i]) + 1;
		if_->rsp -= size;
		memcpy(if_->rsp, argv[i], size);
		addrs[i] = if_->rsp;
	}

	// 3. 스택을 8바이트로 정렬한다.
	while (if_->rsp % 8 != 0) {
		if_->rsp--;
		memset(if_->rsp, 0, sizeof(char));
	}

	// 4. 널 포인터 센티널을 넣는다.
	if_->rsp -= 8;
	memset(if_->rsp, 0, sizeof(char **));

	// 5. 스택에 주소를 넣는다.
	for (int i = argc - 1; i >= 0; i--)
	{
		if_->rsp -= 8;
		memcpy(if_->rsp, &addrs[i], sizeof(char **));
	}

	// 6. 가짜 반환 주소를 넣는다.
	if_->rsp -= 8;
	memset(if_->rsp, 0, sizeof(void *));
}

struct thread *get_child_process(tid_t pid) {
	struct thread *t = thread_current();
	struct list_elem *e;

	for (e = list_begin(&t->child_list); e != list_end(&t->child_list); e = list_next(e)) {
		struct thread *child = list_entry(e, struct thread, child_elem);
		if (pid == child->tid) {
			return child;
		}
	}
	return NULL;
}

void print_intr_frame(struct intr_frame *f) {
	printf("R15: %lld /", f->R.r15);
	printf("R14: %lld /", f->R.r14);
	printf("R13: %lld /", f->R.r13);
	printf("R12: %lld /", f->R.r12);
	printf("R11: %lld /", f->R.r11);
	printf("R10: %lld /", f->R.r10);
	printf("R9: %lld /", f->R.r9);
	printf("R8: %lld /", f->R.r8);
	printf("RDI: %lld /", f->R.rdi);
	printf("RSI: %lld /", f->R.rsi);
	printf("RBP: %lld /", f->R.rbp);
	printf("RBX: %lld /", f->R.rbx);
	printf("RDX: %lld /", f->R.rdx);
	printf("RCX: %lld /", f->R.rcx);
	printf("RAX: %lld /", f->R.rax);
	printf("RIP: %lld /", f->rip);
	printf("CS: %lld /", f->cs);
	printf("EFLAGS: %lld /", f->eflags);
	printf("RSP: %lld /", f->rsp);
	printf("SS: %lld /", f->ss);
	printf("\n\n");
}