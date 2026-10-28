#pragma once

#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

namespace quarkpp::detail
{
// Keep every started worker joined, including when creation of a later worker
// throws. std::thread also supports Apple libc++ versions without std::jthread.
class ThreadGroup final
{
  public:
    explicit ThreadGroup(std::size_t capacity)
    {
        workers_.reserve(capacity);
    }

    ThreadGroup(const ThreadGroup &) = delete;
    ThreadGroup &operator=(const ThreadGroup &) = delete;

    ~ThreadGroup()
    {
        for (auto &worker : workers_)
            if (worker.joinable())
                worker.join();
    }

    template <class Function> void emplace_back(Function &&function)
    {
        workers_.emplace_back(std::forward<Function>(function));
    }

  private:
    std::vector<std::thread> workers_;
};
} // namespace quarkpp::detail
