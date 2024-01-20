// File:	mypthread.c

// List all group members' names: Jim Xie
// iLab machine tested on:

#include "mypthread.h"

// INITAILIZE ALL YOUR VARIABLES HERE	
// YOUR CODE HERE


// umalloc globals
int pt_instantiated = 0; // if page table is instantiated
int no_space = 0; // would eventually lead  to page fault, but for now we will say no - our approach will be when the segfault is triggered from myallocate, it vectors to the handler, the handler will unprotect the region of memory requested and set no_space to 1, then once we return to myallocate -> we will mprotect the region of memory again and return NULL

// global pointers to mainmem and the swapfile
static char* mainmem;

int pages_in_swapfile = 0;

char* protect_this_page = NULL; // when there is no space, this pointer will be set to the page we need to mprotect
// umalloc globals


int quanta = 10000;
int threadIDs = 2; //threadID for new threads - maybe make 1 main thread for now and 0 for scheduler



// Function Declarations: (update as needed)

static void schedule();
static void sched_RR();
static void sched_PSJF();
void psjf_insert(ThreadNode*, Queue*);




int enqueue(ThreadNode* item, Queue* q){ 
	if(q -> head == NULL){ // if queue is empty
		q -> head = item;
		q -> tail = item;
		q -> tail -> next = NULL;
		return 1;
	}
	//otherwise just enqueue as normal
	q -> tail -> next = item;
	q -> tail = q -> tail -> next;
	q -> tail -> next = NULL;
	return 1;
}


ThreadNode* dequeue(Queue* q){
	if(q -> head == NULL){ // queue is empty, cannot dequeue
		return NULL;
	}
	// otherwise dequeue as normal
	ThreadNode* dq_item = q -> head;
	if(q -> tail == q -> head){ // then there is one node
		q -> tail = NULL;
	}
	q -> head = q -> head -> next; // Null if one node
	dq_item -> next = NULL;
    return dq_item;

}

ThreadNode* remove_node(ThreadNode* item, Queue* q){
	if(q -> head == NULL){ // queue is empty, cannot remove
		return NULL;
	}
	// otherwise remove as normal
	ThreadNode* curr = q -> head;
	ThreadNode* prev_node = NULL;
	while(curr != NULL){
		
		// if ids match, remove
		if(curr -> tcb -> threadId == item -> tcb -> threadId){
			if(prev_node == NULL){ // curr is the head
				q -> head = q -> head -> next;
			}else{ // prev_node's next should skip over curr
				prev_node -> next = prev_node -> next -> next;
			}
			curr ->next = NULL; // reset curr node's next to NULL
			break;
		}
		

		// update ptrs
		prev_node = curr;
		curr = curr -> next;
	}
	return curr;
}

void printQueue(Queue* q){
	ThreadNode* ptr = q -> head;
	if(ptr == NULL){
		printf("NULL");
	}
	while(ptr != NULL){
		printf("%u ", ptr -> threadId);
		ptr = ptr -> next;
	}
	puts(""); // new line
}

// Timer Struct
struct itimerval timer;

// Queues
Queue* readyQueue = NULL; // nodes to be put into running based off scheduler
Queue* runningQueue = NULL; // nodes that will run and after finished quanta will go back to ready, with exceptinos
Queue* terminatingQueue = NULL;  // nodes that need to have their return processed, and to be cleaned
Queue* blockedQueue = NULL; // for Mutex step -- might have queue for each mutex, so not needed?
Queue* waitingQueue = NULL; // to be put back onto ready queue - or i dont exactly know - check

// Scheduler ThreadNode Obj
ThreadNode* scheduler_tn = NULL; // free scheduler!!!

// Main ThreadNode Obj
ThreadNode* main_thread = NULL; 

// Current Running Thread
ThreadNode* curr_thread = NULL;

// Default Inactive Thread Library
int thread_lib_active = 0;

// Main Thread Context 
ucontext_t mainthread_context;



// Scheudler Modes (will have to figure this out later) - was told there might be a macro to get it
enum sched_modes{RRm, PSJFm};
#if defined(PSJF)
	int sched_mode = PSJFm;
#else
	int sched_mode = RRm;
#endif


sigset_t signal_set;

// pause the timer
int pause_timer(struct itimerval* old){
	// pause timer
	timer.it_value.tv_sec = 0;
	timer.it_value.tv_usec = 0;
	if (setitimer(ITIMER_VIRTUAL, &timer, old) == -1) {
		perror("Error calling setitimer()");
		exit(1);
	}
}

int resume_timer(struct itimerval old){
	timer.it_value.tv_sec = old.it_value.tv_sec;
	timer.it_value.tv_usec = old.it_value.tv_usec;
	if (setitimer(ITIMER_VIRTUAL, &timer, NULL) == -1) { 
		perror("Error calling setitimer()");
		exit(1);
	}
}


// Our signal handler
void signal_handler(int signal){

	// block signals
	sigprocmask(SIG_BLOCK, &signal_set, NULL); // sigprocmask is very fast so we do not have to worry about it being interrupted during

	switch(signal){ 
		case(SIGVTALRM): // timer expired, switch context

			// swap context back to the scheduler
			swapcontext(curr_thread -> tcb -> context, scheduler_tn -> tcb -> context);

			break;
		case(SIGINT): // thread is terminating
		// get the threadId of the curr_thread and then search the waitingQueue for any thread waiting on that ID
			
			// enqueue the curr_thread into the terminatingQueue
			enqueue(curr_thread, terminatingQueue); // let us enqueue into the terminating queue and clean it up at the end - can free tcb
			
			// search the waitingQueue for anything waiting for that id - should only be one (behavior is undefined if multiple)
			ThreadNode* find_id = waitingQueue -> head;
			while(find_id != NULL){
				// if ids match, this thread is joining on our current thread
				if(find_id -> joining_on == curr_thread -> threadId){
					// remove the node from waiting, put in ready - its return value will also be ready for them at this point - will it?
					ThreadNode* removed_node = remove_node(find_id, waitingQueue);
					if(sched_mode == RRm){
						enqueue(removed_node, readyQueue); 
					}else if(sched_mode == PSJFm){
						psjf_insert(removed_node, runningQueue); 
					}
					break;
				}
				find_id = find_id -> next;
			}
			break;
	}

	// unblock signals
	sigprocmask(SIG_UNBLOCK, &signal_set, NULL);

}

// pair signals with the signal handler
void signal_handler_init(){
	// setting the signal handler for SIGVTALRM - Timer
	signal(SIGVTALRM, &signal_handler);

	// setting the signal handler for SIGINT - thread is terminating
	signal(SIGINT, &signal_handler);

	sigemptyset(&signal_set);
	sigaddset(&signal_set, SIGVTALRM);
	sigaddset(&signal_set, SIGINT);

}

