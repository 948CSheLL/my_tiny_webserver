#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

class sem
{
public:
    sem()
    {
        /* sem_init() 在 sem 指向的地址初始化未命名的信号量。  value 参数指定信
         * 号量的初始值。
         * pshared 参数指示该信号量是在进程的线程之间还是在进程之间共享。
         */
        if (sem_init(&m_sem, 0, 0) != 0)
        {
            /* 如果发生了错误就抛出异常 */
            throw std::exception();
        }
    }
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0)
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        /* sem_destroy() 销毁 sem 指向的地址处的未命名信号量。*/
        sem_destroy(&m_sem);
    }
    bool wait()
    {
        /* sem_wait() 递减（锁定）sem 指向的信号量。 如果信号量的值大于零，
         * 则递减继续，函数立即返回。 如果信号量当前的值为零，则调用会阻塞，
         * 直到可以执行递减（即信号量值上升到零以上），或者信号处理程序中断
         * 调用。
         */
        return sem_wait(&m_sem) == 0;
    }
    bool post()
    {
        /* sem_post() 增加（解锁）sem 指向的信号量。 如果信号量的值因此变得
         * 大于零，则在 sem_wait(3) 调用中阻塞的另一个进程或线程将被唤醒并
         * 继续锁定信号量。
         */
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;
};
class locker
{
public:
    locker()
    {
        /* 初始化一个互斥锁 */
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock()
    {
        /* 上锁 */
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    bool unlock()
    {
        /* 解锁 */
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    pthread_mutex_t *get()
    {
        /* 获得互斥变量 */
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

class cond
{
public:
    cond()
    {
        /* 初始化一个条件变量 */
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            //pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t *m_mutex)
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        /* 等待被唤醒 */
        ret = pthread_cond_wait(&m_cond, m_mutex);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        /* 等待一定的时间返回 */
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool signal()
    {
        /* 唤醒一个线程 */
        return pthread_cond_signal(&m_cond) == 0;
    }
    bool broadcast()
    {
        /* 唤醒所有等待的线程 */
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    //static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;
};
#endif
