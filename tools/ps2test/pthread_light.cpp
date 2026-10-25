// Mutexes that cost no kernel object, for a program that runs on one thread.
//
// The console's kernel has a few hundred semaphores, and the C++ library's pthread layer takes one for every pthread_mutex_init and never gives it back:
// libstdc++ puts a mutex in every shared_ptr control block here (the R5900 has no compare-and-swap, so the library picks the mutex policy), so
// the table was full after a few hundred make_shared calls, and from then on every lock (the allocator's, the C library's files') waited on a semaphore
// that did not exist. With one thread there is nothing to lock against: these keep the calls and drop the semaphores. Link with
// -Wl,--wrap=pthread_mutex_init,... (see CMakeLists.txt).
#include <pthread.h>

extern "C" {

int __wrap_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t*) {
    *mutex = nullptr;
    return 0;
}

int __wrap_pthread_mutex_destroy(pthread_mutex_t*) {
    return 0;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t*) {
    return 0;
}

int __wrap_pthread_mutex_timedlock(pthread_mutex_t*, const struct timespec*) {
    return 0;
}

int __wrap_pthread_mutex_trylock(pthread_mutex_t*) {
    return 0;
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t*) {
    return 0;
}

}
