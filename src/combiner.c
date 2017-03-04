#include "combiner.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

#define container_of(ptr, type, member)                                        \
  ({                                                                           \
    typeof(((type *)0)->member) *__mptr = (ptr);                               \
    (type *)((char *)__mptr - offsetof(type, member));                         \
  })

// For Intel, Arm, Power, Sparc, Mips, Power only currently
// Since compilers conservatively have consume be acquire in
// case of bizzare avoidance of optimized-away data dependencies
// But I would rather be conservative and whitelist architectures instead
#if (defined __amd64__) || (defined __arm__) || (defined __thumb__) ||         \
    (defined __aarch64__) || (defined __i386__) || (defined __sparc__) ||      \
    (defined __mips__) || (defined __powerpc__) || (defined __powerpc64__)
#define ARCH_CONSUME __ATOMIC_RELAXED
#else
#define ARCH_CONSUME __ATOMIC_CONSUME
#endif

#define next_list(ptr) __atomic_load_n(&(ptr)->next, ARCH_CONSUME)
#define prefetch(p) __builtin_prefetch((p), 0, 3)

#define Waiting (void *)0
#define Finished (void *)1

#define MAX_RUN 10

// Combiner functions
static void unlock_work_combiner(struct combiner *cmb,
                                 struct message_metadata *head,
                                 struct message_metadata *must_finish,
                                 int do_work);
static int enter_combiner(struct combiner *cmb, struct message_metadata *msg);
static void notify_waiters(struct combiner *cmb,
                           struct message_metadata *stop_at);
static void do_release(struct combiner *cmb);
static struct message_metadata *perform_work(struct combiner *cmb,
                                             struct message_metadata *head);

// Msg Functions
static void prefetch_meta(struct message_metadata *msg);

static struct message_metadata *advance(struct message_metadata **msg,
                                        struct message_metadata *head);

static void remove_from_queue(struct message_metadata **queue,
                              struct message_metadata *find);

void lock_combiner(struct combiner *cmb, struct combine_message *msg) {
  msg->operation = NULL;
  msg->_meta.is_done = Waiting;
  msg->_meta.blocking_status = 0;
  if (enter_combiner(cmb, &msg->_meta)) {
    return;
  }
  while (__atomic_load_n(&msg->_meta.is_done, __ATOMIC_RELAXED) == Waiting)
    ;
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  return;
}

void unlock_combiner(struct combiner *cmb, struct combine_message *tag) {
  // This needs some internal modifications to perform work with locks faster
  notify_waiters(cmb, &tag->_meta);
}

void message_combiner(struct combiner *cmb, struct combine_message *msg) {
  msg->_meta.blocking_status = 0;
  async_message_combiner(cmb, msg);
  complete_async_message(cmb, msg);
}

void async_message_combiner(struct combiner *cmb, struct combine_message *msg) {
  msg->_meta.is_done = Waiting;
  if (enter_combiner(cmb, &msg->_meta)) {
    unlock_work_combiner(cmb, &msg->_meta, NULL, 1);
  }
}

void init_combiner(struct combiner *cmb) { memset(cmb, 0, sizeof(*cmb)); }

int async_message_status(struct combine_message *msg) {
  return !__atomic_load_n(&msg->_meta.is_done, __ATOMIC_RELAXED);
}

void complete_async_message(struct combiner *cmb, struct combine_message *msg) {
  struct message_metadata *val;
  while (!(val = __atomic_load_n(&msg->_meta.is_done, __ATOMIC_RELAXED)))
    ;
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if (val == Finished) {
    return;
  } else {
    if (msg->operation != NULL) {
      unlock_work_combiner(cmb, val, &msg->_meta, 1);
    }
  }
}

static void unlock_work_combiner(struct combiner *cmb,
                                 struct message_metadata *head,
                                 struct message_metadata *must_finish,
                                 int do_work) {
  head = perform_work(cmb, head);

  if (must_finish) {
    if (must_finish != head && must_finish->is_done == Waiting) {
      // If must_finish is equal to the 'head' then it's already finished
      // Otherwise, it must be completed.
      remove_from_queue(&cmb->queue, must_finish);
    }
  }

  notify_waiters(cmb, head);
}