// initialize queues, create scheduler context, scheduler threadnode obj, saving the main thread's context, 
// yielding to scheduler, scheduler will yield back to us eventually with changes and we just have to enqueue new threads
int scheduler_init(){ // as of now 1 for success, 0 for failure

	thread_lib_active = 1;

	// initialize scheduler ThreadNode obj - giving main thread a reference to our scheduler context and node
	scheduler_tn = (ThreadNode*)myallocate(sizeof(ThreadNode), __FILE__, __LINE__, LIBRARYREQ);
	if(scheduler_tn == NULL){
		return 0; // malloc failure
	}

	// set up scheduler thread control block
	threadControlBlock* scheduler_tcb = (threadControlBlock*)myallocate(sizeof(threadControlBlock), __FILE__, __LINE__, LIBRARYREQ);
	if(scheduler_tcb == NULL){
		return 0; // malloc failure
	}
	scheduler_tcb -> threadId = 0; // scheduler should be threadID 0
	scheduler_tcb -> priority = 0; // don't know what to do for this, maybe ignore

	// malloc for the stack of context for scheduler
	int stack_size = 1048576; // ???
	scheduler_tcb -> context = (ucontext_t*)myallocate(sizeof(ucontext_t), __FILE__, __LINE__, LIBRARYREQ);
	if(scheduler_tcb -> context == NULL){ // malloc failure
		return 0;
	}
	if(getcontext(scheduler_tcb -> context) == -1){
		return 0;
	}
	scheduler_tcb -> context -> uc_stack.ss_sp = (stack_t*)myallocate(stack_size, __FILE__, __LINE__, LIBRARYREQ); // stack pointer
	if(scheduler_tcb -> context -> uc_stack.ss_sp == NULL){
		return 0; // malloc failure - do something here
	}
	scheduler_tcb -> context -> uc_stack.ss_size = stack_size; // stack size
	scheduler_tcb -> context -> uc_stack.ss_flags = 0; // ???

	makecontext(scheduler_tcb -> context, schedule, 0); // create the context for scheduler
	scheduler_tcb -> function = NULL; // for now I do not know if we need a execution function for this thread
	scheduler_tcb -> args = NULL;
	scheduler_tcb -> status = Running;  

	// set up scheduler thread node object
	scheduler_tn -> next = NULL; // to be sure
	scheduler_tn -> retvalue = NULL;
	scheduler_tn -> tcb = scheduler_tcb;
	scheduler_tn -> threadId = 0;
	scheduler_tn -> joining_on = -1;
	
	// initialize queues
	readyQueue = (Queue*)myallocate(sizeof(Queue), __FILE__, __LINE__, LIBRARYREQ);
	runningQueue = (Queue*)myallocate(sizeof(Queue), __FILE__, __LINE__, LIBRARYREQ);
	terminatingQueue = (Queue*)myallocate(sizeof(Queue), __FILE__, __LINE__, LIBRARYREQ);
	blockedQueue = (Queue*)myallocate(sizeof(Queue), __FILE__, __LINE__, LIBRARYREQ);
	waitingQueue = (Queue*)myallocate(sizeof(Queue), __FILE__, __LINE__, LIBRARYREQ);

	// initalize all head and tail values to null
	readyQueue -> head = NULL;
	readyQueue -> tail = NULL;
	runningQueue -> head = NULL;
	runningQueue -> tail = NULL;
	terminatingQueue -> head = NULL;
	terminatingQueue -> tail = NULL;
	blockedQueue -> head = NULL;
	blockedQueue -> tail = NULL;
	waitingQueue -> head = NULL;
	waitingQueue -> tail = NULL;

	// if malloc failed
	if(readyQueue == NULL || 
	   runningQueue == NULL || 
	   terminatingQueue == NULL || 
	   blockedQueue == NULL || 
	   waitingQueue == NULL){
		return 0;
	}

	// this function pairs signals with the handler
	signal_handler_init(); 

	// swapcontext() saving the current main thread context and switching to new context
	swapcontext(&mainthread_context, scheduler_tn -> tcb -> context);

	return 1;

}

void psjf_insert(ThreadNode* item, Queue* q){
	// compare priority and, if they match, age
	// lowest priority # should go first
	// item's age should be set to 0
	item -> tcb -> wait_length = 0;
	ThreadNode* insert_ptr = q -> head;
	ThreadNode* prev = NULL;
	
	// find where to insert
	while(insert_ptr != NULL && (insert_ptr -> tcb -> priority < item -> tcb -> priority) && !(insert_ptr -> tcb -> priority == item -> tcb -> priority && insert_ptr -> tcb -> wait_length >= item -> tcb -> wait_length)){
		prev = insert_ptr;
		insert_ptr = insert_ptr -> next;
	}

	// insert time
	if(insert_ptr == NULL){
		if(q -> head == NULL){ // queue was empty in the first place
			enqueue(item, q);
		}else{ // it goes at the end
			// puts("end");
			q -> tail -> next = item; // should not be null
			q -> tail = q -> tail -> next;
		}
	}else{ // insert in the middle somewhere
		if(prev != NULL){
			prev -> next = item;
		}else{ // it is the head
			q -> head = item;
		}
		item -> next = insert_ptr; 
	}
}



// wrapper function to the original function referenced in mypthread_create() and a call to mypthread_exit()
void execution_function(){ 
	void* retval = curr_thread -> tcb -> function(curr_thread -> tcb -> args);
	
	// sigprocmask(SIG_BLOCK, &signal_set, NULL);
	curr_thread -> retvalue = retval;
	// sigprocmask(SIG_UNBLOCK, &signal_set, NULL);

	// there is no double free error - if they call exit already, it would never get here
	if(curr_thread -> exit_flag == 0){
		mypthread_exit(NULL);  // check frequeuently if this works
	}
}


