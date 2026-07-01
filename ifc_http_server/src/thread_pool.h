#pragma once
#include <vector>
#include <thread>
#include <queue>
#include <functional>
#include <condition_variable>

class ThreadPool {
public:
    explicit ThreadPool(size_t threads);
    ~ThreadPool();
    void enqueue(std::function<void()> task);

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop = false;
};
