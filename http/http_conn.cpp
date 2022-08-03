#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;

/* 初始化用户名密码表并将其存放到users 变量中 */
void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    /* 此处的变量mysql就是从连接池中取出的连接对象 */
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中获得完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    /* 获得文件描述符现有的属性 */
    int old_option = fcntl(fd, F_GETFL);
    /* 在文件描述符现有属性的基础上添加非阻塞的属性 */
    int new_option = old_option | O_NONBLOCK;
    /* 重新设置新的属性 */
    fcntl(fd, F_SETFL, new_option);
    /* 返回值是旧的属性 */
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    /* 定义一个epoll 事件 */
    epoll_event event;
    event.data.fd = fd;

    /* 触发模式，如果是1的话，需要设置ET 触发模式 */
    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    /* 默认为水平触发 */
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    /* Q: 什么是one_shot 属性 */
    if (one_shot)
        event.events |= EPOLLONESHOT;
    /* 将目标描述符以及其对应的事件event一同注册到epoll描述符对应的事件表中
     * 对应的命令是EPOLL_CTL_ADD
     */
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    /* 将描述符变成非阻塞 */
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    /* 从内核事件表中将文件描述符移除，对应的命令是EPOLL_CTL_DEL */
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    /* 关闭移除的文件描述符 */
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    /* 修改内核注册表已有的事件，对应的命令是EPOLL_CTL_MOD */
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

/* 记录已经存在的连接数 */
int http_conn::m_user_count = 0;
/* 保存创建的epoll 描述符 */
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        /* 将一个连接socket 从内核事件注册表中删除 */
        removefd(m_epollfd, m_sockfd);
        /* 赋值为-1 表示没有使用 */
        m_sockfd = -1;
        /* 连接数-1 */
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    /* 保存连接描述符 */
    m_sockfd = sockfd;
    /* 保存客户端地址 */
    m_address = addr;

    /* 将连接描述符添加到内核事件表中 */
    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    /* 已经存在的连接数+1 */
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    /* 内核事件的触发模式 */
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    /* 存储数据库用户名 */
    strcpy(sql_user, user.c_str());
    /* 存储数据库密码 */
    strcpy(sql_passwd, passwd.c_str());
    /* 存储数据库名 */
    strcpy(sql_name, sqlname.c_str());

    /* 对剩下的其他成员变量进行初始化 */
    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    /* 设置主状态机一开始的初始状态 */
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN，分别表示一个完整的行、
//一个错误的行以及一个不完整的行
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    /* 从当前位置m_checked_idx开始，读取到结尾的位置m_read_idx */
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        /* 获取读缓冲区的当前字符 */
        temp = m_read_buf[m_checked_idx];
        /* 如果是回车符 */
        if (temp == '\r')
        {
            /* HTTP 请求以\r\n结束为一行，如果\r 是最后一个字符说明还有\n没
             * 有读入，因此返回LINE_OPEN 状态表示需要继续读取
             */
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            /* 如果下一个字符是\n，说明已经存在\r\n的行结束符，此处将\r\n都
             * 变成\0，之后返回状态LINE_OK 表示已经读取了完整的一行
             */
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            /* 除了以上两种情况之外，行格式都是错误的 */
            return LINE_BAD;
        }
        /* 如果当前字符是\n */
        else if (temp == '\n')
        {
            /* \n不是第一个字符，并且上一个字符是\r，也说明构成了一个完整的
             * 行，返回LINE_OK
             */
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            /* 除了上面这一种情况外，其他情况都是错误的 */
            return LINE_BAD;
        }
    }
    /* 如果没有遇到\r或者\n字符，说明一行没有结束，需要继续从客户端读取内容
     */
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    /* 防止读缓冲区越界 */
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    /* 存储一次recv 读取的字节数 */
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        /* 读取数据 */
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        /* m_read_idx是指向尾部最后一个字符，由于新读取了数据，所以需要更新
         */
        m_read_idx += bytes_read;

        /* recv 返回值为-1，说明发生了错误 */
        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        /* 和上面唯一的不同之处就是增加了while 循环 */
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
//格式形如: GET /562f25980001b1b106000338.jpg HTTP/1.1
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    /* strpbrk() 函数在字符串 text 中定位字符串" \t"中任何字节的第一次出现的位置。
     */
    m_url = strpbrk(text, " \t");
    /* 如果没找到，说明格式有问题，返回错误标志BAD_REQUEST */
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    /* 变成一个C语言风格的字符串 */
    *m_url++ = '\0';
    /* 获得请求的方法 */
    /* 保存请求方法字符串的起始地址 */
    char *method = text;
    /* strcasecmp() 函数执行字符串 s1 和 s2 的逐字节比较，忽略字符的大小写。
     * 如果发现 s1 分别小于、匹配或大于 s2，则它返回一个小于、等于或大于零
     * 的整数。
     * strncasecmp() 函数类似，只是它比较的 s1 和 s2 不超过 n 个字节。
     */
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        /* Q: 什么是cgi */
        cgi = 1;
    }
    /* 只支持上面的两种请求方法，如果是之外的方法，返回请求错误标志 */
    else
        return BAD_REQUEST;
    /* strspn() 函数计算 参数一 的初始段的长度（以字节为单位），该段完全由
     * 参数二 中的字符组成。
     */
    /* 保存url 字符串的起始地址 */
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    /* 如果没找到版本号，说明格式错误 */
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    /* 保存版本号的起始地址 */
    m_version += strspn(m_version, " \t");
    /* 判断版本号是否符合要求 */
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    /* 截取需要的url */
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    /* 判断截取之后的url 是否符合要求 */
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    /* 由于已经处理完了请求行，之后更改主状态机的状态，准备处理请求头 */
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    /* 如果是空行 */
    if (text[0] == '\0')
    {
        /* 如果内容长度不为0，说明还有请求数据要处理，更改主状态机状态处理
         * 请求内容。
         */
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            /* 未处理完毕 */
            return NO_REQUEST;
        }
        /* 如果长度为0，返回GET_REQUEST 表示请求获取完毕 */
        return GET_REQUEST;
    }
    /* 获取请求头中Connection的值，从而设置m_linger 字段 */
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    /* 获取内容长度字段 */
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    /* 获取HOST字段 */
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    /* 请求不完整，需要继续读取，然后解析 */
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        /* 如果请求内容长度为0，那么将把第一个字符变成\0 */
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        /* 返回已经获得了完整的请求 */
        return GET_REQUEST;
    }
    /* 请求不完整 */
    return NO_REQUEST;
}

