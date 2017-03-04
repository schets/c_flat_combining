#ifndef COMBINER_H
#define COMBINER_H

struct combine_message;

/// This is the function type which is called inside the combiner
typedef void (*combine_fn)(struct combine_message *msg);

/// This struct holds the flat-combining lock
struct combiner {
  struct message_metadata *queue;
  int try_takeover;
};

struct message_metadata {
  struct message_metadata *next;
  struct message_metadata *prev;
  struct message_metadata *is_done;
  int blocking_status;
};

/// This struct describes the actions to perform when inside the lock
struct combine_message {

  /// Function to call when inside the lock
  combine_fn operation;

  /// Address of data to prefetch. Can be null
  void *prefetch;

  /// Metadata used by the lock, DO NOT TOUCH
  struct message_metadata _meta;
};

/// Acquires unique access to the combiner like a mutex
/// The passed message does not need any initialization,
/// space is provided simply for the lock. Storing this outside the queue
/// has some nice cache benefits.
void lock_combiner(struct combiner *lck, struct combine_message *msg);

/// Unlocks the combiner that was locked with the tag
void unlock_combiner(struct combiner *lck, struct combine_message *msg);

/// Performs the actions described in the msg struct, blocks until completion
void message_combiner(struct combiner *lck, struct combine_message *msg);

/// Performs the actions described in the msg struct, but does not block.
/// Instead, the message can be queried for status and blocked on until
/// completion
/// Be wary of cache contention on the stack when using this
void async_message_combiner(struct combiner *lck, struct combine_message *msg);

/// Returns the status of an asynchronous message
int async_message_status(struct combine_message *msg);

/// Blocks on an async message tag until completion
void complete_async_message(struct combiner *cmb, struct combine_message *msg);

void init_combiner(struct combiner *cmb);

#endif
