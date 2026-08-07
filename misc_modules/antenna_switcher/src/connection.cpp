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

using antenna_switcher::Capabilities;
using antenna_switcher::Channel;
using antenna_switcher::ChannelState;
using antenna_switcher::Feature;
using antenna_switcher::InputState;
using antenna_switcher::Options;
using antenna_switcher::PlanStep;
using antenna_switcher::TimeUnit;
using antenna_switcher::UnsupportedRequest;

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

    std::string setInput(const Channel channel, const int input) override {
        return sent(channel, client_.setInput(channel, input));
    }

    std::string startAuto(const Channel channel, const int interval, const TimeUnit unit,
                          const std::vector<int>& inputs) override {
        return sent(channel, client_.startAuto(channel, interval, unit, inputs));
    }

    std::string runPlan(const Channel channel, const std::vector<PlanStep>& steps,
                        const bool repeat) override {
        return sent(channel, client_.runPlan(channel, steps, repeat));
    }

    std::string stop(const Channel channel) override {
        return sent(channel, client_.stop(channel));
    }

    std::string off(const Channel channel) override {
        return sent(channel, client_.off(channel));
    }

    std::string setAngleOffset(const Channel channel, const int degrees) override {
        return sent(channel, client_.setAngleOffset(channel, degrees));
    }

    ChannelState state(const Channel channel) const override { return client_.state(channel); }

    void onStateChanged(StateCallback cb) override { client_.onStateChanged(std::move(cb)); }

private:
    // Log the command string the client reports having sent, then hand it back
    // to the caller. Every action goes through here so the log shows the exact
    // wire grammar rather than a paraphrase of the arguments.
    std::string sent(const Channel channel, std::string cmd) const {
        flog::debug("[{0}] ch{1} -> {2}", tag_, channelNum(channel), cmd);
        return cmd;
    }

    antenna_switcher::AntennaSwitcher client_;
    std::string tag_;
};

// --- Mock device: no server, in-memory state + background simulation --------
class MockConnection final : public IConnection {
public:
    MockConnection(const Options& opts, std::string tag, int stateUpdateMs, int bearingUpdateMs,
                   const Capabilities& caps)
        : tag_(std::move(tag)),
          stateUpdateMs_(stateUpdateMs > 0 ? stateUpdateMs : 1000),
          bearingUpdateMs_(bearingUpdateMs > 0 ? bearingUpdateMs : 100) {
        // Both simulated switchers answer with the configured shape, as a board
        // that responded to `id` would.
        for (ChannelState& s : states_) {
            s.capabilities = caps;
            s.capabilities.reported = true;
        }
        flog::debug("[{0}] mock client created (host={1} port={2} stateUpdate={3}ms "
                    "bearingUpdate={4}ms inputs={5} circle={6} features={7})",
                    tag_, opts.host, (int)opts.port, stateUpdateMs_, bearingUpdateMs_,
                    caps.inputCount, caps.circleCount,
                    antenna_switcher::detail::format_feature_flags(caps.features));
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

    std::string setInput(const Channel channel, const int input) override {
        const Capabilities caps = capsOf(channel);
        requireInput(channel, caps, input);
        const std::string cmd = antenna_switcher::detail::build_set_input(input);
        flog::info("[{0}] (mock) ch{1} -> {2}", tag_, channelNum(channel), cmd);
        ChannelState snap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ChannelState& s = states_[channelIdx(channel)];
            s.activeInput = input;
            s.inputState = InputState::Selected;
            s.mode = antenna_switcher::Mode::Manual;
            snap = s;
        }
        emit(channel, snap);
        return cmd;
    }

