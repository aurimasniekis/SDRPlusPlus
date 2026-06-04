#include "connection.h"

#include <utils/flog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace antsw {

using antenna_switcher::Channel;
using antenna_switcher::ChannelState;
using antenna_switcher::Options;
using antenna_switcher::PlanStep;
using antenna_switcher::TimeUnit;

namespace {
int channelNum(const Channel c) { return static_cast<int>(c); }
std::size_t channelIdx(const Channel c) { return c == Channel::One ? 0 : 1; }
}  // namespace

// --- Real device: wraps antenna_switcher::AntennaSwitcher -------------------
class RealConnection final : public IConnection {
public:
    RealConnection(Options opts, std::string tag)
        : client_(std::move(opts)), tag_(std::move(tag)) {
        flog::debug("[{0}] real client created", tag_);
    }

    void connect() override {
        flog::info("[{0}] connecting", tag_);
        client_.connect();
        flog::info("[{0}] connected", tag_);
    }

    void disconnect() override {
        flog::info("[{0}] disconnecting", tag_);
        client_.disconnect();
    }

    bool isConnected() const override { return client_.isConnected(); }

    void setInput(const Channel channel, const int input) override {
        flog::debug("[{0}] setInput ch={1} input={2}", tag_, channelNum(channel), input);
        client_.setInput(channel, input);
    }

    void startAuto(const Channel channel, const int interval, const TimeUnit unit,
                   const std::vector<int>& inputs) override {
        flog::debug("[{0}] startAuto ch={1} interval={2}", tag_, channelNum(channel), interval);
        client_.startAuto(channel, interval, unit, inputs);
    }

    void runPlan(const Channel channel, const std::vector<PlanStep>& steps,
                 const bool repeat) override {
        flog::debug("[{0}] runPlan ch={1} steps={2}", tag_, channelNum(channel),
                    (int)steps.size());
        client_.runPlan(channel, steps, repeat);
    }

    void stop(const Channel channel) override {
        flog::debug("[{0}] stop ch={1}", tag_, channelNum(channel));
        client_.stop(channel);
    }

    void setAngleOffset(const Channel channel, const int degrees) override {
        flog::debug("[{0}] setAngleOffset ch={1} deg={2}", tag_, channelNum(channel), degrees);
        client_.setAngleOffset(channel, degrees);
    }

    ChannelState state(const Channel channel) const override { return client_.state(channel); }

    void onStateChanged(StateCallback cb) override { client_.onStateChanged(std::move(cb)); }

private:
    antenna_switcher::AntennaSwitcher client_;
    std::string tag_;
};

// --- Mock device: no server, in-memory state + background simulation --------
class MockConnection final : public IConnection {
public:
    MockConnection(const Options& opts, std::string tag, int stateUpdateMs, int bearingUpdateMs)
        : tag_(std::move(tag)),
          stateUpdateMs_(stateUpdateMs > 0 ? stateUpdateMs : 1000),
          bearingUpdateMs_(bearingUpdateMs > 0 ? bearingUpdateMs : 100) {
        flog::debug("[{0}] mock client created (host={1} port={2} stateUpdate={3}ms "
                    "bearingUpdate={4}ms)",
                    tag_, opts.host, (int)opts.port, stateUpdateMs_, bearingUpdateMs_);
    }

    ~MockConnection() override { stopWorker(); }

    void connect() override {
        flog::info("[{0}] (mock) connecting", tag_);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            connected_ = true;
        }
        startWorker();
        flog::info("[{0}] (mock) connected", tag_);
    }

    void disconnect() override {
        flog::info("[{0}] (mock) disconnecting", tag_);
        stopWorker();
        std::lock_guard<std::mutex> lk(mtx_);
        connected_ = false;
    }

    bool isConnected() const override {
        std::lock_guard<std::mutex> lk(mtx_);
        return connected_;
    }

    void setInput(const Channel channel, const int input) override {
        flog::info("[{0}] (mock) setInput ch={1} input={2}", tag_, channelNum(channel), input);
        ChannelState snap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ChannelState& s = states_[channelIdx(channel)];
            s.activeInput = input;
            s.mode = antenna_switcher::Mode::Manual;
            snap = s;
        }
        emit(channel, snap);
    }

    void startAuto(const Channel channel, const int interval, const TimeUnit unit,
                   const std::vector<int>& inputs) override {
        flog::info("[{0}] (mock) startAuto ch={1} interval={2}", tag_, channelNum(channel),
                   interval);
        ChannelState snap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            const std::size_t i = channelIdx(channel);
            ChannelState& s = states_[i];
            s.mode = antenna_switcher::Mode::Auto;
            s.intervalUs = unit == TimeUnit::Us ? interval : (long)interval * 1000;
            s.activeInputs = inputs;
            cycleIndex_[i] = 0;
            s.activeInput = inputs.empty() ? 1 : inputs.front();
            snap = s;
        }
        emit(channel, snap);
    }

    void runPlan(const Channel channel, const std::vector<PlanStep>& steps,
                 const bool repeat) override {
        flog::info("[{0}] (mock) runPlan ch={1} steps={2} repeat={3}", tag_, channelNum(channel),
                   (int)steps.size(), repeat);
        ChannelState snap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            const std::size_t i = channelIdx(channel);
            planSteps_[i] = steps;
            planIndex_[i] = 0;
            planRepeat_[i] = repeat;
            states_[i].mode = antenna_switcher::Mode::Plan;
            snap = states_[i];
        }
        emit(channel, snap);
    }

    void stop(const Channel channel) override {
        flog::info("[{0}] (mock) stop ch={1}", tag_, channelNum(channel));
        ChannelState snap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            states_[channelIdx(channel)].mode = antenna_switcher::Mode::Manual;
            snap = states_[channelIdx(channel)];
        }
        emit(channel, snap);
    }

    void setAngleOffset(const Channel channel, const int degrees) override {
        flog::info("[{0}] (mock) setAngleOffset ch={1} deg={2}", tag_, channelNum(channel),
                   degrees);
        ChannelState snap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            states_[channelIdx(channel)].angleOffset = degrees;
            snap = states_[channelIdx(channel)];
        }
        emit(channel, snap);
    }

    ChannelState state(const Channel channel) const override {
        std::lock_guard<std::mutex> lk(mtx_);
        return states_[channelIdx(channel)];
    }

    void onStateChanged(StateCallback cb) override {
        std::lock_guard<std::mutex> lk(mtx_);
        cb_ = std::move(cb);
    }

