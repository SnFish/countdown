#pragma once
#include <algorithm>
#include <cstdint>

namespace countdown {
enum class State { Ready, Running, Paused, Finished };

// Caller-provided monotonic time avoids accumulated drift from missed timer messages.
class Timer {
public:
    static constexpr int maxSeconds = 99 * 3600 + 59 * 60 + 59;
    explicit Timer(int seconds = 300) { reset(seconds); }

    void setRange(int minimum, int maximum, std::uint64_t now) {
        minimum_ = std::clamp(minimum, 1, maxSeconds);
        maximum_ = std::clamp(maximum, minimum_, maxSeconds);
        preset_ = std::clamp(preset_, minimum_, maximum_);
        if (state_ != State::Finished) {
            remaining_ = std::clamp(remainingMs(now), static_cast<std::uint64_t>(minimum_) * 1000,
                                    static_cast<std::uint64_t>(maximum_) * 1000);
            if (state_ == State::Running) deadline_ = now + remaining_;
        }
    }

    void reset(int seconds) {
        preset_ = std::clamp(seconds, minimum_, maximum_);
        remaining_ = static_cast<std::uint64_t>(preset_) * 1000;
        state_ = State::Ready;
    }

    std::uint64_t remainingMs(std::uint64_t now) const {
        return state_ == State::Running ? (deadline_ > now ? deadline_ - now : 0) : remaining_;
    }

    int seconds(std::uint64_t now) const {
        return static_cast<int>((remainingMs(now) + 999) / 1000);
    }

    bool update(std::uint64_t now) {
        if (state_ != State::Running || remainingMs(now) != 0) return false;
        remaining_ = 0;
        state_ = State::Finished;
        return true;
    }

    void toggle(std::uint64_t now) {
        update(now);
        if (state_ == State::Running) {
            remaining_ = remainingMs(now);
            state_ = State::Paused;
        } else {
            if (state_ == State::Finished) reset(preset_);
            deadline_ = now + remaining_;
            state_ = State::Running;
        }
    }

    void adjust(int deltaSeconds, std::uint64_t now) {
        update(now);
        auto value = static_cast<std::int64_t>(remainingMs(now)) + static_cast<std::int64_t>(deltaSeconds) * 1000;
        remaining_ = static_cast<std::uint64_t>(std::clamp<std::int64_t>(value, static_cast<std::int64_t>(minimum_) * 1000,
                                                                     static_cast<std::int64_t>(maximum_) * 1000));
        if (state_ == State::Running) deadline_ = now + remaining_;
        else {
            preset_ = seconds(now);
            if (state_ == State::Finished) state_ = State::Ready;
        }
    }

    State state() const { return state_; }
    int preset() const { return preset_; }

private:
    State state_ = State::Ready;
    int preset_ = 300;
    int minimum_ = 1, maximum_ = maxSeconds;
    std::uint64_t remaining_ = 300000;
    std::uint64_t deadline_ = 0;
};
} // namespace countdown