/* create a new thread */ // DONE
int mypthread_create(mypthread_t * thread, pthread_attr_t * attr, void *(*function)(void*), void * arg)
{

	// malloc for the tcb
	threadControlBlock* tcb = (threadControlBlock*)myallocate(sizeof(threadControlBlock), __FILE__, __LINE__, LIBRARYREQ); 
	if(tcb == NULL){
		perror("malloc or resource error");
		exit(1);
	}


	// sigprocmask(SIG_BLOCK, &signal_set, NULL);
	tcb -> threadId = threadIDs;
	*thread = (uint)(threadIDs); // set the thread ID in the buffer specified by user
	// sigprocmask(SIG_UNBLOCK, &signal_set, NULL);
	
	// thread status starts as ready, will push into ready queue later
	tcb -> status = Ready;

	// thread priority
	tcb -> priority = 0; // ???

	// passing in arguments of the passed in function
	tcb -> args = arg;

	// passing in "function" to the tcb
	tcb -> function = function;

	// allocating for stacks
	int stack_size = 1048576; // ???
	tcb -> context = (ucontext_t*)myallocate(sizeof(ucontext_t), __FILE__, __LINE__, LIBRARYREQ);
	if(tcb -> context == NULL){ // malloc failure
		perror("malloc or resource error");
		exit(1);
	}
	if(getcontext(tcb -> context) == -1){
		perror("malloc or resource error");
		exit(1);
	}
	tcb -> context -> uc_stack.ss_sp = (stack_t*)myallocate(stack_size, __FILE__, __LINE__, LIBRARYREQ); // stack pointer
	if(tcb -> context -> uc_stack.ss_sp == NULL){ // malloc failure
		perror("malloc or resource error");
		exit(1);
	}
	tcb -> context -> uc_stack.ss_size = stack_size; // stack size
	tcb -> context -> uc_stack.ss_flags = 0; // ???

	// create the context 
	makecontext(tcb -> context, execution_function, 0);

	// initialize the scheduler - first time
	if(scheduler_tn == NULL){ // if the scheduler is not yet initialized
		if(scheduler_init() == 0){ // initialization call
			perror("malloc or resource error");
			exit(1);
		}
	}
	
	// ready to push
	ThreadNode* tn = (ThreadNode*)myallocate(sizeof(ThreadNode), __FILE__, __LINE__, LIBRARYREQ); // creating the thread node from tcb
	if(tn == NULL){
		perror("malloc or resource error");
		exit(1);
	}
	
	// setting up ThreadNode obj
	tn -> tcb = tcb;
	tn -> next = NULL;
	// sigprocmask(SIG_BLOCK, &signal_set, NULL);
	tn -> threadId = threadIDs;
	*thread = threadIDs;
	threadIDs++; // update threadID system, rn it just adds
	// sigprocmask(SIG_UNBLOCK, &signal_set, NULL);
	tn -> joining_on = -1;
	tn -> exit_flag = 0;
	if(sched_mode == RRm){
		enqueue(tn, readyQueue); // enqueue thread node into ready queue
	}else if(sched_mode == PSJFm){
		psjf_insert(tn, runningQueue); // insert it into the runningQueue -- FIX THIS FUNC
	}

	return 0;
};

/* current thread voluntarily surrenders its remaining runtime for other threads to use */ // DONE
int mypthread_yield()
{
	// change current thread's state from Running to Ready  -- will be done in scheduler already
	// save context of this thread to its hread control block -- this and the one after are both done in one command
	// switch from this thread's context to the scheduler's context

	// pause timer - don't need it anymore
	pause_timer(NULL);

	// There is no need to handle yield differently in sched i believe
	swapcontext(curr_thread -> tcb -> context, scheduler_tn -> tcb -> context);

	return 0;
};

/* terminate a thread */ // DONE
void mypthread_exit(void *value_ptr)
{

	curr_thread -> tcb -> status = Terminated; // Thread status should be terminated when the function is done running
	signal_handler(SIGINT); // Signal that the thread is terminating


	// MISSING FREES - need to free the scheduler and the terminating queues and other queues - a final clean up process
	// when main thread is ending - so not here

	// preserve the return value pointer if not NULL
	// deallocate any dynamic memory allocated when starting this thread

	// save the retval attr and the struct of ThreadNode but free the tcb - will be saved in the terminatingQueue

	// thread is already dequeued
	if(curr_thread != NULL){

		// sigprocmask(SIG_BLOCK, &signal_set, NULL);
		curr_thread -> exit_flag = 1;
		// sigprocmask(SIG_UNBLOCK, &signal_set, NULL);

		// giving the return value to usr
		value_ptr = curr_thread -> retvalue;

		// just for testing
		uint a = curr_thread -> tcb -> threadId;

		// free the stack of the current thread
		// free(curr_thread -> tcb -> context -> uc_stack.ss_sp); // segfaults - might not be needed

		// free the context
		// free(curr_thread -> tcb -> context);

		// free the tcb of the node
		// free(curr_thread -> tcb);

		pause_timer(NULL);
		// remove the reference to the current thread - in turn the node will no longer be enqueued except within the terminatingQueue
		curr_thread = NULL;

		// printf("Thread %u is done\n", a);


	}else{ // should not happen
		puts("What?");
	}

	
	// block signals
	// sigprocmask(SIG_BLOCK, &signal_set, NULL);

	// swap back to scheduler
	setcontext(scheduler_tn -> tcb -> context); 

	return;
};


/* Wait for thread termination */ // DONE
int mypthread_join(mypthread_t thread, void **value_ptr)
{
	// YOUR CODE HERE

	// wait for a specific thread to terminate
	// deallocate any dynamic memory created by the joining thread


	// if thread we are calling join on is already done: find it in the terminatingQueue
	ThreadNode* find_ret = terminatingQueue -> head;
	while(find_ret != NULL){
		if(find_ret -> threadId == thread){
			if(value_ptr != NULL){
				*value_ptr = find_ret -> retvalue;
			}
			return 0;
		}
		find_ret = find_ret -> next;
	}

	
	pause_timer(NULL);

	// set an attr in node so we know what the node is waiting for
	curr_thread -> joining_on = thread;

	// put it in the waiting queue -- at some point when the thread terminates - send signal? - yes
	enqueue(curr_thread, waitingQueue);

	// printf("Curr job: %u\n", curr_thread -> threadId);

	ThreadNode* temp = curr_thread;

	// remove reference so it would not be enqueued back into the ready queue
	curr_thread = NULL; 

	// swap back to the scheduler
	swapcontext(temp -> tcb -> context, scheduler_tn -> tcb -> context);

	// once we are swapped back here, that means the thread we were waiting on is in terminatingQueue
	// find the return values in the terminating queue


	find_ret = terminatingQueue -> head;
	while(find_ret != NULL){
		if(find_ret -> threadId == thread){

			// sigprocmask(SIG_BLOCK, &signal_set, NULL);
			curr_thread -> joining_on = -1;
			// sigprocmask(SIG_UNBLOCK, &signal_set, NULL);

			if(value_ptr != NULL){
				*value_ptr = find_ret -> retvalue;
			}
			return 0;
		}
		find_ret = find_ret -> next;
	}

	perror("join failed");
	exit(1);
	// should not get here
	return -1;
};

/* initialize the mutex lock */
int mypthread_mutex_init(mypthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr)
{
	// YOUR CODE HERE
	
	//initialize data structures for this mutex
	mutex -> blocked_queue = (Queue*)myallocate(sizeof(Queue), __FILE__, __LINE__, LIBRARYREQ);
	if(mutex -> blocked_queue == NULL){
		perror("malloc or resource error");
		exit(1);
	}
	// instantiate owner to NULL
	mutex -> owner = NULL;
	
	// instantiate lock_flag to be unlocked
	mutex -> lock_flag = 0;


	return 0;
};

