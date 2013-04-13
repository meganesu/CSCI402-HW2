#include "globals.h"
#include "errno.h"

#include "util/debug.h"

#include "proc/kthread.h"
#include "proc/kmutex.h"

/*
 * IMPORTANT: Mutexes can _NEVER_ be locked or unlocked from an
 * interrupt context. Mutexes are _ONLY_ lock or unlocked from a
 * thread context.
 */

void
kmutex_init(kmutex_t *mtx)
{
        sched_queue_init(&mtx->km_waitq);
        mtx->km_holder = NULL;

        /* NOT_YET_IMPLEMENTED("PROCS: kmutex_init"); */
}

/*
 * This should block the current thread (by sleeping on the mutex's
 * wait queue) if the mutex is already taken.
 *
 * No thread should ever try to lock a mutex it already has locked.
 */
void
kmutex_lock(kmutex_t *mtx)
{
        /* If someone has the mutex, go to sleep */
        while (mtx->km_holder != NULL) {
          sched_sleep_on(&mtx->km_waitq);
        }

        /* If you get here, you've been woken up and no one owns mutex */
        mtx->km_holder = curthr;

        /* NOT_YET_IMPLEMENTED("PROCS: kmutex_lock"); */
}

/*
 * This should do the same as kmutex_lock, but use a cancellable sleep
 * instead.
 */
int
kmutex_lock_cancellable(kmutex_t *mtx)
{
        int retval;

        /* If someone has the mutex, go to sleep */
        while (mtx->km_holder != NULL) {
          retval = sched_cancellable_sleep_on(&mtx->km_waitq);

          if (retval == -EINTR) return -EINTR;
        }

        /* If you get here, you've been woken up and no one owns mutex */
        mtx->km_holder = curthr;
        return 0;

        /* NOT_YET_IMPLEMENTED("PROCS: kmutex_lock_cancellable"); */
}

/*
 * If there are any threads waiting to take a lock on the mutex, one
 * should be woken up and given the lock.
 *
 * Note: This should _NOT_ be a blocking operation!
 *
 * Note: Don't forget to add the new owner of the mutex back to the
 * run queue.
 *
 * Note: Make sure that the thread on the head of the mutex's wait
 * queue becomes the new owner of the mutex.
 *
 * @param mtx the mutex to unlock
 */
void
kmutex_unlock(kmutex_t *mtx)
{
        if (curthr == mtx->km_holder) { /* Only unlock mutex if you own it */
          mtx->km_holder = NULL;

          if (sched_queue_empty(&mtx->km_waitq)) return;

          /* Wake up someone who's waiting on the mutex */
          kthread_t *thr = sched_wakeup_on(&mtx->km_waitq);

        }
        else dbg_print("Oops! Current thread does not own mutex.\n");

        /* NOT_YET_IMPLEMENTED("PROCS: kmutex_unlock"); */
}
