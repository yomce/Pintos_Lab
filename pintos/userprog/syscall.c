#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/malloc.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h" // power_off
#include "console.h" // putbuf
#include "userprog/process.h"
#include "threads/vaddr.h"
#include "filesys/filesys.h" // filesys_create
#include "filesys/file.h"

extern struct lock filesys_lock;

void syscall_entry(void);
void syscall_handler(struct intr_frame*);
bool is_valid_user_ptr(void*);
bool is_valid_buffer(void* buffer, size_t size);
/* system calls */
void halt(void);
void exit(int status);

bool create(const char* file, unsigned initial_size);
bool remove(const char* file);
int open(const char* file);
void close(int fd);
int filesize(int fd);

int read(int fd, void* buffer, unsigned size);
int write(int fd, const void* buffer, unsigned size);

void seek(int fd, unsigned position);
unsigned tell(int fd);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

#define MAX_FD 128

#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	/* %rdi, %rsi, %rdx, %r10, %r8, and %r9 */
	int syscall_number = f->R.rax;

	int arg1 = (int)    f->R.rdi;
    void *arg2 = (void*)f->R.rsi;
    unsigned arg3 = (unsigned) f->R.rdx;

	switch (syscall_number) {
		/* Projects 2 and later. */
		/* exit(status) */
		case SYS_HALT:
			halt();
			break;	

		case SYS_EXIT: 
			exit(f->R.rdi);
        	break;

		/* write(fd, buffer, size) */
		case SYS_WRITE:
			f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
				  
		case SYS_CREATE:
			f->R.rax = create(f->R.rdi, f->R.rsi);
			break;

		case SYS_REMOVE:
			f->R.rax = remove(f->R.rdi);
			break;

		case SYS_OPEN:
			f->R.rax = open(f->R.rdi);
			break;

		case SYS_CLOSE:
			close(f->R.rdi);
			break;

		case SYS_FILESIZE:
			f->R.rax = filesize(f->R.rdi);
			break;
		
		case SYS_READ:
			f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
        	break;
		
			case SYS_SEEK:
			seek(f->R.rdi, f->R.rsi);
			break;
		case SYS_TELL:
			f->R.rax = tell(f->R.rdi);
			break;
	
		/* 아직 구현 안 된 나머지 syscall 들… */
	
		default:


	printf ("system call!\n");
	thread_exit ();
	
	break;
	}
}

/* ----------유효한 유저 주소인지 검사----------- */
void valid_user_address(void* addr) {
	if (addr == NULL || !is_user_vaddr(addr) || 
	pml4_get_page(thread_current()->pml4, addr) == NULL){
		exit(-1);
	}
}

/* ----------전체 버퍼 주소 유효성 검사------------- */
bool is_valid_buffer(void *buffer, size_t size) {
	uint8_t *ptr = buffer;
	for (size_t i = 0; i < size; i++) {
	  void *addr = ptr + i;
	  if (!is_user_vaddr(addr) || pml4_get_page(thread_current()->pml4, addr) == NULL)
		return false;
	}
	return true;
  }

/* fd → file* 매핑 */
static struct file *fd_to_file(int fd) {
	if (fd < 2 || fd >= MAX_FD) return NULL;
	return thread_current()->fd_table[fd];
  }
  
  /* file → fd 할당 */
  static int alloc_fd(struct file *file) {
	struct thread *t = thread_current();
	for (int fd = 2; fd < MAX_FD; fd++) {
	  if (t->fd_table[fd] == NULL) {
		t->fd_table[fd] = file;
		return fd;
	  }
	}
	return -1;
  }

/* ---------------system call---------------*/
void halt(void) {
	power_off();
}

void exit(int status) {
    struct thread *curr = thread_current();
    curr->exit_status = status; //부모 프로세스가 알 수 있도록
    printf("%s: exit(%d)\n", thread_name(), status);
    thread_exit();
}

/* SYS_WRITE */
int write(int fd, const void *buffer, unsigned size) {
	if (!is_valid_buffer((void *)buffer, size))
	  exit(-1);
  
	if (fd == 1) {  // STDOUT
	  putbuf(buffer, size);
	  return size;
	}
  
	struct file *f = fd_to_file(fd);
	if (f == NULL) return -1;
  
	lock_acquire(&filesys_lock);
	int bytes = file_write(f, buffer, size);
	lock_release(&filesys_lock);
	return bytes;
  }

/* SYS_CREATE */
bool create(const char *file, unsigned initial_size) {
	valid_user_address((void *)file);
    lock_acquire(&filesys_lock);
    bool success = filesys_create(file, initial_size);
    lock_release(&filesys_lock);
    return success;
}

/* SYS_REMOVE */
bool remove(const char *file) {
    valid_user_address((void *)file);
    lock_acquire(&filesys_lock);
    bool success = filesys_remove(file);
    lock_release(&filesys_lock);
    return success;
}

/* SYS_OPEN */
int open(const char *file) {
    valid_user_address((void *)file);
    lock_acquire(&filesys_lock);
    struct file *f = filesys_open(file);
    int fd = -1;
    if (f != NULL) {
      fd = alloc_fd(f);
      if (fd == -1) file_close(f);  // 테이블에 넣을 공간 없으면 파일도 닫아야 함
    }
    lock_release(&filesys_lock);
    return fd;
}

/* SYS_CLOSE */
void close(int fd) {
	if (fd < 2 || fd >= MAX_FD) return;
	struct thread *t = thread_current();
	struct file *f = t->fd_table[fd];
	if (f == NULL) return;
  
	lock_acquire(&filesys_lock);
	file_close(f);
	lock_release(&filesys_lock);
  
	t->fd_table[fd] = NULL;
  }

/* SYS_FILESIZE */
int filesize(int fd) {
    struct file *f = fd_to_file(fd);
    if (f == NULL) return -1;
    lock_acquire(&filesys_lock);
    int size = file_length(f);
    lock_release(&filesys_lock);
    return size;
}

/* SYS_READ */
int read(int fd, void *buffer, unsigned size) {
    if (!is_valid_buffer(buffer, size))
      exit(-1);

    if (fd == 0) {  // STDIN
      for (unsigned i = 0; i < size; i++)
        ((char *)buffer)[i] = input_getc();
      return size;
    }

    struct file *f = fd_to_file(fd);
    if (f == NULL) return -1;

    lock_acquire(&filesys_lock);
    int bytes = file_read(f, buffer, size);
    lock_release(&filesys_lock);
    return bytes;
}

/* SYS_SEEK */
void seek(int fd, unsigned position) {
	struct file *f = fd_to_file(fd);
	if (f == NULL) return;
	lock_acquire(&filesys_lock);
	file_seek(f, position);
	lock_release(&filesys_lock);
  }
  
  /* SYS_TELL */
  unsigned tell(int fd) {
	struct file *f = fd_to_file(fd);
	if (f == NULL) return -1;
	lock_acquire(&filesys_lock);
	unsigned pos = file_tell(f);
	lock_release(&filesys_lock);
	return pos;
  }
  