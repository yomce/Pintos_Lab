#include "devices/timer.h"
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>

/* 8254 타이머 칩의 하드웨어 세부사항은 [8254] 문서를 참고하세요. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* OS가 부팅된 이후로 지난 타이머 틱(tick)의 수
변수(ticks)는 운영체제가 부팅된 이후 지금까지 얼마나 시간이 흘렀는지를 "틱
단위"로 나타내는 값*/
static int64_t ticks;

/* 타이머 틱(tick)당 루프(loop) 횟수
timer_calibrate() 함수에서 초기화됨 */
/*한 틱 동안 CPU가 몇 번 루프를 돌 수 있는지를 저장*/
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops(unsigned loops);
static void busy_wait(int64_t loops);
static void real_time_sleep(int64_t num, int32_t denom);

/* 8254 프로그래머블 인터벌 타이머(PIT)를 설정하여
초당 PIT_FREQ 회 인터럽트를 발생시키고,
해당 인터럽트를 처리할 핸들러를 등록함 */
/*8254 PIT는 일정한 간격마다 CPU에 인터럽트를 보내주는 하드웨어 타이머.
이 코드는 그 타이머를 설정해서, 예를 들어 초당 100번, 1000번 등 원하는 주기만큼
인터럽트를 발생시키게 만듦 동시에 그 인터럽트가 발생했을 때 실행할 함수(보통
timer_interrupt)도 인터럽트 핸들러로 등록함 즉, 운영체제가 시간 개념을 가질 수
있게 해주는 기본 시계 역할을 설정하는 부분*/
void timer_init(void) {
  /* 8254 입력 주파수를 TIMER_FREQ로 나눈 값이며, 가장 가까운 정수로 반올림함 */
  /*8254 타이머 칩은 보통 기본 입력 주파수 1,193,180Hz를 사용
  그런데 핀토스는 이 타이머를 초당 TIMER_FREQ번 인터럽트를 발생시키도록 설정하고
  싶어 함. (예: #define TIMER_FREQ 100이면 초당 100번 인터럽트) divisor(설정할
  분주 값) = 1193180 / TIMER_FREQ*/
  uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

  outb(0x43, 0x34); /* CW(Control Word): 카운터 0 사용, LSB 먼저 그다음 MSB,
                       모드 2, 이진 모드 */
  outb(0x40, count & 0xff);
  outb(0x40, count >> 8);

  intr_register_ext(0x20, timer_interrupt, "8254 Timer");
}

/* 짧은 지연을 구현할 때 사용하는 loops_per_tick 값을 보정(calibrate)함
CPU가 얼마나 빠른지 체크해서, 잠깐 멈추고 싶을 때 몇 번 루프 돌면 될지 계산하는
함수*/
void timer_calibrate(void) {
  unsigned high_bit, test_bit;

  ASSERT(intr_get_level() == INTR_ON);
  printf("Calibrating timer...  ");

  /* loops_per_tick 값을 타이머 틱 하나보다 작은 값 중 가장 큰 2의 거듭제곱으로
   * 근사함 */
  loops_per_tick = 1u << 10;
  while (!too_many_loops(loops_per_tick << 1)) {
    loops_per_tick <<= 1;
    ASSERT(loops_per_tick != 0);
  }

  /* loops_per_tick 값의 다음 8비트를 정밀하게 보정함 */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops(high_bit | test_bit))
      loops_per_tick |= test_bit;

  printf("%'" PRIu64 " loops/s.\n", (uint64_t)loops_per_tick * TIMER_FREQ);
}

/* 운영체제가 부팅된 이후 지난 타이머 틱(tick)의 수를 반환함 */
int64_t timer_ticks(void) {
  enum intr_level old_level = intr_disable();
  int64_t t = ticks;
  intr_set_level(old_level);
  barrier();
  return t;
}

/* 인자로 받은 THEN 시점 이후 경과한 타이머 틱 수를 반환함.
THEN은 이전에 timer_ticks()가 반환한 값이어야 함 */
int64_t timer_elapsed(int64_t then) { return timer_ticks() - then; }

