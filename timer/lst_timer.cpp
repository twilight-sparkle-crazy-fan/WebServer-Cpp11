#include "lst_timer.h"
#include <time.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <signal.h>
#include <cstring>
#include <assert.h>
#include <unistd.h>

sort_timer_lst::sort_timer_lst(){
    head = nullptr;
    tail = nullptr;
}

sort_timer_lst::~sort_timer_lst(){
    head = nullptr;
    tail = nullptr;

}

void sort_timer_lst::add_timer(util_timer *timer){ 
    if(!timer){
        return;
    }
    if(!head){
        head = tail = timer;
        return;
    }

    if (timer->expire < head ->expire){
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);
}

void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head){
    util_timer *prev = lst_head;
    util_timer *tmp  = prev->next;
    while (tmp){
        if (timer->expire < tmp->expire){
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    if (!tmp){
        prev->next = timer;
        timer->prev = prev;
        timer->next = nullptr;
        tail = timer;
    }
 }

 void sort_timer_lst::adjust_timer(util_timer *timer){
    if (!timer){
        return;
    }
    util_timer *tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire)){
        return;
    }
    if (timer == head){
        head = head->next;
        head->prev = nullptr;
        timer->next = nullptr;
        add_timer(timer, head);
    }
    else{
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
 }

 void sort_timer_lst::del_timer(util_timer *timer){
    if (!timer){
        return;
    }
    if ((timer == head) && (timer == tail)){
        head = nullptr;
        tail = nullptr;
        return;
    }
    if (timer == head){
        head = head->next;
        head->prev = nullptr;
        return;
    }
    if (timer == tail){
        tail = tail->prev;
        tail->next = nullptr;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
 }

 void sort_timer_lst::tick(){
    if (!head){
        return;
    }
    time_t cur = time(nullptr);
    util_timer *tmp = head;
    while (tmp){
        if (cur < tmp->expire){
            break;
        }

        if (tmp ->cb_func){
            tmp->cb_func();
        }

        head = tmp->next;
        if (head){
            head->prev = nullptr;
        }
        tmp = head;
    }
  }

  void Utils::init(int timeslot){
    m_TIMESLOT = timeslot;
  }

  int Utils::setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;

  }

  void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode){
    epoll_event event;
    event.data.fd = fd;

    if (TRIGMode == 1){
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    }
    else{
        event.events = EPOLLIN | EPOLLRDHUP;
    }

    if (one_shot){
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
  }

  void Utils::sig_handler(int sig){
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
  }

  void Utils::addsig(int sig, void(handler)(int), bool restart){
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart){
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, nullptr) != -1);
   }

   void Utils::timer_handler(){
    m_time_lst.tick();
    alarm(m_TIMESLOT);
    }

    void Utils::show_error(int connfd,const std::string& info){
        send(connfd, info.c_str(), info.size(), 0);
        close(connfd);

    }

    int *Utils::u_pipefd = nullptr;
    int Utils::m_epollfd = 0;