// FIX
/* aquire a mutex lock */
int mypthread_mutex_lock(mypthread_mutex_t *mutex)
{
		// YOUR CODE HERE
	
		// use the built-in test-and-set atomic function to test the mutex
		// if the mutex is acquired successfully, return
		// if acquiring mutex fails, put the current thread on the blocked/waiting list and context switch to the scheduler thread


		int check = __sync_lock_test_and_set(&(mutex -> lock_flag), 1); // updates the lock flag to 1 regardless
		if(check){  // failed - lock was already  1
			pause_timer(NULL);
			enqueue(curr_thread, mutex -> blocked_queue);
			ThreadNode* temp = curr_thread;
			curr_thread = NULL; // remove reference so sched won't enqueue it into ready or running
			swapcontext(temp -> tcb -> context, scheduler_tn -> tcb -> context);
		}else{ // succeeded - lock was 0
			mutex -> owner = curr_thread;
		}
		
		return 0;
};


// check out
// FIX
/* release the mutex lock */
int mypthread_mutex_unlock(mypthread_mutex_t *mutex)
{
	// YOUR CODE HERE	
	
	// update the mutex's metadata to indicate it is unlocked
	// put the thread at the front of this mutex's blocked/waiting queue in to the run queue

	if(mutex -> owner == curr_thread){
		// check if anything is waiting
		if(mutex -> blocked_queue -> head == NULL){ // empty

			// unlock the lock_flag
			__sync_lock_test_and_set(&(mutex -> lock_flag), 0); 
			
			//reset the owner to NULL
			mutex -> owner = NULL;

		}else{ // something was waiting
			// set owner to next in line
			mutex -> owner = dequeue(mutex -> blocked_queue);
			// printf("mutex owner: %u", mutex -> owner -> threadId);
			
			// unblock owner
			if(sched_mode == RRm){
				enqueue(mutex -> owner, readyQueue);
			}else if(sched_mode == PSJFm){
				psjf_insert(mutex -> owner, runningQueue);
			}
		}
	}

	




	return 0;
};


/* destroy the mutex */
int mypthread_mutex_destroy(mypthread_mutex_t *mutex)
{
	// YOUR CODE HERE
	
	// deallocate dynamic memory allocated during mypthread_mutex_init
	// free(mutex -> blocked_queue);

	return 0;
};

/* scheduler */ // DONE
static void schedule()
{
	if(sched_mode == RRm){
		sched_RR();
	}else if(sched_mode = PSJFm){
	 	sched_PSJF();
	}
	// each time a timer signal occurs your library should switch in to this context
	
	// be sure to check the SCHED definition to determine which scheduling algorithm you should run
	//   i.e. RR, PSJF or MLFQ

	return;
}

/* Round Robin scheduling algorithm */ // DONE
static void sched_RR()
{

	// will have to turn off timer and interrupts - see test_and_set()
	// also since the scheduler is not swapped to via timer like the rest of the nodes, there may not be a need
	
	// initialize main thread node  the first time
	if(main_thread == NULL){
		main_thread = (ThreadNode*)myallocate(sizeof(ThreadNode), __FILE__, __LINE__, LIBRARYREQ);
		if(main_thread == NULL){
			return; // malloc failure - do something here
		}
		main_thread -> tcb = (threadControlBlock*)myallocate(sizeof(threadControlBlock), __FILE__, __LINE__, LIBRARYREQ);
		if(main_thread -> tcb == NULL){
			return; // malloc failure - do something here
		}
		main_thread -> tcb -> context = &mainthread_context; // do i have to malloc a stack?
		main_thread -> tcb -> status = Running; // will be enqueued soon
		main_thread -> tcb -> threadId = 1;
		main_thread -> tcb -> priority = 0; // ???
		main_thread -> tcb -> function = NULL;
		main_thread -> tcb -> args = NULL;
		main_thread -> retvalue = NULL; // double check, i believe it is null -- might be 0
		main_thread -> next = NULL;
		main_thread -> threadId = 1;
		main_thread -> joining_on = -1;
		enqueue(main_thread, runningQueue); // enqueue
    }
	
	// have to use while - since we are not calling this function in sig hand, we are just continually swapping back to it
	while(main_thread != NULL){ // while main context exists -- change? idk the exact logistics behind when main thread ends

		// going to have this as a while for now - if the runningQueue and the readyQueue is empty, but the main thread exists, we just keep cycling
		// might run into some errors though

				
		if(runningQueue -> head == NULL){ // The queue is empty
			// move everything in ready queue to the running queue
			while(readyQueue -> head != NULL){
				enqueue(dequeue(readyQueue), runningQueue);
			}			
		}

		while(runningQueue -> head != NULL || curr_thread != NULL){
			
			// unprot everything
			for(int i = 0; i < MAINMEMSIZE / PAGESIZE; i++){
				// puts("hi");
				char* ptr = mainmem + i * PAGESIZE;
				int check = mprotect((void*)ptr, PAGESIZE, PROT_READ | PROT_WRITE);
				if(i == 0){
					// printf("check: %d\n", check);
					// printf("UNPROT IN SCHED ptr: %d\n", (int)ptr - (int)mainmem);
				}
			}			

			ThreadNode* next_thread = dequeue(runningQueue); // get the next thread to run -- first time should be main thread

			// move the previous running node back to the ready queue
			ThreadNode* previous_thread = curr_thread;
			if(previous_thread != NULL){ // if it exists
				// printf("previous_thread: %u\n", previous_thread -> threadId);
				enqueue(previous_thread, readyQueue);
				previous_thread -> tcb -> status = Ready; // update status
			}

			// update current thread and status
			curr_thread = next_thread;
			if(curr_thread == NULL){
				break;
			}
			curr_thread -> tcb -> status = Running;

			
			// mprotect everything except for the page table
			char* stuff = mainmem + PAGETABLESIZE;
			for(int i = 0; i < (MAINMEMSIZE - PAGETABLESIZE) / PAGESIZE; i++){
				int check = mprotect((void*)stuff, PAGESIZE, PROT_NONE);
				if(i == 0){
					// printf("%d\n", stuff);
				}
				if(i == (MAINMEMSIZE - PAGETABLESIZE) / PAGESIZE - 1){
					// printf("%d\n", stuff);
				}
				stuff += PAGESIZE;
			}

			// printf("%d\n", (int)(PAGETABLESIZE) / (int)(PAGESIZE));

			// mprotect the page table
			char* ptr10 = mainmem;
			for(int i = 0; i < PAGETABLESIZE / PAGESIZE; i++){
				int check = mprotect((void*)ptr10, PAGESIZE, PROT_NONE);
				ptr10 += PAGESIZE;
			}

			// puts("hi");
			

			// set up the timer
			struct itimerval timer;
			timer.it_value.tv_sec = 0;
			timer.it_value.tv_usec = quanta;
			if (setitimer(ITIMER_VIRTUAL, &timer, NULL) == -1) {  // I can do ITIMER_VIRTUAL or ITIMER_PROF or ITIMER_REAL idk: Prof - counts both usr and sys mode, virtual - only counts usr mode, real - counts real world time
				perror("Error calling setitimer()");
				exit(1);
			}

			// update scheduler context and load in new one
			swapcontext(scheduler_tn -> tcb -> context, curr_thread -> tcb -> context); // for some reason, this brings it back up to line 448

			// pause timer - after we swap back
			// pause_timer(NULL); -- dont have to, swap is from - yield (paused alr), 

			// printf("Ready: ");
			// printQueue(readyQueue);
			// printf("Running: ");
			// printQueue(runningQueue);
			// printf("Waiting: ");
			// printQueue(waitingQueue);
			// printf("Terminating: ");
			// printQueue(terminatingQueue);
			// printf("Curr thread: ");
			// if(curr_thread == NULL){
			// 	puts("NULL");
			// }else{
			// 	printf("%u\n", curr_thread -> threadId);
			// }
			// puts("");
			// puts("");
			// int a = getchar();

		}	
	}

	// Add a clean up process here for anything left
	

	return;
}