    std::string startAuto(const Channel channel, const int interval, const TimeUnit unit,
                          const std::vector<int>& inputs) override {
        const Capabilities caps = capsOf(channel);
        requireFeature(channel, caps, Feature::Auto, "auto");
        for (const int in : inputs) { requireInput(channel, caps, in); }
        const std::string cmd =
            antenna_switcher::detail::build_start_auto(interval, unit, inputs, caps.inputCount);
        flog::info("[{0}] (mock) ch{1} -> {2}", tag_, channelNum(channel), cmd);
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
            s.inputState = InputState::Selected;
            snap = s;
        }
        emit(channel, snap);
        return cmd;
    }

    std::string runPlan(const Channel channel, const std::vector<PlanStep>& steps,
                        const bool repeat) override {
        const Capabilities caps = capsOf(channel);
        requireFeature(channel, caps, Feature::Plan, "plan");
        for (const PlanStep& step : steps) {
            if (step.kind == PlanStep::Kind::Input) { requireInput(channel, caps, step.input); }
        }
        const std::string cmd = antenna_switcher::detail::build_run_plan(steps, repeat);
        flog::info("[{0}] (mock) ch{1} -> {2}", tag_, channelNum(channel), cmd);
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
        return cmd;
    }

    std::string stop(const Channel channel) override {
        const std::string cmd = antenna_switcher::detail::build_stop();
        flog::info("[{0}] (mock) ch{1} -> {2}", tag_, channelNum(channel), cmd);
        ChannelState snap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            states_[channelIdx(channel)].mode = antenna_switcher::Mode::Manual;
            snap = states_[channelIdx(channel)];
        }
        emit(channel, snap);
        return cmd;
    }

    std::string off(const Channel channel) override {
        requireFeature(channel, capsOf(channel), Feature::Off, "off");
        const std::string cmd = antenna_switcher::detail::build_off();
        flog::info("[{0}] (mock) ch{1} -> {2}", tag_, channelNum(channel), cmd);
        ChannelState snap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ChannelState& s = states_[channelIdx(channel)];
            // Every RF port isolated: no input live, nothing cycling.
            s.mode = antenna_switcher::Mode::Manual;
            s.activeInput = 0;
            s.inputState = InputState::Isolated;
            s.activeInputs.clear();
            snap = s;
        }
        emit(channel, snap);
        return cmd;
    }

    std::string setAngleOffset(const Channel channel, const int degrees) override {
        const std::string cmd = "angle_offset=" + std::to_string(degrees);
        flog::info("[{0}] (mock) ch{1} -> {2}", tag_, channelNum(channel), cmd);
        ChannelState snap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            states_[channelIdx(channel)].angleOffset = degrees;
            snap = states_[channelIdx(channel)];
        }
        emit(channel, snap);
        return cmd;
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
    Capabilities capsOf(const Channel channel) const {
        std::lock_guard<std::mutex> lk(mtx_);
        return states_[channelIdx(channel)].capabilities;
    }

    // Mirror the client's local validation so the mock rejects the same
    // requests, on the same thread, with the same exception type.
    static void requireFeature(const Channel channel, const Capabilities& caps, const Feature f,
                               const char* verb) {
        if (!caps.has(f)) {
            throw UnsupportedRequest("antenna-switcher: channel " +
                                     std::to_string(channelNum(channel)) + " does not support '" +
                                     verb + "' (features " +
                                     antenna_switcher::detail::format_feature_flags(caps.features) +
                                     ")");
        }
    }

    static void requireInput(const Channel channel, const Capabilities& caps, const int input) {
        if (input < 1 || input > caps.inputCount) {
            throw UnsupportedRequest("antenna-switcher: channel " +
                                     std::to_string(channelNum(channel)) +
                                     ": input out of range (1.." +
                                     std::to_string(caps.inputCount) + "): " +
                                     std::to_string(input));
        }
    }

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
                // An empty selection cycles every input the board advertises.
                for (int k = 1; k <= s.capabilities.inputCount; k++) { cyc.push_back(k); }
            }
            if (cyc.empty()) { return false; }
            cycleIndex_[i] = (cycleIndex_[i] + 1) % (int)cyc.size();
            s.activeInput = cyc[cycleIndex_[i]];
            s.inputState = InputState::Selected;
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
            if (step.kind == PlanStep::Kind::Input) {
                s.activeInput = step.input;
                s.inputState = InputState::Selected;
            }
            return true;
        }
        return false;
    }

    void simLoop() {
        using clock = std::chrono::steady_clock;
        auto lastState = clock::now();
        auto lastBearing = clock::now();
        const int tickMs = (std::max)(10, (std::min)(stateUpdateMs_, bearingUpdateMs_));

        std::unique_lock<std::mutex> lk(mtx_);
        while (running_) {
            cv_.wait_for(lk, std::chrono::milliseconds(tickMs), [&] { return !running_; });
            if (!running_) { break; }

            const auto now = clock::now();
            bool changed[2] = {false, false};

            if (elapsedMs(lastBearing, now) >= bearingUpdateMs_) {
                lastBearing = now;
                for (int i = 0; i < 2; i++) {
                    // Only a board with a compass reports a bearing at all.
                    if (!states_[i].capabilities.has(Feature::Magnetometer)) { continue; }
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
                                            const int mockBearingUpdateMs,
                                            const Capabilities mockCaps) {
    if (mock) {
        flog::warn("[{0}] using MOCK connection (no real device)", tag);
        return std::make_unique<MockConnection>(opts, std::move(tag), mockStateUpdateMs,
                                                mockBearingUpdateMs, mockCaps);
    }
    return std::make_unique<RealConnection>(opts, std::move(tag));
}

}  // namespace antsw
