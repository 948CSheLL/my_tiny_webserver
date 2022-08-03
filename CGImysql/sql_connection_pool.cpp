#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

/* 线程池构造函数 */
connection_pool::connection_pool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
}

connection_pool *connection_pool::GetInstance()
{
    /* 所有的线程都是用下面的这个线程池实例 */
    /* c++11之后，定义多个线程使用的静态变量时不需要加锁 */
	static connection_pool connPool;
	return &connPool;
}

//构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;

	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
        /* 调用mysql库函数初始化一个mysql 连接对象 */
		con = mysql_init(con);

        /* 处理失败的情况 */
		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
        /* 根据数据库五元组创建一个数据库连接 */
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

        /* 创建失败的情况 */
		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
        /* 将创建的一个连接添加进连接池列表中 */
		connList.push_back(con);
        /* 空闲连接数+1 */
		++m_FreeConn;
	}

    /* 根据空闲连接数设置信号量 */
	reserve = sem(m_FreeConn);

    /* 此时的空闲数也是最大连接数 */
	m_MaxConn = m_FreeConn;
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

    /* 如果列表大小为0，说明连接池中没有连接 */
	if (0 == connList.size())
		return NULL;

    /* 信号量计数减一 */
	reserve.wait();
	/* 由于需要对列表进行操作，所以需要将其上锁 */
	lock.lock();
    /* 获取一个连接对象，并将其从连接池中去掉 */
	con = connList.front();
	connList.pop_front();

    /* 空闲连接数-1 */
	--m_FreeConn;
    /* 使用的连接数+1 */
	++m_CurConn;

	lock.unlock();
	return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.lock();
    /* 将一个连接对象添加到连接池中 */
	connList.push_back(con);
	++m_FreeConn;
	--m_CurConn;

	lock.unlock();

    /* 控制信号量+1，如果有被信号量的wait方法阻塞的线程，将被唤醒 */
	reserve.post();
	return true;
}

//销毁数据库连接池
void connection_pool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
        /* 依次遍历连接，将其关闭 */
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);
		}
		m_CurConn = 0;
		m_FreeConn = 0;
		connList.clear();
	}

	lock.unlock();
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}

/* 连接池析构函数直接调用线程池销毁函数 */
connection_pool::~connection_pool()
{
	DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
    /* 获得一个连接对象 */
	*SQL = connPool->GetConnection();
	
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}
