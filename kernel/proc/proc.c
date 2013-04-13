#include "kernel.h"
#include "config.h"
#include "globals.h"
#include "errno.h"

#include "util/debug.h"
#include "util/list.h"
#include "util/string.h"
#include "util/printf.h"

#include "proc/kthread.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "proc/proc.h"

#include "mm/slab.h"
#include "mm/page.h"
#include "mm/mmobj.h"
#include "mm/mm.h"
#include "mm/mman.h"

#include "vm/vmmap.h"

#include "fs/vfs.h"
#include "fs/vfs_syscall.h"
#include "fs/vnode.h"
#include "fs/file.h"

proc_t *curproc = NULL; /* global */
static slab_allocator_t *proc_allocator = NULL;

static list_t _proc_list;
static proc_t *proc_initproc = NULL; /* Pointer to the init process (PID 1) */

void
proc_init()
{
        list_init(&_proc_list);
        proc_allocator = slab_allocator_create("proc", sizeof(proc_t));
        KASSERT(proc_allocator != NULL);
}

static pid_t next_pid = 0;

/**
 * Returns the next available PID.
 *
 * Note: Where n is the number of running processes, this algorithm is
 * worst case O(n^2). As long as PIDs never wrap around it is O(n).
 *
 * @return the next available PID
 */
static int
_proc_getid()
{
        proc_t *p;
        pid_t pid = next_pid;
        while (1) {
failed:
                list_iterate_begin(&_proc_list, p, proc_t, p_list_link) {
                        if (p->p_pid == pid) {
                                if ((pid = (pid + 1) % PROC_MAX_COUNT) == next_pid) {
                                        return -1;
                                } else {
                                        goto failed;
                                }
                        }
                } list_iterate_end();
                next_pid = (pid + 1) % PROC_MAX_COUNT;
                return pid;
        }
}

/*
 * The new process, although it isn't really running since it has no
 * threads, should be in the PROC_RUNNING state.
 *
 * Don't forget to set proc_initproc when you create the init
 * process. You will need to be able to reference the init process
 * when reparenting processes to the init process.
 */
proc_t *
proc_create(char *name)
{
        /* Allocate proc structure (using slab allocator) */
        proc_t *new_proc = slab_obj_alloc(proc_allocator); /* slab_obj_alloc(struct slab_allocator *allocator) */
        /* Returns pointer (void *) to object allocated */

        /* dbg_print("Allocated proc structure\n"); */

        /* Initialize data structures */
        /*   p_pid: Process ID number */
        new_proc->p_pid = _proc_getid();
        dbg_print("Initialize pid, %d\n", new_proc->p_pid);

        /* Set proc_initproc if init process (p_pid == 1) */
        if (new_proc->p_pid == 1) { proc_initproc = new_proc;
          /* dbg_print("Set proc_initproc if pid == 1\n");  */
        }

        /*   p_comm[PROC_NAME_LEN]: Name of process */
        strncpy((char *)&new_proc->p_comm, name, PROC_NAME_LEN);
        /* strncpy doesn't necessarily add the null character to the end of the string */
        new_proc->p_comm[PROC_NAME_LEN-1] = '\0';
        /* dbg_print("Initialize name of proc\n"); */

        /*   p_threads: List of threads for process */
        list_init(&new_proc->p_threads);
        /* dbg_print("Initialize list of proc threads\n"); */
        /*   p_children: List for process's children processes */
        list_init(&new_proc->p_children);
        /* dbg_print("Initialized list of proc children\n"); */

        /*   *p_pproc: Pointer to parent process
         *               since curproc called proc_create(), that's the parent
         */
        new_proc->p_pproc = curproc;
        /* dbg_print("Initialized parent proc pointer\n"); */

        /*   p_status: Don't need to set until process exits */

        /*   p_state: Should be PROC_RUNNING */
        new_proc->p_state = PROC_RUNNING;
        /* dbg_print("Set proc state\n"); */

        /*   p_wait: Wait queue for process */
        sched_queue_init(&new_proc->p_wait);
        /* dbg_print("Initialized wait queue for proc\n"); */

        /* Make links. Add to child list for curproc. Add to list of all processes. */
        /*   p_list_link: Link for list of all processes */
        list_link_init(&new_proc->p_list_link);
        list_insert_tail(&_proc_list, &new_proc->p_list_link);
        /* dbg_print("Initialized and set list link for proc list\n"); */
        /*   p_child_link: Link for list of children of parent process */
        if (new_proc->p_pid != PID_IDLE) { /* If you're the idle process, you don't have a parent */
          list_link_init(&new_proc->p_child_link);
          /* dbg_print("Initialized list link for parent proc's child list\n"); */
          list_insert_tail(&curproc->p_children, &new_proc->p_child_link);
          /* dbg_print("Set list link for parent proc's child list\n"); */
        }

        /* Set up page table. pt_create_pagedir(), in kernel/mm/pagetable.c */
        /*   *p_pagedir */
        new_proc->p_pagedir = pt_create_pagedir(); /* Returns pointer to pagedir_t created */
        /* dbg_print("Allocated page table\n"); */

        return new_proc;

        /* NOT_YET_IMPLEMENTED("PROCS: proc_create");
        return NULL; */
}

