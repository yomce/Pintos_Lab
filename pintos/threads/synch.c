/* 이 파일은 Nachos 교육용 운영체제의 소스 코드를 기반으로 작성되었습니다.
아래에는 Nachos의 저작권 공지가 전체 그대로 포함되어 있습니다. */

/* Copyright (c) 1992-1996 캘리포니아 대학교(The Regents of the University of
   California). 모든 권리 보유.

   이 소프트웨어 및 문서는 어떠한 목적이든, 비용 없이,
   그리고 별도의 서면 계약 없이 사용, 복사, 수정 및 배포할 수 있습니다.
   단, 위의 저작권 공지와 다음 두 단락은 모든 복사본에 포함되어야 합니다.

   이 소프트웨어 및 문서를 사용함으로 인해 발생하는
   직접적, 간접적, 특별, 부수적 또는 결과적 손해에 대해
   캘리포니아 대학교는 책임을 지지 않습니다.
   설사 그러한 손해 가능성을 사전에 통보받았더라도 마찬가지입니다.

   캘리포니아 대학교는 상품성이나 특정 목적에의 적합성에 대한
   묵시적 보증을 포함하여 어떠한 보증도 명시적으로 하거나 암묵적으로 하지
   않습니다. 이 소프트웨어는 "있는 그대로(AS IS)" 제공되며, 캘리포니아 대학교는
   유지보수, 지원, 업데이트, 기능 개선, 수정 등을 제공할 의무가 없습니다.
   */

#include "threads/synch.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include <stdio.h>
#include <string.h>

///////////////// compare function //////////////////////

bool sema_compare_priority(const struct list_elem *l,
  const struct list_elem *s, void *aux UNUSED) {
struct semaphore_elem *l_sema = list_entry(l, struct semaphore_elem, elem);
struct semaphore_elem *s_sema = list_entry(s, struct semaphore_elem, elem);
struct list *waiter_l_sema = &(l_sema->semaphore.waiters);
struct list *waiter_s_sema = &(s_sema->semaphore.waiters);
return list_entry(list_begin(waiter_l_sema), struct thread, ready_elem)
->priority >
list_entry(list_begin(waiter_s_sema), struct thread, ready_elem)
->priority;
}

////////////////////////////////////////////////////////

/* 세마포어 SEMA를 VALUE 값으로 초기화함.
   세마포어는 음수가 될 수 없는 정수와, 그 값을 조작하기 위한
   두 가지 원자적(atomic) 연산자로 구성됨:

   -down 또는 "P" 연산: 값이 양수가 될 때까지 기다렸다가,
    양수가 되면 그 값을 1 감소시킴.

   -up 또는 "V" 연산: 값을 1 증가시키고,
    대기 중인 스레드가 있다면 그 중 하나를 깨움.
    */
void sema_init(struct semaphore *sema, unsigned value) {
  ASSERT(sema != NULL);

  sema->value = value;
  list_init(&sema->waiters);
}

/* 세마포어에 대한 down 또는 "P" 연산.
   SEMA의 값이 양수가 될 때까지 기다렸다가,
   값이 양수가 되면 그것을 원자적으로 감소시킴.

   이 함수는 sleep할 수 있으므로 인터럽트 핸들러 내에서 호출하면 안 됨.
   인터럽트가 비활성화된 상태에서도 호출할 수는 있지만,
   만약 이 함수가 sleep하게 되면 다음에 스케줄되는 스레드가
   인터럽트를 다시 활성화할 가능성이 있음.

   이 함수는 sema_down 함수임.
   */
void sema_down(struct semaphore *sema) {
  enum intr_level old_level;

  ASSERT(sema != NULL);
  ASSERT(!intr_context());

  old_level = intr_disable();
  while (sema->value == 0) {
    // list_push_back (&sema->waiters, &thread_current ()->ready_elem);
    list_insert_ordered(&sema->waiters, &thread_current()->ready_elem,
                        thread_compare_priority, NULL);
    thread_block();
  }
  sema->value--;
  intr_set_level(old_level);
}

/* 세마포어에 대한 down 또는 "P" 연산을 수행하되,
   세마포어 값이 이미 0이 아닌 경우에만 수행함.
   세마포어 값이 감소되었으면 true를 반환하고, 그렇지 않으면 false를 반환함.

   이 함수는 인터럽트 핸들러에서 호출해도 안전함. */
