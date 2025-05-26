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
#include "intrinsic.h"
#include "lib/string.h"  /* for strtok_r() */
#ifdef VM
#include "vm/vm.h"
#endif

#define MAX_ARGS 128

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);
static void push_args_to_stack(char **argv, int argc,
							   struct intr_frame *if_);
static bool setup_stack(struct intr_frame *if_,
                        char **argv, int argc);

/* initd 및 기타 프로세스를 위한 일반 프로세스 초기화프로그램.*/
static void
process_init (void) {
	struct thread* current = thread_current ();
	/* file descriptor 초기화 */
    memset(current->fd_table, 0, sizeof current->fd_table);
	current->next_fd  = 2;   /* 0=stdin, 1=stdout 예약 */
}

/* FILE_NAME에서 로드된 "initd"라는 첫 번째 유저랜드 프로그램을 시작합니다.
 * 새 스레드는 process_create_initd()가 반환되기 전에 스케줄되거나 (종료될 수도) 합니다.
 * initd의 스레드 ID를 반환하며, 스레드를 생성할 수 없으면 TID_ERROR를 반환합니다.
 * 이 함수는 단 한 번만 호출되어야 합니다.*/
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy, *name_copy;
    tid_t tid;
    char *name, *save_ptr;
	
	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* 실행 파일 이름만 뽑을 때 쓸 또 다른 복사본 */
    name_copy = palloc_get_page(0);
    if (name_copy == NULL) {
        palloc_free_page(fn_copy);
        return TID_ERROR;
    }

	/* Create a new thread to execute FILE_NAME. */
	strlcpy(name_copy, file_name, PGSIZE);
    name = strtok_r(name_copy, " ", &save_ptr);
    /* name_copy 은 더 이상 필요 없으니 해제 */
    palloc_free_page(name_copy);

	tid = thread_create(name, PRI_DEFAULT, initd, fn_copy);
    if (tid == TID_ERROR)
        palloc_free_page(fn_copy);
    return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
	char *fn_copy = f_name;
    process_init();
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	if (process_exec (fn_copy) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	/* Clone current thread to new thread.*/
	return thread_create (name,
			PRI_DEFAULT, __do_fork, thread_current ());
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if;
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
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	process_init ();

	/* Finally, switch to the newly created process. */
	if (succ)
		do_iret (&if_);
error:
	thread_exit ();
}

/* 현재 실행 중인 실행 컨텍스트를 f_name으로 전환합니다.
 * 실패 시 -1을 반환합니다. */
int process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* 스레드 구조체의 intr_frame을 사용할 수 없습니다.
     * 이는 현재 스레드가 재스케줄될 때
     * 실행 정보를 해당 멤버에 저장하기 때문입니다. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* 기존 실행 컨텍스트 정리 */
	process_cleanup ();

	/* 그리고 바이너리를 로드합니다. */
	/* 여기서 load() 호출 시, argv_store/argc_store를
       setup_stack() 안에서 참조하도록 구현해야 합니다. */
	success = load (file_name, &_if);
	// hex_dump((void*)_if.rsp, (void*)_if.rsp, USER_STACK - (uint64_t)_if.rsp, true);

	/* 로드에 실패하면 종료합니다. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* 전환된 프로세스를 시작합니다. */
	do_iret (&_if);
	NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid UNUSED) {
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */
	for (int i = 0; i < 1000000000; i++){

	}
	return -1;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */

	process_cleanup ();
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
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
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
	uint64_t e_entry; // *중요 필드* 실행 시작 주소(->rip에 넣을 값)
	uint64_t e_phoff; // *중요 필드* 프로그램 헤더 테이블의 시작 오프셋
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum; // *중요 필드* 프로그램 헤더의 개수
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type; // 세그먼트 종류(PT_LOAD는 메모리에 로드할 세그먼트)
	uint32_t p_flags; // 읽기/쓰기/실행 권한
	uint64_t p_offset; // 파일에서 이 세그먼트가 시작되는 위치
	uint64_t p_vaddr; // 메모리에 적재될 가상 주소
	uint64_t p_paddr;
	uint64_t p_filesz; // 파일에서 읽어야 할 바이트 수
	uint64_t p_memsz; // 메모리에 필요한 전체 크기(파일보다 클 수 있음 -> 나머지는 0으로 채움)
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_, char **argv, int argc);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

static void push_args_to_stack(char **argv, int argc,
                               struct intr_frame *if_);

/* FILE_NAME에서 ELF 실행 파일을 현재 스레드에 로드합니다.
 * 실행 파일의 진입점을 *RIP에 저장하고,
 * 초기 스택 포인터를 *RSP에 저장합니다.
 * 성공하면 true를, 실패하면 false를 반환합니다. */
