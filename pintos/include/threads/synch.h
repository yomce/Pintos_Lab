#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <list.h>
#include <stdbool.h>

/* 계수형 세마포어(counting semaphore) */
struct semaphore {
  unsigned value;      /* Current value. */
  struct list waiters; /* List of waiting threads. */
};

/* 리스트에 포함된 하나의 세마포어 요소 
조건 변수 내부에 포함된 세마포어 요소의 구조체*/
struct semaphore_elem {
  struct list_elem elem;      /* 리스트 요소 */
  struct semaphore semaphore; /* 실제 세마포어 객체 */
};

void sema_init(struct semaphore *, unsigned value);
void sema_down(struct semaphore *);
bool sema_try_down(struct semaphore *);
void sema_up(struct semaphore *);
void sema_self_test(void);

/* priority scheduling, sync 
비교 함수: cond_wait에서 조건 변수의 waiter를 우선순위 기준으로 정렬하기 위해 사용 */
bool sema_compare_priority(const struct list_elem *a,
  const struct list_elem *b, void *aux);

/* Lock. */
struct lock {
  struct thread *holder;      /* Thread holding lock (for debugging). */
  struct semaphore semaphore; /* Binary semaphore controlling access. */
};

void lock_init(struct lock *);
void lock_acquire(struct lock *);
bool lock_try_acquire(struct lock *);
void lock_release(struct lock *);
bool lock_held_by_current_thread(const struct lock *);

/* Condition variable. */
struct condition {
  struct list waiters; /* List of waiting threads. */
};

void cond_init(struct condition *);
void cond_wait(struct condition *, struct lock *);
void cond_signal(struct condition *, struct lock *);
void cond_broadcast(struct condition *, struct lock *);

/* Optimization barrier.
 *
 * The compiler will not reorder operations across an
 * optimization barrier.  See "Optimization Barriers" in the
 * reference guide for more information.*/
#define barrier() asm volatile("" : : : "memory")

#endif /* threads/synch.h */