/* 主状态机 */
http_conn::HTTP_CODE http_conn::process_read()
{
    /* 设置行初始状态 */
    LINE_STATUS line_status = LINE_OK;
    /* 设置HTTP解析码为请求不完整表示的NO_REQUEST */
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    /* 只有从状态机返回完整的行的时候此while循环才会执行 */
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        /* 获取一行的起始地址 */
        text = get_line();
        m_start_line = m_checked_idx;
        /* 将一行的内容写道日志中 */
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        /* 解析请求行 */
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        /* 解析请求头 */
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            /* 如果解析得到了完整的请求，就需要处理请求，准备需要返回的内容
             * 如果是GET 请求，就会执行下面的do_request 函数。
             * 如果是POST 请求，下面的步骤将不会执行。
             */
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        /* 解析响应正文 */
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    /* 将请求资源的根目录拷贝到，存放真实文件名的字符数组m_real_file中 */
    strcpy(m_real_file, doc_root);
    /* 获取请求资源的根目录长度 */
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    //找到最后一个'/' 字符所在的位置
    const char *p = strrchr(m_url, '/');

    //处理cgi，即POST 请求 
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                /* 执行插入语句 */
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                /* 先放在m_url 中，之后会进行处理 */
                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    /* 找到了需要的文件，创建文件描述符 */
    int fd = open(m_real_file, O_RDONLY);
    /* 创建文件对应的内存映射 */
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    /* 关闭文件描述符 */
    close(fd);
    return FILE_REQUEST;
}

/* 取消文件的内存映射 */
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::write()
{
    int temp = 0;

    //若要发送的数据长度为0，表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        /* Q: 为什么要重新注册 */
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        /* 聚集写 */
        temp = writev(m_sockfd, m_iv, m_iv_count);

        /* 错误处理 */
        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        /* 已经发送的字节数+ */
        bytes_have_send += temp;
        /* 待发送的字节数- */
        bytes_to_send -= temp;
        //第一个iovec头部信息的数据已发送完，发送第二个iovec数据
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            /* (bytes_have_send - m_write_idx)就是第二个iovec 已经发送的数
             * 据 
             **/
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        //继续发送第一个iovec头部信息的数据
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        /* 如果发送完毕 */
        if (bytes_to_send <= 0)
        {
            /* 解除文件与内存的绑定 */
            unmap();
            /* Q: 为什么要重新注册 */
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            /* Q: 不知道m_linger 有什么用 */
            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    /* va_start的第二个参数是不变长参数的最后一个 */
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    /* 防止越界 */
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    /* arg_list使用完毕后需要做处理 */
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}

/* 添加响应的状态行 */
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

/* 添加响应头 */
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}

/* 添加响应正文的长度字段 */
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

/* 添加响应类型字段 */
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

/* 添加是否长期连接字段 */
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

/* 添加空行 */
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

/* 添加响应正文 */
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

/* 生成响应内容 */
/* 参数是解析响应请求的结果 */
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    /* 将响应数据存放到m_iv 中的正确的位置 */
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            /* 应该发送的数据量是响应结果、响应头以及响应内容的总和 */
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    /* 如果读到的请求不完整，重新注册读事件 */
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    /* 如果发送响应失败，就关闭连接 */
    if (!write_ret)
    {
        close_conn();
    }
    /* Q: 为什么重新注册写事件 */
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
