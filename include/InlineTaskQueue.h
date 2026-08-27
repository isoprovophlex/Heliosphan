#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace MPL
{
    class InlineTaskQueue
    {
    public:
        using Task = std::function<void()>;

        void Submit(Task a_task)
        {
            bool drain = false;
            {
                std::scoped_lock lock(mutex);
                tasks.push_back(std::move(a_task));
                if (!draining)
                {
                    draining = true;
                    drain = true;
                }
            }
            if (drain)
            {
                Drain();
            }
        }

    private:
        void Drain()
        {
            try
            {
                while (true)
                {
                    Task task;
                    {
                        std::scoped_lock lock(mutex);
                        if (tasks.empty())
                        {
                            draining = false;
                            return;
                        }
                        task = std::move(tasks.front());
                        tasks.pop_front();
                    }
                    task();
                }
            }
            catch (...)
            {
                std::scoped_lock lock(mutex);
                draining = false;
                throw;
            }
        }

        std::mutex mutex;
        std::deque<Task> tasks;
        bool draining = false;
    };
}  // namespace MPL
