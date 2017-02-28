#include "combiner.h"

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
static void do_unlock_combiner(struct combiner* cmb, int do_work);
static int enter_combiner(struct combiner *cmb, struct combine_message *msg);
static void notify_waiters(struct combiner* cmb);
static void do_release(struct combiner *cmb);
static void perform_work(struct combiner* cmb);

// Msg Functions
static void run_msg(struct combine_message *msg);
static void prefetch_meta(struct message_metadata *msg);
static struct message_metadata *next_list(struct message_metadata **msg,
                                          struct message_metadata *head);

void message_combiner(struct combiner* cmb,
                      struct combine_message* msg) {
    async_message_combiner(cmb, msg);
    complete_async_message(cmb, msg);
}


void async_message_combiner(struct combiner* cmb,
                            struct combine_message *msg) {
    msg->_meta.is_done = 0;
    enter_combiner(cmb, msg);
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
            do_unlock_combiner(cmb, 1);
        }
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
    struct message_metadata *next = next_list(&cmb->queue,
                                              __atomic_load_n(&cmb->queue, __ATOMIC_CONSUME));
    if (next != NULL) {
        __atomic_store_n(&next->is_done, TakeOver, __ATOMIC_RELEASE);
    }
}

static int enter_combiner(struct combiner *cmb, struct combine_message *msg) {
    msg->_meta.next = NULL;

    struct message_metadata *prev = __atomic_exchange_n(&cmb->queue, &msg->_meta, __ATOMIC_ACQ_REL);

    if (prev != NULL) {
        __atomic_store_n(&prev->next, &msg->_meta, __ATOMIC_RELEASE);
    }
    return prev == NULL;
}

static void perform_work(struct combiner* cmb) {
    struct message_metadata *cur;

    struct message_metadata *head = __atomic_load_n(&cmb->queue, __ATOMIC_CONSUME);
    while (head) {
        struct message_metadata* next = next_list(&cmb->queue, head);
        struct combine_message *cur_msg = container_of(head, struct combine_message, _meta);
        prefetch_meta(next);
        run_msg(cur_msg);
        head = next;
    }
}

static void run_msg(struct combine_message *msg) {
    msg->operation(msg);
    __atomic_store_n(&msg->_meta.is_done, Finished, __ATOMIC_RELEASE);
}

static void prefetch_meta(struct message_metadata *msg) {
    if (msg) {
        struct combine_message *msg_m = container_of(msg, struct combine_message, _meta);
        prefetch(msg);
        prefetch(msg_m->operation);
    }
}

static struct message_metadata *next_list(struct message_metadata **queue,
                                          struct message_metadata *head) {
    struct message_metadata *next = __atomic_load_n(&head->next, __ATOMIC_CONSUME);
    if (next == NULL) {
        if (__atomic_compare_exchange_n(queue, head, NULL, 1,
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
