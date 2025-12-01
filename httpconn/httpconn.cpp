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

std::mutex httpConn::m_lock;
std::map<std::string, std::string> httpConn::m_user;

void httpConn::initmysql_result(sql_connection_pool *connPool)
{
    MYSQL *mysqlconn = nullptr;
    connectionRAII mysql(&mysqlconn, connPool);

    if (mysql_query(mysqlconn, "SELECT username,passwd FROM user"))
    {
        if (0 == m_closeLog)
        {
            Log::get_instance().write_log(3, "SELECT error: %s\n", mysql_error(mysqlconn));
            Log::get_instance().flush();
        }
    }

    MYSQL_RES *result = mysql_store_result(mysqlconn);

    // int num_fields = mysql_num_fields(result);
    // 这行没用

    MYSQL_FIELD *fields = mysql_fetch_fields(result);
    std::lock_guard<std::mutex> guard(m_lock);
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        std::string temp1(row[0]);
        std::string temp2(row[1]);
        m_user[temp1] = temp2;
    }
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
    return LINE_OPEN;
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
        if (0 == m_closeLog)
        {
            Log::get_instance().write_log(1, "oop! unknow header %s", text);
            Log::get_instance().flush();
        }
    }
    return NO_REQUEST;
}

httpConn::HTTP_CODE httpConn::parse_content(char *text)
{
    if (m_readIdx >= (m_contentLength + m_checkedIdx))
    {
        text[m_contentLength] = '\0';
        m_requestHeadData = text;
        return GET_REQUEST;
        // 这个\0 有存在破坏下一个请求的可能
    }
    return NO_REQUEST;
}

// 主状态机
httpConn::HTTP_CODE httpConn::process_read()
{
    LINE_STATUS lineStatus = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = nullptr;

    while ((m_checkState == CHECK_STATE_CONTENT && lineStatus == LINE_OK) || (lineStatus = parse_line()) == LINE_OK)
    {
        text = get_line();
        m_startLine = m_checkedIdx;

        if (0 == m_closeLog)
        {
            Log::get_instance().write_log(1, "got 1 http line: %s", text);
            Log::get_instance().flush();
        }
        switch (m_checkedIdx)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
            {
                return do_request();
            }
            lineStatus = LINE_OPEN;
            break;
        }
        default:
        {
            return INTERNAL_ERROR;
        }
        }
    }
    return NO_REQUEST;
}

