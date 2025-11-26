#include "httpconn.h"
#include <mysql/mysql.h>

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

std::lock_guard<std::mutex> httpConn::m_lock;
std::map<std::string,std::string> httpConn::m_user;

void httpConn::initmysql_result(sql_connection_pool *connPool) {
    MYSQL *mysql = nullptr;
    connectionRAII mysql(&mysql,connPool);

    if (mysql_query(mysql, "SELECT username,passwd FROM user")) {
        //LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    MYSQL_RES *result = mysql_store_result(mysql);

    int num_fields = mysql_num_fields(result);

    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    while(MYSQL_ROW row = mysql_fetch_row(result)){
        std::string temp1(row[0]);
        std::string temp2(row[1]);
        m_user[temp1] = temp2;
    }

}


