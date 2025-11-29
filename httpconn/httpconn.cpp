#include "httpconn.h"
#include <mysql/mysql.h>
#include <iostream>

// 定义http响应的一些状态信息
constexpr const char *ok_200_title = "OK";
constexpr const char *error_400_title = "Bad Request";
constexpr const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
constexpr const char *error_403_title = "Forbidden";
constexpr const char *error_403_form = "You do not have permission to get file form this server.\n";
constexpr const char *error_404_title = "Not Found";
constexpr const char *error_404_form = "The requested file was not found on this server.\n";
constexpr const char *error_500_title = "Internal Error";
constexpr const char *error_500_form = "There was an unusual problem serving the request file.\n";

std::mutex m_lock;
std::map<std::string, std::string> httpConn::m_user;

void httpConn::initmysql_result(sql_connection_pool *connPool)
{
    MYSQL *mysql = nullptr;
    connectionRAII mysql(&mysql, connPool);

    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        // LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        // TODO
    }

    MYSQL_RES *result = mysql_store_result(mysql);

    // int num_fields = mysql_num_fields(result);
    // 这行没用

    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        std::string temp1(row[0]);
        std::string temp2(row[1]);
        m_user[temp1] = temp2;
    }
}

void addfd(int epollfd, int fd, bool one_shot, int TrigMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (TrigMode == 1)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
    // F_GETFL 获取文件描述符的属性
    // F_SETFL 设置新的状态标志
}

void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev, int TrigMode)
{
    epoll_event event;
    event.data.fd = fd;
    if (TrigMode == 1)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

std::atomic_int httpConn::m_epollfd{-1};
std::atomic_int httpConn::m_userCount{0};

void httpConn::closeConn(bool real_close)
{
    if (real_close && m_sockfd != -1)
    {
        // std::cout << "closeConn"<< m_sockfd << std::endl;
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_userCount--;
    }
}

void httpConn::init(int sockfd, const sockaddr_in &addr, std::string root, int trigMode, int closeLog, std::string user, std::string password, std::string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;
    m_userCount++;
    m_trigMode = trigMode;
    m_closeLog = closeLog;

    addfd(m_epollfd, sockfd, true, m_trigMode);

    docRoot = std::move(root);
    sqlUser = std::move(user);
    sqlPassword = std::move(password);
    sqlName = std::move(sqlname);

    init();
}

void httpConn::init()
{
    mysql = nullptr;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_checkState = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = nullptr;
    m_version = nullptr;
    m_host = nullptr;
    m_contentLength = 0;

    m_startLine = 0;
    m_checkedIdx = 0;
    m_readIdx = 0;

    cgi = 0;
    m_state = IO_state::READ;
    timer_flag = 0;
    m_processing_finished = false;

    memset(m_readBuf, '\0', READ_BUFFER_SIZE);
    memset(m_writeBuf, '\0', WRITE_BUFFER_SIZE);
    memset(m_realFile, '\0', FILENAME_SIZE);
}

httpConn::LINE_STATUS httpConn::parse_line()
{
    char temp;
    for (; m_checkedIdx < m_readIdx; ++m_checkedIdx)
    {
        temp = m_readBuf[m_checkedIdx];
        if (temp == '\r')
        {
            if (m_checkedIdx + 1 == m_readIdx)
                return LINE_OPEN;
            else if (m_readBuf[m_checkedIdx + 1] == '\n')
            {
                m_readBuf[m_checkedIdx++] = '\0';
                m_readBuf[m_checkedIdx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checkedIdx > 1 && m_readBuf[m_checkedIdx - 1] == '\r')
            {
                m_readBuf[m_checkedIdx - 1] = '\0';
                m_readBuf[m_checkedIdx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    };
}

bool httpConn::read_once()
{
    if (m_readIdx >= READ_BUFFER_SIZE)
        return false;
    int bytes_read = 0;

    if (m_trigMode == 0)
    {
        bytes_read = recv(m_sockfd, m_readBuf + m_readIdx, READ_BUFFER_SIZE - m_readIdx, 0);
        m_readIdx += bytes_read;
        if (bytes_read <= 0)
            return false;
        return true;
    }
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_readBuf + m_readIdx, READ_BUFFER_SIZE - m_readIdx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                // 内核缓冲区没有数据
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_readIdx += bytes_read;
        }
        return true;
    }
}

// 解析http请求行，获得请求方法，目标url及http版本号
httpConn::HTTP_CODE httpConn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
    {
        return BAD_REQUEST;
    }
    // strpbrk 返回第一个匹配的字符位置
    // strspn 返回开头匹配的字符串长度
    // strcspn 返回开头不匹配的字符串长度
    // strchr(s, c)：查找字符 c 在字符串 s 中第一次出现的位置

    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if (strncasecmp(m_url, "https://", 7) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }
    if (strlen(m_url) == 1)
        strcat(m_url, "index.html");
    // strcat 连接字符串
    m_checkState = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

/*   参考报文
POST /2CGISQL.cgi HTTP/1.1
Host: 192.168.1.100:9006
Connection: keep-alive
Content-Length: 23
Content-Type: application/x-www-form-urlencoded
user=zhangsan&passwd=123
*/

httpConn::HTTP_CODE httpConn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_contentLength != 0)
        {
            m_checkState = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_contentLength = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        // LOG_INFO("oop! unknow header %s", text);
    }
    return NO_REQUEST;
}

httpConn::HTTP_CODE httpConn::parse_content(char *text){
    if (m_readIdx >= (m_contentLength + m_checkedIdx)){
        text[m_contentLength] = '\0';
        m_requestHeadData = text;
        return GET_REQUEST;
        //这个\0 有存在破坏下一个请求的可能
    }
    return NO_REQUEST;
}

//主状态机
httpConn::HTTP_CODE httpConn::process_read(){
    
}