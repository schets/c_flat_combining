#ifndef COMBINER_H
#define COMBINER_H

struct combine_message;

/// This is the function type which is called inside the combiner
typedef void (*combine_fn)(struct combine_message *msg);

/// This struct holds the flat-combining lock
struct combiner {
    struct message_metadata *head;
    char locked;
};

struct message_metadata {
    struct message_metadata *next;
    int is_done;
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

/// Acquires the combiner like a normal lock
void lock_combiner(struct combiner* lck);

/// Attempts to lock the combiner, returns nonzero on success
int try_lock_combiner(struct combiner* lck);

/// Releases the combiner lock and helps waiting threads
void unlock_combiner(struct combiner* lck);

/// Releases the combiner thread without helping waiting threasd
void unlock_combiner_now(struct combiner *lck);

/// Performs the actions described in the msg struct, blocks until completion
void message_combiner(struct combiner* lck,
                      struct combine_message *msg);

/// Performs the actions described in the msg struct, but does not block.
/// Instead, the message can be queried for status and blocked on until completion
void async_message_combiner(struct combiner* lck,
                            struct combine_message *msg);

/// Returns the status of an asynchronous message
int async_message_status(struct combine_message* msg);

/// Blocks on an async message tag until completion
void complete_async_message(struct combine_message* msg);

void init_combiner(struct combiner *cmb);

#endif
