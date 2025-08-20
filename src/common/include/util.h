
#ifndef UTIL_H
#define UTIL_H

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/access.hpp>
#include <condition_variable>  // pthread_condition_t
#include <functional>
#include <iostream>
#include <mutex>  // pthread_mutex_t
#include <queue>
#include <random>
#include <sstream>
#include <thread>
#include "config.h"

template <class F>
class DeferClass {
 public:
  DeferClass(F&& f) : m_func(std::forward<F>(f)) {}
  DeferClass(const F& f) : m_func(f) {}
  ~DeferClass() { m_func(); }

  DeferClass(const DeferClass& e) = delete;
  DeferClass& operator=(const DeferClass& e) = delete;

 private:
  F m_func;
};

#define _CONCAT(a, b) a##b
#define _MAKE_DEFER_(line) DeferClass _CONCAT(defer_placeholder, line) = [&]()

#undef DEFER
#define DEFER _MAKE_DEFER_(__LINE__)

void DPrintf(const char* format, ...);

void myAssert(bool condition, std::string message = "Assertion failed!");

template <typename... Args>
std::string format(const char* format_str, Args... args) {
    int size_s = std::snprintf(nullptr, 0, format_str, args...) + 1; // "\0"
    if (size_s <= 0) { throw std::runtime_error("Error during formatting."); }
    auto size = static_cast<size_t>(size_s);
    std::vector<char> buf(size);
    std::snprintf(buf.data(), size, format_str, args...);
    return std::string(buf.data(), buf.data() + size - 1);  // remove '\0'
}

std::chrono::_V2::system_clock::time_point now();

std::chrono::milliseconds getRandomizedElectionTimeout();
void sleepNMilliseconds(int N);


template <typename T>
class LockQueue {
public:
    // 向队列中推送一个元素，并唤醒一个正在等待的线程
    void Push(const T& data) {
        std::lock_guard<std::mutex> lock(m_mutex); 
        m_queue.push(data);
        m_condvariable.notify_one();
    }

    // 如果队列为空，当前线程会阻塞直到有数据可用
    T Pop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condvariable.wait(lock, [this] { return !m_queue.empty(); });
        T data = std::move(m_queue.front()); 
        m_queue.pop();                       
        return data;
    }
  
    // 尝试在指定超时时间内弹出一个元素
    bool timeOutPop(int timeout, T* ResData) {
        std::unique_lock<std::mutex> lock(m_mutex); 
        if (!m_condvariable.wait_for(lock, std::chrono::milliseconds(timeout), [this] { return !m_queue.empty(); })) {
            return false; 
        }
        *ResData = std::move(m_queue.front()); 
        m_queue.pop();                        
        return true;
    }
private:
  std::queue<T> m_queue;
  std::mutex m_mutex;
  std::condition_variable m_condvariable;
};

// 这个Op是kv传递给raft的command
class Op {
public:
    std::string Operation;  // "Get" "Put" "Append"
    std::string Key;
    std::string Value;
    std::string ClientId;  // 客户端ID
    int RequestId;         // 客户端ID请求的Request的序列号，为了保证线性一致性
            
public:
  // TODO: 正常字符中不能包含|，待升级序列化方法为protobuf
    std::string asString() const {
        std::stringstream ss;
        boost::archive::text_oarchive oa(ss);
        oa << *this;
        return ss.str();
    }

    bool parseFromString(std::string str) {
        std::stringstream iss(str);
        boost::archive::text_iarchive ia(iss);
        ia >> *this;
        return true;  
    }

public:
    friend std::ostream& operator<<(std::ostream& os, const Op& obj) {
        os << "[MyClass:Operation{" + obj.Operation + "},Key{" + obj.Key + "},Value{" + obj.Value + "},ClientId{" +
                obj.ClientId + "},RequestId{" + std::to_string(obj.RequestId) + "}";  // 在这里实现自定义的输出格式
        return os;
    }

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const unsigned int version) {
        ar& Operation;
        ar& Key;
        ar& Value;
        ar& ClientId;
        ar& RequestId;
    }
};

// kvserver reply err to clerk

const std::string OK = "OK";
const std::string ErrNoKey = "ErrNoKey";
const std::string ErrWrongLeader = "ErrWrongLeader";


bool isReleasePort(unsigned short usPort);
bool getReleasePort(short& port);
std::string writeConfig(int nodeIndex, short port);


#endif  //  UTIL_H