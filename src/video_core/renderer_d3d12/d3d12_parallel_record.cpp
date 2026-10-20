// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_parallel_record.h"

namespace D3D12 {
ParallelRecordWorker::ParallelRecordWorker(Initialize initialize)
    : worker{[this, initialize = std::move(initialize)]() mutable { Run(std::move(initialize)); }} {
    while (!ready.load(std::memory_order_acquire))
        ready.wait(0, std::memory_order_acquire);
    try {
        Check();
    } catch (...) {
        stream.Close();
        worker.join();
        throw;
    }
}
ParallelRecordWorker::~ParallelRecordWorker() {
    stream.Close();
    worker.join();
}
void ParallelRecordWorker::Check() {
    if (failed.load(std::memory_order_acquire)) {
        std::lock_guard lock{error_mutex};
        std::rethrow_exception(error);
    }
}
void ParallelRecordWorker::Fail() noexcept {
    {
        std::lock_guard lock{error_mutex};
        if (!error)
            error = std::current_exception();
    }
    failed.store(true, std::memory_order_release);
    // Increment a generation to wake Wait even when no completion packet ran.
    finished.fetch_add(1, std::memory_order_release);
    finished.notify_all();
}
void ParallelRecordWorker::Run(Initialize initialize) noexcept {
    void* target{};
    try {
        target = initialize();
    } catch (...) {
        Fail();
    }
    ready.store(1, std::memory_order_release);
    ready.notify_all();
    // Failure drains the bounded queue, releasing COM and descriptor leases and
    // allowing a producer already blocked in Write to observe the original error.
    for (;;) {
        try {
            if (!stream.Consume(target, !failed.load(std::memory_order_acquire)))
                break;
        } catch (...) {
            Fail();
        }
    }
}
void ParallelRecordWorker::Publish(std::uint64_t sequence) {
    EncodeCall<[](void*, std::uintptr_t address, std::uint64_t value) {
        auto* self = reinterpret_cast<ParallelRecordWorker*>(address);
        self->finished.store(value, std::memory_order_release);
        self->finished.notify_all();
    }>(stream, reinterpret_cast<std::uintptr_t>(this), sequence);
    stream.Flush();
    Check();
}
void ParallelRecordWorker::Wait(std::uint64_t sequence) {
    auto value = finished.load(std::memory_order_acquire);
    for (;;) {
        Check();
        if (value >= sequence)
            return;
        finished.wait(value, std::memory_order_acquire);
        value = finished.load(std::memory_order_acquire);
    }
}
} // namespace D3D12
