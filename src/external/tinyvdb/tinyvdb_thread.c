#include "tinyvdb_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#if !defined(TINYVDB_THREAD_C11) && !defined(TINYVDB_THREAD_PTHREAD) &&                            \
    !defined(TINYVDB_THREAD_GCD) && !defined(TINYVDB_THREAD_NONE)
#if defined(__APPLE__)
#define TINYVDB_THREAD_GCD 1
#elif !defined(__STDC_NO_THREADS__)
#define TINYVDB_THREAD_C11 1
#else
#define TINYVDB_THREAD_NONE 1
#endif
#endif

#if defined(TINYVDB_THREAD_C11)
#include <threads.h>
typedef thrd_t worker_t;
typedef mtx_t mutex_t;
typedef cnd_t condition_t;
static int mutex_init(mutex_t *m) { return mtx_init(m, mtx_plain) == thrd_success; }
static void mutex_lock(mutex_t *m) { mtx_lock(m); }
static void mutex_unlock(mutex_t *m) { mtx_unlock(m); }
static void mutex_destroy(mutex_t *m) { mtx_destroy(m); }
static int condition_init(condition_t *c) { return cnd_init(c) == thrd_success; }
static void condition_wait(condition_t *c, mutex_t *m) { cnd_wait(c, m); }
static void condition_wake(condition_t *c) { cnd_broadcast(c); }
static void condition_destroy(condition_t *c) { cnd_destroy(c); }
#elif defined(TINYVDB_THREAD_PTHREAD)
#include <pthread.h>
typedef pthread_t worker_t;
typedef pthread_mutex_t mutex_t;
typedef pthread_cond_t condition_t;
static int mutex_init(mutex_t *m) { return pthread_mutex_init(m, NULL) == 0; }
static void mutex_lock(mutex_t *m) { pthread_mutex_lock(m); }
static void mutex_unlock(mutex_t *m) { pthread_mutex_unlock(m); }
static void mutex_destroy(mutex_t *m) { pthread_mutex_destroy(m); }
static int condition_init(condition_t *c) { return pthread_cond_init(c, NULL) == 0; }
static void condition_wait(condition_t *c, mutex_t *m) { pthread_cond_wait(c, m); }
static void condition_wake(condition_t *c) { pthread_cond_broadcast(c); }
static void condition_destroy(condition_t *c) { pthread_cond_destroy(c); }
#elif defined(TINYVDB_THREAD_GCD)
#include <dispatch/dispatch.h>
#endif
#if !defined(TINYVDB_THREAD_NONE)
#include <stdatomic.h>
#if defined(_MSC_VER)
#define TVDB_THREAD_LOCAL __declspec(thread)
#else
#define TVDB_THREAD_LOCAL _Thread_local
#endif
static TVDB_THREAD_LOCAL tvdb_thread_pool_t *executing_pool;
#endif

static tvdb_status_t thread_error(tvdb_error_t *err, tvdb_status_t s, const char *msg) {
    if (err) {
        memset(err, 0, sizeof(*err));
        err->status = s;
        snprintf(err->message, sizeof(err->message), "%s", msg);
    }
    return s;
}
size_t tvdb_thread_default_count(void) {
    size_t n = 4;
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (info.dwNumberOfProcessors)
        n = info.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online > 0)
        n = (size_t)online;
#endif
    return n > 32 ? 32 : n;
}
const char *tvdb_thread_backend_name(void) {
#if defined(TINYVDB_THREAD_C11)
    return "c11";
#elif defined(TINYVDB_THREAD_PTHREAD)
    return "pthread";
#elif defined(TINYVDB_THREAD_GCD)
    return "gcd";
#else
    return "none";
#endif
}

