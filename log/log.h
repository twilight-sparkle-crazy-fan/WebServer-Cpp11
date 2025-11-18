#ifndef LOG_H
#define LOG_H


#include <string>
#include <mutex>
#include <memory>
#include <thread>
#include "block_queue.h"

class Log {
    public:
    static Log &get_instance(){
        static Log instance;
        return instance;
    };

    bool init(const std::string file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    void write_log(int level, const std::string& format, ...);

    void flush(void);

    private:
    Log();
    virtual ~Log();

    void asyncWrite_log(){
        std::string single_log;
        while(m_log_queue->pop(single_log)){
            std::lock_guard<std::mutex> lock(m_mutex);
            fputs(single_log.c_str(), m_fp);
        }
    }

    private:
    std::string dir_name;
    std::string log_name;
    int m_split_lines;
    int m_log_buf_size;
    long long m_count;
    int m_today;
    FILE *m_fp;
    std::unique_ptr<char[]> m_buf;
    std::unique_ptr<block_queue<std::string>> m_log_queue;
    bool m_is_async;
    std::mutex m_mutex;
    int m_close_log;

};

#endif