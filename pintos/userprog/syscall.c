#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h" // power_off
#include "console.h" // putbuf
#include "userprog/process.h"
#include "threads/vaddr.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame*);
bool is_valid_user_ptr(void*);
bool is_valid_buffer(void* buffer, size_t size);
/* system calls */
void halt(void);
void exit(int status);
int read(int fd, void* buffer, unsigned size);
int write(int fd, const void* buffer, unsigned size);

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

	int syscall_number = f->R.rax;

	int arg1 = (int)    f->R.rdi;
    void *arg2 = (void*)f->R.rsi;
    unsigned arg3 = (unsigned) f->R.rdx;

	switch (syscall_number) {
		/* exit(status) */
		case SYS_EXIT: {
		  int status = arg1;
		  thread_current()->exit_status = status;
		  printf ("%s: exit(%d)\n", thread_name(), status);
		  thread_exit ();
		  break;
		}
		/* write(fd, buffer, size) */
		case SYS_WRITE: {
		  int fd      = arg1;
		  const char *buffer = (const char *) arg2;
		  unsigned size     = (unsigned) arg3;
	
		  if (fd == STDOUT_FILENO) {
			/* 콘솔에 찍기 */
			putbuf (buffer, size);
			f->R.rax = size;      /* return: 쓴 바이트 수 */
		  } else {
			f->R.rax = -1;        /* 잘못된 fd */
		  }
		  break;
		}
	
		/* 아직 구현 안 된 나머지 syscall 들… */
	
		default:


	printf ("system call!\n");
	thread_exit ();
	
	break;
	}
}