bool sema_try_down(struct semaphore *sema) {
  enum intr_level old_level;
  bool success;

  ASSERT(sema != NULL);

  old_level = intr_disable();
  if (sema->value > 0) {
    sema->value--;
    success = true;
  } else
    success = false;
  intr_set_level(old_level);

  return success;
}

/* 세마포어에 대한 up 또는 "V" 연산.
   SEMA의 값을 1 증가시키고, SEMA를 기다리고 있는 스레드가 있다면 그 중 하나를
   깨움.

   이 함수는 인터럽트 핸들러에서 호출해도 안전함. */
void sema_up(struct semaphore *sema) {
  enum intr_level old_level;

  ASSERT(sema != NULL);

  old_level = intr_disable();
  if (!list_empty(&sema->waiters)) {
    list_sort(&sema->waiters, thread_compare_priority, NULL);
    thread_unblock(
        list_entry(list_pop_front(&sema->waiters), struct thread, ready_elem));
  }

  sema->value++;
  try_priority_yield();
  intr_set_level(old_level);
}

static void sema_test_helper(void *sema_);

/* 세마포어의 동작을 자가 테스트하는 함수로,
   두 스레드 사이에서 제어 흐름이 "핑퐁"처럼 오가도록 만듦.
   동작 과정을 확인하려면 printf() 호출을 삽입해볼 것. */
void sema_self_test(void) {
  struct semaphore sema[2];
  int i;

  printf("Testing semaphores...");
  sema_init(&sema[0], 0);
  sema_init(&sema[1], 0);
  thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) {
    sema_up(&sema[0]);
    sema_down(&sema[1]);
  }
  printf("done.\n");
}

/* sema_self_test()에서 사용하는 스레드 함수 */
static void sema_test_helper(void *sema_) {
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) {
    sema_down(&sema[0]);
    sema_up(&sema[1]);
  }
}

/* LOCK을 초기화함.
   락은 한 번에 최대 하나의 스레드만 소유할 수 있음.
   우리 구현의 락은 "재귀적(recursive)" 락이 아니므로,
   이미 락을 보유하고 있는 스레드가 다시 그 락을 획득하려 하면 오류임.

   락은 초기값이 1인 세마포어의 특수한 형태로 볼 수 있음.
   락과 그런 세마포어 사이의 차이점은 두 가지임.
   첫째, 세마포어는 1보다 큰 값을 가질 수 있지만,
   락은 한 번에 오직 하나의 스레드만 소유할 수 있음.
   둘째, 세마포어는 소유자가 없기 때문에
   한 스레드가 "down"하고 다른 스레드가 "up"할 수도 있지만,
   락은 반드시 동일한 스레드가 획득(acquire)하고 해제(release)해야 함.

   만약 이러한 제약이 너무 불편하다면,
   그 경우에는 락보다 세마포어를 사용하는 것이 더 적절하다는 신호임. */
void lock_init(struct lock *lock) {
  ASSERT(lock != NULL);

  lock->holder = NULL;
  sema_init(&lock->semaphore, 1);
}

/* LOCK을 획득함. 필요하다면 사용 가능해질 때까지 sleep하며 기다림.
   현재 스레드가 이미 이 락을 보유하고 있어서는 안 됨.

   이 함수는 sleep할 수 있으므로 인터럽트 핸들러 내에서 호출하면 안 됨.
   인터럽트가 비활성화된 상태에서도 호출할 수는 있지만,
   sleep이 필요한 경우 인터럽트는 다시 활성화됨. */
void lock_acquire(struct lock *lock) {
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(!lock_held_by_current_thread(lock));

  // Priority donation 우선순위 기부
  struct thread *curr = thread_current();

  if (lock->holder) {
        curr->wait_on_lock = lock;
        list_insert_ordered(&lock->holder->donations, &curr->donation_elem,
                            thread_compare_donate_priority, NULL);
        donate_priority();
    }

    sema_down(&lock->semaphore);
    curr->wait_on_lock = NULL;
    lock->holder = curr;
}


/* LOCK을 획득 시도함. 성공하면 true를 반환하고, 실패하면 false를 반환함.
   현재 스레드가 이미 이 락을 보유하고 있어서는 안 됨.

   이 함수는 sleep하지 않으므로 인터럽트 핸들러 내에서 호출해도 됨. */
bool lock_try_acquire(struct lock *lock) {
  bool success;

  ASSERT(lock != NULL);
  ASSERT(!lock_held_by_current_thread(lock));

  success = sema_try_down(&lock->semaphore);
  if (success)
    lock->holder = thread_current();
  return success;
}

