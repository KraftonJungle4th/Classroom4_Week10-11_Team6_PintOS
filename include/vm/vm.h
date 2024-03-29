#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "threads/palloc.h"
#include "lib/kernel/hash.h"

enum vm_type {
	/* page not initialized 
	   페이지가 초기화되지 않음 */
	VM_UNINIT = 0,
	/* page not related to the file, aka anonymous page 
	   파일과 관련이 없는 페이지(일명 익명 페이지) */
	VM_ANON = 1,
	/* page that realated to the file 
	   파일과 관련이 있는 페이지 */
	VM_FILE = 2,
	/* page that hold the page cache, for project 4 
	   페이지 캐시가 있는 페이지, 프로젝트 4의 경우 */
	VM_PAGE_CACHE = 3,

	/* Bit flags to store state 
	   스토어 상태를 나타내는 비트 플래그 */

	/* Auxillary bit flag marker for store information. You can add more
	 * markers, until the value is fit in the int. 
	   스토어 정보를 위한 보조 비트 플래그 마커입니다. 값이 int에 맞을 때까지
	   마커를 더 추가할 수 있습니다.*/
	VM_MARKER_0 = (1 << 3),
	VM_MARKER_1 = (1 << 4),

	/* DO NOT EXCEED THIS VALUE. 
	   이 값을 초과하지 마세요. */
	VM_MARKER_END = (1 << 31),
};

#include "vm/uninit.h"
#include "vm/anon.h"
#include "vm/file.h"
#ifdef EFILESYS
#include "filesys/page_cache.h"
#endif

struct page_operations;
struct thread;

#define VM_TYPE(type) ((type) & 7)

/* The representation of "page".
 * This is kind of "parent class", which has four "child class"es, which are
 * uninit_page, file_page, anon_page, and page cache (project4).
 * DO NOT REMOVE/MODIFY PREDEFINED MEMBER OF THIS STRUCTURE. 
 * 
 * "페이지"의 표현입니다.
 * 이것은 일종의 "부모 클래스"로, 4개의 "자식 클래스"인 uninit_page, file_page, 
 * anon_page 및 페이지 캐시(project 4)가 있습니다.
 * 이 구조의 미리 정의된 멤버를 제거/수정하지 마세요. */
struct page {
	const struct page_operations *operations;
	void *va;              /* Address in terms of user space , 사용자 공간 측면에서의 주소 -> 사용자 공간에 있는 페이지의 주소 */
	struct frame *frame;   /* Back reference for frame 프레임의 역참조 -> 해당 페이지가 어떤 프레임 참조하는지 */

	/* Your implementation */
	struct hash_elem hash_elem;		/*Hash table element*/
 	bool writable;
	/* Per-type data are binded into the union.
	 * Each function automatically detects the current union 
	   유형별 데이터는 유니언에 바인딩된다. 
	   각 함수는 현재 유니언을 자동으로 감지한다. 
	   
	   유니언 자료형은 하나의 메모리 영역에 다른 타입의 데이터를 저장하는 것을 허용하는 특별한 자료형이다. 
	   하나의 유니언은 여러 개의 멤버를 가질 수 있지만, 한 번에 멤버 중 하나의 값을 가질 수 있다.
	   */
	union {
		struct uninit_page uninit;
		struct anon_page anon;
		struct file_page file;
#ifdef EFILESYS
		struct page_cache page_cache;
#endif
	};
};

/* The representation of "frame" 
   "프레임"의 표현입니다. */
struct frame {
	void *kva;	//프레임의 커널 가상 주소를 가리키는 포인터 -> 페이지 프레임이 실제로 메모리에서 어디에 위치하는지
	struct page *page; //프레임이 참조하는 페이지를 가리키는 포인터 -> 해당 프레임이 어떤 페이지를 가리키는지
	struct list_elem frame_elem; //frame 구조체의 list_elem
};

/* The function table for page operations.
 * This is one way of implementing "interface" in C.
 * Put the table of "method" into the struct's member, and
 * call it whenever you needed. 
 * 
 * 페이지 작업을 위한 함수 테이블입니다.
 * C에서 "인터페이스"를 구현하는 한 가지 방법입니다.
 * "메서드" 테이블을 구조체의 멤버에 넣고 필요할 때마다 호출합니다. */
struct page_operations {
	bool (*swap_in) (struct page *, void *);
	bool (*swap_out) (struct page *);
	void (*destroy) (struct page *);
	enum vm_type type;
};

#define swap_in(page, v) (page)->operations->swap_in ((page), v)
#define swap_out(page) (page)->operations->swap_out (page)
#define destroy(page) \
	if ((page)->operations->destroy) (page)->operations->destroy (page)

/* Representation of current process's memory space.
 * We don't want to force you to obey any specific design for this struct.
 * All designs up to you for this. 
 * 
 * 현재 프로세스의 메모리 공간 표시
 * 이 구조에 대해 특정 설계를 따르도록 강요하고 싶지 않습니다.
 * 모든 설계는 여러분의 몫입니다. */
struct supplemental_page_table {
	struct hash hash_table;
};

#include "threads/thread.h"
void supplemental_page_table_init (struct supplemental_page_table *spt);
bool supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src);
void supplemental_page_table_kill (struct supplemental_page_table *spt);
struct page *spt_find_page (struct supplemental_page_table *spt,
		void *va);
bool spt_insert_page (struct supplemental_page_table *spt, struct page *page);
void spt_remove_page (struct supplemental_page_table *spt, struct page *page);

void vm_init (void);
bool vm_try_handle_fault (struct intr_frame *f, void *addr, bool user,
		bool write, bool not_present);

#define vm_alloc_page(type, upage, writable) \
	vm_alloc_page_with_initializer ((type), (upage), (writable), NULL, NULL)
bool vm_alloc_page_with_initializer (enum vm_type type, void *upage,
		bool writable, vm_initializer *init, void *aux);
void vm_dealloc_page (struct page *page);
bool vm_claim_page (void *va);
enum vm_type page_get_type (struct page *page);
#endif  /* VM_VM_H */
