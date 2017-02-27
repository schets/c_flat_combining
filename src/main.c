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

void inc_val(struct combine_message *val) {
    locked_val_msg *msg = (locked_val_msg *)val;
    msg->rval = *msg->val;
    *msg->val += 1;
}

int main(int argc, char** argv) {
    struct combiner cmb;
    init_combiner(&cmb);
    val = 0;

    for (int i = 0; i < 10; i++) {
        locked_val_msg m;
        m.msg.operation = inc_val;
        m.msg.prefetch = &val;
        m.val = &val;
        m.rval = -1;
        message_combiner(&cmb, &m.msg);
        assert(m.rval != -1);
    }
}