/* Preemptive PSJF (STCF) scheduling algorithm */ // DONE
static void sched_PSJF()
{
	// YOUR CODE HERE

	// initialize main thread node  the first time
	if(main_thread == NULL){
		main_thread = (ThreadNode*)myallocate(sizeof(ThreadNode), __FILE__, __LINE__, LIBRARYREQ);
		if(main_thread == NULL){
			return; // malloc failure - do something here
		}
		main_thread -> tcb = (threadControlBlock*)myallocate(sizeof(threadControlBlock), __FILE__, __LINE__, LIBRARYREQ);
		if(main_thread -> tcb == NULL){
			return; // malloc failure - do something here
		}
		main_thread -> tcb -> context = &mainthread_context; // do i have to malloc a stack?
		main_thread -> tcb -> status = Running; // will be enqueued soon
		main_thread -> tcb -> threadId = 1;
		main_thread -> tcb -> priority = 0; // ???
		main_thread -> tcb -> function = NULL;
		main_thread -> tcb -> args = NULL;
		main_thread -> retvalue = NULL; // double check, i believe it is null -- might be 0
		main_thread -> next = NULL;
		main_thread -> threadId = 1;
		main_thread -> joining_on = -1;
		enqueue(main_thread, runningQueue); // enqueue
    }
	
	// have to use while - since we are not calling this function in sig hand, we are just continually swapping back to it
	while(main_thread != NULL){ // while main context exists -- change? idk the exact logistics behind when main thread ends


		// unprot everything
		for(int i = 0; i < MAINMEMSIZE / PAGESIZE; i++){
			// puts("hi");
			char* ptr = mainmem + i * PAGESIZE;
			int check = mprotect((void*)ptr, PAGESIZE, PROT_READ | PROT_WRITE);
			if(i == 0){
				// printf("check: %d\n", check);
				// printf("UNPROT IN SCHED ptr: %d\n", (int)ptr - (int)mainmem);
			}
		}	
		
		// change this PSJF
		while(runningQueue -> head != NULL || curr_thread != NULL){ // if everything is waiting, if not deadlock?


			ThreadNode* next_thread = dequeue(runningQueue); // get the next thread to run -- first time should be main thread

			// move the previous running node back to the ready queue
			ThreadNode* previous_thread = curr_thread;
			if(previous_thread != NULL){ // if it exists
				psjf_insert(previous_thread, runningQueue);
				previous_thread -> tcb -> status = Ready; // update status
			}

			// update current thread and status
			curr_thread = next_thread;
			if(curr_thread == NULL){
				break;
			}
			curr_thread -> tcb -> status = Running;

			

			// PSJF: update quanta (same as priority) of curr_thread
			curr_thread -> tcb -> priority += 0.2;
			// printf("Prio of %u: %d\n", curr_thread -> threadId , (int)curr_thread -> tcb -> priority);

			// update ages of threads that are not run
			ThreadNode* age_updater = runningQueue -> head;
			while(age_updater != NULL){ // the next node to run would already be removed from runningQueue by now
				age_updater -> tcb -> wait_length += 1;
				age_updater = age_updater -> next;
			}


			// mprotect everything except for the page table
			char* stuff = mainmem + PAGETABLESIZE;
			for(int i = 0; i < (MAINMEMSIZE - PAGETABLESIZE) / PAGESIZE; i++){
				int check = mprotect((void*)stuff, PAGESIZE, PROT_NONE);
				if(i == 0){
					// printf("%d\n", stuff);
				}
				if(i == (MAINMEMSIZE - PAGETABLESIZE) / PAGESIZE - 1){
					// printf("%d\n", stuff);
				}
				stuff += PAGESIZE;
			}

			// printf("%d\n", (int)(PAGETABLESIZE) / (int)(PAGESIZE));

			// mprotect the page table
			char* ptr10 = mainmem;
			for(int i = 0; i < PAGETABLESIZE / PAGESIZE; i++){
				int check = mprotect((void*)ptr10, PAGESIZE, PROT_NONE);
				ptr10 += PAGESIZE;
			}
			// printf("%d\n", ((int)((curr_thread -> tcb -> priority) + 1) * quanta) / 1000000);
			// printf("%d\n", ((int)((curr_thread -> tcb -> priority) + 1) * quanta) % 1000000);

			// set up the timer
			timer.it_value.tv_sec = ((int)((curr_thread -> tcb -> priority) + 1) * quanta) / 1000000;
			timer.it_value.tv_usec = ((int)((curr_thread -> tcb -> priority) + 1) * quanta) % 1000000;
			// printf("usec of %u is: %d\n", curr_thread -> threadId ,(curr_thread -> tcb -> priority) * quanta);
			if (setitimer(ITIMER_VIRTUAL, &timer, NULL) == -1) {  // I can do ITIMER_VIRTUAL or ITIMER_PROF or ITIMER_REAL idk: Prof - counts both usr and sys mode, virtual - only counts usr mode, real - counts real world time
				// puts("asdfasd");
				perror("Error calling setitimer()");
				exit(1);
			}

			// update scheduler context and load in new one
			swapcontext(scheduler_tn -> tcb -> context, curr_thread -> tcb -> context); // for some reason, this brings it back up to line 448

			// printf("Ready: ");
			// printQueue(readyQueue);
			// printf("Running: ");
			// printQueue(runningQueue);
			// printf("Waiting: ");
			// printQueue(waitingQueue);
			// printf("Terminating: ");
			// printQueue(terminatingQueue);
			// printf("Curr thread: ");
			// if(curr_thread == NULL){
			// 	puts("NULL");
			// }else{
			// 	printf("%u\n", curr_thread -> threadId);
			// }
			// puts("");
			// puts("");
			// int a = getchar();
			

		}	
	}

	return;
}

