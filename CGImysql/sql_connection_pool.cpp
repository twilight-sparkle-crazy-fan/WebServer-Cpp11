#include "sql_connection_pool.h"
#include <cstdlib>

sql_connection_pool::sql_connection_pool()
{
    m_curConn = 0;
    m_freeConn = 0;
}

sql_connection_pool::~sql_connection_pool()
{
    destoryPool();
}

sql_connection_pool *sql_connection_pool::getInstance()
{
    static sql_connection_pool connPool;
    return &connPool;
}

void sql_connection_pool::init(std::string url, std::string user, std::string passwd, std::string dbName, int port, int close_log, int maxConn)
{

    m_url = url;
    m_user = user;
    m_passwd = passwd;
    m_port = port;
    m_dbname = dbName;
    m_close_log = close_log;

    for (int i = 0; i < maxConn; i++)
    {
        MYSQL *con = nullptr;
        con = mysql_init(con);
        if (con == nullptr)
        {
            if (0 == m_close_log)
            {
                Log::get_instance().write_log(3, "MySQL Error: mysql_init failed\n");
                Log::get_instance().flush();
            }
            exit(-1);
        }
        con = mysql_real_connect(con, m_url.c_str(), m_user.c_str(), m_passwd.c_str(), m_dbname.c_str(), m_port, nullptr, 0);

        if (con == nullptr)
        {

            if (0 == m_close_log)
            {
                Log::get_instance().write_log(3, "MySQL Error: %s\n", mysql_error(con));
                Log::get_instance().flush();
            }
            exit(-1);
        }

        connList.push_back(con);
        ++m_freeConn;
    }
    m_maxConn = m_freeConn;
    reserve.reset(m_freeConn);
}

MYSQL *sql_connection_pool::getConnection()
{
    MYSQL *con = nullptr;

    reserve.wait();
    std::lock_guard<std::mutex> lock(m_mutex);

    con = connList.front();
    connList.pop_front();

    --m_freeConn;
    ++m_curConn;

    return con;
}

bool sql_connection_pool::releaseConnection(MYSQL *con)
{
    if (con == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    connList.push_back(con);
    ++m_freeConn;
    --m_curConn;
    reserve.post();
    return true;
}

void sql_connection_pool::destoryPool()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    {
        for (auto &con : connList)
        {
            mysql_close(con);
        }
        m_curConn = m_freeConn = 0;
        connList.clear();
    }
}

int sql_connection_pool::getFreeConn()
{
    return m_freeConn;
}

connectionRAII::connectionRAII(MYSQL **SQL, sql_connection_pool *connPool)
{
    *SQL = connPool->getConnection();
    conRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII()
{
    poolRAII->releaseConnection(conRAII);
}
