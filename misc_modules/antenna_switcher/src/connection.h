#pragma once

#include <antenna_switcher/client.hpp>
#include <memory>
#include <string>
#include <vector>

namespace antsw {

// Thin adapter over the antenna-switcher client. Mirrors the parts of
// antenna_switcher::AntennaSwitcher the module uses, so a mock implementation
// can stand in for a real device when no server is available.
class IConnection {
public:
    using StateCallback = antenna_switcher::AntennaSwitcher::StateCallback;

    virtual ~IConnection() = default;

    virtual void connect() = 0;
    virtual void disconnect() = 0;
    [[nodiscard]] virtual bool isConnected() const = 0;

    // Actions mirror the client: each validates the request against the
    // channel's advertised capabilities on the calling thread — throwing
    // antenna_switcher::UnsupportedRequest for an unadvertised feature or an
    // out-of-range input, before anything reaches the wire — and returns the
    // command string it sent.
    virtual std::string setInput(antenna_switcher::Channel channel, int input) = 0;
    virtual std::string startAuto(antenna_switcher::Channel channel,
                                  int interval,
                                  antenna_switcher::TimeUnit unit,
                                  const std::vector<int>& inputs) = 0;
    virtual std::string runPlan(antenna_switcher::Channel channel,
                                const std::vector<antenna_switcher::PlanStep>& steps,
                                bool repeat) = 0;
    virtual std::string stop(antenna_switcher::Channel channel) = 0;
    // Isolate every RF port. Needs Feature::Off.
    virtual std::string off(antenna_switcher::Channel channel) = 0;
    virtual std::string setAngleOffset(antenna_switcher::Channel channel, int degrees) = 0;

    // The snapshot carries the channel's advertised Capabilities, so the module
    // reads those from `state(...).capabilities` rather than a second getter.
    [[nodiscard]] virtual antenna_switcher::ChannelState state(antenna_switcher::Channel channel) const = 0;
    virtual void onStateChanged(StateCallback cb) = 0;
};

// Build a connection. When `mock` is true a fake, server-less implementation is
// returned (useful for UI/testing); otherwise a real client wrapping
// antenna_switcher::AntennaSwitcher. `tag` prefixes log lines. The mock simulates
// auto/plan progression every `mockStateUpdateMs`, spins the bearing every
// `mockBearingUpdateMs` (both ignored by the real client) and reports
// `mockCaps` as the shape of both simulated switchers.
std::unique_ptr<IConnection> makeConnection(const antenna_switcher::Options& opts,
                                            bool mock,
                                            std::string tag,
                                            int mockStateUpdateMs = 1000,
                                            int mockBearingUpdateMs = 100,
                                            antenna_switcher::Capabilities mockCaps = {});

}  // namespace antsw