/* Preemptive MLFQ scheduling algorithm */
/* Graduate Students Only */
static void sched_MLFQ() {
	// YOUR CODE HERE
	
	// Your own implementation of MLFQ
	// (feel free to modify arguments and return types)

	return;
}

// Feel free to add any other functions you need




// Segfault handler
static void handler(int sig, siginfo_t *si, void *unused)
{
	sigemptyset(&signal_set);
	sigaddset(&signal_set, SIGVTALRM);
	sigaddset(&signal_set, SIGINT);

	// block signals
	sigprocmask(SIG_BLOCK, &signal_set, NULL); 

	// puts("In handler");

	// unprot everything
	for(int i = 0; i < MAINMEMSIZE / PAGESIZE; i++){
		char* ptr = mainmem + i * PAGESIZE;
		int check = mprotect((void*)ptr, PAGESIZE, PROT_READ | PROT_WRITE);
	}

    // printf("Got SIGSEGV at address: %d\n",(int) si->si_addr);
	// printf("Got SIGSEGV at address: %d\n",(int) si->si_addr - (int)mainmem);
	// printf("%d\n", *(int*)mainmem);

    int threadIDtofind;

    // if the thread library is active or not
    if(thread_lib_active){ 
        // check curr thread (unless thread library is not active)
        threadIDtofind = curr_thread -> threadId;
    }else{
        // set it to 1 because it is the main thread
        threadIDtofind = 1;
    }

    int pagetofind = (((char*)si->si_addr - mainmem - PAGETABLESIZE)) / PAGESIZE;
	
	// char* rtp = mainmem + PTMETADATASIZE;
	// int page_needed = 0;
	// for(int i = 0; i < *(int*)mainmem; i++){
	// 	if(*(int*)(rtp + PTTHREADID) == threadIDtofind && *(int*)(rtp + PTPAGENUM) >= page_needed){
	// 		page_needed = *(int*)(rtp + PTPAGENUM) + 1;
	// 	}
	// 	rtp += PTENTRYSIZE;
	// }

    
    // find an entry matching threadIDtofind and pagetofind and swap it into the correct position, if none exist create a new page
    int pagefound = 0; // flag for if the entry was found and thereby the page or not


    // ptr will point to each page table entry
    char* ptr1 = mainmem + PTMETADATASIZE;
    for(int j = 0; j < *(int*)mainmem; j++){

        // check if the entry matches both threadID and page number
        if(*(int*)(ptr1 + PTTHREADID) == threadIDtofind && *(int*)(ptr1 + PTPAGENUM) == pagetofind){

            
            // page is found!
            pagefound = 1;
			// printf("Curr Thread : %d , Target Page Offset: %d\n", curr_thread -> threadId, *(int*)(ptr1 + PTOFFSET));

			if(*(int*)(ptr1 + PTOFFSET) >= MAINMEMSIZE){ // in the swapfile

				// puts("???");

				// swap with page in swapfile

				int page1offset = *(int*)(ptr1 + PTOFFSET); // target in the swapfile
				int page2offset = PAGETABLESIZE + pagetofind * PAGESIZE; // destination

				// puts("???1");

				// perform the swap, save what was in the swapfilee
				char temp[PAGESIZE];
				FILE* fp = fopen("swapfile.txt", "rb");
				fseek(fp, page1offset - MAINMEMSIZE, SEEK_SET);
				fread(temp, sizeof(temp[0]), PAGESIZE, fp);

				// puts("???2");

				// write everything from evicted page to swapfile
				char* page_to_evict_byte = (char*)(mainmem + page2offset);
				fp = fopen("swapfile.txt", "wb");
				fseek(fp, page1offset - MAINMEMSIZE, SEEK_SET);
				fwrite(page_to_evict_byte,PAGESIZE,1,fp);

				// puts("???3");

				// write everything from temp to where the old page was evicted from
				for(int i = 0; i < PAGESIZE; i++){
					// puts("Get in");
					// printf("%d\n", page2offset);
					// printf("%d\n", page1offset - MAINMEMSIZE);
					*(char*)(mainmem + page2offset + i) = temp[i];
				}

				// puts("???4");

				// swap the offsets
				// start of page table entries
				char* ptr11 = mainmem + PTMETADATASIZE;

				// loop through the entries to find the two offsets and swap them
				for(int k = 0; k < *(int*)mainmem; k++){
				
					// swap the offsets
					if(*(int*)(ptr11 + PTOFFSET) == page1offset){
						*(int*)(ptr11 + PTOFFSET) = page2offset;
					}else if(*(int*)(ptr11 + PTOFFSET) == page2offset){
						*(int*)(ptr11 + PTOFFSET) = page1offset;
					}

					// update ptr
					ptr11 += PTENTRYSIZE;
				}

				// printf("In Swapfile, num entries: %d\n", *(int*)mainmem);

			}else{ // not in swapfile

				// determine the offsets to swap
				int page1offset = *(int*)(ptr1 + PTOFFSET);
				int page2offset = PAGETABLESIZE + pagetofind * PAGESIZE;

				//swap the page to the correct position
				for(int i = 0; i < PAGESIZE; i++){

					// swap byte by byte
					char* page1byte = (char*)(mainmem + page1offset + i);
					char* page2byte = (char*)(mainmem + page2offset + i);

					// swap
					char temp = *page1byte;
					*page1byte = *page2byte;
					*page2byte = temp;
					
				}

				// start of page table entries
				char* ptr2 = mainmem + PTMETADATASIZE;

				// loop through the entries to find the two offsets and swap them
				for(int k = 0; k < *(int*)mainmem; k++){
				
					// swap the offsets
					if(*(int*)(ptr2 + PTOFFSET) == page1offset){
						*(int*)(ptr2 + PTOFFSET) = page2offset;
					}else if(*(int*)(ptr2 + PTOFFSET) == page2offset){
						*(int*)(ptr2 + PTOFFSET) = page1offset;
					}

					// update ptr
					ptr2 += PTENTRYSIZE;
				}

				// puts("Not in swapfile");

			}

            break;

        }

        // update ptr
        ptr1 += PTENTRYSIZE;

    }

    // if we don't find the page we have to create new page, this should only happen when the segfault occured in our myalloc function
    if(!pagefound){

        // if there is space
        if(*(int*)mainmem * PAGESIZE + PAGETABLESIZE + SHAREDMEMSIZE + PAGESIZE < MAINMEMSIZE){

            // create entry, find place for new page and put offset -> which should *(int*)mainmem * PAGESIZE + PAGETABLESIZE

			// loop through entries to find the next page number needed
			char* rtp = mainmem + PTMETADATASIZE;
			int page_needed = 0;
			for(int i = 0; i < *(int*)mainmem; i++){
				if(*(int*)(rtp + PTTHREADID) == threadIDtofind && *(int*)(rtp + PTPAGENUM) >= page_needed){
					page_needed = *(int*)(rtp + PTPAGENUM) + 1;
				}
				rtp += PTENTRYSIZE;
			}

			// printf("%d\n", page_needed);

            char* ptr4 = mainmem + *(int*)mainmem * PTENTRYSIZE + PTMETADATASIZE; // starting pos of new entry

            // create the new entry
            *(int*)(ptr4 + PTTHREADID) = threadIDtofind;
            *(int*)(ptr4 + PTPAGENUM) = page_needed; 

			// printf("%d\n", page_needed);
			// printf("%d\n", threadIDtofind);

			 // update the number of entries
            *(int*)mainmem = *(int*)mainmem + 1;

            *(int*)(ptr4 + PTOFFSET) = *(int*)mainmem * PAGESIZE + PAGETABLESIZE;

            // find the offsets
            int page1offset = *(int*)(ptr4 + PTOFFSET);
            int page2offset = PAGETABLESIZE + page_needed * PAGESIZE;


            //swap the page to the correct position
            for(int i = 0; i < PAGESIZE; i++){

                // swap byte by byte
                char* page1byte = (char*)(mainmem + page1offset + i);
                char* page2byte = (char*)(mainmem + page2offset + i);

                // swap
                char temp = *page1byte;
                *page1byte = *page2byte;
                *page2byte = temp;
                
            }


            // start of page table entries
            char* ptr5 = mainmem + PTMETADATASIZE;

            // loop through the entries to find the two offsets and swap them
            for(int k = 0; k < *(int*)mainmem; k++){
            
                // swap the offsets
                if(*(int*)(ptr5 + PTOFFSET) == page1offset){
                    *(int*)(ptr5 + PTOFFSET) = page2offset;
                }else if(*(int*)(ptr5 + PTOFFSET) == page2offset){
                    *(int*)(ptr5 + PTOFFSET) = page1offset;
                }

                // update ptr
                ptr5 += PTENTRYSIZE;
            }


			// put the user malloc metadata in
			if(page_needed == 0){
				char* ptr20 = mainmem + PAGETABLESIZE;
				*(int*)(ptr20 + FREEFLAG) = FREE;
				*(int*)(ptr20 + MALLOCSIZE) = MAINMEMSIZE + SWAPFILESIZE - SHAREDMEMSIZE - PAGETABLESIZE;
			}

			
            
			// mprotect everything except the incoming thread's pages (unprotect those)
			for(int i = 0; i < *(int*)mainmem; i++){
				char* ptr6 = mainmem + PTMETADATASIZE;
				int check;
				// puts("below?");
				if(curr_thread == NULL || *(int*)(ptr6 + PTTHREADID) == curr_thread -> threadId){
					// puts("Where is it?");
					check = mprotect((void*)(mainmem + *(int*)(ptr6 + PTOFFSET)), PAGESIZE, PROT_READ | PROT_WRITE);
				}else{
					check = mprotect((void*)(mainmem + *(int*)(ptr6 + PTOFFSET)), PAGESIZE, PROT_NONE);	
				}
				ptr6 += PTENTRYSIZE;
			}

			

			// mprotect the page table
			for(int i = 0; i < PAGETABLESIZE / PAGESIZE; i++){
				char* ptr = mainmem + i * PAGESIZE;
				int check = mprotect((void*)(mainmem + *(int*)(ptr + PTOFFSET)), PAGESIZE, PROT_NONE);
			}

			// puts("Where is it?2 ");

		}else if(*(int*)mainmem * PAGESIZE <  MAINMEMSIZE - PAGETABLESIZE - SHAREDMEMSIZE + SWAPFILESIZE){

			puts("Why are u making an entry in the swapfile u dumb fuck");
			printf("%d\n", *(int*)(mainmem));

			char* rtp = mainmem + PTMETADATASIZE;
			int page_needed = 0;
			for(int i = 0; i < *(int*)mainmem; i++){
				if(*(int*)(rtp + PTTHREADID) == threadIDtofind && *(int*)(rtp + PTPAGENUM) >= page_needed){
					page_needed = *(int*)(rtp + PTPAGENUM) + 1;
				}
				rtp += PTENTRYSIZE;
			}

			// Evict to swapfile 

			char* ptr7 = mainmem + *(int*)mainmem * PTENTRYSIZE + PTMETADATASIZE; // starting pos of new entry

            // create the new entry
            *(int*)(ptr7 + PTTHREADID) = threadIDtofind;
            *(int*)(ptr7 + PTPAGENUM) = page_needed;

			 // update the number of entries
            *(int*)mainmem = *(int*)mainmem + 1;

			// offset for evicted page
            int page_to_evict_offset = PAGETABLESIZE + page_needed * PAGESIZE; // this page is going to be written to swapfile

			// this is now the offset for our new page
			*(int*)(ptr7 + PTOFFSET) = page_to_evict_offset;

			// offset in swapfile
            int swapfile_offset = pages_in_swapfile * PAGESIZE + MAINMEMSIZE;
			pages_in_swapfile++;

			char* page_to_evict_byte = (char*)(mainmem + page_to_evict_offset);

			// write everything from old page to swapfile
			FILE* fp = fopen("swapfile.txt", "wb");
			fseek(fp, pages_in_swapfile * PAGESIZE, SEEK_SET);
			fwrite(page_to_evict_byte,PAGESIZE,1,fp);

			// loop through page table entries and find the entry that has the offset of the evicted page, change that offset
			// start of page table entries
            char* ptr8 = mainmem + PTMETADATASIZE;

            // loop through the entries to find the evict offset and update it
            for(int k = 0; k < *(int*)mainmem; k++){
            
                // update the offset
                if(*(int*)(ptr8 + PTOFFSET) == page_to_evict_offset){
                    *(int*)(ptr8 + PTOFFSET) = swapfile_offset;
                }

                // update ptr
                ptr8 += PTENTRYSIZE;
            }


            
        }else{ // no space due to other threads or sharedmem space


            // set our no space flag
            no_space = 1;

            // calculate the start of the page
            char* currpage = (char*)si->si_addr - (((char*)(si->si_addr) - mainmem) % PAGESIZE);

            // unprotect the region
            mprotect(currpage, PAGESIZE, PROT_READ | PROT_WRITE); 

            // set our protect_this_page to currpage
            protect_this_page = currpage;


        }


    }

	// block signals
	sigprocmask(SIG_UNBLOCK, &signal_set, NULL); 

    


}


