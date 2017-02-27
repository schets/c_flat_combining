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
static void spin_on(char *tag);
static int try_lock(char *tag);
static void unlock(char *tag);

// Combiner functions
static void do_unlock_combiner(struct combiner* cmb, int do_work);
static void notify_waiters(struct combiner* cmb);
static void do_release(struct combiner *cmb);
static void perform_work(struct combiner* cmb);
static struct message_metadata *get_work_sublist(struct combiner *cmb);

// Msg Functions
static void run_msg(struct combine_message *msg);
static void enter_queue(struct combine_message *msg, struct combiner *cmb);
static void perform_work_sublist(struct message_metadata* msgs);
static void prefetch_meta(struct message_metadata *msg);

void lock_combiner(struct combiner* cmb) {
    spin_on(&cmb->locked);
}

int try_lock_combiner(struct combiner* cmb) {
    try_lock(&cmb->locked);
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
    complete_async_message(cmb, msg);
}


void async_message_combiner(struct combiner* cmb,
                            struct combine_message *msg) {
    msg->_meta.is_done = 0;
    msg->_meta.next = NULL;
    if (try_lock_combiner(cmb)) {
        run_msg(msg);
        msg->_meta.is_done = 2;
        unlock_combiner(cmb);
    }
    else {
        enter_queue(msg, cmb);
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

void complete_async_message(struct combiner *cmb, struct combine_message *msg) {
    int val;
    while (!(val = __atomic_load_n(&msg->_meta.is_done, __ATOMIC_RELAXED)));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (val == 2) {
        // complete
        return;
    }
    else {
        do_unlock_combiner(cmb, 1);
    }
}



static void prefetch(void *p) {
    __builtin_prefetch(p, 0, 3);
}

static void spin_on(char *tag) {
    /// busywait is temp! Will do actual locking down the line
    do {
        while (__atomic_load_n(tag, __ATOMIC_RELAXED));
    } while (__atomic_test_and_set(tag, __ATOMIC_ACQUIRE));
}

static int try_lock(char *tag) {
    return !__atomic_load_n(tag, __ATOMIC_RELAXED)
           && !__atomic_test_and_set(tag, __ATOMIC_ACQUIRE);
}

static void unlock(char *tag) {
    __atomic_clear(tag, __ATOMIC_RELEASE);
}



static void do_unlock_combiner(struct combiner* cmb, int do_work) {
    if (do_work) {
        perform_work(cmb);
    }

    notify_waiters(cmb);
}

static void notify_waiters(struct combiner* cmb) {
    // TODO: find waiting threads and notify them
    spin_on(&cmb->combiner_spin);
    if (cmb->head) {
        __atomic_store_n(&cmb->head->is_done, 1, __ATOMIC_RELEASE);
        unlock(&cmb->combiner_spin);
    }
    else {
        unlock(&cmb->combiner_spin);
        do_release(cmb);
    }
}

static void do_release(struct combiner* cmb) {
    unlock(&cmb->locked);
}

static void perform_work(struct combiner* cmb) {
    struct message_metadata *cur;

    while (cur = get_work_sublist(cmb)) {
        perform_work_sublist(cur);
    }
}

static struct message_metadata *get_work_sublist(struct combiner *cmb) {
    spin_on(&cmb->combiner_spin);
    struct message_metadata *rval = cmb->head;
    cmb->head = NULL;
    unlock(&cmb->combiner_spin);
    return rval;
}



static void run_msg(struct combine_message *msg) {
    msg->operation(msg);
    __atomic_store_n(&msg->_meta.is_done, 2, __ATOMIC_RELEASE);
}

static void enter_queue(struct combine_message *msg, struct combiner *cmb) {
    spin_on(&cmb->combiner_spin);
    if (try_lock_combiner(cmb)) {
        unlock(&cmb->combiner_spin);
        run_msg(msg);
        msg->_meta.is_done = 2;
        unlock_combiner(cmb);
    }
    else {
        msg->_meta.next = cmb->head;
        cmb->head = &msg->_meta;
        unlock(&cmb->combiner_spin);
    }
}

static void perform_work_sublist(struct message_metadata* head) {
    while (head) {
        struct message_metadata* next = head->next;
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
