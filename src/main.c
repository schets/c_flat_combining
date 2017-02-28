#include "combiner.h"

#include <stdio.h>
#include <assert.h>

#include <pthread.h>

typedef struct {
    struct combine_message msg;
    int *val;
    int rval;
} locked_val_msg;

int val;
int num_run = 10000;
int tid = 0;

#define N_THREADS 4

void inc_val(struct combine_message *val) {
    locked_val_msg *msg = (locked_val_msg *)val;
    msg->rval = *msg->val;
    *msg->val += 1;
}

void *perform_incs(void *data) {
    int mtid = __atomic_fetch_add(&tid, 1, __ATOMIC_SEQ_CST);
    printf("Entering %d\n", tid);
    struct combiner *cmb = data;
    for (int i = 0; i < num_run; i++) {
        locked_val_msg m;
        m.msg.operation = inc_val;
        m.msg.prefetch = &val;
        m.val = &val;
        m.rval = -1;
        message_combiner(cmb, &m.msg);
    }
    printf("Exiting %d\n", tid);
    return NULL;
}

int main(int argc, char** argv) {
    struct combiner cmb;
    init_combiner(&cmb);
    val = 0;

    pthread_t threads[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        pthread_create(&threads[i], NULL, perform_incs, &cmb);
    }
    for (int i = 0; i < N_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    printf("%d\n", val);
    assert(val == N_THREADS * num_run);
}
