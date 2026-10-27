// NxCourier · Copyright (C) 2026 granule256 · GPL-2.0-only
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>

namespace nxc::jobs {
// 一个网络生产者、一个串行执行者；仅状态交接使用原子，工具无需并发访问系统服务。
// 网络线程独占 submit/retire/cancel，工作线程独占 take/finish。
template<int Capacity, int InputBytes, int OutputBytes>
class Queue {
public:
    enum class State { Free, Pending, Running, Complete };
    struct Job {
        std::atomic<State> state{State::Free};
        std::atomic<bool> cancelled{false};
        int client = -1;
        uint64_t generation = 0, ticket = 0;
        bool tooLong = false;
        char input[InputBytes]{};
        char output[OutputBytes]{};
        int outputBytes = 0, sent = 0;
    };
    Job* submit(int client, uint64_t generation, const char* line, bool tooLong) {
        if (std::strlen(line) >= InputBytes) return nullptr;
        int occupied=0;
        for (const auto& j : entries_)
            if (j.state.load(std::memory_order_acquire) != State::Free &&
                j.client == client && j.generation == generation) ++occupied;
        if (occupied >= 2) return nullptr;
        for (auto& j : entries_) {
            if (j.state.load(std::memory_order_acquire) != State::Free) continue;
            j.client = client; j.generation = generation; j.ticket = nextTicket_++;
            j.tooLong = tooLong; j.outputBytes = 0; j.sent = 0;
            j.cancelled.store(false, std::memory_order_relaxed);
            std::strcpy(j.input, line);
            j.state.store(State::Pending, std::memory_order_release);
            return &j;
        }
        return nullptr;
    }
    Job* take() {
        Job* next = nullptr;
        for (auto& j : entries_)
            if (j.state.load(std::memory_order_acquire) == State::Pending &&
                (next == nullptr || j.ticket < next->ticket)) next = &j;
        if (next) next->state.store(State::Running, std::memory_order_release);
        return next;
    }
    void finish(Job& j) { j.state.store(State::Complete, std::memory_order_release); }
    void retire(Job& j) { j.state.store(State::Free, std::memory_order_release); }
    void cancelClient(int client, uint64_t generation) {
        for (auto& j : entries_)
            if (j.state.load(std::memory_order_acquire) != State::Free &&
                j.client == client && j.generation == generation)
                j.cancelled.store(true, std::memory_order_release);
    }
    void cancelAll() {
        for (auto& j : entries_)
            if (j.state.load(std::memory_order_acquire) != State::Free)
                j.cancelled.store(true, std::memory_order_release);
    }
    bool hasWork() const {
        for (const auto& j : entries_) if (j.state.load(std::memory_order_acquire) != State::Free) return true;
        return false;
    }
    bool hasClient(int client, uint64_t generation) const {
        for (const auto& j : entries_)
            if (j.state.load(std::memory_order_acquire) != State::Free &&
                j.client == client && j.generation == generation) return true;
        return false;
    }
    Job* completeFor(int client, uint64_t generation) {
        Job* first = nullptr;
        for (auto& j : entries_) {
            const auto s = j.state.load(std::memory_order_acquire);
            if (s != State::Free && j.client == client && j.generation == generation &&
                (first == nullptr || j.ticket < first->ticket)) first = &j;
        }
        return first && first->state.load(std::memory_order_acquire) == State::Complete ? first : nullptr;
    }
    template<class Live> void discardDisconnected(Live live) {
        for (auto& j : entries_)
            if (j.state.load(std::memory_order_acquire) == State::Complete && !live(j.client, j.generation)) retire(j);
    }
private:
    Job entries_[Capacity];
    uint64_t nextTicket_ = 0;
};
} // namespace nxc::jobs
