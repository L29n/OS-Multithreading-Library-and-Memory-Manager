// File:	mypthread_t.h

// List all group members' names:
// iLab machine tested on:

#ifndef MYTHREAD_T_H
#define MYTHREAD_T_H

#define _GNU_SOURCE

/* in order to use the built-in Linux pthread library as a control for benchmarking, you have to comment the USE_MYTHREAD macro */
#define USE_MYTHREAD 1

/* include lib header files that you need here: */
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
//my added allocations
#include <ucontext.h>
#include <signal.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/mman.h>

typedef uint mypthread_t;

	/* add important states in a thread control block */
typedef struct _threadControlBlock
{
	// YOUR CODE HERE	
	
	// thread Id
	int threadId;
	// thread status
	int status; 
	// thread context
	ucontext_t* context;
	// thread priority
	double priority; // in PJSF same as quanta or time run
	// arguments of the passed in function
	void* args;
	// the passed in function
	void* (*function)(void* args);
	// how long it has been waiting - resets when the node is swapped to
	int wait_length;

} threadControlBlock;

// ThreadNode Struct
typedef	struct _ThreadNode{
	threadControlBlock* tcb;
	void* retvalue;
	mypthread_t threadId; // added this afterward, noticing that I free the other one ... :(
	mypthread_t joining_on; 
	int exit_flag;
	struct _ThreadNode* next;
}ThreadNode;


// Queue Implementation
typedef struct _Queue{
	ThreadNode* head;
	ThreadNode* tail;
}Queue;

/* mutex struct definition */
typedef struct mypthread_mutex_t
{
	// YOUR CODE HERE
	Queue* blocked_queue; // queue of anything waiting on the mutex
	ThreadNode* owner; // owner of the queue, only threadNode that can unlock / modify
	int lock_flag; // 0 for unlocked, 1 for locked

	
} mypthread_mutex_t;


// Feel free to add your own auxiliary data structures (linked list or queue etc...)
enum thread_states{Running, Blocked, Ready, Terminated, Yielded}; // an enum for our thread states (add more)


/* Function Declarations: */

/* create a new thread */
int mypthread_create(mypthread_t * thread, pthread_attr_t * attr, void *(*function)(void*), void * arg);

/* current thread voluntarily surrenders its remaining runtime for other threads to use */
int mypthread_yield();

/* terminate a thread */
void mypthread_exit(void *value_ptr);

/* wait for thread termination */
int mypthread_join(mypthread_t thread, void **value_ptr);

/* initialize a mutex */
int mypthread_mutex_init(mypthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr);

/* aquire a mutex (lock) */
int mypthread_mutex_lock(mypthread_mutex_t *mutex);

/* release a mutex (unlock) */
int mypthread_mutex_unlock(mypthread_mutex_t *mutex);

/* destroy a mutex */
int mypthread_mutex_destroy(mypthread_mutex_t *mutex);


#ifdef USE_MYTHREAD
#define pthread_t mypthread_t
#define pthread_mutex_t mypthread_mutex_t
#define pthread_create mypthread_create
#define pthread_exit mypthread_exit
#define pthread_join mypthread_join
#define pthread_mutex_init mypthread_mutex_init
#define pthread_mutex_lock mypthread_mutex_lock
#define pthread_mutex_unlock mypthread_mutex_unlock
#define pthread_mutex_destroy mypthread_mutex_destroy
#endif

// UMALLOC stuff

// Macro Definitions
#define malloc(x) myallocate(x, __FILE__, __LINE__, THREADREQ)
#define free(x) mydeallocate(x, __FILE__, __LINE__, THREADREQ)
#define THREADREQ 1 // enumeration for THREADREQ
#define LIBRARYREQ 0 // enumeration for LIBRARYREQ

// Size Macros
#define PAGESIZE sysconf(_SC_PAGE_SIZE) // page size
#define MAINMEMSIZE (8388608) // 8 MiB for main memory
#define SWAPFILESIZE (16777216) // 16 MiB for swap file
#define SHAREDMEMSIZE (4 * PAGESIZE)


// Page Table Macros
#define PTENTRYSIZE (3 * sizeof(int)) // size of each entry in the page table (threadID type size (int size) + which page # for the thread + offset type size (int))
#define PTMETADATASIZE (sizeof(int)) // number of page table entries which is also equal to the number of pages allocated
#define PAGETABLESIZE (((MAINMEMSIZE + SWAPFILESIZE - (SHAREDMEMSIZE + PTMETADATASIZE)) / (PAGESIZE / PTENTRYSIZE + 1) + PTMETADATASIZE) / PAGESIZE * PAGESIZE + PAGESIZE)// static size for PAGETABLE
#define PTTHREADID (0)
#define PTPAGENUM (sizeof(int))
#define PTOFFSET (2*sizeof(int))

 
// User Requested Malloc Macros
#define MALLOCMETADATASIZE (2 * sizeof(int)) // free int and size of request
#define FREEFLAG (0) // offset for the free flag
#define MALLOCSIZE (sizeof(int)) // offset for the size of malloc chunk
#define FREE (0)
#define NOTFREE (1)


// function declarations
void* myallocate(size_t size, const char* file, int line, int mode);
void mydeallocate(void* ptr, const char* file, int line, int mode);



#endif
