#define _POSIX_C_SOURCE 200809L

#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include "thread_pool.h"

#ifdef THREADPOOL_DEBUG
#define THREADPOOL_DEBUG 1
#else
#define THREADPOOL_DEBUG 0
#endif


#if !defined(DISABLE_PRINT) || defined(THREADPOOL_DEBUG)
#define err(str) fprintf(stderr, str)
#else
#define err(str)
#endif

static volatile int threads_keepalive;
static volatile int threads_on_hold;


/* --------------------------------structures--------------------------------- */
/* binary semaphore */
typedef struct _bsem
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int v;
}bsem;

/* task */
typedef struct _task
{
    struct _task *prev;
    void (*func_ptr)(void *arg);
    void *arg;
}task;

/* task queue */
typedef struct _taskqueue
{
    pthread_mutex_t rw_mutex;
    task *front;
    task *rear;
    bsem *has_tasks;
    int len;
}taskqueue;

/* thread */
typedef struct _thread
{
    int id; // friendly id
    pthread_t pthread; //pointer to actual thread
    struct _threadpool *p_tp;   // access to threadpool
}thread;

/* threadpool */
typedef struct _threadpool
{
    thread **threads;
    volatile int num_threads_alive;
    volatile int num_threads_working;
    pthread_mutex_t thread_count_lock;
    pthread_cond_t threads_all_idle;
    taskqueue task_queue;
}threadpool;


/* ------------------------------- prototypes ----------------------------- */
static int thread_init(threadpool *p_tp, thread **p_thread, int id);
static void *thread_do(thread *p_thread);
static void thread_hold(int sig_id);
static void thread_destroy(thread *p_thread);

static int taskqueue_init(taskqueue *p_task_queue);
static void taskqueue_clear(taskqueue *p_task_queue);
static void taskqueue_push(taskqueue *p_task_queue, task *p_task);
static task *taskqueue_pull(taskqueue *p_task_queue);
static void taskqueue_destroy(taskqueue *p_task_queue);

static void bsem_init(bsem *p_bsem, int value);
static void bsem_reset(bsem *p_bsem);
static void bsem_post(bsem *p_bsem);
static void bsem_post_all(bsem *p_bsem);
static void bsem_wait(bsem *p_bsem);


/* ------------------------------- threadpool ----------------------------- */
pthreadpool threadpool_init(int num_threads)
{
    threads_on_hold = 0;
    threads_keepalive = 1;
    if (num_threads < 0)
        num_threads = 0;

    /* create new thread pool */
    pthreadpool p_tp = (pthreadpool)malloc(sizeof(threadpool));
    if (p_tp == NULL)
    {
        err("threadpool_init(): could not allocate memory for thread pool\n");
        return NULL;
    }

    p_tp->num_threads_alive = 0;
    p_tp->num_threads_working = 0;

    /* init task queue */
    if (taskqueue_init(&p_tp->task_queue) == -1)
    {
        err("threadpool_init(): could not allocate memory for task queue\n");
        free(p_tp);
        return NULL;
    }

    /* create threads in pool */
    p_tp->threads = (thread **)malloc(num_threads * sizeof(thread *));
    if (p_tp->threads == NULL)
    {
        err("threadpool_init(): could not allocate memory for threads\n");
        taskqueue_destroy(&p_tp->task_queue);
        free(p_tp);
        return NULL;
    }

    pthread_mutex_init(&(p_tp->thread_count_lock), NULL);
    pthread_cond_init(&(p_tp->threads_all_idle), NULL);

    /* thread init */
    for (int n=0; n < num_threads; n++)
    {
        thread_init(p_tp, &p_tp->threads[n], n);
#if THREADPOOL_DEBUG
        printf("THREADPOOL_DEBUG: create thread %d in pool \n", n);
#endif
    }

    while (p_tp->num_threads_alive != num_threads)
    {
    }

    return p_tp;
}


/* add work to thread pool */
int threadpool_add_work(pthreadpool p_tp, void (*func_ptr)(void *), void *arg)
{
    task *new_task = (task *)malloc(sizeof(task));
    if (new_task == NULL)
    {
        err("threadpool_add_work(): could not allocate memory for new task\n");
        return -1;
    }

    new_task->func_ptr = func_ptr;
    new_task->arg= arg;

    taskqueue_push(&p_tp->task_queue, new_task);

    return 0;
}


