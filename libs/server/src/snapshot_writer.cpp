#include <hog/core/assert.hpp>
#include <hog/server/snapshot_writer.hpp>

#include <utility>

namespace hog::server {

namespace {

// Checked before the thread starts, which a failed constructor could never join.
std::size_t checked_keep(std::size_t keep) {
    HOG_ASSERT(keep > 0, "a writer has to keep the snapshots it writes");
    return keep;
}

} // namespace

SaveTimer::SaveTimer(GameTime start, GameDuration interval)
    : interval_{interval}, next_{start + interval} {
    HOG_ASSERT(interval > GameDuration::zero(), "snapshots need an interval");
}

bool SaveTimer::due(GameTime now) {
    if (now < next_) {
        return false;
    }
    next_ = now + interval_;
    return true;
}

SnapshotWriter::SnapshotWriter(save::SnapshotStore& store, std::size_t keep)
    : store_{&store}, keep_{checked_keep(keep)}, thread_{[this] { run(); }} {}

SnapshotWriter::~SnapshotWriter() {
    {
        const std::scoped_lock lock{mutex_};
        stopping_ = true;
    }
    wake_.notify_one();
    thread_.join();
}

void SnapshotWriter::submit(save::Snapshot snapshot) {
    {
        const std::scoped_lock lock{mutex_};
        if (waiting_) {
            ++stats_.replaced;
        }
        waiting_ = std::move(snapshot);
    }
    wake_.notify_one();
}

Result<void> SnapshotWriter::flush() {
    std::unique_lock lock{mutex_};
    idle_.wait(lock, [this] { return !waiting_ && !writing_; });
    if (failure_) {
        auto failure = std::move(*failure_);
        failure_.reset();
        return std::unexpected(std::move(failure));
    }
    return {};
}

SnapshotWriter::Stats SnapshotWriter::stats() const {
    const std::scoped_lock lock{mutex_};
    return stats_;
}

void SnapshotWriter::run() {
    std::unique_lock lock{mutex_};
    for (;;) {
        wake_.wait(lock, [this] { return waiting_ || stopping_; });
        if (!waiting_) {
            return;
        }
        const save::Snapshot snapshot = *std::move(waiting_);
        waiting_.reset();
        writing_ = true;
        lock.unlock();
        auto saved = save::save(*store_, snapshot, keep_);
        lock.lock();
        writing_ = false;
        if (saved) {
            ++stats_.written;
        } else {
            ++stats_.failed;
            if (!failure_) {
                failure_ = std::move(saved.error());
            }
        }
        idle_.notify_all();
    }
}

} // namespace hog::server
