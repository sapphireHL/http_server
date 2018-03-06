#ifndef THREAD_POOL_H_INCLUDED
#define THREAD_POOL_H_INCLUDED

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"//线程同步包装类

//线程池类，定义为模板类为了代码复用，模板参数T是任务类
template<typename T>
class threadpool
{
public:
    //构造函数，thread_number为线程池中线程的数量，max_requests为请求队列中最多允许的等待处理的请求的数量
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    //向请求队列中添加任务
    bool append(T* request);

private:
    //线程工作函数，从请求队列中取出任务并执行
    static void* worker(void* arg);
    void run();

private:
    int m_thread_number;    //线程池中的线程数量
    int m_max_requests;     //请求队列中允许的最大请求数量
    pthread_t * m_threads;  //描述线程池的数组，大小为m_thread_number
    std::list<T*> m_request_queue;  //请求队列
    locker m_queue_locker;      //保护请求队列的互斥锁
    sem m_queue_stat;       //是否有任务需要处理
    bool m_stop;        //是否结束线程
};

template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests):
    m_thread_number(thread_number), m_max_requests(max_requests),
    m_threads(NULL), m_stop(false)
{
    if(thread_number <= 0 || max_requests <= 0){
        throw std::exception();
    }
    //创建线程池
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads){
        throw std::exception();
    }
    //创建thread_number个线程，均设置为脱离线程
    for(int i = 0; i < m_thread_number; i++){
        printf("create %dth thread...\n", i + 1);
        //第三个参数是静态成员函数，第四个参数传递类的对象
        if(pthread_create(m_threads + i, NULL, worker, this) != 0){
            delete [] m_threads;
            throw std::exception();
        }
        if(pthread_detach(m_threads[i]) != 0){
            delete [] m_threads;
            throw std::exception();
        }
    }
}

template<typename T>
threadpool<T>::~threadpool()
{
    delete [] m_threads;
    m_stop = true;
}

template<typename T>
bool threadpool<T>::append(T* request)
{
    //请求队列加锁，因为它被所有线程共享
    m_queue_locker.lock();
    if(m_request_queue.size() > m_max_requests){
        m_queue_locker.unlock();
        return false;
    }
    m_request_queue.push_back(request);
    m_queue_locker.unlock();
    m_queue_stat.post();
    return true;
}

template<typename T>
void* threadpool<T>::worker(void* arg)
{
    //静态成员函数调用该对象的动态方法来访问类的动态成员
    threadpool* pool = (threadpool*)arg;
    pool->run();
    return pool;
}

template<typename T>
void threadpool<T>::run()
{
    while(!m_stop){
        m_queue_stat.wait();
        m_queue_locker.lock();
        if(m_request_queue.empty()){
            m_queue_locker.unlock();
            continue;
        }
        T* request = m_request_queue.front();
        m_request_queue.pop_front();
        m_queue_locker.unlock();
        if(!request)
            continue;
        request->process();
    }
}
#endif // THREAD_POOL_H_INCLUDED