static bool 
load(const char* file_name, struct intr_frame* if_) {
	struct thread* t = thread_current(); //현재 스레드 가져오기
	struct ELF ehdr;					 //ELF 헤더를 저장할 구조체
	struct file* file = NULL;			 //실행 파일 핸들
	off_t file_ofs;						 //프로그램 헤더의 파일 오프셋
	bool success = false;				 //로드 성공 여부 플래그
	int i;

	/* arg 파싱 */
	int argc = 0;
	char* argv[128]; // 인자 최대 크기로 제한(인자 스트링 저장)

	char* token, * save_ptr;
	for (token = strtok_r((char *) file_name, " ", &save_ptr); token != NULL;
		token = strtok_r(NULL, " ", &save_ptr)) {
		argv[argc++] = token;
	}
	file_name = argv[0]; // file name 변경

	/* 1. 새 페이지 디렉터리(PML4) 생성 및 활성화 */
	t->pml4 = pml4_create();   			 // 새 PML4 (페이지 테이블) 생성
	if (t->pml4 == NULL)				 
		goto done;						 // 생성 실패시 종료
	process_activate(thread_current());  // 생성된 PML4를 활성화 (CR3 레지스터 설정) 
										 //	context switching 유발

	/* 2. 실행 파일 열기 */
	file = filesys_open(file_name);  	 // 파일 시스템에서 ELF 열기
	if (file == NULL) {
		printf("load: %s: open failed\n", file_name);
		goto done;
	}

	/* 3. ELF 헤더 읽기 및 기본 검증 */
    if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr     // 헤더 크기만큼 읽기
        || memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7)       // 매직 넘버 확인
        || ehdr.e_type != 2                                    // 실행 파일 타입 확인
        || ehdr.e_machine != 0x3E                              // AMD64 아키텍처 확인
        || ehdr.e_version != 1                                 // ELF 버전 확인
        || ehdr.e_phentsize != sizeof(struct Phdr)            // 프로그램 헤더 엔트리 크기 확인
        || ehdr.e_phnum > 1024) {                              // 프로그램 헤더 개수 상한
        printf("load: %s: error loading executable\n", file_name);
        goto done;
    }

	/*
        ELF 파일 안의 로딩 가능한 세그먼트(PT_LOAD)들을 찾아,
        적절한 위치에 메모리 할당 + 내용 복사 + 초기화
        ehdr.e_phoff: 프로그램 헤더들이 파일 안에서 시작하는 위치
        ehdr.e_phnum: 프로그램 헤더 개수
        for 문이 모든 프로그램 헤더를 순회 */
	/* 4. 프로그램 헤더 순회: 각 PT_LOAD 세그먼트를 로드 */
    file_ofs = ehdr.e_phoff;                       // 프로그램 헤더 시작 오프셋
    for (i = 0; i < ehdr.e_phnum; i++) {
        struct Phdr phdr;
		// 헤더 읽을 위치로 이동 및 읽기. 파일 범위를 벗어나면 goto done
		if (file_ofs < 0 || file_ofs > file_length(file))
			goto done;
		file_seek(file, file_ofs); // 해당 프로그램 헤더 위치로 이동

		if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
		case PT_NULL:
		case PT_NOTE:
		case PT_PHDR:
		case PT_STACK:
		default:
			/* 이 세그먼트를 무시합니다. */
			break;
		case PT_DYNAMIC:
		case PT_INTERP:
		case PT_SHLIB:
			goto done;
		case PT_LOAD:
			if (validate_segment(&phdr, file)) {
				bool writable = (phdr.p_flags & PF_W) != 0;
				uint64_t file_page = phdr.p_offset & ~PGMASK;
				uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
				uint64_t page_offset = phdr.p_vaddr & PGMASK;
				uint32_t read_bytes, zero_bytes;
				if (phdr.p_filesz > 0) {
					/* 일반 세그먼트.
					   디스크에서 읽을 바이트 수 계산
 					   디스크에서 초기 부분을 읽고 나머지는 0으로 채웁니다.*/
					read_bytes = page_offset + phdr.p_filesz;
					zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE)
						- read_bytes);
				}
				else {
					/* 전체 페이지 0으로 초기화합니다.
 					 * 디스크에서 어떤 것도 읽지 않습니다. */
					read_bytes = 0;
					zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
				}
				/* 실제 메모리 매핑 수행 */
				if (!load_segment(file, file_page, (void*)mem_page,
					read_bytes, zero_bytes, writable))
					goto done;
			}
			else
				goto done;
			break;
		}
	}

	/* 스택 설정. */
	if (!setup_stack(if_, argv, argc)) // 스택 페이지 할당 및 RSP 설정 rsp (스택 시작 지점)
		goto done;

	/* 시작 주소. */
	/* 6. 진입점(entry point) 설정 */
	if_->rip = ehdr.e_entry; // ELF 헤더의 e_entry를 RIP에 저장

	strlcpy(thread_current()->name, file_name, sizeof thread_current()->name);

	/* TODO: 인수 전달 구현 (argv, argc 스택에 푸시) 여기에 코드를 작성하세요.
 	 * TODO: 인수 전달을 구현합니다 (project2/argument_passing.html 참조). */

	success = true;