/**
 * Cleans up as much as the process as can be done from within the
 * process. This involves:
 *    - Closing all open files (VFS)
 *    - Cleaning up VM mappings (VM)
 *    - Waking up its parent if it is waiting
 *    - Reparenting any children to the init process
 *    - Setting its status and state appropriately
 *
 * The parent will finish destroying the process within do_waitpid (make
 * sure you understand why it cannot be done here). Until the parent
 * finishes destroying it, the process is informally called a 'zombie'
 * process.
 *
 * This is also where any children of the current process should be
 * reparented to the init process (unless, of course, the current
 * process is the init process. However, the init process should not
 * have any children at the time it exits).
 *
 * Note: You do _NOT_ have to special case the idle process. It should
 * never exit this way.
 *
 * @param status the status to exit the process with
 */
void
proc_cleanup(int status)
{
        /* Wake up parent if it's sleeping */
        /*   Note: parent may be sleeping on its own wait queue, or curproc wait queue */
        sched_broadcast_on(&curproc->p_pproc->p_wait);
        sched_broadcast_on(&curproc->p_wait);

        /* Reparent children processes to init process */
        proc_t *p;
        list_iterate_begin(&curproc->p_children, p, proc_t, p_child_link) {

          /* Remove child proc from curproc's p_children */
          list_remove(&p->p_child_link);

          /* Add child to list of init proc's children */
          list_insert_tail(&proc_initproc->p_children, &p->p_child_link);

          /* Change child proc's p_pproc parent pointer */
          p->p_pproc = proc_initproc;

        } list_iterate_end();

        /* Set exit status and state */
        curproc->p_status = status;
        curproc->p_state = PROC_DEAD;

        /* Context switch out of curproc forever */
        sched_switch();

        /* NOT_YET_IMPLEMENTED("PROCS: proc_cleanup"); */
}

/*
 * This has nothing to do with signals and kill(1).
 *
 * Calling this on the current process is equivalent to calling
 * do_exit().
 *
 * In Weenix, this is only called from proc_kill_all.
 */
void
proc_kill(proc_t *p, int status)
{
        if (p == curproc) {
          dbg_print("Killing curproc\n");
          do_exit(status); /* curproc will never return from here */
        }
        
        /* If you get here, you need to cancel thread for p */
        kthread_t *thr;
        list_iterate_begin(&p->p_threads, thr, kthread_t, kt_plink) {
          kthread_cancel(thr, &status);
        } list_iterate_end();

        /* NOT_YET_IMPLEMENTED("PROCS: proc_kill"); */
}

/*
 * Remember, proc_kill on the current process will _NOT_ return.
 * Don't kill direct children of the idle process.
 *
 * In Weenix, this is only called by sys_halt.
 */
void
proc_kill_all()
{
        /* Cancel all threads in processes not children of idle process */

        /* Walk global process list and spread bad news. DON'T KILL INITPROC. */

        /* Call proc_kill() somewhere in this function */

        /* proc_kill() on curproc LAST */

        proc_t *p;

        /*
        list_iterate_begin(&_proc_list, p, proc_t, p_list_link) {
          dbg_print("Process alive before walking list: %d\n", p->p_pid);
        } list_iterate_end();
        */

        list_iterate_begin(&_proc_list, p, proc_t, p_list_link) {
          if (p != curproc && p->p_pid > PID_INIT) { /* Don't kill idle, init, or curproc */
            proc_kill(p, 0);
          }
        } list_iterate_end();
        /* dbg_print("Cancel request sent to all other processes. Yay.\n"); */

        /*
        list_iterate_begin(&_proc_list, p, proc_t, p_list_link) {
          dbg_print("Process still alive: %d\n", p->p_pid);
        } list_iterate_end();
        */
        /* dbg_print("curproc pid: %d\n", curproc->p_pid); */

        /* Once you've gotten rid of all of these, then you can kill curproc */
        if (curproc->p_pid > PID_INIT) {
          proc_kill(curproc, 0);
          /* dbg_print("Killed current process. Yay.\n"); */
        }

        /* NOT_YET_IMPLEMENTED("PROCS: proc_kill_all"); */
}