static void notify_waiters(struct combiner *cmb,
                           struct message_metadata *stop_at) {
  // advance past stop_at definitvely, this must be done before we release
  // stop_at from the queue
  struct message_metadata *next = advance(&cmb->queue, stop_at);
  __atomic_store_n(&stop_at->is_done, Finished, __ATOMIC_RELAXED);
  if (next != NULL) {
    // This is fine without an acquire fence - in the case of not
    // returning null, next is loaded with at least consume ordering
    // and this holds a data dependency on next
    __atomic_store_n(&next->is_done, next, __ATOMIC_RELAXED);
  }
}

static int enter_combiner(struct combiner *cmb, struct message_metadata *msg) {
  msg->next = NULL;

  struct message_metadata *prev =
      __atomic_exchange_n(&cmb->queue, msg, __ATOMIC_RELEASE);

  if (prev != NULL) {
    __atomic_store_n(&prev->next, msg, __ATOMIC_RELEASE);
  }
  msg->prev = prev;
  return prev == NULL;
}
static struct message_metadata *perform_work(struct combiner *cmb,
                                             struct message_metadata *head) {
  struct message_metadata *prev = head;

  assert(head);

  int nrun = 0;
  do {
    struct message_metadata *next = next_list(head);
    struct combine_message *cur_msg =
        container_of(head, struct combine_message, _meta);
    if (nrun < MAX_RUN || cur_msg->operation != NULL) {
      ++nrun;
      prefetch_meta(next);
      cur_msg->operation(cur_msg);
      // We only update the previous since the current head may be used later on
      // This doesn't need to special case for there only being one item because
      // as of now, the first item will always be the current thread's node from
      // list entry or take over
      __atomic_store_n(&prev->is_done, Finished, __ATOMIC_RELEASE);
      prev = head;
    } else {
      next = NULL;
    }
    head = next;
  } while (head);

  return prev;
}

static void prefetch_meta(struct message_metadata *msg) {
  return;
  if (msg) {
    struct combine_message *msg_m =
        container_of(msg, struct combine_message, _meta);
    prefetch(msg);
    prefetch(msg_m->operation);
  }
}

static struct message_metadata *advance(struct message_metadata **queue,
                                        struct message_metadata *head) {
  struct message_metadata *next = next_list(head);
  __atomic_thread_fence(__ATOMIC_ACQ_REL);
  if (next == NULL) {
    struct message_metadata *tmp_head = head;
    if (__atomic_compare_exchange_n(queue, &tmp_head, NULL, 1, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED)) {
      next = NULL;
    } else {
      while (!(next = next_list(head)))
        ;
    }
  }
  return next;
}

static void remove_from_queue(struct message_metadata **queue,
                              struct message_metadata *find) {
  // Do the work to be found
  struct combine_message *cur_msg =
      container_of(find, struct combine_message, _meta);
  cur_msg->operation(cur_msg);
  find->is_done = Finished;

  // Find the previous pointer in the list to this one
  struct message_metadata *prev = find->prev;

  if (prev == NULL) {
    // find is the first in the queue, we can advance it like any other
    advance(queue, find);
  } else {
    assert(prev->next == find);
    struct message_metadata *next = next_list(find);
    if (next) {
      // Find is internal to the list, and we dont need to worry about
      // advancing the head. There's a data dependency on next, so we're
      // good without a fence
      __atomic_store_n(&prev->next, next, __ATOMIC_RELEASE);
    } else {
      // This is only ever read by the thread holding the lock
      // so It's ok if this store is ordered-before the read
      prev->next = NULL;
      struct message_metadata *tmp_find = find;
      // Since find->next was null, we expect *queue to equal find.
      // If this is still true, then we CAS *queue to find prev.
      // If the cas succeeds, all is well. If it fails, we must
      // wait for the next ptr of find to get set and then set prev accordingly
      if (!__atomic_compare_exchange_n(queue, &tmp_find, prev, 1,
                                       __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        // CAS failed, wait for find->next and then go bananas
        while (!(next = next_list(find)))
          ;
        // Ordered after next_list due to consume and data dependency
        __atomic_store_n(&prev->next, next, __ATOMIC_RELAXED);
      }
    }
  }
}
