/*************************************************************
*循环数组实现的阻塞队列，m_back = (m_back + 1) % m_max_size;  
*线程安全，每个操作前都要先加互斥锁，操作完后，再解锁
**************************************************************/

#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"
using namespace std;

template <class T>
class block_queue
{
public:
    /* 阻塞队列默认初始大小是1000 */
    block_queue(int max_size = 1000)
    {
        /* 如果传入的0，是一种错误 */
        if (max_size <= 0)
        {
            exit(-1);
        }

        m_max_size = max_size;
        /* 动态创建一个阻塞队列的实体，数组，数组的容量为传进来的参数，数组
         * 是循环数组
         **/
        m_array = new T[max_size];
        /* m_size是已经使用的数组中的大小 */
        m_size = 0;
        /* 数组一开始初始化为-1 */
        m_front = -1;
        m_back = -1;
    }

    /* 清空阻塞队列 */
    void clear()
    {
        /* 由于需要操作数组，所以先要获得锁 */
        m_mutex.lock();
        m_size = 0;
        m_front = -1;
        m_back = -1;
        m_mutex.unlock();
    }

    /* 阻塞队列的析沟函数 */
    ~block_queue()
    {
        /* 由于需要操作数组，所以先要获得锁 */
        m_mutex.lock();
        /* 只有m_array 被new 之后才能被delete */
        if (m_array != NULL)
            delete [] m_array;

        /* 之后需要解锁 */
        m_mutex.unlock();
    }
    //判断队列是否满了
    bool full() 
    {
        m_mutex.lock();
        /* 如果使用的数组容量达到了最大容量说明满了 */
        if (m_size >= m_max_size)
        {

            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }
    //判断队列是否为空
    bool empty() 
    {
        m_mutex.lock();
        /* 如果使用的大小m_size 为0，说明为空 */
        if (0 == m_size)
        {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }
    //返回队首元素
    bool front(T &value) 
    {
        m_mutex.lock();
        /* 数组没有元素 */
        if (0 == m_size)
        {
            m_mutex.unlock();
            return false;
        }
        /* 数组有元素返回第一个元素 */
        value = m_array[m_front];
        m_mutex.unlock();
        return true;
    }
    //返回队尾元素
    bool back(T &value) 
    {
        m_mutex.lock();
        if (0 == m_size)
        {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_back];
        m_mutex.unlock();
        return true;
    }

    /* 返回数组大小 */
    int size() 
    {
        int tmp = 0;

        m_mutex.lock();
        tmp = m_size;

        m_mutex.unlock();
        return tmp;
    }

    /* 返回数组能够承受的最大大小 */
    int max_size()
    {
        int tmp = 0;

        m_mutex.lock();
        tmp = m_max_size;

        m_mutex.unlock();
        return tmp;
    }
    //往队列添加元素，需要将所有使用队列的线程先唤醒
    //当有元素push进队列,相当于生产者生产了一个元素
    //若当前没有线程等待条件变量,则唤醒无意义
    //生产者调用的函数
    bool push(const T &item)
    {

        m_mutex.lock();
        /* 先判断当前的阻塞队列是否已经满了 */
        if (m_size >= m_max_size)
        {

            /* 唤醒阻塞的线程 */
            m_cond.broadcast();
            m_mutex.unlock();
            return false;
        }

        /* 如果数组没有满，那么就插入一个元素到阻塞队列后面 */
        /* 由于是循环列表所以需要对最大容量取模 */
        m_back = (m_back + 1) % m_max_size;
        m_array[m_back] = item;

        /* 使用的容量增加 */
        m_size++;

        /* 通知线程从阻塞队列中取出元素消费 */
        m_cond.broadcast();
        m_mutex.unlock();
        return true;
    }
    //pop时,如果当前队列没有元素,将会等待条件变量
    //消费者调用的函数
    bool pop(T &item)
    {

        m_mutex.lock();
        while (m_size <= 0)
        {
            
            /* 如果当前队列中没有元素，那么就进入条件等待状态 */
            /* 如果被唤醒，将退出循环 */
            if (!m_cond.wait(m_mutex.get()))
            {
                m_mutex.unlock();
                return false;
            }
        }

        /* 从队列中取出一个元素 */
        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

    //增加了超时处理
    bool pop(T &item, int ms_timeout)
    {
        struct timespec t = {0, 0};
        struct timeval now = {0, 0};
        /* 获取当前时间 */
        gettimeofday(&now, NULL);
        m_mutex.lock();
        if (m_size <= 0)
        {
            t.tv_sec = now.tv_sec + ms_timeout / 1000;
            t.tv_nsec = (ms_timeout % 1000) * 1000;
            /* 设置条件等待时间 */
            if (!m_cond.timewait(m_mutex.get(), t))
            {
                m_mutex.unlock();
                return false;
            }
        }

        if (m_size <= 0)
        {
            m_mutex.unlock();
            return false;
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

private:
    /* 阻塞队列通过条件变量协调生产者和消费者 */
    locker m_mutex;
    cond m_cond;

    /* 数组 */
    T *m_array;
    /* 阻塞队列中的消息数目 */
    int m_size;
    /* 可以容纳的最大消息数目 */
    int m_max_size;
    /* 队列头部 */
    int m_front;
    /* 队列尾部 */
    int m_back;
};

#endif
