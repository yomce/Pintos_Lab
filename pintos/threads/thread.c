#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* struct thread의 magic' 멤버에 사용할 무작위 값.
스택 오버플로우를 감지하는 데 사용됨. 
자세한 내용은 thread.h 상단의 주석 참조. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 스레드에 사용할 무작위 값 
이 값은 수정하지 말 것. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태에 있는 프로세스들의 리스트. 
즉, 실행 준비는 되었지만 실제로 실행 중은 아닌 프로세스들. */
static struct list ready_list;

/* idle 스레드 */
static struct thread *idle_thread;

/* 초기 스레드, init.c의 main()을 실행하는 스레드 */
static struct thread *initial_thread;

/* allocate_tid()에서 사용되는 락 */
static struct lock tid_lock;

/* 스레드 파괴 요청 목록 */
static struct list destruction_req;

/* 통계 정보 */
static long long idle_ticks;    /* idle 상태에서 소비된 타이머 틱 수 */
static long long kernel_ticks;  /* 커널 스레드에서 소비된 타이머 틱 수 */
static long long user_ticks;    /* 사용자 프로그램에서 소비된 타이머 틱 수 */

/* 스케줄링 */
#define TIME_SLICE 4            /* 각 스레드에 할당되는 타이머 틱 수 */
static unsigned thread_ticks;   /* 마지막 yield 이후 경과한 타이머 틱 수 */

/* false이면(기본값) 라운드 로빈 스케줄러를 사용하고, 
true이면 다단계 피드백 큐 스케줄러를 사용함. 
커널 커맨드라인 옵션 "-o mlfqs"로 설정할 수 있음. */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* T가 유효한 스레드를 가리키는 것으로 보이면 true를 반환함 */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 현재 실행 중인 스레드를 반환함.
 * CPU의 스택 포인터 `rsp`를 읽고, 그 값을 페이지의 시작 주소로 내림.
 * `struct thread`는 항상 페이지의 시작 위치에 있고,
 * 스택 포인터는 그 중간 어딘가에 있기 때문에, 현재 스레드를 찾아낼 수 있음. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// thread_start를 위한 전역 디스크립터 테이블(GDT).
// gdt는 thread_init 이후에 설정되기 때문에,
// 임시 gdt를 먼저 설정해야 함.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

static bool
priority_compare_func(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);

/* 현재 실행 중인 코드를 스레드로 변환하여 스레드 시스템을 초기화함.
일반적으로는 이런 방식이 불가능하지만, loader.S가
스택의 바닥을 페이지 경계에 맞춰 배치했기 때문에 이번 경우에만 가능함.

또한 실행 큐(run queue)와 tid 락도 초기화함.

이 함수를 호출한 이후에는 thread_create()로 스레드를 만들기 전에
반드시 페이지 할당자(page allocator)를 초기화해야 함.

이 함수가 끝나기 전까지는 thread_current()를 호출하면 안 됨.
*/
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* 커널용 임시 GDT를 다시 로드함
	 * 이 GDT는 사용자 컨텍스트를 포함하지 않음.
	 * 커널은 나중에 gdt_init()에서 사용자 컨텍스트를 포함한 GDT를 재구성할 것임. */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* 전역 스레드 컨텍스트 초기화 */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);

	/* 현재 실행 중인 코드에 대한 스레드 구조체를 설정 */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* 인터럽트를 활성화하여 선점형 스레드 스케줄링을 시작함.  
   또한 idle 스레드를 생성함. */
void
thread_start (void) {
	/* idle 스레드 생성 */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* 선점형 스레드 스케줄링 시작 */
	intr_enable ();

	/* idle 스레드가 idle_thread를 초기화할 때까지 대기 */
	sema_down (&idle_started);
}

/* 타이머 인터럽트 핸들러가 매 타이머 틱마다 호출함.  
   따라서 이 함수는 외부 인터럽트 컨텍스트에서 실행됨. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* 통계 정보 갱신 */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* 선점을 강제함 */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* 스레드 통계 정보를 출력함 */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* 이름이 NAME이고 초기 우선순위가 PRIORITY인 새로운 커널 스레드를 생성함.  
   이 스레드는 FUNCTION을 실행하며, AUX를 인자로 전달함.  
   생성된 스레드는 ready 큐에 추가됨.  
   성공 시 새 스레드의 ID를 반환하고, 실패 시 TID_ERROR를 반환함.

   thread_start()가 이미 호출된 상태라면,  
   새 스레드는 thread_create()가 반환되기 전에 스케줄될 수 있음.  
   심지어 생성되자마자 종료될 수도 있음.  
   반대로, 원래의 스레드가 새 스레드보다 훨씬 오래 실행될 수도 있음.  
   이러한 실행 순서를 보장하려면 세마포어나 다른 동기화 수단을 사용할 것.

   현재 구현은 새 스레드의 `priority` 멤버만 설정하며,  
   실제 우선순위 기반 스케줄링은 구현되어 있지 않음.  
   우선순위 스케줄링은 과제 1-3의 목표임.
