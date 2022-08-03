#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <map>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../CGImysql/sql_connection_pool.h"
#include "../lock/locker.h"
#include "../log/log.h"
#include "../timer/lst_timer.h"

class http_conn {
public:
    //设置要读取的文件的名称m_real_file的大小
    static const int FILENAME_LEN = 200;
    //设置读缓冲区m_read_buf大小
    static const int READ_BUFFER_SIZE = 2048;
    //设置写缓冲区m_write_buf大小
    static const int WRITE_BUFFER_SIZE = 1024;
    //报文的请求方法，本项目只用到GET和POST
    enum METHOD {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    //主状态机的状态
    /* 三种状态，标识解析位置。
     *
     * CHECK_STATE_REQUESTLINE，解析请求行
     *
     * CHECK_STATE_HEADER，解析请求头
     *
     * CHECK_STATE_CONTENT，解析消息体，仅用于解析POST请求
     */
    enum CHECK_STATE {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    //报文解析的结果
    /* 表示HTTP请求的处理结果，在头文件中初始化了八种情形，在报文解析与响应
     * 中只用到了七种。
     *
     * NO_REQUEST
     *
     * 请求不完整，需要继续读取请求报文数据
     *
     * 跳转主线程继续监测读事件
     *
     * GET_REQUEST
     *
     * 获得了完整的HTTP请求
     *
     * 调用do_request完成请求资源映射
     *
     * NO_RESOURCE
     *
     * 请求资源不存在
     *
     * 跳转process_write完成响应报文
     *
     * BAD_REQUEST
     *
     * HTTP请求报文有语法错误或请求资源为目录
     *
     * 跳转process_write完成响应报文
     *
     * FORBIDDEN_REQUEST
     *
     * 请求资源禁止访问，没有读取权限
     *
     * 跳转process_write完成响应报文
     *
     * FILE_REQUEST
     *
     * 请求资源可以正常访问
     *
     * 跳转process_write完成响应报文
     *
     * INTERNAL_ERROR
     *
     * 服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
     */
    enum HTTP_CODE {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    //从状态机的状态
    /* 三种状态，标识解析一行的读取状态。
     *
     * LINE_OK，完整读取一行
     *
     * LINE_BAD，报文语法有误
     *
     * LINE_OPEN，读取的行不完整
     */
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

public:
    http_conn() {}
    ~http_conn() {}

public:
    //初始化套接字地址，函数内部会调用私有方法init
    void init(int sockfd, const sockaddr_in& addr, char*, int, int,
        string user, string passwd, string sqlname);
    //关闭http连接
    void close_conn(bool real_close = true);
    /* 各子线程通过process函数对任务进行处理，调用process_read函数和
     * process_write函数分别完成报文解析与报文响应两个任务。 
     */
    void process();
    //读取浏览器端发来的全部数据
    bool read_once();
    //响应报文写入函数
    bool write();
    // 获取客户端的IP 地址
    sockaddr_in* get_address() { return &m_address; }
    //同步线程初始化数据库读取表
    void initmysql_result(connection_pool* connPool);
    // 代表读操作出问题了，通过定时器删掉这个连接
    int timer_flag;
    // 代表read操作是否完成
    int improv;

private:
    void init();
    //从m_read_buf读取，并处理请求报文
    HTTP_CODE process_read();
    //向m_write_buf写入响应报文数据
    bool process_write(HTTP_CODE ret);
    //主状态机解析报文中的请求行数据
    HTTP_CODE parse_request_line(char* text);
    //主状态机解析报文中的请求头数据
    HTTP_CODE parse_headers(char* text);
    //主状态机解析报文中的请求内容(post)
    HTTP_CODE parse_content(char* text);
    //生成响应报文
    HTTP_CODE do_request();
    // m_start_line是已经解析的字符
    // get_line用于将指针向后偏移，指向未处理的字符
    char* get_line() { return m_read_buf + m_start_line; };
    //从状态机读取一行，分析是请求报文的哪一部分
    LINE_STATUS parse_line();
    // 内存映射取消函数
    void unmap();
    //根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
    /* 下面的5个函数，均是内部调用add_response函数更新m_write_idx指针
     * 和缓冲区m_write_buf中的内容，将响应内容写入缓冲区。
     */
    bool add_response(const char* format, ...);
    /* 添加响应正文 */
    bool add_content(const char* content);
    /* 添加状态行：http/1.1 状态码 状态消息 */
    bool add_status_line(int status, const char* title);
    /* 添加消息报头，内部调用add_content_length和add_linger函数
     *
     * content-length记录响应报文长度，用于浏览器端判断服务器是否发送完数据
     *
     * connection记录连接状态，用于告诉浏览器端保持长连接
     */
    bool add_headers(int content_length);
    //添加文本类型，这里是html
    bool add_content_type();
    //添加Content-Length，表示响应报文的长度
    bool add_content_length(int content_length);
    //添加连接状态，通知浏览器端是保持连接还是关闭
    bool add_linger();
    //添加空行
    bool add_blank_line();

public:
    // epoll函数族使用的文件描述符
    static int m_epollfd;
    /* 当前已经连接的用户数量 */
    static int m_user_count;
    /* 一个数据库连接 */
    MYSQL* mysql;
    /* 标记执行读任务还是写任务 */
    int m_state; //读为0, 写为1

private:
    /* 服务端接收数据的socket 接口 */
    int m_sockfd;
    /* 客户端的地址 */
    sockaddr_in m_address;
    //存储读取的请求报文数据
    char m_read_buf[READ_BUFFER_SIZE];
    //缓冲区中m_read_buf中数据的最后一个字节的下一个位置
    int m_read_idx;
    //m_read_buf中当前读取的位置m_checked_idx
    int m_checked_idx;
    //m_read_buf中已经解析的字符个数
    int m_start_line;
    //存储发出的响应结果、和响应头，响应内容是文件的内存地址
    char m_write_buf[WRITE_BUFFER_SIZE];
    //指示buffer中的长度
    int m_write_idx;
    //主状态机的状态
    CHECK_STATE m_check_state;
    //请求方法
    METHOD m_method;
    //以下为解析请求报文中对应的6个变量
    //存储读取文件的名称
    char m_real_file[FILENAME_LEN];
    /* m_url为请求报文中解析出的请求资源，以/开头，也就是/xxx，项目中解析后
     * 的m_url有8种情况。
     *

    GET请求，跳转到judge.html，即欢迎访问页面

        /0

        POST请求，跳转到register.html，即注册页面

        /1

        POST请求，跳转到log.html，即登录页面

        /2CGISQL.cgi

        POST请求，进行登录校验

        验证成功跳转到welcome.html，即资源请求成功页面

        验证失败跳转到logError.html，即登录失败页面

        /3CGISQL.cgi

        POST请求，进行注册校验

        注册成功跳转到log.html，即登录页面

        注册失败跳转到registerError.html，即注册失败页面

        /5

        POST请求，跳转到picture.html，即图片请求页面

        /6

        POST请求，跳转到video.html，即视频请求页面

        /7

        POST请求，跳转到fans.html，即关注页面
     */
    char* m_url;
    // 存储HTTP 版本号
    char* m_version;
    // 存储请求头HOST 字段
    char* m_host;
    // 存储请求头部内容长度
    int m_content_length;
    // 如果请求头Connection 字段对应的内容是keep-alive，则m_linger 为True
    bool m_linger;
    // 存储文件对应的内存映射区域的起始地址
    char* m_file_address;
    // 存储m_real_file文件对应的文件属性
    struct stat m_file_stat;
    // 作为聚集写writev 的一个参数
    struct iovec m_iv[2];
    // 由于上面的iovec 数组的大小是2，所以下面的计数变量也是2
    int m_iv_count;
    int cgi;        //是否启用的POST
    //存储请求头数据，此项目中对应的是post 请求的用户名和密码
    char* m_string;
    // 存储需要发送的字节数
    int bytes_to_send;
    // 存储已发送的字节数
    int bytes_have_send;
    //网站根目录，文件夹内存放请求的资源和跳转的html文件
    char* doc_root;

    // 存储从数据库获取的用户名和密码
    map<string, string> m_users;
    /* 内核事件的触发模式 */
    int m_TRIGMode;
    int m_close_log;

    //登陆数据库用户名
    char sql_user[100];
    //登陆数据库密码
    char sql_passwd[100];
    //登陆数据库名
    char sql_name[100];
};

#endif
