#include "webserver.h"

/* 构造函数 */
WebServer::WebServer()
{
    //http_conn类对象
    users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    /* 获得客户端需要的资源的路径 */
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    //定时器
    users_timer = new client_data[MAX_FD];
}

/* 析构函数 */
WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

/* web 服务器的初始化函数 */
void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    /* 服务器的端口 */
    m_port = port;
    /* 数据库用户名 */
    m_user = user;
    /* 数据库密码 */
    m_passWord = passWord;
    /* 数据库名 */
    m_databaseName = databaseName;
    /* 数据库连接池容量，默认为8 */
    m_sql_num = sql_num;
    /* 线程池容量，默认为8 */
    m_thread_num = thread_num;
    /* 日志的写入方式，默认为同步 */
    m_log_write = log_write;
    /* Q: 不知道有什么用 */
    m_OPT_LINGER = opt_linger;
    /* 触发模式，默认为水平触发(LT) */
    m_TRIGMode = trigmode;
    /* 是否关闭日志，默认不关闭 */
    m_close_log = close_log;
    /* 设置并发模型，默认为proactor */
    m_actormodel = actor_model;
}

void WebServer::trig_mode()
{
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    /* 如果不关闭日志 */
    if (0 == m_close_log)
    {
        //初始化日志
        // 如果日志写入方式是异步
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

//初始化数据库连接池
void WebServer::sql_pool()
{
    //先创建数据库连接池实例
    m_connPool = connection_pool::GetInstance();
    // 作初始化相关的工作
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //初始化用户名与密码的映射
    users->initmysql_result(m_connPool);
}

/* 初始化线程池变量 */
void WebServer::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::eventListen()
{
    //网络编程基础步骤
    //Q:一般监听描述副第一个参数是AF_INET
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    //优雅关闭连接
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        // Q: 下面这个socket 选项的设置是什么意思
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    /* 下面是服务器端的地址配置 */
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    /* Q: 设置端口复用 */
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    /* 将地址与监听端口进行绑定 */
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    /* 监听队列最多5个连接请求 */
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);
    /* 设置utils 对象的时间间隔为TIMESLOT */
    utils.init(TIMESLOT);

    //epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    //创建epoll 对应的文件描述副
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    /* 往m_epollfd 中添加监听描述副，触发事件根据m_LISTENTrigmode 而定 */
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    /* 给http_conn 的静态成员m_epollfd 赋值 */
    http_conn::m_epollfd = m_epollfd;

    /* 创建匿名管道 */
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    /* 将管道的写端设置为非阻塞描述副 */
    utils.setnonblocking(m_pipefd[1]);
    /* 将管道的读端描述副添加到epoll 描述符中 */
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    /* 设置下面三个信号的信号处理函数 */
    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    /* 设置 TIMESLOT秒发送SIGALRM 信号 */
    alarm(TIMESLOT);

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

/* 给connfd 创建一个定时器 */
void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    //成员变量users_timer 是一个client_data 的结构体，其需要保存客户端的地
    //址以及连接描述符。
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    /* 为connfd 对应的连接创建一个定时器 */
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    /* 设置回调函数 */
    timer->cb_func = cb_func;
    /* 设置未来的到期时间为当前时间向后推移3 * TIMESLOT*/
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    /* 将新创建的定时器添加到双向链表里 */
    utils.m_timer_lst.add_timer(timer);
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    /* 当有连接进行了数据交换时，其定时器的过期时间会被重新设置，其在双向列表
     * 中的位置也是需要变化的
     */
    utils.m_timer_lst.adjust_timer(timer);
    /* 写入日志文件 */
    LOG_INFO("%s", "adjust timer once");
}

/* 删除定时器，以及关闭连接描述符 */
void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    /* 调用cb_func 函数关闭连接，并删除连接对应的定时器 */
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        /* 从有序双向链表中删除对应的定时器 */
        utils.m_timer_lst.del_timer(timer);
    }
    /* 将关闭信息写入日志 */
    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

/* 建立与客户端的连接 */
bool WebServer::dealclinetdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    /* 如果监听描述符的触发模式是LT */
    if (0 == m_LISTENTrigmode)
    {
        /* 建立一个socket 连接 */
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        /* 建立失败就写入错误 */
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        /* 如果服务的用户数达到了最大值 */
        if (http_conn::m_user_count >= MAX_FD)
        {
            /* 给客户端发送错误信息，并关闭描述符 */
            utils.show_error(connfd, "Internal server busy");
            /* 将错误信息写入日志文件 */
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }

    /* 如果监听描述符的触发模式是ET */
    else
    {
        /* Q: 不明白下面为什么要这么写 */
        while (1)
        {
            /* 建立一个socket 连接 */
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            /* 如果服务的用户数达到了最大值 */
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

/* 处理到达的信号，通过设置timeout和stop_server的值来判断一个连接是否超时，
 * 是否应该暂停服务 
 **/
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    /* 从管道的读端读取信号发送的信息 */
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    /* recv 错误 */
    if (ret == -1)
    {
        return false;
    }
    /* 没有数据 */
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        /* 每个信号只会发送一个字节来标识是什么信号到达了 */
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            /* 如果是SIGALRM 信号那么设置超时 */
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            /* 标记终止 */
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

/* 线程池中的run方法处理读事件 */
void WebServer::dealwithread(int sockfd)
{
    /* 获取socket 上的定时器 */
    util_timer *timer = users_timer[sockfd].timer;

    //reactor
    if (1 == m_actormodel)
    {
        /* 如果timer不为空，需要重置到期时间 */
        if (timer)
        {
            /* 调整该连接描述符的计时器 */
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        //第二个参数为0时，表示的是读请求
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            /* 如果处理完成 */
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    /* 删除定时器，以及关闭连接描述符 */
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

/* 线程池中的run方法处理写事件，操作与dealwithread类似 */
void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }
        /* 将写事件添加到阻塞队列中 */
        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    /* 通过变量stop_server 控制服务的结束 */
    while (!stop_server)
    {
        /* 等待在epoll 中注册的事件 */
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        /* 如果遇到了除中断意外的错误就结束服务 */
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        /* 一一处理每个事件 */
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == m_listenfd)
            {
                bool flag = dealclinetdata();
                if (false == flag)
                    continue;
            }
            /* 处理错误 */
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                /* 删除定时器，以及关闭连接描述符 */
                deal_timer(timer, sockfd);
            }
            //处理信号
            //如果管道的读端可读
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                /* 处理到达的信号，通过设置timeout和stop_server的值来判断一个连接是否超时，
                 * 是否应该暂停服务 
                 **/
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            // 处理写事件
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        /* 如果存在超时的连接，需要进行处理 */
        if (timeout)
        {
            /* 处理超时的计时器 */
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}