typedef struct {
#if !defined(TINYVDB_THREAD_NONE)
    atomic_size_t next;
#endif
    size_t end, grain;
    tvdb_range_fn fn;
    void *user;
    tvdb_thread_pool_t *pool;
} range_job;
struct tvdb_thread_pool {
    size_t size;
#if defined(TINYVDB_THREAD_C11) || defined(TINYVDB_THREAD_PTHREAD)
    worker_t *workers;
    size_t created, pending;
    mutex_t mutex, submit;
    condition_t ready, done;
    unsigned initialized;
    uint64_t generation;
    int stop;
    range_job job;
#endif
};
static void serial_for(size_t begin, size_t end, size_t grain, tvdb_range_fn fn, void *user) {
    while (begin < end) {
        size_t take = end - begin;
        if (take > grain)
            take = grain;
        fn(begin, begin + take, user);
        begin += take;
    }
}
#if !defined(TINYVDB_THREAD_NONE)
static void drain_job(range_job *job) {
    tvdb_thread_pool_t *previous = executing_pool;
    executing_pool = job->pool;
    size_t begin = atomic_load_explicit(&job->next, memory_order_relaxed);
    for (;;) {
        if (begin >= job->end)
            break;
        size_t take = job->end - begin;
        if (take > job->grain)
            take = job->grain;
        if (!atomic_compare_exchange_weak_explicit(&job->next, &begin, begin + take,
                                                   memory_order_relaxed, memory_order_relaxed))
            continue;
        job->fn(begin, begin + take, job->user);
        begin = atomic_load_explicit(&job->next, memory_order_relaxed);
    }
    executing_pool = previous;
}
#endif
#if defined(TINYVDB_THREAD_C11) || defined(TINYVDB_THREAD_PTHREAD)
static void worker_loop(tvdb_thread_pool_t *p) {
    uint64_t seen = 0;
    mutex_lock(&p->mutex);
    for (;;) {
        while (!p->stop && seen == p->generation)
            condition_wait(&p->ready, &p->mutex);
        if (p->stop)
            break;
        seen = p->generation;
        mutex_unlock(&p->mutex);
        drain_job(&p->job);
        mutex_lock(&p->mutex);
        if (--p->pending == 0)
            condition_wake(&p->done);
    }
    mutex_unlock(&p->mutex);
}
#if defined(TINYVDB_THREAD_C11)
static int thread_entry(void *arg) {
    worker_loop(arg);
    return 0;
}
static int create_worker(worker_t *t, tvdb_thread_pool_t *p) {
    return thrd_create(t, thread_entry, p) == thrd_success;
}
static void join_worker(worker_t t) { thrd_join(t, NULL); }
#else
static void *thread_entry(void *arg) {
    worker_loop(arg);
    return NULL;
}
static int create_worker(worker_t *t, tvdb_thread_pool_t *p) {
    return pthread_create(t, NULL, thread_entry, p) == 0;
}
static void join_worker(worker_t t) { pthread_join(t, NULL); }
#endif
#endif

tvdb_status_t tvdb_thread_pool_create(size_t count, tvdb_thread_pool_t **out, tvdb_error_t *err) {
    if (!out)
        return thread_error(err, TVDB_ERROR_INVALID_ARGUMENT, "null thread pool output");
    *out = NULL;
    if (count > 256)
        return thread_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid thread pool size");
    if (!count)
        count = tvdb_thread_default_count();
#if defined(TINYVDB_THREAD_NONE)
    count = 1;
#endif
    tvdb_thread_pool_t *p = calloc(1, sizeof(*p));
    if (!p)
        return thread_error(err, TVDB_ERROR_OUT_OF_MEMORY, "thread pool allocation failed");
    p->size = count;
#if defined(TINYVDB_THREAD_C11) || defined(TINYVDB_THREAD_PTHREAD)
    if (count > 1) {
        if (!mutex_init(&p->mutex))
            goto fail;
        p->initialized |= 1;
        if (!mutex_init(&p->submit))
            goto fail;
        p->initialized |= 2;
        if (!condition_init(&p->ready))
            goto fail;
        p->initialized |= 4;
        if (!condition_init(&p->done))
            goto fail;
        p->initialized |= 8;
        p->workers = calloc(count - 1, sizeof(*p->workers));
        if (!p->workers) {
            tvdb_thread_pool_destroy(p);
            return thread_error(err, TVDB_ERROR_OUT_OF_MEMORY, "thread handles allocation failed");
        }
        for (; p->created < count - 1; ++p->created)
            if (!create_worker(&p->workers[p->created], p))
                goto fail;
    }
#endif
    *out = p;
    return TVDB_OK;
#if defined(TINYVDB_THREAD_C11) || defined(TINYVDB_THREAD_PTHREAD)
fail:
    tvdb_thread_pool_destroy(p);
    return thread_error(err, TVDB_ERROR_IO, "thread pool initialization failed");
#endif
}
void tvdb_thread_pool_destroy(tvdb_thread_pool_t *p) {
    if (!p)
        return;
#if defined(TINYVDB_THREAD_C11) || defined(TINYVDB_THREAD_PTHREAD)
    if (p->created) {
        mutex_lock(&p->mutex);
        p->stop = 1;
        condition_wake(&p->ready);
        mutex_unlock(&p->mutex);
        for (size_t i = 0; i < p->created; ++i)
            join_worker(p->workers[i]);
    }
    free(p->workers);
    if (p->initialized & 8)
        condition_destroy(&p->done);
    if (p->initialized & 4)
        condition_destroy(&p->ready);
    if (p->initialized & 2)
        mutex_destroy(&p->submit);
    if (p->initialized & 1)
        mutex_destroy(&p->mutex);
#endif
    free(p);
}
size_t tvdb_thread_pool_size(const tvdb_thread_pool_t *p) { return p ? p->size : 0; }
#if defined(TINYVDB_THREAD_GCD)
static void dispatch_worker(void *context, size_t index) {
    (void)index;
    drain_job(context);
}
#endif

