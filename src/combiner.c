#include "combiner.h"

#include <stddef.h>
#include <string.h>

#define container_of(ptr, type, member) ({                      \
        typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})

typedef struct {
    struct combine_message *work_msg[16];
} visible_list;

// General util
static void prefetch(void *p);

// Combiner functions
static void do_unlock_combiner(struct combiner* cmb, int do_work);
static void notify_waiters(struct combiner* cmb);
static void do_release(struct combiner *cmb);
static void perform_work(struct combiner* cmb);

// Msg Functions
static void run_msg(struct combine_message *msg);
static void enter_queue(struct message_metadata *msg, struct message_metadata **head);
static void perform_work_sublist(struct message_metadata* msgs);
static void prefetch_meta(struct message_metadata *msg);

void lock_combiner(struct combiner* cmb) {
    /// busywait is temp! Will do actual locking down the line
    do {
        while (__atomic_load_n(&cmb->locked, __ATOMIC_RELAXED));
    } while (!__atomic_test_and_set(&cmb->locked, __ATOMIC_ACQUIRE));
}

int try_lock_combiner(struct combiner* cmb) {
    return !__atomic_load_n(&cmb->locked, __ATOMIC_RELAXED)
           && !__atomic_test_and_set(&cmb->locked, __ATOMIC_ACQUIRE);
}

void unlock_combiner(struct combiner* cmb) {
    do_unlock_combiner(cmb, 1);
}

void unlock_combiner_now(struct combiner* cmb) {
    do_unlock_combiner(cmb, 0);
}

void message_combiner(struct combiner* cmb,
                      struct combine_message* msg) {
    async_message_combiner(cmb, msg);
    complete_async_message(msg);
}


void async_message_combiner(struct combiner* cmb,
                            struct combine_message *msg) {
    msg->_meta.is_done = 0;
    msg->_meta.next = NULL;
    if (try_lock_combiner(cmb)) {
        run_msg(msg);
        msg->_meta.is_done = 1;
        unlock_combiner(cmb);
    }
    else {
        enter_queue(&msg->_meta, &cmb->head);
        if (try_lock_combiner(cmb)) {
            do_unlock_combiner(cmb, !__atomic_load_n(&msg->_meta.is_done, __ATOMIC_ACQUIRE));
        }
    }
}

void init_combiner(struct combiner *cmb) {
    memset(cmb, 0, sizeof(*cmb));
}

int async_message_status(struct combine_message *msg) {
    !__atomic_load_n(&msg->_meta.is_done, __ATOMIC_RELAXED);
}

void complete_async_message(struct combine_message *msg) {
    while (!__atomic_load_n(&msg->_meta.is_done, __ATOMIC_RELAXED));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

static void prefetch(void *p) {
    __builtin_prefetch(p, 0, 3);
}

static void do_unlock_combiner(struct combiner* cmb, int do_work) {
    if (do_work) {
        perform_work(cmb);
    }

    notify_waiters(cmb);
}

static void notify_waiters(struct combiner* cmb) {
    // TODO: find waiting threads and notify them
    do_release(cmb);
}

static void do_release(struct combiner* cmb) {
    __atomic_clear(&cmb->locked, __ATOMIC_RELEASE);
}

static void perform_work(struct combiner* cmb) {
    struct message_metadata *cur;

    while (cmb->head
           && (cur = __atomic_exchange_n(&cmb->head, NULL, __ATOMIC_ACQUIRE))) {
        perform_work_sublist(cur);
    }
}



static void run_msg(struct combine_message *msg) {
    msg->operation(msg);
}

// TODO: make entry into the waitlist not require atomics?
// Could do a sort of thread-local, or optimistically register threads
// to a spot and let late ones enter a list

static void enter_queue(struct message_metadata *msg, struct message_metadata **head) {
    struct message_metadata *cur_head = __atomic_load_n(head, __ATOMIC_RELAXED);
    do {
        msg->next = cur_head;
    } while (!__atomic_compare_exchange_n(head, &cur_head, msg, 0,
                                          __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
}

static void perform_work_sublist(struct message_metadata* head) {
    while (head) {
        struct message_metadata* next = __atomic_load_n(&head->next, __ATOMIC_CONSUME);
        struct combine_message *cur_msg = container_of(head, struct combine_message, _meta);
        prefetch_meta(next);
        run_msg(cur_msg);
        head = next;
    }
}

static void prefetch_meta(struct message_metadata *msg) {
    if (msg) {
        struct combine_message *msg_m = container_of(msg, struct combine_message, _meta);
        prefetch(msg);
        prefetch(msg_m->operation);
    }
}