void* myallocate(size_t size, const char* file, int line, int mode)
{
    // if global allocation from threading library, whatever
    if(mode == LIBRARYREQ){
        #undef malloc
        return malloc(size); 
        #define malloc(x) myallocate(x, __FILE__, __LINE__, THREADREQ)
    }

    // handle invalid mode
    if(mode != THREADREQ){
        puts("Invalid mode.");
        return NULL;
    }

    // instantiate page table metadata if first time
    if(!pt_instantiated){
		pt_instantiated = 1;
        // instantiating the mainmem char array, must be memaligned for mprotect
        if (posix_memalign((void**)&mainmem, PAGESIZE, MAINMEMSIZE) != 0)
        {
            // errno is not set, posix_memalign returns an enumeration of an error value
        }

        // instantiate the page table metadata
        *(int*)mainmem = 0;  // number of page table entries

        // specify the handler that handles segfaults
        struct sigaction sa;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sa.sa_sigaction = handler;

        if (sigaction(SIGSEGV, &sa, NULL) == -1)
        {
            printf("Fatal error setting up signal handler\n");
            exit(EXIT_FAILURE);    //explode!
        }

        // instantiate the first myalloc metadata
        char* ptr = mainmem + PAGETABLESIZE;
        *(int*)(ptr + FREEFLAG) = FREE;
        *(int*)(ptr + MALLOCSIZE) = MAINMEMSIZE - SHAREDMEMSIZE - PAGETABLESIZE;
    
    }

	// puts("a");

    // looking for next free chunk
    char* ptr = mainmem + PAGETABLESIZE;
    while(!(*(int*)(ptr + FREEFLAG) == FREE && *(int*)(ptr + MALLOCSIZE) >= size)){ // if free and has enough space 
        if((ptr + *(int*)(ptr + MALLOCSIZE) + MALLOCMETADATASIZE) < mainmem + MAINMEMSIZE - SHAREDMEMSIZE - MALLOCMETADATASIZE){
            ptr += *(int*)(ptr + MALLOCSIZE) + MALLOCMETADATASIZE;
        }else{
			// puts("here?");
            return NULL;
        }
    }

	// puts("b");

    // Need to handle IF THERE IS NO NEXT FREE CHUNK

    // if they are accessing something completely outside of memory - NOTE we dont have to handle if it goes into shared mem rn, other code checks that
    if((ptr + MALLOCMETADATASIZE + size) > mainmem + PAGETABLESIZE + MAINMEMSIZE){
        return NULL;
    }

    // access furthest memory point so that we can ascertain if we have space 
    *(char*)(ptr + MALLOCMETADATASIZE + size) = *(char*)(ptr + MALLOCMETADATASIZE + size); // setting it to itself, doing nothing

    // if we do not have space because of other threads
    if(no_space){ 
        // mprotect the page(s) that had been unprotected by the signal handler and then return NULL
		// puts("tHere?");
        mprotect(protect_this_page, PAGESIZE, PROT_NONE);
        return NULL;
    }

	// puts("c");

    // generating the new metadata
    int oldSize = *(int*)(ptr + MALLOCSIZE);
    *(int*)(ptr + FREEFLAG) = (int)NOTFREE;
    *(int*)(ptr + MALLOCSIZE) = (int)size;

    // pushing freed section back
    if(oldSize - size > MALLOCMETADATASIZE){
        ptr += size + MALLOCMETADATASIZE;
        *(int*)(ptr + FREEFLAG) = (int)FREE;
        *(int*)(ptr + MALLOCSIZE) = (int)(oldSize - size);
		// printf("%d\n", *(int*)(ptr + FREEFLAG) = (int)FREE);
		// printf("%d\n", *(int*)(ptr + MALLOCSIZE) = (int)(oldSize - size));
        ptr -= size + MALLOCMETADATASIZE; // reset pointer back
    }

    return (void*)(ptr + MALLOCMETADATASIZE);



}