proc_t *
proc_lookup(int pid)
{
        proc_t *p;
        list_iterate_begin(&_proc_list, p, proc_t, p_list_link) {
                if (p->p_pid == pid) {
                        return p;
                }
        } list_iterate_end();
        return NULL;
}

list_t *
proc_list()
{
        return &_proc_list;
}

/*
 * This function is only called from kthread_exit.
 *
 * Unless you are implementing MTP, this just means that the process
 * needs to be cleaned up and a new thread needs to be scheduled to
 * run. If you are implementing MTP, a single thread exiting does not
 * necessarily mean that the process should be exited.
 */
void
proc_thread_exited(void *retval)
{
        /* Make sure threads are all done */
        /*   guaranteed, since kthread_exit sets thread state to KT_EXITED
         *   and only one thread per process, so all have exited */
        int *status_ptr = (int *) retval;
        int status;
        if (status_ptr == NULL) status = 0;
        else { status = *status_ptr; }
        proc_cleanup(status);

        /* NOT_YET_IMPLEMENTED("PROCS: proc_thread_exited"); */
}

/* If pid is -1 dispose of one of the exited children of the current
 * process and return its exit status in the status argument, or if
 * all children of this process are still running, then this function
 * blocks on its own p_wait queue until one exits.
 *
 * If pid is greater than 0 and the given pid is a child of the
 * current process then wait for the given pid to exit and dispose
 * of it.
 *
 * If the current process has no children, or the given pid is not
 * a child of the current process return -ECHILD.
 *
 * Pids other than -1 and positive numbers are not supported.
 * Options other than 0 are not supported.
 */
pid_t
do_waitpid(pid_t pid, int options, int *status)
{

/**
 * This function implements the waitpid(2) system call.
 *
 * @param pid see waitpid man page, only -1 or positive numbers are supported
 * @param options see waitpid man page, only 0 is supported
 * @param status used to return the exit status of the child
 *
 * @return the pid of the child process which was cleaned up, or
 * -ECHILD if there are no children of this process
 */

        /* If curproc has no children, return -ECHILD */
        if (list_empty(&curproc->p_children)) return -ECHILD;

        proc_t *dying_proc; /* Process we'll be cleaning up after */
        kthread_t *thr; /* What we'll use to clean up threads */

        /* If pid is -1, go through children and look for one that's dead, set exit status */
        if (pid == -1) {
          proc_t *p;
          loop:
          list_iterate_begin(&curproc->p_children, p, proc_t, p_child_link){
            /* If child process is dead, set status to child's exit status, return child pid */
            if (p->p_state == PROC_DEAD) {
              *status = p->p_status;
              dying_proc = p; /* This is the process we want to clean up after */
              dbg_print("Found proc to clean up after, pid -1\n");
              goto cleaning; /* Break out of loop */
            }
          } list_iterate_end();
          /* If you get here, all your children are running, so wait on curproc wait queue until someone exits */
          sched_sleep_on(&curproc->p_wait);

          /* When you get here, someone exited, so go through list again until you find it */
          goto loop;
        }

        /* If pid greater than 0, given pid is child of current process, wait on child's wait queue for child to exit */
        /* If pid not a child of curproc, return -ECHILD */
        else if (pid > 0 ) {
          proc_t *p;
          list_iterate_begin(&curproc->p_children, p, proc_t, p_child_link){
            if (p->p_pid == pid) { /* If this is the child you're looking for, wait on its wait queue */
              while (p->p_state != PROC_DEAD) {
                sched_sleep_on(&p->p_wait);
              }
              /* If you get here, p is dead */
              *status = p->p_status;
              dying_proc = p;
              dbg_print("Found proc to clean up after, pid > 0\n");
              goto cleaning;
            }
          } list_iterate_end();
          /* If you get here, pid didn't match any of your children. */
          return -ECHILD;
        }

        cleaning:

        /* Iterate through all threads in process, calling kthread_destroy on each */
        list_iterate_begin(&dying_proc->p_threads, thr, kthread_t, kt_plink){
          kthread_destroy(thr);
        } list_iterate_end();

        /* Unlink from list of child processes and list of all processes */
        list_remove(&dying_proc->p_child_link);
        list_remove(&dying_proc->p_list_link);

        /* Free child page table */
        pt_destroy_pagedir(dying_proc->p_pagedir);

        /* Set value of pid to return after cleanup */
        pid_t pid_cleaned = dying_proc->p_pid;

        /* Free child proc structure (using proc slab allocator) */
        slab_obj_free(proc_allocator, dying_proc);

        /* NOT_YET_IMPLEMENTED("PROCS: do_waitpid"); */
        return pid_cleaned;
}

