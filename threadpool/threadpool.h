#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <thread>
#include <mutex>
#include <list>
#include <vector>
#include <exception>
#include "../locker/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{

public:
    threadpool(int actor_model, sql_connection_pool *conn_pool, int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    void worker();
    void run();

private:
    int m_thread_number;
    int m_max_requests;
    std::vector<std::thread> m_threads;
    std::list<T *> m_workqueue;
    std::mutex m_mutex_queue;
    sem m_queue_sem;
    sql_connection_pool *m_connPool;
    int m_actor_model;
};

template <typename T>
threadpool<T>::threadpool(int actor_model, sql_connection_pool *conn_pool, int thread_number, int max_requests)
:m_actor_model(actor_model), m_thread_number(thread_number), m_max_requests(max_requests), m_threads(thread_number), m_connPool(conn_pool)
{

    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();

    for (int i = 0; i < thread_number; ++i){
        m_threads[i] = std::thread([this](){this->worker();});
        m_threads[i].detach();
    }
}

template <typename T>
threadpool<T>::~threadpool(){
}

template <typename T>
bool threadpool<T>::append(T *request, int state){ 
    std::unique_lock<std::mutex> lock(m_mutex_queue);
    if (m_workqueue.size() >= m_max_requests){
        return false;
    }
    request->m_state = state;
    //设置状态 0：读操作 1：写操作
    m_workqueue.push_back(request);
    lock.unlock();
    m_queue_sem.post();
    return true;
}

template <typename T>
bool threadpool<T>::append_p(T *request){
    std::unique_lock<std::mutex> lock(m_mutex_queue);
    if (m_workqueue.size() >= m_max_requests){
        return false;
    }
    m_workqueue.push_back(request);
    lock.unlock();
    m_queue_sem.post();
    return true;
}

template <typename T>
void threadpool<T>::worker(){
    run();
}
//这个没什么用

template <typename T>
void threadpool<T>::run(){
    while (true){
        m_queue_sem.wait();
        std::unique_lock<std::mutex> lock(m_mutex_queue);
        if (m_workqueue.empty()){
            lock.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        lock.unlock();
        if (!request){
            continue;
        }
        if (1 == m_actor_model)
        {
            if (0 == request->m_state)
            {
                if (request->read_once())
                {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            else
            {
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }

    }
 }
#endif