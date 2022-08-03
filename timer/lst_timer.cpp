#include "lst_timer.h"
#include "../http/http_conn.h"

/* 定时器双向链表构造函数 */
sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}

/* 定时器双向链表析造函数 */
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

/* 将一个定时器添加到有序的双向队列中 */
void sort_timer_lst::add_timer(util_timer *timer)
{
    /* 如果添加的定时器为空，直接退出 */
    if (!timer)
    {
        return;
    }
    /* 如果添加的定时器是第一个 */
    if (!head)
    {
        head = tail = timer;
        return;
    }
    /* 定时器应该添加到双向链表的开始位置 */
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    /* 调用同名私有方法的add_timer 函数处理剩下的情况 */
    add_timer(timer, head);
}

/* 当有连接进行了数据交换时，其定时器的过期时间会被重新设置，其在双向列表
 * 中的位置也是需要变化的
 */
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    /* 如果timer的后继定时器为空或者新的过期时间小于后继过期时间，就不需要
     * 进行调整
     */
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    /* 如果timer在开头 */
    if (timer == head)
    {
        /* 下面的操作相当于是将timer取出，之后在重新插入回去 */
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    /* 如果timer不再开头 */
    else
    {
        /* 下面的操作相当于是将timer取出，之后在重新插入回去 */
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

/* 从有序双向链表中删除对应的定时器 */
void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    /* 如果链表中只有一个定时器 */
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    /* 待删除的定时器在开头 */
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    /* 待删除的定时器在末尾 */
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    /* 待删除的定时器在中间 */
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

/* 删除过期的连接 */
void sort_timer_lst::tick()
{
    /* 如果头部为空直接返回 */
    if (!head)
    {
        return;
    }
    /* 获取当前时间如果存在小于当前时间说明过期了 */
    time_t cur = time(NULL);
    util_timer *tmp = head;
    while (tmp)
    {
        if (cur < tmp->expire)
        {
            break;
        }
        /* 从注册表中删除，并关闭连接 */
        tmp->cb_func(tmp->user_data);
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        /* 删除对应的定时器 */
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    /* 处理插入位置为非开头的情况 */
    /* 需要保存上一个定时器的位置，和当前位置 */
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp)
    {
        /* 如果满足添加说明应该将定时器插入到tmp指向的定时器之前
         */
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    /* 定时器应该被插入到末尾 */
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

/* 给m_TIMESLOT 成员赋值 */
void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//信号处理函数
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    /* m_TIMESLOT 秒之后发送SIGALRM */
    alarm(m_TIMESLOT);
}

/* 给客户端发送错误信息，并关闭描述符 */
void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
/* 从注册表中删除连接对应的描述符，并关闭连接 */
void cb_func(client_data *user_data)
{
    /* 从注册表中删除 */
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    /* 连接数-1 */
    http_conn::m_user_count--;
}