/* 현재 스레드가 보유하고 있는 LOCK을 해제함.
   이 함수는 lock_release 함수임.

   인터럽트 핸들러에서는 락을 획득할 수 없으므로,
   인터럽트 핸들러 내에서 락을 해제하려는 시도는 의미가 없음. */
void lock_release(struct lock *lock) {
  ASSERT(lock != NULL);
  ASSERT(lock_held_by_current_thread(lock)); // <- 여기서 충돌 시
  
  remove_with_lock(lock);
  refresh_priority(); //원래 priority로 복구
  
  lock->holder = NULL;
  sema_up(&lock->semaphore);
  }

/* 현재 스레드가 LOCK을 보유하고 있으면 true를,
   그렇지 않으면 false를 반환함.
   (참고: 다른 스레드가 락을 보유하고 있는지를 검사하는 것은
   경쟁 상태(race condition)를 유발할 수 있음) */
bool lock_held_by_current_thread(const struct lock *lock) {
  ASSERT(lock != NULL);

  return lock->holder == thread_current();
}

/* 조건 변수 COND를 초기화함.
   조건 변수는 한 쪽 코드에서 어떤 조건이 발생했음을 신호로 보내고,
   협력하는 다른 코드가 그 신호를 받아 동작할 수 있도록 해줌. */
void cond_init(struct condition *cond) {
  ASSERT(cond != NULL);

  list_init(&cond->waiters);
}

/* LOCK을 원자적으로 해제한 뒤, COND가 다른 코드에 의해 signal될 때까지 기다림.
   COND가 signal된 후에는 다시 LOCK을 획득하고 반환됨.
   이 함수를 호출하기 전에 LOCK을 반드시 보유하고 있어야 함.

   이 함수가 구현하는 모니터는 \"Mesa\" 스타일이며, \"Hoare\" 스타일이 아님.
   즉, signal을 보내는 쪽과 받는 쪽의 동작이 원자적으로 이루어지지 않음.
   따라서 보통 wait가 끝난 후 조건을 다시 확인하고, 필요하다면 다시 대기해야 함.

   하나의 조건 변수는 단 하나의 락과만 연관될 수 있지만,
   하나의 락은 여러 개의 조건 변수와 연관될 수 있음.
   즉, 락과 조건 변수는 1:N 관계임.

   이 함수는 sleep할 수 있으므로 인터럽트 핸들러에서 호출하면 안 됨.
   인터럽트가 꺼진 상태에서도 호출할 수는 있지만,
   sleep이 필요한 경우 인터럽트는 다시 켜질 수 있음. */
void cond_wait(struct condition *cond, struct lock *lock) {
  struct semaphore_elem waiter;

  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  sema_init(&waiter.semaphore, 0);
  list_push_back(&cond->waiters, &waiter.elem);
  // list_insert_ordered(&cond->waiters, &waiter.elem, sema_compare_priority,
  // NULL);
  lock_release(lock);
  sema_down(&waiter.semaphore);
  lock_acquire(lock);
}

/* 만약 COND(LOCK으로 보호됨)를 기다리는 스레드가 있다면,
   이 함수는 그 중 하나에게 signal을 보내 대기 상태에서 깨어나도록 함.
   이 함수를 호출하기 전에 LOCK을 반드시 보유하고 있어야 함.

   인터럽트 핸들러에서는 락을 획득할 수 없으므로,
   조건 변수를 signal하려는 시도는 인터럽트 핸들러 내에서 의미가 없음. */
void cond_signal(struct condition *cond, struct lock *lock UNUSED) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  if (!list_empty(&cond->waiters)) {
    /* 이미 insert_ordered 했다고 가정했지만, 안전하게 다시 정렬 */
    list_sort(&cond->waiters, sema_compare_priority, NULL);
    struct semaphore_elem *e =
        list_entry(list_pop_front(&cond->waiters), struct semaphore_elem, elem);
    sema_up(&e->semaphore);
  }
}

/* COND(LOCK으로 보호됨)를 기다리는 모든 스레드가 있다면,
   그들을 모두 깨움.
   이 함수를 호출하기 전에 LOCK을 반드시 보유하고 있어야 함.

   인터럽트 핸들러에서는 락을 획득할 수 없으므로,
   조건 변수를 signal하려는 시도는 인터럽트 핸들러 내에서 의미가 없음. */
void cond_broadcast(struct condition *cond, struct lock *lock) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);

  while (!list_empty(&cond->waiters))
    cond_signal(cond, lock);
}