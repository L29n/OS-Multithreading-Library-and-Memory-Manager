# OS-Multithreading-Library-and-Memory-Manager
Operating Systems Project (or rather 2 that go hand in hand) 

1. A Multi-threading Library that supports the Posix commands: pthread_create, pthread_exit, pthread_join, pthread_yield, pthread_mutex_init, pthread_mutex_lock, pthread_mutex_unlock, pthread_mutex_destroy. It contains a scheduler, handles mutexes, and allows creation and use of threads. 
2. A Memory Manager with a "main memory" of 16 MB in size (technically more than 16 MB because it is powers of 2, 16 MiB) and a "secondary storage" 8 MB in size (a file that I write to simulate secondary storage). It supports 2 commands: malloc and free. The memory manager uses Paging, a Page Table, and Page Faults to allow for multiple threads to use malloc and free, maintaining the illusion that each thread has the space all to itself.   