done:
	/* 로드 성공 여부와 상관없이 이 지점에 도달합니다. */
	file_close(file);                            // 열린 파일 닫기
    return success;                              // 로드 결과 반환
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
setup_stack(struct intr_frame *if_, char **argv, int argc) {
    uint8_t *kpage;
    bool success = false;

    /* 1) 스택용 페이지 할당 */
    kpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (kpage != NULL) {
        /* 2) 유저 스택 가상주소에 매핑 시도 */
        success = install_page((void *)(USER_STACK - PGSIZE), kpage, true);
        if (success) {
            /* 3) 성공했을 때만 rsp 세팅 & 인수 푸시 */
            if_->rsp = USER_STACK;
            push_args_to_stack(argv, argc, if_);
        } else {
            /* 4) 매핑 실패 시 방금 할당한 페이지 해제 */
            palloc_free_page(kpage);
        }
    }
    return success;
}

static void
push_args_to_stack (char **argv, int argc, struct intr_frame *if_) {
    /* 1) rsp 변수에 현재 스택 포인터 저장 */
    void *rsp = (void *) if_->rsp;
    void *arg_addrs[MAX_ARGS];

    /* 2) 각 인자를 문자열(널 포함) 형태로 스택에 복사 (뒤에서부터) */
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1; // null 포함한 길이
        rsp -= len;
        memcpy(rsp, argv[i], len);
        arg_addrs[i] = rsp; // 이후 argv 배열 생성 시 주소 사용
    }

    /* 3) word align: 8바이트 정렬이 될 때까지 바이트 단위로 0 써 넣기 */
    while ((uintptr_t) rsp % 8 != 0) {
	        rsp--;
	        *(uint8_t *)rsp = 0;
	    }

    /* 4) argv[argc] = NULL (sentinel) */
    rsp -= sizeof(char *); // argv[argc] = NULL
    *(char **)rsp = NULL;

    /* 5) argv[argc-1] … argv[0] 포인터 푸시 */
    for (int i = argc - 1; i >= 0; i--) {
        rsp -= sizeof(char *);
        memcpy(rsp, &arg_addrs[i], sizeof(char *));
    }

    /* 6) fake return address (0) 푸시 */
    rsp -= sizeof(void *);
    *(void **)rsp = 0;

    /* 7) 레지스터에 rdi = argc, rsi = argv 주소 설정 */
    if_->R.rdi = argc;                              /* 첫 번째 인자: argc */
    if_->R.rsi = (uint64_t)(rsp + sizeof(void *));  /* 두 번째 인자: &argv[0] */

    /* 8) 최종 rsp 갱신 */
    if_->rsp = (uint64_t) rsp;
	
}

/* 새로운 file*을 프로세스 테이블에 등록하고 fd 반환.
   실패 시 -1 */
   int
   process_add_file (struct file *f) {
	 struct thread *t = thread_current ();
	 for (int fd = t->next_fd; fd < 128; fd++) {
	   if (t->fd_table[fd] == NULL) {
		 t->fd_table[fd] = f;
		 t->next_fd = fd + 1;
		 return fd;
	   }
	 }
	 /* wrap-around */
	 for (int fd = 2; fd < t->next_fd; fd++) {
	   if (t->fd_table[fd] == NULL) {
		 t->fd_table[fd] = f;
		 t->next_fd = fd + 1;
		 return fd;
	   }
	 }
	 return -1;
   }
   
   /* fd → struct file* 조회, 없으면 NULL */
   struct file *
   process_get_file (int fd) {
	 if (fd < 0 || fd >= 128)
	   return NULL;
	 return thread_current ()->fd_table[fd];
   }
   
   /* fd 닫고 테이블에서 제거, 성공 시 true */
   bool
   process_close_file (int fd) {
	 struct file *f = process_get_file(fd);
	 if (!f) return false;
	 file_close(f);
	 thread_current()->fd_table[fd] = NULL;
	 if (fd < thread_current()->next_fd)
	   thread_current()->next_fd = fd;
	 return true;
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

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
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
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

// /* Create a PAGE of stack at the USER_STACK. Return true on success. */
// static bool
// setup_stack(struct intr_frame *if_, char **argv, int argc) {
// 	bool success = false;
// 	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

// 	/* TODO: Map the stack on stack_bottom and claim the page immediately.
// 	 * TODO: If success, set the rsp accordingly.
// 	 * TODO: You should mark the page is stack. */
// 	/* TODO: Your code goes here */

// 	return success;
// }
#endif /* VM */