*/
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* 스레드 메모리 할당 */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* 스레드 초기화 */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* 스케줄될 경우 kernel_thread를 호출하도록 설정  
	 * 참고: rdi는 첫 번째 인자, rsi는 두 번째 인자임 */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* 실행 큐에 추가 */
	thread_unblock(t);

	// 새로 생성한 스레드의 우선순위가 현재보다 높으면 양보
	if (t->priority > thread_current()->priority) {
		thread_yield();
	}

	return tid;
}

/* 현재 스레드를 sleep 상태로 전환함.  
   thread_unblock()에 의해 깨워지기 전까지는 스케줄되지 않음.

   이 함수는 반드시 인터럽트가 비활성화된 상태에서 호출해야 함.  
   일반적으로는 synch.h에 정의된 동기화 프리미티브를 사용하는 것이 더 좋음. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* 비교 함수: 우선순위 높은 스레드가 앞으로 오도록 */
static bool
priority_compare_func(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
    struct thread *t_a = list_entry(a, struct thread, elem);
    struct thread *t_b = list_entry(b, struct thread, elem);
    return t_a->priority > t_b->priority;  // 높은 우선순위가 먼저
}

/* 블록된 스레드 T를 실행 준비 상태(ready-to-run)로 전환함.  
   T가 블록된 상태가 아니라면 오류임.  
   (현재 실행 중인 스레드를 준비 상태로 만들고 싶다면 thread_yield()를 사용할 것)

   이 함수는 현재 실행 중인 스레드를 선점하지 않음.  
   이는 중요한데, 호출자가 인터럽트를 비활성화한 상태라면  
   스레드를 원자적으로 unblock하고 다른 데이터를 수정할 수 있다고 기대할 수 있기 때문임. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	list_insert_ordered(&ready_list, &t->elem, priority_compare_func, NULL);
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* 현재 실행 중인 스레드의 이름을 반환함 */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* 현재 실행 중인 스레드를 반환함.  
   running_thread()에 몇 가지 안정성 검사를 추가한 버전임.  
   자세한 내용은 thread.h 상단의 큰 주석을 참고할 것. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* T가 실제로 유효한 스레드인지 확인.  
	   아래 어떤 ASSERT라도 실패한다면, 해당 스레드는  
	   스택 오버플로우가 발생했을 가능성이 있음.  
	   각 스레드는 4KB 미만의 스택만 사용 가능하므로,  
	   자동 배열이 너무 크거나 재귀 호출이 깊으면 스택이 넘칠 수 있음. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* 현재 실행 중인 스레드의 tid(스레드 식별자)를 반환함 */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* 현재 스레드를 스케줄링에서 제거하고 파괴함.  
   이 함수는 호출한 곳으로 절대 되돌아가지 않음. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* 상태를 THREAD_DYING으로 설정하고 다른 프로세스를 스케줄함.  
	   이 스레드는 schedule_tail() 호출 중에 파괴될 예정임. */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* CPU를 양보함. 현재 스레드는 sleep 상태로 전환되지 않으며, 
스케줄러의 판단에 따라 즉시 다시 스케줄될 수도 있음. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_insert_ordered(&ready_list, &curr->elem, priority_compare_func, NULL);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* 현재 스레드의 우선순위를 NEW_PRIORITY로 설정함 */
void
thread_set_priority (int new_priority) {
	thread_current ()->priority = new_priority;

	// 현재보다 높은 우선순위 스레드가 준비 리스트에 있다면 양보
	if (!list_empty(&ready_list)) {
	  struct thread *highest = list_entry(list_front(&ready_list), struct thread, elem);
	  if (highest->priority > thread_current()->priority)
	    thread_yield();
	}
}

/* 현재 스레드의 우선순위를 반환함 */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* 현재 스레드의 nice 값을 NICE로 설정함 */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: 여기에 구현을 작성하세요 */
}

/* 현재 스레드의 nice 값을 반환함 */
int
thread_get_nice (void) {
	/* TODO: 여기에 구현을 작성하세요 */
	return 0;
}

/* 시스템 부하 평균(load average)의 100배 값을 반환함 */
int
thread_get_load_avg (void) {
	/* TODO: 여기에 구현을 작성하세요 */
	return 0;
}

/* 현재 스레드의 recent_cpu 값에 100을 곱한 값을 반환함 */
int
thread_get_recent_cpu (void) {
	/* TODO: 여기에 구현을 작성하세요 */
	return 0;
}

