#ifndef __THREAD_POOL__
#define __THREAD_POOL__

#ifdef __cplusplus
extern "C" {
#endif

    /*--------------------------------API----------------------------------*/
    typedef struct _threadpool* pthreadpool;

    pthreadpool threadpool_init(int num_thrds);

    int threadpool_add_work(pthreadpool, void (*func_ptr)(void *), void *arg);

    void threadpool_wait(pthreadpool);

    void threadpool_pause(pthreadpool);

    void threadpool_resume(pthreadpool);

    void threadpool_destroy(pthreadpool);

    int threadpool_num_threads_working(pthreadpool);

#ifdef __cplusplus
}
#endif

#endif
