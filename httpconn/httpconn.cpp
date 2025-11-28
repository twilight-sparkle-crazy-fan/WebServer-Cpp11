#include "httpconn.h"
#include <mysql/mysql.h>
#include <iostream>

//定义http响应的一些状态信息
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
std::map<std::string,std::string> httpConn::m_user;

void httpConn::initmysql_result(sql_connection_pool *connPool) {
    MYSQL *mysql = nullptr;
    connectionRAII mysql(&mysql,connPool);

    if (mysql_query(mysql, "SELECT username,passwd FROM user")) {
        //LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        //TODO
    }

    MYSQL_RES *result = mysql_store_result(mysql);

    //int num_fields = mysql_num_fields(result);
    //这行没用，目前

    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    while(MYSQL_ROW row = mysql_fetch_row(result)){
        std::string temp1(row[0]);
        std::string temp2(row[1]);
        m_user[temp1] = temp2;
    }

}

void addfd(int epollfd, int fd, bool one_shot,int TrigMode){
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

int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
    // F_GETFL 获取文件描述符的属性
    // F_SETFL 设置新的状态标志
}

void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev,int TrigMode) {
    epoll_event event;
    event.data.fd = fd;
    if (TrigMode == 1)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
 }

 std::atomic_int httpConn::m_epollfd {-1};
 std::atomic_int httpConn::m_userCount {0};

void httpConn::closeConn(bool real_close) {
    if (real_close && m_sockfd != -1){
        //std::cout << "closeConn"<< m_sockfd << std::endl;
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_userCount--;
    }
}

void httpConn::init(int sockfd, const sockaddr_in& addr,std::string root,int trigMode,int closeLog,std:: string user,std:: string password,std::string sqlname) {
    m_sockfd = sockfd;
    m_address = addr;
    m_userCount ++;
    m_trigMode = trigMode;
    m_closeLog = closeLog;

    addfd(m_epollfd, sockfd, true, m_trigMode);
    
    docRoot = std::move(root);
    sqlUser = std::move(user);
    sqlPassword = std::move(password);
    sqlName = std::move(sqlname);

    init ();
 }


 void httpConn::init() {
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






