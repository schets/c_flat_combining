extern "C" {
#include "combiner.h"
}

#include <queue>
#include <random>
#include <vector>

#include <assert.h>
#include <stdio.h>

#include <pthread.h>

typedef struct {
  struct combine_message msg;
  size_t to_add;
  int was_called;
} locked_val_msg;

std::priority_queue<size_t> gqueue;
int gval = 0;
int num_run = 1000000;
int tid = 0;

#define N_THREADS 4

void add_queue(struct combine_message *val) {
  locked_val_msg *msg = (locked_val_msg *)val;
  assert(msg->was_called == 0);
  gqueue.push(msg->to_add);
  gqueue.pop();
  msg->was_called += 1;
}

void *perform_incs(void *data) {
  int mtid = __atomic_fetch_add(&tid, 1, __ATOMIC_SEQ_CST);
  printf("Entering %d\n", mtid);
  std::random_device r;
  std::mt19937_64 rng(r());
  struct combiner *cmb = (struct combiner *)data;
  for (int i = 0; i < num_run; i++) {
    locked_val_msg m;
    m.msg.operation = add_queue;
    m.msg.prefetch = &gqueue;
    m.was_called = 0;
    m.to_add = rng();
    if (0) {
      message_combiner(cmb, &m.msg);
      assert(m.was_called == 1);
    } else {
      lock_combiner(cmb, &m.msg);
      gqueue.push(m.to_add);
      gqueue.pop();
      unlock_combiner(cmb, &m.msg);
    }
  }
  printf("Exiting %d\n", mtid);
  return NULL;
}

int main(int argc, char **argv) {
  std::random_device r;
  std::mt19937_64 rng(r());
  for (int i = 0; i < 5000; i++) {
    gqueue.push(rng());
  }
  struct combiner cmb;
  init_combiner(&cmb);

  pthread_t threads[N_THREADS];
  for (int i = 0; i < N_THREADS; i++) {
    pthread_create(&threads[i], NULL, perform_incs, &cmb);
  }
  for (int i = 0; i < N_THREADS; i++) {
    pthread_join(threads[i], NULL);
  }
}
