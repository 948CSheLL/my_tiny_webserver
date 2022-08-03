#include "config.h"

int main(int argc, char *argv[])
{
    //需要修改的数据库信息,登录名,密码,库名
    string user = "root";
    string passwd = "root";
    string databasename = "yourdb";

    //命令行解析
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;

    //初始化服务器相关的参数
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite, 
                config.OPT_LINGER, config.TRIGMode,  config.sql_num,  config.thread_num, 
                config.close_log, config.actor_model);
    

    //日志初始化，包括日志文件名，如果是异步还包括阻塞队列的长度
    //日志实例使用的是单例模式
    server.log_write();

    //数据库连接池初始化
    //数据库连接池也是单例
    server.sql_pool();

    //线程池初始化
    server.thread_pool();

    //初始化监听和连接描述符触发模式
    server.trig_mode();

    //监听
    server.eventListen();

    //运行
    server.eventLoop();

    return 0;
}