void threadpool_wait(pthreadpool p_tp)
{
    pthread_mutex_lock(&p_tp->thread_count_lock);
    while (p_tp->task_queue.len || p_tp->num_threads_working)
    {
        pthread_cond_wait(&p_tp->threads_all_idle, &p_tp->thread_count_lock);
    }
    pthread_mutex_unlock(&p_tp->thread_count_lock);
}


void threadpool_destroy(pthreadpool p_tp)
{
    if (p_tp == NULL)
        return;

    volatile int threads_total = p_tp->num_threads_alive;
    threads_keepalive = 0;

    /* give one second to kill idle threads */
    double TIMEOUT = 1.0;
    time_t start, end;
    double tpassed = 0.0;
    time(&start);
    while (tpassed < TIMEOUT && p_tp->num_threads_alive)
    {
        bsem_post_all(p_tp->task_queue.has_tasks);
        time(&end);
        tpassed = difftime(end, start);
    }

    /* poll remaining threads */
    while (p_tp->num_threads_alive)
    {
        bsem_post_all(p_tp->task_queue.has_tasks);
        sleep(1);
    }

    /* task queue cleanup */
    taskqueue_destroy(&p_tp->task_queue);

    /* deallocs */
    for (int n=0; n < threads_total; n++)
    {
        thread_destroy(p_tp->threads[n]);
    }
    free(p_tp->threads);
    free(p_tp);
}


/* pause all threads in thread pool */
void threadpool_pause(pthreadpool p_tp)
{
    for (int n=0; n < p_tp->num_threads_alive; n++)
    {
        pthread_kill(p_tp->threads[n]->pthread, SIGUSR1);
    }
}


/* resume all threads in thread pool */
void threadpool_resume(pthreadpool p_tp)
{
    /* has`t been implemented yet */
    (void)p_tp;
    threads_on_hold = 0;
}


int threadpool_num_threads_working(pthreadpool p_tp)
{
    return p_tp->num_threads_working;
}


/*----------------------------- thread -------------------------------*/
static int thread_init(pthreadpool p_tp, thread **p_thread, int id)
{
    *p_thread = (thread *)malloc(sizeof(thread));
    if (p_thread == NULL)
    {
        err("thread_init(): could not allocate memory for thread\n");
        return -1;
    }

    (*p_thread)->p_tp = p_tp;
    (*p_thread)->id = id;

    //static int idx = 0;
    //printf("-----------------create threads: %d\n", idx);
    pthread_create(&(*p_thread)->pthread, NULL, (void *)thread_do, (*p_thread));
    pthread_detach((*p_thread)->pthread);

    return 0;
}


/* set the calling thread on hold */
static void thread_hold(int sig_id)
{
    (void)sig_id;
    threads_on_hold = 1;
    while (threads_on_hold)
    {
        sleep(1);
    }
}


static void *thread_do(thread *p_thread)
{
    char thread_name[128] = {0};
    sprintf(thread_name, "thread-pool-%d", p_thread->id);

#if defined(__linux__)
    prctl(PR_SET_NAME, thread_name);
#elif defined(__APPLE__) && defined(__MACH__)
    pthread_setname_np(thread_name);
#else
    err("thread_do(): pthread_setname_np is not supported on the system\n");
#endif
    
    pthreadpool p_tp = p_thread->p_tp;

    /* register signal handler */
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = thread_hold;
    if (sigaction(SIGUSR1, &act, NULL) == -1)
    {
        err("thread_do(): cannot handle SIGUSER1\n");
    }

    /* mark thread as alive (initialized) */
    pthread_mutex_lock(&p_tp->thread_count_lock);
    p_tp->num_threads_alive += 1;
    pthread_mutex_unlock(&p_tp->thread_count_lock);

    while (threads_keepalive)
    {
        bsem_wait(p_tp->task_queue.has_tasks);
        if (threads_keepalive)
        {
            pthread_mutex_lock(&p_tp->thread_count_lock);
            p_tp->num_threads_working++;
            pthread_mutex_unlock(&p_tp->thread_count_lock);

            /* read task from queue and execute it */
            void (*func_buf)(void *);  //?????
            void *arg_buf;
            task *p_task = taskqueue_pull(&p_tp->task_queue);
            if (p_task != NULL)
            {
                func_buf = p_task->func_ptr;
                arg_buf = p_task->arg;
                func_buf(arg_buf);
                free(p_task);
            }

            pthread_mutex_lock(&(p_tp->thread_count_lock));
            p_tp->num_threads_working--;
            if (p_tp->num_threads_working == 0)
            {
                pthread_cond_signal(&p_tp->threads_all_idle);
            }
            pthread_mutex_unlock(&p_tp->thread_count_lock);
        }
    }

    pthread_mutex_lock(&p_tp->thread_count_lock);
    p_tp->num_threads_alive--;
    pthread_mutex_unlock(&p_tp->thread_count_lock);

    return NULL;
}


