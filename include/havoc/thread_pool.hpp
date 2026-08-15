#pragma once

/// @file thread_pool.hpp
/// @brief Thread pool for search workers and timer threads.

#include "havoc/move_order.hpp"
#include "havoc/eval/evaluator.hpp"
#include "havoc/eval/hce.hpp"
#include "havoc/material_table.hpp"
#include "havoc/parameters.hpp"
#include "havoc/pawn_table.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace havoc {

// ─── Worker thread ──────────────────────────────────────────────────────────

class Workerthread {
    std::thread thread_;
    std::function<void()> func_;

  public:
    Workerthread() = default;
    explicit Workerthread(std::function<void()> func) : thread_(func), func_(std::move(func)) {}
    Workerthread(const Workerthread&) = delete;
    Workerthread& operator=(const Workerthread&) = delete;
    virtual ~Workerthread() = default;

    std::thread& thread() { return thread_; }
};

// ─── Search thread (owns eval tables) ───────────────────────────────────────

class Searchthread : public Workerthread {
  public:
    parameters params;
    /// Move-ordering statistics. Per thread, not shared: Lazy SMP has every
    /// thread writing these tables concurrently, and they are plain ints with
    /// no synchronisation, so a shared instance is a data race. Sharing also
    /// defeats the point of running different threads down different parts of
    /// the tree -- they would all be steered by one another's statistics.
    Movehistory history;
    pawn_table pawn_tbl{params};
    material_table material_tbl{params};

    Searchthread() { set_evaluator(std::make_unique<HCEEvaluator>(pawn_tbl, material_tbl, params)); }

    explicit Searchthread(std::function<void()> func) : Workerthread(std::move(func)) {
        set_evaluator(std::make_unique<HCEEvaluator>(pawn_tbl, material_tbl, params));
    }

    /// Installs the evaluator and caches whether it wants move deltas.
    ///
    /// The two travel together deliberately: the cached flag is read on every
    /// node, and a flag that disagreed with the installed evaluator would
    /// either silently stop feeding an incremental evaluator -- desynchronising
    /// its accumulator from the board -- or pay for calls nobody consumes.
    void set_evaluator(std::unique_ptr<IEvaluator> e) {
        evaluator_ = std::move(e);
        wants_deltas_ = evaluator_->wants_deltas();
    }

    [[nodiscard]] IEvaluator& evaluator() { return *evaluator_; }
    [[nodiscard]] const IEvaluator& evaluator() const { return *evaluator_; }

    /// Cached `evaluator().wants_deltas()`; see `IEvaluator::wants_deltas`.
    [[nodiscard]] bool wants_deltas() const { return wants_deltas_; }

  private:
    std::unique_ptr<IEvaluator> evaluator_;
    bool wants_deltas_ = false;
};

// ─── Thread pool ────────────────────────────────────────────────────────────

template <class T> class Threadpool {
    std::vector<std::unique_ptr<T>> workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex m_;
    std::condition_variable cv_task_;
    std::condition_variable cv_finished_;
    std::atomic<unsigned> busy_{0};
    std::atomic<unsigned> processed_{0};
    std::atomic<bool> stop_{true};
    unsigned num_threads_ = 0;

    void thread_func() {
        while (true) {
            std::unique_lock<std::mutex> lock(m_);
            cv_task_.wait(lock, [this]() { return stop_.load() || !tasks_.empty(); });
            if (!tasks_.empty()) {
                ++busy_;
                auto fn = tasks_.front();
                tasks_.pop_front();
                lock.unlock();
                fn();
                ++processed_;
                lock.lock();
                --busy_;
                cv_finished_.notify_one();
            } else if (stop_.load()) {
                break;
            }
        }
    }

  public:
    Threadpool() = default;

    explicit Threadpool(unsigned n) : busy_(0), processed_(0), stop_(false), num_threads_(n) {
        for (unsigned i = 0; i < n; ++i)
            workers_.push_back(std::make_unique<T>([this]() { this->thread_func(); }));
    }

    ~Threadpool() {
        if (!stop_.load())
            exit();
    }

    T* operator[](int idx) { return workers_[idx].get(); }
    const T* operator[](int idx) const { return workers_[idx].get(); }

    void init(int numThreads) {
        if (!stop_.load())
            exit();
        workers_.clear();

        busy_ = 0;
        processed_ = 0;
        stop_ = false;
        num_threads_ = static_cast<unsigned>(numThreads);

        for (unsigned i = 0; i < num_threads_; ++i)
            workers_.push_back(std::make_unique<T>([this]() { this->thread_func(); }));
    }

    size_t num_workers() const { return workers_.size(); }

    template <class F, typename... Args> void enqueue(F&& f, Args&&... args) {
        std::unique_lock<std::mutex> lock(m_);
        tasks_.emplace_back(
            [func = std::forward<F>(f), ... captured_args = std::forward<Args>(args)]() mutable {
                func(captured_args...);
            });
        cv_task_.notify_one();
    }

    void clear_tasks() {
        std::unique_lock<std::mutex> lock(m_);
        tasks_.clear();
    }

    unsigned size() const { return num_threads_; }

    void wait_finished() {
        std::unique_lock<std::mutex> lock(m_);
        cv_finished_.wait(lock, [this]() { return tasks_.empty() && (busy_ == 0); });
    }

    unsigned get_processed() const { return processed_.load(); }

    void exit() {
        {
            std::unique_lock<std::mutex> lock(m_);
            stop_ = true;
            cv_task_.notify_all();
        }
        for (auto& t : workers_) {
            if (t->thread().joinable())
                t->thread().join();
        }
    }
};

} // namespace havoc