httpConn::HTTP_CODE httpConn::do_request()
{
    strcpy(m_realFile, docRoot.c_str());
    int len = strlen(docRoot.c_str());
    const char *p = strrchr(m_url, '/');
    // 查找最后一次的/

    // 处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        char flag = m_url[1];
    }

    char m_url_real[200];
    strcpy(m_url_real, "/");
    strcat(m_url_real, m_url + 2);
    strncpy(m_realFile + len, m_url_real, FILENAME_SIZE - len - 1);

    std::string name;
    std::string password;
    // user=123&passwd=123

    std::string requestHeadData = m_requestHeadData; // 假设这是 user=zhangsan&passwd=123

    // 1. 找 user
    size_t posUser = requestHeadData.find("user=");

    // 2. 找 passwd
    size_t posPassword = requestHeadData.find("passwd=");

    if (posUser != std::string::npos && posPassword != std::string::npos)
    {
        size_t endUser = requestHeadData.find('&', posUser);
        if (endUser == std::string::npos)
        {
            name = requestHeadData.substr(posUser + 5);
        }
        else
        {
            name = requestHeadData.substr(posUser + 5, endUser - (posUser + 5));
        }
        size_t endPassword = requestHeadData.find('&', posPassword);
        if (endPassword == std::string::npos)
        {
            password = requestHeadData.substr(posPassword + 7);
        }
        else
        {
            password = requestHeadData.substr(posPassword + 7, endPassword - (posPassword + 7));
        }
    }

    if (*(p + 1) == '3')
    {
        std::string sql = "SELECT * FROM user WHERE username='" + name + "' AND password='" + password + "'";

        if (m_user.find(name) != m_user.end())
        {
            m_lock.lock();
            int res = mysql_query(mysql, sql.c_str());
            m_user.insert(std::make_pair(name, password));
            m_lock.unlock();

            if (!res)
            {
                strcpy(m_url, "/log.html");
            }
            else
            {
                strcpy(m_url, "/registerError.html");
            }
        }
        else
        {
            strcpy(m_url, "/registerError.html");
        }
    }
    else if (*(p + 1) == '2')
    {
        if (m_user.find(name) != m_user.end() && m_user[name] == password)
        {
            strcpy(m_url, "/welcome.html");
        }
        else
        {
            strcpy(m_url, "/logError.html");
        }
    }
    len = docRoot.size();
    const char *target_file = nullptr;
    char action = *(p + 1);
    switch (action)
    {
    case '0':
        target_file = "/register.html";
        break;
    case '1':
        target_file = "/log.html";
        break;
    case '5':
        target_file = "/picture.html";
        break;
    case '6':
        target_file = "/video.html";
        break;
    case '7':
        target_file = "/fans.html";
        break;
    default:
        target_file = m_url;
        break;
    }

    strcpy(m_realFile, docRoot.c_str());
    strncat(m_realFile, target_file, FILENAME_SIZE - len - 1);

    if (stat(m_realFile, &m_fileStat) < 0)
        return NO_RESOURCE;

    // 调用系统函数 stat，把文件的所有信息读到 m_file_stat 结构体里

    if (!(m_fileStat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    // 如果没有读权限，返回 FORBIDDEN_REQUEST

    if (S_ISDIR(m_fileStat.st_mode))
        return BAD_REQUEST;
    // 如果是目录，返回 BAD_REQUEST

    int fd = open(m_realFile, O_RDONLY);
    m_fileAddress = (char *)mmap(0, m_fileStat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    // 调用系统函数 mmap，将目标文件映射到内存中
    close(fd);
    return FILE_REQUEST;
}

void httpConn::unmap()
{
    if (m_fileAddress)
    {
        munmap(m_fileAddress, m_fileStat.st_size);
        m_fileAddress = nullptr;
    }
}
// 解除映射

bool httpConn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_trigMode);
        // 待发送字节为 0，说明这次请求不需要回数据（或者出错了）
        // 把epoll模式改为读模式
        init();
        return true;
    }

    while (true)
    {
        temp = writev(m_sockfd, m_iv, m_ivCount);
        // m_iv   [0] HTTP头部的地址和长度  [1]文件内容的地址和长度
        // int iovcnt  发几块  头 + 文件
        // 返回实际写入的字节数

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                {
                    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_trigMode);
                    return true;
                }
                unmap();
                return false;
            }
            // 资源暂不可用，返回 true，继续发送  其他就退出

            bytes_have_send += temp;
            bytes_to_send -= temp;

            if (bytes_have_send >= m_iv[0].iov_len)
            {
                m_iv[0].iov_len = 0;
                m_iv[1].iov_base = m_fileAddress + (bytes_have_send - m_writeIdx);
                // 整个文件的新起点 = 文件首地址 + (总发送量 - 头部长度)
                m_iv[1].iov_len = bytes_to_send;
            }
            else
            {
                m_iv[0].iov_base = m_writeBuf + bytes_have_send;
                m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
            }

            if (bytes_to_send <= 0)
            {
                unmap();
                modfd(m_epollfd, m_sockfd, EPOLLIN, m_trigMode);

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
}

bool httpConn::add_response(const char *format, ...)
{
    if (m_writeIdx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_writeBuf + m_writeIdx, WRITE_BUFFER_SIZE - 1 - m_writeIdx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_writeIdx))
    {
        va_end(arg_list);
        return false;
    }
    m_writeIdx += len;
    va_end(arg_list);

    if (0 == m_closeLog)
    {
        Log::get_instance().write_log(1, "request:%s", m_writeBuf);
        Log::get_instance().flush();
    }

    return true;
}

bool httpConn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool httpConn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() && add_blank_line();
}

bool httpConn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool httpConn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool httpConn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n", content_len);
}

bool httpConn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool httpConn::add_content(const char *content)
{
    return add_response("%s", content);
}

bool httpConn::process_write(HTTP_CODE ret)
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
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_fileStat.st_size != 0)
        {
            add_headers(m_fileStat.st_size);
            m_iv[0].iov_base = m_writeBuf;
            m_iv[0].iov_len = m_writeIdx;
            m_iv[1].iov_base = m_fileAddress;
            m_iv[1].iov_len = m_fileStat.st_size;
            m_ivCount = 2;
            bytes_to_send = m_writeIdx + m_fileStat.st_size;
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

    m_iv[0].iov_base = m_writeBuf;
    m_iv[0].iov_len = m_writeIdx;
    m_ivCount = 1;
    bytes_to_send = m_writeIdx;
    return true;
}

void httpConn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_trigMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        closeConn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_trigMode);
}