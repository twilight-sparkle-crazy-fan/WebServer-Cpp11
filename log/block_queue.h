#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <mutex>              
#include <condition_variable> 
#include <queue>             
#include <chrono>
#include <assert.h>
#include <stdexcept>
#include <utility>  

template<typename T>
class block_queue {
    public:
    block_queue(int max_size = 1000):m_max_size(max_size){
        if (max_size <= 0){
            throw std::invalid_argument("max_size must be positive");
        }
    }
    
    bool full(){
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size() >= m_max_size;
    }

    bool empty(){
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    bool push(const T& data){
        std::lock_guard<std::mutex> lock(m_mutex);
        if(m_queue.size() >= m_max_size){
            m_cond.notify_all();
            return false;
        }
        m_queue.push(data);
        m_cond.notify_one();
        return true;
    }

    bool push(T&& data){
        std::lock_guard<std::mutex> lock(m_mutex);
        if(m_queue.size() >= m_max_size){
            m_cond.notify_all();
            return false;
        }
        m_queue.push(std::move(data));
        m_cond.notify_one();
        return true;
     }

    bool pop(T& data){
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cond.wait(lock,[this]{return !m_queue.empty();});
        data = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }

    bool pop(T& data, int ms_timeout){
        std::unique_lock<std::mutex> lock(m_mutex);
        if(m_cond.wait_for(lock,std::chrono::milliseconds(ms_timeout),[this]{return !m_queue.empty();}))
        {
        data = std::move(m_queue.front());
        m_queue.pop();
        return true;
        }
        return false;
     }

    private:
    std::mutex m_mutex;
    std::condition_variable m_cond;
    std::queue<T> m_queue;
    int m_max_size;
};
#endif