/* sleep_list에 넣을 우선순위를 비교 */
bool sleep_compare(const struct list_elem *a, const struct list_elem *b,
                   void *aux UNUSED) {
  struct thread *ta = list_entry(a, struct thread, ready_elem);
  struct thread *tb = list_entry(b, struct thread, ready_elem);

  return ta->wake_up_tick < tb->wake_up_tick;
}

/* 약 TICKS 틱 동안 현재 스레드의 실행을 일시 중단함 */
void timer_sleep(int64_t ticks) {
  int64_t start = timer_ticks();

  ASSERT(intr_get_level() == INTR_ON);
  // while (timer_elapsed (start) < ticks)
  // 	thread_yield ();

  thread_sleep(start + ticks);
}

/* 약 MS 밀리초 동안 현재 스레드의 실행을 일시 중단함 */
void timer_msleep(int64_t ms) { real_time_sleep(ms, 1000); }

/* 약 US 마이크로초(μs) 동안 현재 스레드의 실행을 일시 중단함 */
void timer_usleep(int64_t us) { real_time_sleep(us, 1000 * 1000); }

/* 약 NS 나노초(ns) 동안 현재 스레드의 실행을 일시 중단함 */
void timer_nsleep(int64_t ns) { real_time_sleep(ns, 1000 * 1000 * 1000); }

/* 타이머 통계 정보를 출력함 */
void timer_print_stats(void) {
  printf("Timer: %" PRId64 " ticks\n", timer_ticks());
}

/* 타이머 인터럽트 핸들러 */
static void timer_interrupt(struct intr_frame *args UNUSED) {
  ticks++;
  thread_awake(ticks);
  thread_tick();
}

/* LOOPS번 루프를 돌면 1 틱 이상 걸리는 경우 true를 반환하고,
그렇지 않으면 false를 반환함 */
static bool too_many_loops(unsigned loops) {
  /* 하나의 타이머 틱이 지나갈 때까지 기다림 */
  int64_t start = ticks;
  while (ticks == start)
    barrier();

  /* LOOPS 횟수만큼 루프를 반복 실행함 */
  start = ticks;
  busy_wait(loops);

  /* 틱(tick) 값이 바뀌었다면, 루프를 너무 오래 반복한 것임 */
  barrier();
  return start != ticks;
}

/* 간단한 루프를 LOOPS 횟수만큼 반복하여
짧은 지연(brief delay)을 구현함.

이 함수는 NO_INLINE 속성으로 표시되어 있음.
왜냐하면 코드 정렬(code alignment)이 실행 시간에 큰 영향을 줄 수 있기 때문에,
이 함수가 서로 다른 위치에 인라인(inline)될 경우
시간 측정 결과가 예측 불가능해질 수 있음. */
static void NO_INLINE busy_wait(int64_t loops) {
  while (loops-- > 0)
    barrier();
}

/* 약 NUM/DENOM 초 동안 잠시 실행을 중단함 */
static void real_time_sleep(int64_t num, int32_t denom) {
  /* NUM/DENOM 초를 타이머 틱(tick) 단위로 변환하며,
  소수점 이하는 버림(내림) 처리함.

  변환 과정:

   (NUM / DENOM) 초
  -------------------------  =  NUM * TIMER_FREQ / DENOM 틱
  1초당 TIMER_FREQ 틱수
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT(intr_get_level() == INTR_ON);
  if (ticks > 0) {
    /* 최소한 한 틱(tick) 이상 기다릴 예정이므로, timer_sleep()을 사용함.
    이 함수는 CPU를 다른 프로세스(스레드)에게 양보(yield)함. */
    timer_sleep(ticks);
  } else {
    /* 그렇지 않은 경우(한 틱보다 짧게 기다릴 경우),
    더 정밀한 틱 이하(sub-tick) 단위의 타이밍을 위해 busy-wait 루프를 사용함.
    오버플로우를 방지하기 위해 분자(numerator)와 분모(denominator)를
    1000으로 나누어 크기를 줄임. */
    ASSERT(denom % 1000 == 0);
    busy_wait(loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
  }
}