static void thread_destroy(thread *p_thread)
{
    free(p_thread);
    p_thread = NULL;
}


/*============================= task queue =============================*/
static int taskqueue_init(taskqueue *p_task_queue)
{
    p_task_queue->len = 0;
    p_task_queue->front = NULL;
    p_task_queue->rear = NULL;
    p_task_queue->has_tasks = (bsem *)malloc(sizeof(bsem));
    if (p_task_queue->has_tasks == NULL)
    {
        return -1;
    }

    pthread_mutex_init(&(p_task_queue->rw_mutex), NULL);
    bsem_init(p_task_queue->has_tasks, 0);

    return 0;
}


static void taskqueue_clear(taskqueue *p_task_queue)
{
    while (p_task_queue->len != 0)
    {
        free(taskqueue_pull(p_task_queue));
    }
    p_task_queue->front = NULL;
    p_task_queue->rear = NULL;
    bsem_reset(p_task_queue->has_tasks);
    p_task_queue->len = 0;
}

static void taskqueue_push(taskqueue *p_task_queue, task *p_task)
{
    pthread_mutex_lock(&p_task_queue->rw_mutex);
    p_task->prev = NULL;
    switch (p_task_queue->len)
    {
        case 0: // no tasks in queue
            p_task_queue->front = p_task;
            p_task_queue->rear = p_task;
            break;

        default:
            p_task_queue->rear->prev = p_task;
            p_task_queue->rear = p_task;
    }
    
    p_task_queue->len++;
    bsem_post(p_task_queue->has_tasks);
    pthread_mutex_unlock(&p_task_queue->rw_mutex);
}

static task *taskqueue_pull(taskqueue *p_task_queue)
{
    pthread_mutex_lock(&p_task_queue->rw_mutex);
    task *p_task = p_task_queue->front;
    if (p_task_queue->len == 0)
    {
    }
    else if (p_task_queue->len == 1)
    {
        p_task_queue->front = NULL;
        p_task_queue->rear = NULL;
        p_task_queue->len = 0;
    }
    else
    {
        p_task_queue->front = p_task->prev;
        p_task_queue->len--;
        bsem_post(p_task_queue->has_tasks);
    }
    pthread_mutex_unlock(&p_task_queue->rw_mutex);

    return p_task;
}

static void taskqueue_destroy(taskqueue *p_task_queue)
{
    taskqueue_clear(p_task_queue);
    free(p_task_queue->has_tasks);
    p_task_queue->has_tasks = NULL;
}


/*============================= synchronisation =============================*/
static void bsem_init(bsem *p_bsem, int value)
{
    if (value < 0 || value > 1)
    {
        err("bsem_init(): binary semaphore can take only values 1 or 0\n");
        exit(1);
    }
    pthread_mutex_init(&(p_bsem->mutex), NULL);
    pthread_cond_init(&(p_bsem->cond), NULL);
    p_bsem->v = value;
}

static void bsem_reset(bsem *p_bsem)
{
    bsem_init(p_bsem, 0);
}


/* post at least one thread */
static void bsem_post(bsem *p_bsem)
{
    pthread_mutex_lock(&p_bsem->mutex);
    p_bsem->v = 1;
    pthread_cond_signal(&p_bsem->cond);
    pthread_mutex_unlock(&p_bsem->mutex);
}

static void bsem_post_all(bsem *p_bsem)
{
    pthread_mutex_lock(&p_bsem->mutex);
    p_bsem->v = 1;
    pthread_cond_broadcast(&p_bsem->cond);
    pthread_mutex_unlock(&p_bsem->mutex);
}

static void bsem_wait(bsem *p_bsem)
{
    pthread_mutex_lock(&p_bsem->mutex);
    while (p_bsem->v != 1)
    {
        pthread_cond_wait(&p_bsem->cond, &p_bsem->mutex);
    }
    p_bsem->v = 0;
    pthread_mutex_unlock(&p_bsem->mutex);
}

