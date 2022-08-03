#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

Log::Log()
{
    m_count = 0;
    m_is_async = false;
}

Log::~Log()
{
    if (m_fp != NULL)
    {
        fclose(m_fp);
    }
}
//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
    //如果设置了max_queue_size,则设置为异步
    if (max_queue_size >= 1)
    {
        /* 标记为异步 */
        m_is_async = true;
        /* 创建阻塞队列 */
        /* 阻塞队列是模板类，因此需要指定类型 */
        m_log_queue = new block_queue<string>(max_queue_size);
        /* 创建一个线程作为阻塞队列的消费者，从阻塞队列中取出日志信息异步写
         * 入日志文件
         **/
        pthread_t tid;
        //flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }
    
    /* 标记是否关闭日志 */
    m_close_log = close_log;
    /* 设置日志缓冲区大小。
     * Q: 有什么用？
     */
    m_log_buf_size = log_buf_size;
    /* 设置日志缓冲区。
     * Q: 日志缓冲区有什么用？
     */
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    /* 设置一个日志文件的最大行数，如果超过此值那么就会将日志写入下一个文件
     **/
    m_split_lines = split_lines;

    /* 调用time 函数获得1970 到现在经过的秒数 */
    time_t t = time(NULL);
    /* 将上面的时间进行结构化 */
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    /* 寻找文件名中的最后一个'/' */
    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    /* 如果没有找到'/' */
    if (p == NULL)
    {
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else
    {
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    /* 保存当天日期 */
    m_today = my_tm.tm_mday;
    
    /* 保存打开的日志文件的文件指针 */
    m_fp = fopen(log_full_name, "a");
    if (m_fp == NULL)
    {
        return false;
    }

    return true;
}

void Log::write_log(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    /* 获取当前时间 */
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    /* 查看日志等级 */
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }
    //写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();
    /* 行数+1 */
    m_count++;

    /* 如果不上当天，或者一个日志文件超过了最大行数 */
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) //everyday log
    {
        
        char new_log[256] = {0};
        /* 强制刷新，并关闭文件指针 */
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};
       
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
       
        if (m_today != my_tm.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else
        {
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        /* 打开一个新的日志文件 */
        m_fp = fopen(new_log, "a");
    }
 
    m_mutex.unlock();

    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    //写入的具体时间内容格式
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    /* 异步模式，放入阻塞队列中 */
    if (m_is_async && !m_log_queue->full())
    {
        /* 生产者生产一个消息 */
        m_log_queue->push(log_str);
    }
    /* 同步模式 */
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

/* 对于输出流， fflush() 通过流的底层写入函数强制写入给定输出或更新流的所有
 * 用户空间缓冲数据。
 * 对于与可查找文件相关的输入流（例如，磁盘文件，但不是管道或终端），
 * fflush() 丢弃已从底层文件获取但尚未被应用程序使用的任何缓冲数据。
 * 流的打开状态不受影响。
 * 如果流参数为 NULL，则 fflush() 刷新所有打开的输出流。
 */
void Log::flush(void)
{
    m_mutex.lock();
    //强制调用write系统调用进行写出
    fflush(m_fp);
    m_mutex.unlock();
}
