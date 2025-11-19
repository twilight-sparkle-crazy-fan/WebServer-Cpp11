#include "log.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <cstdarg>


Log::Log(){
    m_line_count = 0;
    m_is_async = false;
}

Log::~Log(){
    if(m_fp != nullptr){
        fclose(m_fp);
    }
}

void Log::flush(){
    std::lock_guard<std::mutex> lock(m_mutex);
    fflush(m_fp);
    //强制刷新写入流缓冲区
}

bool Log::init(const std::string file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size){
    if(max_queue_size > 0){
        m_is_async = true;
        m_log_queue.reset(new block_queue<std::string>(max_queue_size));
        std::thread t (&Log::asyncWrite_log, this);
        t.detach();
    }

    m_close_log = close_log;
    m_log_buf_size = log_buf_size;  
    m_buf.reset(new char[m_log_buf_size]);
    memset(m_buf.get(), '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(nullptr);
    struct tm my_tm;
    localtime_r(&t, &my_tm);

    size_t last_slash_pos = file_name.find_last_of('/');
    char log_full_name[256] = {0};

    if (last_slash_pos == std::string::npos){
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name.c_str());

        log_name = file_name;
        dir_name = "";

    }
    else{
        std::string log_name_str = file_name.substr(last_slash_pos + 1);
        std::string dir_name_str = file_name.substr(0, last_slash_pos + 1);

        log_name = log_name_str; 
        dir_name = dir_name_str;

        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name_str.c_str(), my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name_str.c_str());
    }

    m_today = my_tm.tm_mday;

    m_fp = fopen(log_full_name, "a");

    if(m_fp == nullptr){
        return false;
    }
    return true;
 }

 void Log::write_log(int level, const char* format, ...){
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    struct tm my_tm;
    localtime_r(&now_time_t, &my_tm);
    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;
    long microsecond = now_us.count();

    std::string s;
    switch(level){
        case 0:
        s = "[debug]: ";
        break;
        case 1:
        s = "[info]: ";
        break;
        case 2:
        s = "[warn]: ";
        break;
        case 3:
        s = "[error]: ";
        break;
        default:
        s = "[info]: ";
        break;
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    m_line_count++;
    if (m_today != my_tm.tm_mday || m_line_count % m_split_lines == 0) {
        fflush(m_fp);
        fclose(m_fp);
        char time_buf[32] = {0};
        snprintf(time_buf, 32, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
        std::string tail = time_buf;
        std::string new_log_name;
        if (m_today != my_tm.tm_mday) {
            new_log_name = dir_name + tail + log_name;
            m_today = my_tm.tm_mday;
            m_line_count = 0;
        }
        else{
            new_log_name = dir_name + tail + log_name + "." + std::to_string(m_line_count / m_split_lines);
        }
        m_fp = fopen(new_log_name.c_str(), "a");
    }

        
        va_list valist;
        va_start(valist, format);
        std::string log_str;

        int n = snprintf(m_buf.get(),48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, microsecond, s.c_str());
        
        int m = vsnprintf(m_buf.get() + n, m_log_buf_size - n - 1, format, valist);
        va_end(valist);

        m_buf.get()[n + m] = '\n';
        m_buf.get()[n + m + 1] = '\0';
        log_str = m_buf.get();
        lock.unlock();
        if (m_is_async && !m_log_queue->full()){
            m_log_queue->push(log_str);
        }
        else{
            lock.lock();
            fputs(log_str.c_str(), m_fp);
        }   


 }