private:
    // Fire the state callback (if any) outside the lock to avoid re-entrancy.
    void emit(const Channel channel, const ChannelState& s) {
        StateCallback cb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cb = cb_;
        }
        if (cb) { cb(channel, s); }
    }

    void startWorker() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (running_) { return; }
        running_ = true;
        worker_ = std::thread(&MockConnection::simLoop, this);
    }

    void stopWorker() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!running_) { return; }
            running_ = false;
            cv_.notify_all();
        }
        if (worker_.joinable()) { worker_.join(); }
    }

    static long elapsedMs(std::chrono::steady_clock::time_point a,
                          std::chrono::steady_clock::time_point b) {
        return (long)std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    }

    // Advance one channel's auto/plan simulation by a step. Caller holds mtx_.
    bool advanceSim(const int i) {
        ChannelState& s = states_[i];
        if (s.mode == antenna_switcher::Mode::Auto) {
            std::vector<int> cyc = s.activeInputs;
            if (cyc.empty()) {
                for (int k = 1; k <= 10; k++) { cyc.push_back(k); }
            }
            cycleIndex_[i] = (cycleIndex_[i] + 1) % (int)cyc.size();
            s.activeInput = cyc[cycleIndex_[i]];
            return true;
        }
        if (s.mode == antenna_switcher::Mode::Plan) {
            if (planSteps_[i].empty()) {
                s.mode = antenna_switcher::Mode::Manual;
                return true;
            }
            if (planIndex_[i] >= (int)planSteps_[i].size()) {
                if (planRepeat_[i]) {
                    planIndex_[i] = 0;
                }
                else {
                    s.mode = antenna_switcher::Mode::Manual;
                    return true;
                }
            }
            const PlanStep& step = planSteps_[i][planIndex_[i]];
            planIndex_[i]++;
            if (step.kind == PlanStep::Kind::Input) { s.activeInput = step.input; }
            return true;
        }
        return false;
    }

    void simLoop() {
        using clock = std::chrono::steady_clock;
        auto lastState = clock::now();
        auto lastBearing = clock::now();
        const int tickMs = std::max(10, std::min(stateUpdateMs_, bearingUpdateMs_));

        std::unique_lock<std::mutex> lk(mtx_);
        while (running_) {
            cv_.wait_for(lk, std::chrono::milliseconds(tickMs), [&] { return !running_; });
            if (!running_) { break; }

            const auto now = clock::now();
            bool changed[2] = {false, false};

            if (elapsedMs(lastBearing, now) >= bearingUpdateMs_) {
                lastBearing = now;
                for (int i = 0; i < 2; i++) {
                    states_[i].bearing = (states_[i].bearing + 1) % 360;
                    changed[i] = true;
                }
            }
            if (elapsedMs(lastState, now) >= stateUpdateMs_) {
                lastState = now;
                for (int i = 0; i < 2; i++) {
                    if (advanceSim(i)) { changed[i] = true; }
                }
            }

            if (changed[0] || changed[1]) {
                StateCallback cb = cb_;
                const ChannelState s0 = states_[0];
                const ChannelState s1 = states_[1];
                const bool c0 = changed[0];
                const bool c1 = changed[1];
                lk.unlock();
                if (cb) {
                    if (c0) { cb(Channel::One, s0); }
                    if (c1) { cb(Channel::Two, s1); }
                }
                lk.lock();
            }
        }
    }

    std::string tag_;
    const int stateUpdateMs_;
    const int bearingUpdateMs_;

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::thread worker_;
    bool running_ = false;
    bool connected_ = false;

    std::array<ChannelState, 2> states_{};
    int cycleIndex_[2] = {0, 0};
    std::vector<PlanStep> planSteps_[2];
    int planIndex_[2] = {0, 0};
    bool planRepeat_[2] = {false, false};
    StateCallback cb_;
};

std::unique_ptr<IConnection> makeConnection(const Options& opts, const bool mock, std::string tag,
                                            const int mockStateUpdateMs,
                                            const int mockBearingUpdateMs) {
    if (mock) {
        flog::warn("[{0}] using MOCK connection (no real device)", tag);
        return std::make_unique<MockConnection>(opts, std::move(tag), mockStateUpdateMs,
                                                mockBearingUpdateMs);
    }
    return std::make_unique<RealConnection>(opts, std::move(tag));
}

}  // namespace antsw
