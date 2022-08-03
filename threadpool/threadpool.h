#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    /* actor_model 标记使用的是reactor 还是proactor */
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    /* 将请求添加到请求队列中，针对reactor模式 */
    bool append(T *request, int state);
    /* 将请求添加到请求队列中，针对proactor模式 */
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    /* 为了防止this 陷阱，run 被上面的worker 函数调用 */
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    connection_pool *m_connPool;  //数据库
    int m_actor_model;          //模型切换
};

template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    /* 如果线程数和一次可以接受的最大请求数小于0，抛出异常 */
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    /* new m_thread_number个线程 */
    m_threads = new pthread_t[m_thread_number];
    /* 创建失败，抛出异常 */
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        /* 让这m_thread_number 个线程运行worker程序 */
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        /* 将线程变成detach状态，这样就不用pthread_join */
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    /* 如果处理的请求已经满了，就退出 */
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    /* 标记任务的类型，0表示为读任务，1表示为写任务 */
    request->m_state = state;
    /* 将请求添加到工作队列中 */
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    /* 唤醒等待的线程 */
    m_queuestat.post();
    return true;
}

/* proactor 模式下调用的请求添加函数，此模式不需要区分读写，所以第二个参数
 * 可以省去
 **/
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

/* 相当于一个消费者，消费请求队列中的请求 */
template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        /* 如果此时请求队列中没有请求，那么就会阻塞等待 */
        m_queuestat.wait();
        /* 由于要对请求队列进行操作，因此需要进行上锁 */
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        /* 从请求队列中取出一个请求 */
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        /* 请求队列的操作完毕，释放锁 */
        m_queuelocker.unlock();
        if (!request)
            continue;
        /* 如果采用reactor 模式 */
        if (1 == m_actor_model)
        {
            /* 如果是读请求 */
            if (0 == request->m_state)
            {
                if (request->read_once())
                {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            /* 如果是写请求 */
            else
            {
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        /* 使用proactor 模式 */
        else
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            /* proactor 模式读到请求后直接写 */
            request->process();
        }
    }
}
#endif