tvdb_status_t tvdb_thread_pool_for(tvdb_thread_pool_t *p, size_t begin, size_t end, size_t grain,
                                   tvdb_range_fn fn, void *user, tvdb_error_t *err) {
    if (!p || !fn || !grain || begin > end)
        return thread_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid task range");
    if (begin == end)
        return TVDB_OK;
    if (p->size == 1 || end - begin <= grain
#if !defined(TINYVDB_THREAD_NONE)
        || executing_pool == p
#endif
    ) {
#if !defined(TINYVDB_THREAD_NONE)
        tvdb_thread_pool_t *previous = executing_pool;
        executing_pool = p;
#endif
        serial_for(begin, end, grain, fn, user);
#if !defined(TINYVDB_THREAD_NONE)
        executing_pool = previous;
#endif
        return TVDB_OK;
    }
#if defined(TINYVDB_THREAD_GCD)
    range_job job;
    atomic_init(&job.next, begin);
    job.end = end;
    job.grain = grain;
    job.fn = fn;
    job.user = user;
    job.pool = p;
    size_t chunks = (end - begin) / grain + ((end - begin) % grain != 0);
    if (chunks > p->size)
        chunks = p->size;
    dispatch_apply_f(chunks, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), &job,
                     dispatch_worker);
#elif defined(TINYVDB_THREAD_C11) || defined(TINYVDB_THREAD_PTHREAD)
    mutex_lock(&p->submit);
    mutex_lock(&p->mutex);
    atomic_init(&p->job.next, begin);
    p->job.end = end;
    p->job.grain = grain;
    p->job.fn = fn;
    p->job.user = user;
    p->job.pool = p;
    p->pending = p->created;
    ++p->generation;
    condition_wake(&p->ready);
    mutex_unlock(&p->mutex);
    drain_job(&p->job);
    mutex_lock(&p->mutex);
    while (p->pending)
        condition_wait(&p->done, &p->mutex);
    mutex_unlock(&p->mutex);
    mutex_unlock(&p->submit);
#else
    serial_for(begin, end, grain, fn, user);
#endif
    return TVDB_OK;
}

typedef struct {
    int start;
    void (*fn)(int, void *);
    void *user;
} legacy_job;
static void legacy_range(size_t begin, size_t end, void *user) {
    legacy_job *j = user;
    for (size_t i = begin; i < end; ++i)
        j->fn((int)((int64_t)j->start + (int64_t)i), j->user);
}
void tvdb_parallel_for(int start, int end, void (*fn)(int, void *), void *user) {
    if (!fn || start >= end)
        return;
    legacy_job j = {start, fn, user};
    size_t n = (size_t)((int64_t)end - start);
    tvdb_thread_pool_t *p = NULL;
    if (tvdb_thread_pool_create(0, &p, NULL) != TVDB_OK) {
        serial_for(0, n, 64, legacy_range, &j);
        return;
    }
    tvdb_thread_pool_for(p, 0, n, 64, legacy_range, &j, NULL);
    tvdb_thread_pool_destroy(p);
}
