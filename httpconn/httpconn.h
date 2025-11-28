#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#endif

#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <atomic>
#include <mutex>
#include <map>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include "../CGImysql/sql_connection_pool.h"

class httpConn
{
public:
    static constexpr int FILENAME_SIZE = 200;
    static constexpr int READ_BUFFER_SIZE = 2048;
    static constexpr int WRITE_BUFFER_SIZE = 1024;
    enum METHOD
    {
        // 请求方法
        GET,
        POST,
        // 本项目后面都没用到
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE, // 真正分析请求行
        CHECK_STATE_HEADER,      // 分析请求头
        CHECK_STATE_CONTENT      // 分析请求体
    };
    enum HTTP_CODE
    {
        NO_REQUEST,        // 请求不完整，需要继续接收
        GET_REQUEST,       // 获取完整请求，可调用doRequest()
        BAD_REQUEST,       // 请求语法错误,返回400
        NO_RESOURCE,       // 资源不存在,返回404
        FORBIDDEN_REQUEST, // 请求的资源被禁止,返回403
        FILE_REQUEST,      // 文件请求,准备返回文件
        INTERNAL_ERROR,    // 内部错误,返回500
        CLOSED_CONNECTION  // 连接被关闭,返回-1
    };
    enum LINE_STATUS
    {
        LINE_OK,  // 行数据完整  因为找到了\r\n
        LINE_BAD, // 行数据错误  没到换行符就非法字符，或不符合HTTP协议
        LINE_OPEN // 行数据不完整  数据没发完，通过epoll等待下次数据
    };

    enum class IO_state
    {
        READ,
        WRITE
    };

public:
    httpConn() {};
    ~httpConn() {};

public:
    void init(int sockfd, const sockaddr_in &addr, std::string root, int trigMode, int closeLog, std::string username, std::string password, std::string sqlname);
    void closeConn(bool real_close = true);
    void process();
    bool read();
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }

    void initmysql_result(sql_connection_pool *connPool);
    bool timer_flag;
    std::atomic_bool m_processing_finished;

private:
    void init();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE process_read();
    HTTP_CODE do_request();
    HTTP_CODE parse_request_line();

public:
    static std::atomic_int m_epollfd;
    static std::atomic_int m_userCount;
    MYSQL *mysql;
    IO_state m_state;

private:
    int m_sockfd;
    sockaddr_in m_address;

    static std::mutex m_lock;
    static std::map<std::string, std::string> m_user;

    std::string docRoot; // 网页根目录

    int m_trigMode;
    int m_closeLog;

    std::string sqlUser;
    std::string sqlPassword;
    std::string sqlName;

    int bytes_to_send;
    int bytes_have_send;

private:
    char m_readBuf[READ_BUFFER_SIZE];
    char m_writeBuf[WRITE_BUFFER_SIZE];
    char m_realFile[FILE_NAME];
    CHECK_STATE m_checkState;
    METHOD m_method;
    bool m_linger;   // 是否保持连接 默认是关的
    std::string m_url;
    std::string m_version;
    std::string m_host;
    long m_contentLength;
    
    int m_startLine;
    int m_checkedIdx;
    int m_readIdx;
    int m_writeIdx;
    int cgi;  // 是否启用的POST


};