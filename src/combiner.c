#include "combiner.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

#define container_of(ptr, type, member) ({                          \
            typeof( ((type *)0)->member ) *__mptr = (ptr);          \
            (type *)( (char *)__mptr - offsetof(type,member) );})

enum {
    Waiting = 0,
    TakeOver = 1,
    Finished = 2
};

// General util
static void prefetch(void *p);

// Combiner functions
static void do_unlock_combiner(struct combiner* cmb, struct message_metadata *head, int do_work);
static int enter_combiner(struct combiner *cmb, struct message_metadata *msg);
static void notify_waiters(struct combiner* cmb, struct message_metadata *stop_at);
static void do_release(struct combiner *cmb);
static struct message_metadata *perform_work(struct combiner* cmb, struct message_metadata *head);

// Msg Functions
static void prefetch_meta(struct message_metadata *msg);
static struct message_metadata *next_list(struct message_metadata **msg,
                                          struct message_metadata *head,
                                          int advance_end);

void message_combiner(struct combiner* cmb,
                      struct combine_message* msg) {
    async_message_combiner(cmb, msg);
    complete_async_message(cmb, msg);
}


void async_message_combiner(struct combiner* cmb,
                            struct combine_message *msg) {
    msg->_meta.is_done = 0;
    if (enter_combiner(cmb, &msg->_meta)) {
        do_unlock_combiner(cmb, &msg->_meta, 1);
    }
}

void init_combiner(struct combiner *cmb) {
    memset(cmb, 0, sizeof(*cmb));
}

int async_message_status(struct combine_message *msg) {
    !__atomic_load_n(&msg->_meta.is_done, __ATOMIC_RELAXED);
}

void complete_async_message(struct combiner *cmb, struct combine_message *msg) {
    int val;
    while (!(val = __atomic_load_n(&msg->_meta.is_done, __ATOMIC_RELAXED)));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (val == Finished) {
        return;
    }
    else {
        do_unlock_combiner(cmb, &msg->_meta, 1);
    }
}



static void prefetch(void *p) {
    __builtin_prefetch(p, 0, 3);
}

static void do_unlock_combiner(struct combiner* cmb, struct message_metadata *head, int do_work) {
    head = perform_work(cmb, head);
    notify_waiters(cmb, head);
}

static void notify_waiters(struct combiner* cmb, struct message_metadata *stop_at) {
    struct message_metadata *next = next_list(&cmb->queue, stop_at, 1);
    if (next != NULL) {
        __atomic_store_n(&next->is_done, TakeOver, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&stop_at->is_done, Finished, __ATOMIC_RELEASE);
}

static int enter_combiner(struct combiner *cmb, struct message_metadata *msg) {
    msg->next = NULL;

    struct message_metadata *prev = __atomic_exchange_n(&cmb->queue, msg, __ATOMIC_ACQ_REL);

    if (prev != NULL) {
        __atomic_store_n(&prev->next, msg, __ATOMIC_RELEASE);
    }
    return prev == NULL;
}

static struct message_metadata* perform_work(struct combiner* cmb, struct message_metadata *head) {
    struct message_metadata *prev = head;

    assert(head);

    do {
        struct message_metadata* next = next_list(&cmb->queue, head, 0);
        struct combine_message *cur_msg = container_of(head, struct combine_message, _meta);
        prefetch_meta(next);
        cur_msg->operation(cur_msg);
        // We only update the previous since the current head may be used later on
        // This doesn't need to special case for there only being one item because as of now,
        // the first item will always be the current thread's node from list entry or
        // take over
        __atomic_store_n(&prev->is_done, Finished, __ATOMIC_RELEASE);
        prev = head;
        head = next;
    } while(head);

    return prev;
}

static void prefetch_meta(struct message_metadata *msg) {
    if (msg) {
        struct combine_message *msg_m = container_of(msg, struct combine_message, _meta);
        prefetch(msg);
        prefetch(msg_m->operation);
    }
}

static struct message_metadata *next_list(struct message_metadata **queue,
                                          struct message_metadata *head,
                                          int advance_end) {
    struct message_metadata *next = __atomic_load_n(&head->next, __ATOMIC_CONSUME);
    if (next == NULL) {
        struct message_metadata *tmp_head = head;
        if (!advance_end
            || __atomic_compare_exchange_n(queue, &tmp_head, NULL, 1,
                                           __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            return NULL;
        }
        else {
            while (!(next = __atomic_load_n(&head->next, __ATOMIC_CONSUME)));
        }
    }
    else {
        return next;
    }
}
