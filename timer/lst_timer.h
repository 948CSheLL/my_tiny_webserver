#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

class util_timer;

struct client_data
{
    /* 存储客户端地址 */
    sockaddr_in address;
    /* 存储连接socket */
    int sockfd;
    /* 存储客户端连接的定时器 */
    util_timer *timer;
};

/* 所有的客户端定时器是根据时间排序的，通过双向链表进行维护 */
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    /* 存储未来到期的时间 */
    time_t expire;
    
    /* 定时器到时触发时调用的回调函数，用来从epoll注册表中删除非活动连接对
     * 应的事件，之后会关闭文件描述符，控制http_conn::m_user_count-1
     */
    void (* cb_func)(client_data *);
    /* 指向该定时器控制的连接资源 */
    client_data *user_data;
    /* 前向定时器 */
    util_timer *prev;
    /* 后继定时器 */
    util_timer *next;
};

class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    /* 将一个定时器添加到有序的双向队列中 */
    void add_timer(util_timer *timer);
    /* 当有连接进行了数据交换时，其定时器的过期时间会被重新设置，其在双向列表
     * 中的位置也是需要变化的
     */
    void adjust_timer(util_timer *timer);
    /* 从有序双向链表中删除对应的定时器 */
    void del_timer(util_timer *timer);
    /* 删除过期的连接 */
    void tick();

private:
    /* 将一个定时器添加到有序的双向队列中，需要和上面的公有函数一起使用。
     **/
    void add_timer(util_timer *timer, util_timer *lst_head);

    /* 双向链表头指针 */
    util_timer *head;
    /* 双向链表尾指针 */
    util_timer *tail;
};

class Utils
{
public:
    Utils() {}
    ~Utils() {}

    /* 给m_TIMESLOT 成员赋值 */
    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    /* 给客户端发送错误信息，并关闭描述符 */
    void show_error(int connfd, const char *info);

public:
    /* 管道描述符 */
    static int *u_pipefd;
    /* 定时器双向链表 */
    sort_timer_lst m_timer_lst;
    /* epoll 描述符 */
    static int u_epollfd;
    /* 给m_TIMESLOT 成员赋值 */
    int m_TIMESLOT;
};

/* 从注册表中删除连接对应的描述符，并关闭连接 */
void cb_func(client_data *user_data);

#endif