/* Idle 스레드.  
   실행 가능한 다른 스레드가 없을 때 실행됨.

   idle 스레드는 thread_start()에 의해 최초로 ready 리스트에 추가됨.  
   최초로 스케줄되면 idle_thread를 초기화하고,  
   전달받은 세마포어를 up하여 thread_start()가 계속 진행될 수 있도록 한 뒤  
   즉시 자기 자신을 block 상태로 전환함.  

   이후에는 idle 스레드는 ready 리스트에 다시 나타나지 않으며,  
   ready 리스트가 비었을 때 next_thread_to_run()이 특별히 이 스레드를 반환함. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* 다른 스레드가 실행할 수 있도록 양보 */
		intr_disable ();
		thread_block ();

		/* 인터럽트를 다시 활성화하고 다음 인터럽트를 대기함.

		   `sti` 명령어는 다음 명령어가 완료될 때까지 인터럽트를 비활성화하므로,  
		   이 두 명령어는 원자적으로 실행됨. 이 원자성은 중요한데,  
		   인터럽트를 다시 활성화한 직후부터 다음 인터럽트를 대기하기 전 사이에  
		   인터럽트가 발생해버리면 최대 한 틱만큼의 시간을 낭비할 수 있음.

		   관련 내용은 [IA32-v2a] \"HLT\", [IA32-v2b] \"STI\",  
		   그리고 [IA32-v3a] 7.11.1 \"HLT Instruction\"을 참조. */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* 커널 스레드의 기반으로 사용되는 함수 */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* 스케줄러는 인터럽트가 꺼진 상태에서 실행되므로, 여기서 인터럽트를 활성화함 */
	function (aux);       /* 전달받은 함수 실행 */
	thread_exit ();       /* 함수 실행이 끝나면 스레드를 종료시킴 */
}


/* T를 이름이 NAME인 블록된 스레드로 기본 초기화함 */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;
}

/* 다음으로 스케줄할 스레드를 선택하여 반환함.  
   실행 큐(run queue)에 스레드가 있다면 그 중 하나를 반환해야 함.  
   (현재 실행 중인 스레드가 계속 실행 가능한 상태라면, 그것도 실행 큐에 있음)  
   실행 큐가 비어 있다면 idle_thread를 반환함. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* iretq 명령어를 사용하여 스레드를 실행시킴 */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* 새로운 스레드의 페이지 테이블을 활성화하여 스레드를 전환하고,  
   이전 스레드가 종료 상태(THREAD_DYING)라면 그것을 파괴함.

   이 함수가 호출되는 시점에는 이전 스레드(PREV)에서 이미 전환이 이루어졌고,  
   새로운 스레드는 실행 중이며, 인터럽트는 여전히 비활성화되어 있음.

   스레드 전환이 완전히 끝나기 전까지는 printf() 호출은 안전하지 않음.  
   실제로는 printf()는 함수의 마지막 부분에 넣는 것이 좋음. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* 스레드 전환의 핵심 로직.
먼저 전체 실행 컨텍스트를 intr_frame에 복원한 다음,
do_iret 호출을 통해 다음 스레드로 전환함.
주의할 점은, 스레드 전환이 완료되기 전까지는
이 지점부터 스택을 절대 사용하면 안 된다는 것임. */
	__asm __volatile (
			/* 사용할 레지스터들을 저장함 */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* 새 프로세스를 스케줄링함. 함수가 진입할 때는 인터럽트가 꺼져 있어야 함.
이 함수는 현재 스레드의 상태를 인자로 받은 status로 변경한 뒤,
실행할 다음 스레드를 찾아 그 스레드로 전환함.
이 함수 내에서는 printf()를 호출하는 것이 안전하지 않음. */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* 현재 스레드를 실행 중인 상태로 표시함 */
	next->status = THREAD_RUNNING;

	/* 새로운 타임 슬라이스를 시작함 */
	thread_ticks = 0;

#ifdef USERPROG
	/* 새로운 주소 공간을 활성화함 */
	process_activate (next);
#endif

	if (curr != next) {
		/* 전환 이전에 실행되던 스레드가 종료 상태(THREAD_DYING)라면,
			해당 스레드의 구조체(struct thread)를 파괴함.
			이는 thread_exit() 내부에서 스스로의 자원을 먼저 해제하지 않도록
			나중에 처리되어야 함.
			현재 해당 스레드의 페이지는 스택으로 사용 중이므로,
			여기서는 단지 페이지 해제 요청만 큐에 넣어둠.
			실제로 파괴(해제)하는 로직은 다음 스케줄의 시작 시점(schedule()의 시작)에서 실행됨. */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* 스레드를 전환하기 전에,
			먼저 현재 실행 중인 스레드의 정보를 저장함. */
		thread_launch (next);
	}
}

/* 새 스레드에 사용할 tid(스레드 식별자)를 반환함 */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}
