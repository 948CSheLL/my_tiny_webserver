#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

const int MAX_FD = 65536;           //最大文件描述符
const int MAX_EVENT_NUMBER = 10000; //最大事件数
const int TIMESLOT = 5;             //最小超时单位

class WebServer
{
public:
    WebServer();
    ~WebServer();
    /* 根据Config的对象进行一些基本选项的初始化 */
    void init(int port , string user, string passWord, string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model);

    //线程池初始化
    void thread_pool();
    //数据库连接池初始化
    //数据库连接池也是单例
    void sql_pool();
    //日志初始化，包括日志文件名，如果是异步还包括阻塞队列的长度
    //日志实例使用的是单例模式
    void log_write();
    //初始化监听和连接描述符触发模式
    void trig_mode();
    //监听客户端发起的请求
    void eventListen();
    //建立连接，处理请求，返回响应报文，会调用下面这些函数
    void eventLoop();
    /* 给connfd 创建一个定时器 */
    void timer(int connfd, struct sockaddr_in client_address);
    //若有数据传输，则将定时器往后延迟3个单位
    //并对新的定时器在链表上的位置进行调整
    void adjust_timer(util_timer *timer);
    /* 删除定时器，以及关闭连接描述符 */
    void deal_timer(util_timer *timer, int sockfd);
    /* 建立与客户端的连接，并添加定时器 */
    bool dealclinetdata();
    /* 处理到达的信号，通过设置timeout和stop_server的值来判断一个连接是否超时，
     * 是否应该暂停服务 
     **/
    bool dealwithsignal(bool& timeout, bool& stop_server);
    /* 线程池中的run方法处理读事件 */
    void dealwithread(int sockfd);
    /* 线程池中的run方法处理写事件 */
    void dealwithwrite(int sockfd);

public:
    //基础
    // 服务运行时的端口号
    int m_port;
    /* 客户端请求的资源所在的路径 */
    char *m_root;
    /* 标记日志写入的方式，同步或者异步 */
    int m_log_write;
    /* 是否关闭日志 */
    int m_close_log;
    /* 设置并发模型，默认为proactor */
    int m_actormodel;

    /* 管道的描述符，用来查看是否有信号(SIGALRM)到达 */
    int m_pipefd[2];
    /* epoll 的文件描述符 */
    int m_epollfd;
    /* 存储已经建立起来的http 连接 */
    http_conn *users;

    //数据库相关
    connection_pool *m_connPool;
    string m_user;         //登陆数据库用户名
    string m_passWord;     //登陆数据库密码
    string m_databaseName; //使用数据库名
    int m_sql_num;

    //线程池相关
    // 线程池
    threadpool<http_conn> *m_pool;
    /* 线程池中线程数量 */
    int m_thread_num;

    //epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];

    /* 监听描述符，只需要一个监听描述符就可以了 */
    int m_listenfd;
    /* Q: 这个目前不知道有什么用 */
    int m_OPT_LINGER;
    /* 控制listenfd 和connfd的触发模式 */
    int m_TRIGMode;
    /* 存储listenfd的触发模式 */
    int m_LISTENTrigmode;
    /* 存储connfd的触发模式 */
    int m_CONNTrigmode;

    //定时器相关
    //和上面的成员变量users 一一对应，指向的都是数组，可以通过数组的用法使
    //用users和users_timer
    client_data *users_timer;
    /* 信号工具类 */
    Utils utils;
};
#endif