void mydeallocate(void* ptr, const char* file, int line, int mode)
{
	
	if(mode == LIBRARYREQ){
		#undef free
		free(ptr);
		#define free(x) mydeallocate(x, __FILE__, __LINE__, THREADREQ)
		return; 
	}

	*(char*)ptr -= MALLOCMETADATASIZE;

	// printf("						In free for %d", curr_thread -> threadId);

	// unprotect page table regions
	for(int i = 0; i < PAGETABLESIZE / PAGESIZE; i++){
		char* ptr2 = mainmem + i * PAGESIZE;
		int check = mprotect(ptr2, PAGESIZE, PROT_READ | PROT_WRITE);
	}

	int num_entries = *(int*)(mainmem);
	char* previous;

    // Loop through the page table entries and if ptr exists as one of those entries, then change free bit of region
	int valid_ptr = 0;
	int last = 0; // check if the pointer we are freeing is the last one in the chain rn
	char* ptr2 = mainmem + PAGETABLESIZE;
	for(int i = 0; i < num_entries; i++){
		// printf("						%d\n", (int)ptr);
		// printf("						%d\n", (int)(ptr2 + MALLOCMETADATASIZE));
		if(ptr2 + MALLOCMETADATASIZE == (char*)ptr){
			valid_ptr = 1;
			if(i == num_entries - 1){
				last = 1;
			}
			break;
		}
		previous = ptr2;
		ptr2 += *(int*)(ptr2 + MALLOCSIZE) + MALLOCMETADATASIZE;
	}


	// if the ptr is actually a free-able ptr
	if(valid_ptr){
    	*(int*)(ptr + FREEFLAG) = FREE;
	}else{
		return;
	}

	// unprotect page table regions
	for(int i = 0; i < PAGETABLESIZE / PAGESIZE; i++){
		char* ptr2 = mainmem + i * PAGESIZE;
		int check = mprotect(ptr2, PAGESIZE, PROT_READ | PROT_WRITE);
	}

	// puts("Here?");
    // Coalesce the region before newly freed section if free
	if(previous != NULL && *(int*)(previous + FREEFLAG) == FREE){
		// puts("Here?");
		*(int*)(previous + MALLOCSIZE) += *(int*)(ptr2 + MALLOCSIZE);
	}

	

	// Coalesce the region after newly freed section if free and if it exists
	if(!last && *(int*)(ptr2 + *(int*)(ptr2 + MALLOCSIZE) + FREEFLAG) == FREE){
		*(int*)(ptr2 + MALLOCSIZE) += *(int*)(ptr2 + *(int*)(ptr2 + MALLOCSIZE) + MALLOCSIZE);
	}

	// protect page table regions -- to make sure, thought it may be completed via signal handler anyway
	for(int i = 0; i < PAGETABLESIZE / PAGESIZE; i++){
		char* ptr2 = mainmem + i * PAGESIZE;
		int check = mprotect(ptr2, PAGESIZE, PROT_NONE);
	}


}