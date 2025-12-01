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
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/uio.h>
#include "../log/log.h"
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
    bool read_once();
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
    HTTP_CODE process_read();                 // 主状态机的处理函数
    HTTP_CODE do_request();

    httpConn::LINE_STATUS parse_line(); // 解析一行

    HTTP_CODE parse_request_line(char *text); // 解析请求行
    HTTP_CODE parse_headers(char *text);      // 解析请求头
    HTTP_CODE parse_content(char *text);      // 判断是否被完整读入


    char *get_line() { return m_readBuf + m_startLine; }

    void unmap();

    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
    

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

    std ::string docRoot; // 网页根目录

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
    char m_realFile[FILENAME_SIZE];
    CHECK_STATE m_checkState;
    METHOD m_method;
    bool m_linger; // 是否保持连接 默认是关的
    char *m_url;
    char *m_version;
    char *m_host;
    long m_contentLength;

    int m_startLine;  // 正在解析的那一行的起始位置
    int m_checkedIdx; // 负责找到下一行的起始位置
    int m_readIdx;    // 读缓冲区上限
    int m_writeIdx;
    int cgi;                 // 是否启用的POST
    char *m_requestHeadData; // 请求头数据

    char *m_fileAddress;
    struct stat m_fileStat; // 文件信息缓存
    struct iovec m_iv[2];   // 系统调用 writev 的专属参数
    int m_ivCount;
};