/*
 * Cancel all threads, join with them, and exit from the current
 * thread.
 *
 * @param status the exit status of the process
 */
void
do_exit(int status)
{
        /* Cancel all threads */
        /* Join with threads */
        /* Set thread state */
        /* Exit from current thread */
        curthr->kt_state = KT_EXITED;
        proc_cleanup(status);

        /* NOT_YET_IMPLEMENTED("PROCS: do_exit"); */
}

size_t
proc_info(const void *arg, char *buf, size_t osize)
{
        const proc_t *p = (proc_t *) arg;
        size_t size = osize;
        proc_t *child;

        KASSERT(NULL != p);
        KASSERT(NULL != buf);

        iprintf(&buf, &size, "pid:          %i\n", p->p_pid);
        iprintf(&buf, &size, "name:         %s\n", p->p_comm);
        if (NULL != p->p_pproc) {
                iprintf(&buf, &size, "parent:       %i (%s)\n",
                        p->p_pproc->p_pid, p->p_pproc->p_comm);
        } else {
                iprintf(&buf, &size, "parent:       -\n");
        }

#ifdef __MTP__
        int count = 0;
        kthread_t *kthr;
        list_iterate_begin(&p->p_threads, kthr, kthread_t, kt_plink) {
                ++count;
        } list_iterate_end();
        iprintf(&buf, &size, "thread count: %i\n", count);
#endif

        if (list_empty(&p->p_children)) {
                iprintf(&buf, &size, "children:     -\n");
        } else {
                iprintf(&buf, &size, "children:\n");
        }
        list_iterate_begin(&p->p_children, child, proc_t, p_child_link) {
                iprintf(&buf, &size, "     %i (%s)\n", child->p_pid, child->p_comm);
        } list_iterate_end();

        iprintf(&buf, &size, "status:       %i\n", p->p_status);
        iprintf(&buf, &size, "state:        %i\n", p->p_state);

#ifdef __VFS__
#ifdef __GETCWD__
        if (NULL != p->p_cwd) {
                char cwd[256];
                lookup_dirpath(p->p_cwd, cwd, sizeof(cwd));
                iprintf(&buf, &size, "cwd:          %-s\n", cwd);
        } else {
                iprintf(&buf, &size, "cwd:          -\n");
        }
#endif /* __GETCWD__ */
#endif

#ifdef __VM__
        iprintf(&buf, &size, "start brk:    0x%p\n", p->p_start_brk);
        iprintf(&buf, &size, "brk:          0x%p\n", p->p_brk);
#endif

        return size;
}

size_t
proc_list_info(const void *arg, char *buf, size_t osize)
{
        size_t size = osize;
        proc_t *p;

        KASSERT(NULL == arg);
        KASSERT(NULL != buf);

#if defined(__VFS__) && defined(__GETCWD__)
        iprintf(&buf, &size, "%5s %-13s %-18s %-s\n", "PID", "NAME", "PARENT", "CWD");
#else
        iprintf(&buf, &size, "%5s %-13s %-s\n", "PID", "NAME", "PARENT");
#endif

        list_iterate_begin(&_proc_list, p, proc_t, p_list_link) {
                char parent[64];
                if (NULL != p->p_pproc) {
                        snprintf(parent, sizeof(parent),
                                 "%3i (%s)", p->p_pproc->p_pid, p->p_pproc->p_comm);
                } else {
                        snprintf(parent, sizeof(parent), "  -");
                }

#if defined(__VFS__) && defined(__GETCWD__)
                if (NULL != p->p_cwd) {
                        char cwd[256];
                        lookup_dirpath(p->p_cwd, cwd, sizeof(cwd));
                        iprintf(&buf, &size, " %3i  %-13s %-18s %-s\n",
                                p->p_pid, p->p_comm, parent, cwd);
                } else {
                        iprintf(&buf, &size, " %3i  %-13s %-18s -\n",
                                p->p_pid, p->p_comm, parent);
                }
#else
                iprintf(&buf, &size, " %3i  %-13s %-s\n",
                        p->p_pid, p->p_comm, parent);
#endif
        } list_iterate_end();
        return size;
}
