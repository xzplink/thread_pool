#include <stdio.h>
#include <pthread.h>
#include "thread_pool.h"


void task1()
{
    printf("Thread #%u working on task1\n", (int)pthread_self());
}

void task2()
{
    printf("Thread #%u working on task2\n", (int)pthread_self());
}


int main()
{
    puts("Making threadpool with 4 threads");
    pthreadpool p_tp = threadpool_init(4);

    puts("Adding 40 tasks to threadpool");
    int i;
    for (i=0; i < 20; i++)
    {
        threadpool_add_work(p_tp, (void*)task1, NULL);
        threadpool_add_work(p_tp, (void*)task2, NULL);
    };

    puts("Killing threadpool");
    threadpool_destroy(p_tp);

    return 0;
}
