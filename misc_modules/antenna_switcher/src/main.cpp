#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <config.h>
#include <core.h>
#include <signal_path/signal_path.h>
#include <utils/flog.h>
#include <antenna_switcher/client.hpp>
#include "connection.h"
#include <string>
#include <vector>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <exception>

SDRPP_MOD_INFO{
    /* Name:            */ "antenna_switcher",
    /* Description:     */ "Antenna Switcher Module",
    /* Author:          */ "aurimasniekis",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

#define CONCAT(a, b) ((std::string(a) + b).c_str())

ConfigManager config;

class AntennaSwitcherModule : public ModuleManager::Instance {
public:
    enum class ConnState {
        Idle,        // not connected, no attempt in progress
        Connecting,  // attempting to connect
        Connected,   // connected and ready
        Failed,      // last attempt failed
        Retrying     // waiting before the next reconnect attempt
    };

    AntennaSwitcherModule(const std::string& name) {
        this->name = name;

        // Load defaults
        strcpy(host, "10.28.0.2");
        noisePsk[0] = '\0';
        strcpy(clientInfo, "antenna-switcher-client");

        // Load config (grouped by instance name)
        config.acquire();
        auto& cfg = config.conf[name];
        if (cfg.contains("host")) {
            std::string h = cfg["host"];
            strcpy(host, h.c_str());
        }
        if (cfg.contains("port")) {
            port = cfg["port"];
        }
        if (cfg.contains("noisePsk")) {
            std::string p = cfg["noisePsk"];
            strcpy(noisePsk, p.c_str());
        }
        if (cfg.contains("clientInfo")) {
            std::string ci = cfg["clientInfo"];
            strcpy(clientInfo, ci.c_str());
        }
        if (cfg.contains("autoConnect")) {
            autoConnect = cfg["autoConnect"];
        }
        if (cfg.contains("retryInterval")) {
            retryInterval = cfg["retryInterval"];
        }
        if (cfg.contains("channel1Enabled")) {
            channel1Enabled = cfg["channel1Enabled"];
        }
        if (cfg.contains("channel2Enabled")) {
            channel2Enabled = cfg["channel2Enabled"];
        }
        if (cfg.contains("mockClient")) {
            mockClient = cfg["mockClient"];
        }
        // Mock-only simulation timings (no UI, config-only).
        if (cfg.contains("mock_state_update")) {
            mockStateUpdate = cfg["mock_state_update"];
        }
        if (cfg.contains("mock_bearing_update")) {
            mockBearingUpdate = cfg["mock_bearing_update"];
        }
        loadChannelConfig(cfg);
        config.release();

        // Wire the loaded config into the client options.
        buildOptions();

        flog::info("[antenna_switcher:{0}] created (host={1} port={2} mock={3} autoConnect={4})",
                   name, options.host, (int)options.port, mockClient, autoConnect);

        // Spin up the connection worker.
        running = true;
        worker = std::thread(&AntennaSwitcherModule::workerLoop, this);

        if (autoConnect) {
            flog::info("[antenna_switcher:{0}] auto-connect enabled", name);
            requestConnect(true);
        }

        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~AntennaSwitcherModule() override {
        gui::menu.removeEntry(name);

        // Stop the worker thread.
        {
            std::lock_guard<std::mutex> lk(mtx);
            running = false;
            cv.notify_all();
        }
        if (worker.joinable()) { worker.join(); }
    }

    void postInit() override {}

    void enable() override {
        enabled = true;
    }

    void disable() override {
        enabled = false;
    }

    bool isEnabled() override {
        return enabled;
    }

private:
    struct ChannelUi;  // defined below; referenced by helpers above its definition

    static void menuHandler(void* ctx) {
        auto _this = static_cast<AntennaSwitcherModule*>(ctx);

        const float startX = ImGui::GetCursorPosX();
        const float fullWidth = ImGui::GetContentRegionAvail().x;

        // "Status:" label
        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();

        // Colored status text
        const ConnState state = _this->connState.load();
        ImVec4 color;
        char status[64];
        switch (state) {
        case ConnState::Connected:
            color = ImVec4(0.20f, 0.80f, 0.20f, 1.0f);  // green
            snprintf(status, sizeof(status), "Connected");
            break;
        case ConnState::Connecting:
            color = ImVec4(0.90f, 0.90f, 0.20f, 1.0f);  // yellow
            snprintf(status, sizeof(status), "Connecting");
            break;
        case ConnState::Failed:
            color = ImVec4(0.90f, 0.20f, 0.20f, 1.0f);  // red
            snprintf(status, sizeof(status), "Failed");
            break;
        case ConnState::Retrying:
            color = ImVec4(1.00f, 0.60f, 0.10f, 1.0f);  // orange
            snprintf(status, sizeof(status), "Retrying in %d s", _this->retryRemaining.load());
            break;
        case ConnState::Idle:
        default:
            color = ImVec4(1.00f, 1.00f, 1.00f, 1.0f);  // white (default)
            snprintf(status, sizeof(status), "Idle");
            break;
        }
        ImGui::TextColored(color, "%s", status);

        // While connected, show how long we've been connected.
        if (state == ConnState::Connected) {
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - _this->connectedSince)
                                  .count();
            ImGui::SameLine();
            if (secs < 60) {
                ImGui::TextDisabled("(%lld s)", (long long)secs);
            }
            else {
                ImGui::TextDisabled("(%lld min)", (long long)(secs / 60));
            }
        }

        // Right-aligned action button.
        const char* label;
        switch (state) {
        case ConnState::Connected:  label = "Disconnect"; break;
        case ConnState::Connecting:
        case ConnState::Retrying:   label = "Stop"; break;
        case ConnState::Failed:
        case ConnState::Idle:
        default:                    label = "Connect"; break;
        }
        char btnId[128];
        snprintf(btnId, sizeof(btnId), "%s##%s_conn_btn", label, _this->name.c_str());
        const float btnWidth = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(startX + fullWidth - btnWidth);
        if (ImGui::Button(btnId)) {
            _this->onActionButton();
        }

        // Tabs: Channel 1 / Channel 2 / Settings. Channel tabs are only present
        // when enabled in config, and disabled (greyed) until connected.
        const bool connected = state == ConnState::Connected;
        char tabBarId[128];
        snprintf(tabBarId, sizeof(tabBarId), "##%s_tabs", _this->name.c_str());
        // The tab bar underline uses the (Unfocused)TabActive color; force it to
        // the neutral separator gray so it reads as a plain divider. Per-tab
        // colors below are pushed later, so the active tab keeps its tint.
        const ImVec4 sepCol = ImGui::GetStyleColorVec4(ImGuiCol_Separator);
        ImGui::PushStyleColor(ImGuiCol_TabActive, sepCol);
        ImGui::PushStyleColor(ImGuiCol_TabUnfocusedActive, sepCol);
        if (ImGui::BeginTabBar(tabBarId)) {
            if (_this->channel1Enabled) {
                ImGui::BeginDisabled(!connected);
                pushTabColors(ImVec4(0.15f, 0.45f, 0.40f, 1.0f));  // teal
                const bool open = ImGui::BeginTabItem("Channel 1");
                popTabColors();
                if (open) {
                    _this->channelTab(antenna_switcher::Channel::One);
                    ImGui::EndTabItem();
                }
                ImGui::EndDisabled();
            }
            if (_this->channel2Enabled) {
                ImGui::BeginDisabled(!connected);
                pushTabColors(ImVec4(0.45f, 0.30f, 0.55f, 1.0f));  // purple
                const bool open = ImGui::BeginTabItem("Channel 2");
                popTabColors();
                if (open) {
                    _this->channelTab(antenna_switcher::Channel::Two);
                    ImGui::EndTabItem();
                }
                ImGui::EndDisabled();
            }
            // When not connected the channel tabs are disabled, so default the
            // selection to Settings instead of leaving a greyed channel tab open.
            const ImGuiTabItemFlags settingsFlags =
                connected ? ImGuiTabItemFlags_None : ImGuiTabItemFlags_SetSelected;
            pushTabColors(ImVec4(0.35f, 0.35f, 0.40f, 1.0f));  // grey
            const bool settingsOpen = ImGui::BeginTabItem("Settings", nullptr, settingsFlags);
            popTabColors();
            if (settingsOpen) {
                _this->settingsTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::PopStyleColor(2);  // TabActive / TabUnfocusedActive (underline)

        // Floating compass windows (rendered top-level so they persist across
        // tab switches / collapsed sections).
        if (_this->channel1Enabled) { _this->renderCompassWindow(antenna_switcher::Channel::One, 0); }
        if (_this->channel2Enabled) { _this->renderCompassWindow(antenna_switcher::Channel::Two, 1); }
    }

    // A button rendered in a custom color.
    static bool coloredButton(const char* label, const ImVec4& c,
                              const ImVec2& size = ImVec2(0, 0)) {
        ImGui::PushStyleColor(ImGuiCol_Button, c);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(c.x + 0.10f, c.y + 0.10f, c.z + 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(c.x - 0.05f, c.y - 0.05f, c.z - 0.05f, 1.0f));
        const bool r = ImGui::Button(label, size);
        ImGui::PopStyleColor(3);
        return r;
    }

    // Tint a tab so the three tabs are visually distinct.
    static void pushTabColors(const ImVec4& c) {
        ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(c.x * 0.45f, c.y * 0.45f, c.z * 0.45f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TabHovered,
                              ImVec4(c.x + 0.10f, c.y + 0.10f, c.z + 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TabActive, c);
        ImGui::PushStyleColor(ImGuiCol_TabUnfocused,
                              ImVec4(c.x * 0.40f, c.y * 0.40f, c.z * 0.40f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TabUnfocusedActive,
                              ImVec4(c.x * 0.80f, c.y * 0.80f, c.z * 0.80f, 1.0f));
    }
    static void popTabColors() { ImGui::PopStyleColor(5); }

    // A status row: dim label on the left, value right-aligned.
    static void statusRow(const char* label, const std::string& value) {
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine();
        const float avail = ImGui::GetContentRegionAvail().x;
        const float tw = ImGui::CalcTextSize(value.c_str()).x;
        if (tw < avail) { ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - tw)); }
        ImGui::TextUnformatted(value.c_str());
    }

    static void drawCenteredText(ImDrawList* dl, const char* txt, const ImVec2& center, ImU32 col,
                                 float fontSize) {
        ImFont* font = ImGui::GetFont();
        const float scale = fontSize / ImGui::GetFontSize();
        ImVec2 ts = ImGui::CalcTextSize(txt);
        ts.x *= scale;
        ts.y *= scale;
        dl->AddText(font, fontSize, ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), col, txt);
    }

    static bool autoContains(const ChannelUi& ui, int input) {
        return std::find(ui.autoOrder.begin(), ui.autoOrder.end(), input) != ui.autoOrder.end();
    }

    static void autoToggle(ChannelUi& ui, int input) {
        auto it = std::find(ui.autoOrder.begin(), ui.autoOrder.end(), input);
        if (it != ui.autoOrder.end()) { ui.autoOrder.erase(it); }
        else { ui.autoOrder.push_back(input); }
    }

    static bool activeCycleContains(const antenna_switcher::ChannelState& st, int input) {
        return std::find(st.activeInputs.begin(), st.activeInputs.end(), input) !=
               st.activeInputs.end();
    }

    // Fill color for an input chip, driven purely by device state: the selected
    // input is solid blue, members of the auto cycle are a translucent blue, and
    // everything else is idle grey.
    static ImVec4 inputStateColor(int input, const antenna_switcher::ChannelState& st) {
        if (st.activeInput == input) { return ImVec4(0.16f, 0.51f, 0.94f, 1.0f); }      // solid blue
        if (activeCycleContains(st, input)) { return ImVec4(0.16f, 0.51f, 0.94f, 0.28f); } // translucent
        return ImVec4(0.22f, 0.22f, 0.26f, 1.0f);                                       // grey
    }

    // ImDrawList has no dashed-circle primitive; stroke alternating arc segments.
    static void addDashedCircle(ImDrawList* dl, const ImVec2& c, float r, ImU32 col,
                                float thickness, int dashes = 16) {
        constexpr float TWO_PI = 6.28318531f;
        for (int d = 0; d < dashes; d++) {
            const float a0 = TWO_PI * (float)d / (float)dashes;
            dl->PathArcTo(c, r, a0, a0 + TWO_PI * 0.5f / (float)dashes, 6);  // half on, half off
            dl->PathStroke(col, false, thickness);
        }
    }

    // Handle a click on an input node: plain = SET, shift = toggle auto,
    // cmd/super or alt = append to plan.
    void handleInputClick(antenna_switcher::Channel channel, ChannelUi& ui, int ci, int input) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift) {
            autoToggle(ui, input);
            flog::debug("[antenna_switcher:{0}] ch{1} compass: toggle auto input {2}", name,
                        ci + 1, input);
        }
        else if (io.KeySuper || io.KeyAlt) {
            ui.planSteps.push_back(antenna_switcher::PlanStep::input_step(input));
            flog::debug("[antenna_switcher:{0}] ch{1} compass: add plan input {2}", name, ci + 1,
                        input);
        }
        else {
            flog::info("[antenna_switcher:{0}] ch{1} compass: setInput {2}", name, ci + 1, input);
            withConn([&](antsw::IConnection& c) { c.setInput(channel, input); });
        }
    }

    // Compass widget: 8 inputs around a ring, a bearing needle, and inputs 9/10
    // as buttons below.
    void compassSection(antenna_switcher::Channel channel, ChannelUi& ui, int ci,
                        const antenna_switcher::ChannelState& st, float heightBudget = -1.0f) {
        constexpr float PI = 3.14159265f;
        const float avail = ImGui::GetContentRegionAvail().x;
        // Scale with the available width, optionally clamped to a height budget so
        // the compass + chips fit without a scrollbar. No upper limit, and every
        // dimension is proportional so the ratio is maintained at any size.
        float size = avail;
        if (heightBudget > 0.0f) {
            // The chip row below is ~10% of the compass size; reserve it (plus one
            // item spacing) so the whole thing fits without a scrollbar.
            const float maxByHeight = (heightBudget - ImGui::GetStyle().ItemSpacing.y) / 1.10f;
            size = (std::min)(size, maxByHeight);
        }
        if (size < 60.0f) { size = 60.0f; }
        const float nodeR = (std::max)(8.0f, size * 0.047f);
        const float margin = size * 0.0625f;
        const float lineW = (std::max)(1.0f, size * 0.0047f);

        const ImVec2 cursor = ImGui::GetCursorPos();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float cx = origin.x + avail * 0.5f;
        const float cy = origin.y + size * 0.5f;
        const float ringR = size * 0.5f - nodeR - margin;
        const float labelR = ringR + nodeR + margin * 0.7f;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 ringCol = IM_COL32(120, 120, 130, 160);
        const ImU32 white = IM_COL32(235, 235, 235, 255);
        const ImU32 dim = IM_COL32(190, 190, 190, 255);

        // Ring
        dl->AddCircle(ImVec2(cx, cy), ringR, ringCol, 64, lineW);

        // Cardinal labels rotate with the bearing: input 1 (and the needle) stay
        // fixed at the top, so the label for direction (i*45)° sits at screen angle
        // (i*45 - bearing)° clockwise from the top. When input 1 points north the
        // "N" is at the top; when it points south the "S" is.
        static const char* CARD[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        for (int i = 0; i < 8; i++) {
            const float a = ((float)i * 45.0f - (float)st.bearing) * PI / 180.0f;
            drawCenteredText(dl, CARD[i],
                             ImVec2(cx + labelR * sinf(a), cy - labelR * cosf(a)), dim, size * 0.05f);
        }

        // Bearing needle (red): always points straight up to input 1.
        dl->AddLine(ImVec2(cx, cy), ImVec2(cx, cy - ringR), IM_COL32(230, 40, 40, 255), lineW * 1.4f);

        // Center bearing readout.
        char btxt[16];
        snprintf(btxt, sizeof(btxt), "%d\xC2\xB0", st.bearing);
        drawCenteredText(dl, btxt, ImVec2(cx, cy + size * 0.056f), white, size * 0.06f);

        // Input nodes 1-8 around the ring, fixed: input 1 at the top, 2..8 clockwise.
        const ImU32 blue = IM_COL32(40, 130, 240, 255);
        for (int i = 0; i < 8; i++) {
            const int input = i + 1;
            const float a = (float)i * 45.0f * PI / 180.0f;
            const ImVec2 p(cx + ringR * sinf(a), cy - ringR * cosf(a));
            char id[48];
            snprintf(id, sizeof(id), "##antsw_node%d_ch%d", input, ci);
            ImGui::SetCursorScreenPos(ImVec2(p.x - nodeR, p.y - nodeR));
            ImGui::InvisibleButton(id, ImVec2(nodeR * 2.0f, nodeR * 2.0f));
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) { handleInputClick(channel, ui, ci, input); }
            char num[8];
            snprintf(num, sizeof(num), "%d", input);
            if (st.activeInput == input) {
                // Currently selected input: solid blue.
                dl->AddCircleFilled(p, nodeR, hovered ? IM_COL32(70, 155, 255, 255) : blue);
                dl->AddCircle(p, nodeR, IM_COL32(255, 255, 255, 60), 0, lineW);
                drawCenteredText(dl, num, p, white, nodeR * 1.2f);
            }
            else if (activeCycleContains(st, input)) {
                // Auto-cycle member: translucent blue fill + dashed blue border.
                dl->AddCircleFilled(p, nodeR, IM_COL32(40, 130, 240, hovered ? 80 : 45));
                addDashedCircle(dl, p, nodeR, IM_COL32(70, 150, 250, 255), lineW * 1.3f);
                drawCenteredText(dl, num, p, white, nodeR * 1.2f);
            }
            else {
                // Idle.
                dl->AddCircleFilled(p, nodeR, IM_COL32(45, 45, 52, hovered ? 200 : 120));
                dl->AddCircle(p, nodeR, IM_COL32(120, 120, 130, 90), 0, lineW);
                drawCenteredText(dl, num, p, dim, nodeR * 1.2f);
            }
        }

        // Reserve the compass area in the layout.
        ImGui::SetCursorPos(cursor);
        ImGui::Dummy(ImVec2(avail, size));

        // Inputs 9 and 10 as centered pill chips below the compass, scaled to match.
        const float fScale = (std::max)(0.85f, size / 320.0f);
        ImGui::SetWindowFontScale(fScale);
        const float chipW = size * 0.15f;
        const float chipH = size * 0.10f;
        const float total = chipW * 2.0f + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(0.0f, (avail - total) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, chipH * 0.5f);
        for (int input = 9; input <= 10; input++) {
            const ImVec4 cv = inputStateColor(input, st);
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "%d##antsw_node%d_ch%d", input, input, ci);
            if (coloredButton(lbl, cv, ImVec2(chipW, chipH))) { handleInputClick(channel, ui, ci, input); }
            if (input == 9) { ImGui::SameLine(); }
        }
        ImGui::PopStyleVar();
        ImGui::SetWindowFontScale(1.0f);
    }

    // Render the compass in a floating window while popped out. Closing the
    // window docks it back into the section.
    void renderCompassWindow(antenna_switcher::Channel channel, int ci) {
        ChannelUi& ui = chUi[ci];
        if (!ui.compassFloating) { return; }

        char title[192];
        snprintf(title, sizeof(title), "%s Channel %d Compass##antsw_compass_win_%s_%d",
                 name.c_str(), ci + 1, name.c_str(), ci);
        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(360.0f, 430.0f), ImGuiCond_FirstUseEver);
        // Minimum size keeps the whole compass + chips visible (no scrollbar);
        // no upper bound; keep a square aspect so it scales proportionally.
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(240.0f, 280.0f), ImVec2(FLT_MAX, FLT_MAX),
            [](ImGuiSizeCallbackData* data) { data->DesiredSize.y = data->DesiredSize.x; });
        if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse)) {
            const antenna_switcher::ChannelState st = channelStateSnapshot(channel);
            compassSection(channel, ui, ci, st, ImGui::GetContentRegionAvail().y);
        }
        ImGui::End();
        if (!open) {
            ui.compassFloating = false;
            flog::debug("[antenna_switcher:{0}] ch{1} compass docked back", name, ci + 1);
        }
    }

    // Per-channel tab content: Status / Manual / Auto / Plan, mirroring the
    // device web UI.
    void channelTab(antenna_switcher::Channel channel) {
        using antenna_switcher::Mode;
        using antenna_switcher::PlanStep;
        using antenna_switcher::TimeUnit;

        static const char* INPUT_ITEMS =
            "1\0""2\0""3\0""4\0""5\0""6\0""7\0""8\0""9\0""10\0";
        static const char* UNIT_ITEMS = "ms\0us\0";

        const int ci = channel == antenna_switcher::Channel::One ? 0 : 1;
        ChannelUi& ui = chUi[ci];
        const std::string sfx = name + "_ch" + std::to_string(ci);
        const float w = ImGui::GetContentRegionAvail().x;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;  // matches SameLine() default
        const float framePad = ImGui::GetStyle().FramePadding.x;

        const ImVec4 colPreset(0.20f, 0.40f, 0.65f, 1.0f);  // blue (1-8)
        const ImVec4 colStart(0.20f, 0.55f, 0.25f, 1.0f);   // green (START / RUN)
        const ImVec4 colReset(0.65f, 0.40f, 0.15f, 1.0f);   // orange (RESET)
        const ImVec4 colStop(0.85f, 0.20f, 0.20f, 1.0f);    // red (STOP)

        const antenna_switcher::ChannelState st = channelStateSnapshot(channel);

        const float unitW = ImGui::CalcTextSize("ms").x + framePad * 2.0f + ImGui::GetFrameHeight();

        // --- Compass --- (with a pop-out button on the header)
        bool popClicked = false;
        const bool compassOpen = beginSection("Compass", "compass", ci, sfx, ui.compassOpen,
                                              ui.compassFloating ? nullptr : "[ ]", &popClicked);
        if (popClicked) {
            ui.compassFloating = true;
            flog::debug("[antenna_switcher:{0}] ch{1} compass popped out", name, ci + 1);
        }
        if (compassOpen) {
            if (ui.compassFloating) {
                ImGui::TextDisabled("Compass in floating window");
            }
            else {
                compassSection(channel, ui, ci, st);
            }
        }

        // --- Status ---
        if (beginSection("Status", "status", ci, sfx, ui.statusOpen)) {
            const char* modeStr = st.mode == Mode::Manual  ? "manual"
                                  : st.mode == Mode::Auto   ? "auto"
                                  : st.mode == Mode::Plan   ? "plan"
                                                            : "unknown";
            statusRow("Mode", modeStr);
            statusRow("Active Input", st.activeInput > 0 ? std::to_string(st.activeInput) : "-");
            statusRow("Bearing", std::to_string(st.bearing) + "\xC2\xB0");
            std::string intervalStr;
            if (st.intervalUs <= 0) { intervalStr = "-"; }
            else if (st.intervalUs % 1000 == 0) { intervalStr = std::to_string(st.intervalUs / 1000) + " ms"; }
            else { intervalStr = std::to_string(st.intervalUs) + " us"; }
            statusRow("Interval", intervalStr);
            std::string activeInputs;
            for (size_t i = 0; i < st.activeInputs.size(); i++) {
                if (i) { activeInputs += ", "; }
                activeInputs += std::to_string(st.activeInputs[i]);
            }
            statusRow("Active Inputs", activeInputs.empty() ? "-" : activeInputs);
        }

        // --- Manual ---
        if (beginSection("Manual", "manual", ci, sfx, ui.manualOpen)) {
            const float setW = ImGui::CalcTextSize("SET").x + framePad * 2.0f;
            ImGui::SetNextItemWidth(w - setW - spacing);
            ImGui::Combo(CONCAT("##antsw_manual_", sfx), &ui.manualSel, INPUT_ITEMS);
            ImGui::SameLine();
            if (ImGui::Button(CONCAT("SET##antsw_set_", sfx))) {
                const int input = ui.manualSel + 1;
                flog::info("[antenna_switcher:{0}] ch{1} setInput {2}", name, ci + 1, input);
                withConn([&](antsw::IConnection& c) { c.setInput(channel, input); });
            }
        }

        // --- Auto ---
        if (beginSection("Auto", "auto", ci, sfx, ui.autoOpen)) {
            ImGui::SetNextItemWidth(w - unitW - spacing);
            if (ImGui::InputInt(CONCAT("##antsw_autoiv_", sfx), &ui.autoInterval, 0, 0)) {
                if (ui.autoInterval < 0) { ui.autoInterval = 0; }
                saveChannelConfig(ci, "auto", "delay_value", ui.autoInterval);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(unitW);
            if (ImGui::Combo(CONCAT("##antsw_autounit_", sfx), &ui.autoUnit, UNIT_ITEMS)) {
                saveChannelConfig(ci, "auto", "delay_unit", std::string(unitToStr(ui.autoUnit)));
            }

            // Checkboxes; clicking adds/removes the input from the cycle. The
            // checkbox state mirrors membership in the click-ordered list.
            for (int i = 0; i < 10; i++) {
                if (i == 8) { ImGui::Separator(); }  // split 1-8 from 9-10, like the web UI
                const int input = i + 1;
                bool checked = autoContains(ui, input);
                char cbid[64];
                snprintf(cbid, sizeof(cbid), "%d##antsw_autocb%d_%s", input, i, sfx.c_str());
                if (ImGui::Checkbox(cbid, &checked)) { autoToggle(ui, input); }
                if ((i % 4) != 3 && i != 9) { ImGui::SameLine(); }
            }
            ImGui::Separator();

            // Click-order preview: green chips joined by arrows, wrapping as needed.
            if (!ui.autoOrder.empty()) {
                const float chipW = ImGui::CalcTextSize("00").x + framePad * 2.0f;
                const float rightX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                for (size_t k = 0; k < ui.autoOrder.size(); k++) {
                    char clbl[40];
                    snprintf(clbl, sizeof(clbl), "%d##antsw_autoord%zu_%s", ui.autoOrder[k], k,
                             sfx.c_str());
                    coloredButton(clbl, colStart, ImVec2(chipW, 0));
                    if (k + 1 < ui.autoOrder.size()) {
                        ImGui::SameLine(0.0f, 4.0f);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("->");
                        // Keep the next chip on this line only if it fits.
                        if (ImGui::GetItemRectMax().x + 4.0f + spacing + chipW < rightX) {
                            ImGui::SameLine(0.0f, 4.0f);
                        }
                    }
                }
                ImGui::Separator();
            }

            if (coloredButton(CONCAT("1-8##antsw_auto18_", sfx), colPreset)) {
                ui.autoOrder.clear();
                for (int i = 1; i <= 8; i++) { ui.autoOrder.push_back(i); }
            }
            ImGui::SameLine();
            if (coloredButton(CONCAT("START##antsw_autostart_", sfx), colStart)) {
                const std::vector<int> inputs = ui.autoOrder;
                const TimeUnit unit = ui.autoUnit == 1 ? TimeUnit::Us : TimeUnit::Ms;
                flog::info("[antenna_switcher:{0}] ch{1} startAuto interval={2} inputs={3}", name,
                           ci + 1, ui.autoInterval, (int)inputs.size());
                withConn([&](antsw::IConnection& c) {
                    c.startAuto(channel, ui.autoInterval, unit, inputs);
                });
                ui.autoOrder.clear();  // start resets the selection, like the web UI
            }
            ImGui::SameLine();
            if (coloredButton(CONCAT("RESET##antsw_autoreset_", sfx), colReset)) {
                ui.autoOrder.clear();
            }
        }

        // --- Plan ---
        if (beginSection("Plan", "plan", ci, sfx, ui.planOpen)) {
            const float addInW = ImGui::CalcTextSize("+ INPUT").x + framePad * 2.0f;
            ImGui::SetNextItemWidth(w - addInW - spacing);
            ImGui::Combo(CONCAT("##antsw_planinput_", sfx), &ui.planInputSel, INPUT_ITEMS);
            ImGui::SameLine();
            if (ImGui::Button(CONCAT("+ INPUT##antsw_planaddin_", sfx))) {
                ui.planSteps.push_back(PlanStep::input_step(ui.planInputSel + 1));
            }

            const float addDlW = ImGui::CalcTextSize("+ DELAY").x + framePad * 2.0f;
            ImGui::SetNextItemWidth(w - addDlW - unitW - spacing * 2.0f);
            if (ImGui::InputInt(CONCAT("##antsw_plandelay_", sfx), &ui.planDelay, 0, 0)) {
                if (ui.planDelay < 0) { ui.planDelay = 0; }
                saveChannelConfig(ci, "plan", "delay_value", ui.planDelay);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(unitW);
            if (ImGui::Combo(CONCAT("##antsw_planunit_", sfx), &ui.planUnit, UNIT_ITEMS)) {
                saveChannelConfig(ci, "plan", "delay_unit", std::string(unitToStr(ui.planUnit)));
            }
            ImGui::SameLine();
            if (ImGui::Button(CONCAT("+ DELAY##antsw_planadddl_", sfx))) {
                const TimeUnit unit = ui.planUnit == 1 ? TimeUnit::Us : TimeUnit::Ms;
                ui.planSteps.push_back(PlanStep::delay_step(ui.planDelay, unit));
            }

            // Plan step list with per-row remove.
            for (size_t i = 0; i < ui.planSteps.size(); i++) {
                const PlanStep& s = ui.planSteps[i];
                char rid[64];
                snprintf(rid, sizeof(rid), "x##antsw_planrm%zu_%s", i, sfx.c_str());
                if (ImGui::SmallButton(rid)) {
                    ui.planSteps.erase(ui.planSteps.begin() + i);
                    break;
                }
                ImGui::SameLine();
                if (s.kind == PlanStep::Kind::Input) {
                    ImGui::Text("%zu. Input %d", i + 1, s.input);
                }
                else {
                    ImGui::Text("%zu. Delay %d %s", i + 1, s.delay,
                                s.unit == TimeUnit::Us ? "us" : "ms");
                }
            }

            ImGui::Checkbox(CONCAT("Repeat##antsw_planrepeat_", sfx), &ui.planRepeat);
            if (coloredButton(CONCAT("RUN##antsw_planrun_", sfx), colStart)) {
                flog::info("[antenna_switcher:{0}] ch{1} runPlan steps={2} repeat={3}", name, ci + 1,
                           (int)ui.planSteps.size(), ui.planRepeat);
                std::vector<PlanStep> steps = ui.planSteps;
                const bool repeat = ui.planRepeat;
                withConn([&](antsw::IConnection& c) { c.runPlan(channel, steps, repeat); });
            }
            ImGui::SameLine();
            if (coloredButton(CONCAT("RESET##antsw_planreset_", sfx), colReset)) {
                ui.planSteps.clear();
            }
        }

        // --- Stop ---
        ImGui::Spacing();
        if (coloredButton(CONCAT("STOP##antsw_stop_", sfx), colStop, ImVec2(w, 0))) {
            flog::info("[antenna_switcher:{0}] ch{1} stop", name, ci + 1);
            withConn([&](antsw::IConnection& c) { c.stop(channel); });
        }
    }

    // Settings tab content: edit the config (grouped by instance name).
    void settingsTab() {
        const ConnState st = connState.load();
        // Connection-defining fields can't be changed mid-session; only editable
        // while idle or failed.
        const bool busy = st != ConnState::Idle && st != ConnState::Failed;

        if (busy) { style::beginDisabled(); }

        ImGui::LeftLabel("Host");
        ImGui::FillWidth();
        if (ImGui::InputText(CONCAT("##antsw_host_", name), host, sizeof(host))) {
            saveConfig("host", std::string(host));
            buildOptions();
        }

        ImGui::LeftLabel("Port");
        ImGui::FillWidth();
        if (ImGui::InputInt(CONCAT("##antsw_port_", name), &port, 0, 0)) {
            if (port < 1) { port = 1; }
            if (port > 65535) { port = 65535; }
            saveConfig("port", port);
            buildOptions();
        }

        ImGui::LeftLabel("Noise PSK");
        ImGui::FillWidth();
        if (ImGui::InputText(CONCAT("##antsw_psk_", name), noisePsk, sizeof(noisePsk),
                             ImGuiInputTextFlags_Password)) {
            saveConfig("noisePsk", std::string(noisePsk));
            buildOptions();
        }

        ImGui::LeftLabel("Client info");
        ImGui::FillWidth();
        if (ImGui::InputText(CONCAT("##antsw_cinfo_", name), clientInfo, sizeof(clientInfo))) {
            saveConfig("clientInfo", std::string(clientInfo));
            buildOptions();
        }

        if (ImGui::Checkbox(CONCAT("Mock client (no device)##antsw_mock_", name), &mockClient)) {
            saveConfig("mockClient", mockClient);
        }

        if (busy) { style::endDisabled(); }

        ImGui::Separator();

        if (ImGui::Checkbox(CONCAT("Auto connect##antsw_ac_", name), &autoConnect)) {
            saveConfig("autoConnect", autoConnect);
        }

        ImGui::LeftLabel("Retry interval (s)");
        ImGui::FillWidth();
        if (ImGui::InputInt(CONCAT("##antsw_retry_", name), &retryInterval, 0, 0)) {
            if (retryInterval < 0) { retryInterval = 0; }
            saveConfig("retryInterval", retryInterval);
        }

        ImGui::Separator();

        if (ImGui::Checkbox(CONCAT("Channel 1 enabled##antsw_ch1_", name), &channel1Enabled)) {
            saveConfig("channel1Enabled", channel1Enabled);
        }
        if (ImGui::Checkbox(CONCAT("Channel 2 enabled##antsw_ch2_", name), &channel2Enabled)) {
            saveConfig("channel2Enabled", channel2Enabled);
        }
    }

    // Handle the connect/disconnect/stop button: toggle the desired state and
    // let the worker thread reconcile.
    void onActionButton() {
        switch (connState.load()) {
        case ConnState::Connected:
        case ConnState::Connecting:
        case ConnState::Retrying:
            requestConnect(false);
            break;
        case ConnState::Failed:
        case ConnState::Idle:
        default:
            requestConnect(true);
            break;
        }
    }

    // Signal the worker thread whether we want to be connected.
    void requestConnect(bool want) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            wantConnected = want;
        }
        cv.notify_all();
    }

    // Background reconciliation loop: brings the actual connection in line with
    // `wantConnected`, handling connect / retry / disconnect with logging.
    void workerLoop() {
        std::unique_lock<std::mutex> lk(mtx);
        while (running) {
            if (wantConnected && !(conn && conn->isConnected())) {
                // Need to (re)connect.
                connState = ConnState::Connecting;
                lk.unlock();

                flog::info("[antenna_switcher:{0}] connecting to {1}:{2}", name, options.host,
                           (int)options.port);
                std::unique_ptr<antsw::IConnection> c;
                bool ok = false;
                try {
                    c = antsw::makeConnection(options, mockClient, "antenna_switcher:" + name,
                                              mockStateUpdate, mockBearingUpdate);
                    c->connect();
                    ok = c->isConnected();
                }
                catch (const std::exception& e) {
                    flog::error("[antenna_switcher:{0}] connection error: {1}", name, e.what());
                }

                lk.lock();
                if (!running) { break; }

                if (!wantConnected) {
                    // Cancelled mid-connect.
                    if (c && c->isConnected()) {
                        lk.unlock();
                        try { c->disconnect(); }
                        catch (...) {}
                        lk.lock();
                    }
                    connState = ConnState::Idle;
                    continue;
                }

                if (ok) {
                    conn = std::move(c);
                    connectedSince = std::chrono::steady_clock::now();
                    connState = ConnState::Connected;
                    flog::info("[antenna_switcher:{0}] connected", name);
                    continue;
                }

                // Failed: retry after the configured interval, or give up.
                if (retryInterval > 0) {
                    flog::warn("[antenna_switcher:{0}] connect failed, retrying in {1} s", name,
                               retryInterval);
                    for (int s = retryInterval; s > 0 && running && wantConnected; --s) {
                        retryRemaining = s;
                        connState = ConnState::Retrying;
                        cv.wait_for(lk, std::chrono::seconds(1),
                                    [&] { return !running || !wantConnected; });
                    }
                    continue;
                }
                connState = ConnState::Failed;
                wantConnected = false;
                flog::error("[antenna_switcher:{0}] connect failed", name);
                continue;
            }

            if (!wantConnected && conn) {
                // Need to disconnect.
                lk.unlock();
                flog::info("[antenna_switcher:{0}] disconnecting", name);
                try { conn->disconnect(); }
                catch (const std::exception& e) {
                    flog::error("[antenna_switcher:{0}] disconnect error: {1}", name, e.what());
                }
                lk.lock();
                conn.reset();
                connState = ConnState::Idle;
                flog::info("[antenna_switcher:{0}] disconnected", name);
                continue;
            }

            // Settled and not connected (e.g. retry cancelled): reflect idle,
            // but keep a sticky Failed state visible.
            if (!wantConnected && !conn && connState.load() != ConnState::Failed) {
                connState = ConnState::Idle;
            }

            // Nothing to do: wait for a request or shutdown.
            cv.wait(lk, [&] {
                return !running || (wantConnected && !conn) || (!wantConnected && conn);
            });
        }

        // Shutdown: make sure we leave the device disconnected.
        if (conn) {
            lk.unlock();
            try { conn->disconnect(); }
            catch (...) {}
            lk.lock();
            conn.reset();
        }
        flog::debug("[antenna_switcher:{0}] worker stopped", name);
    }

    // Run a command against the live connection (thread-safe; no-op if not
    // connected). Errors are logged, not thrown.
    template <typename F>
    void withConn(F&& fn) {
        std::lock_guard<std::mutex> lk(mtx);
        if (!conn) { return; }
        try {
            fn(*conn);
        }
        catch (const std::exception& e) {
            flog::error("[antenna_switcher:{0}] command error: {1}", name, e.what());
        }
    }

    // Thread-safe snapshot of a channel's state (empty if not connected).
    antenna_switcher::ChannelState channelStateSnapshot(antenna_switcher::Channel channel) {
        std::lock_guard<std::mutex> lk(mtx);
        if (conn) {
            try {
                return conn->state(channel);
            }
            catch (...) {}
        }
        return {};
    }

    // Populate the client options from the config-backed members.
    void buildOptions() {
        options.host = host;
        options.port = static_cast<std::uint16_t>(port);
        options.noise_psk = noisePsk;
        options.client_info = clientInfo;
    }

    // Persist a single config value under this instance's group.
    template <typename T>
    void saveConfig(const char* key, const T& value) {
        config.acquire();
        config.conf[name][key] = value;
        config.release(true);
    }

    // Persist a per-channel value: channels[ci].<section>.<key> = value.
    template <typename T>
    void saveChannelConfig(int ci, const char* section, const char* key, const T& value) {
        config.acquire();
        config.conf[name]["channels"][ci][section][key] = value;
        config.release(true);
    }

    static int unitFromStr(const std::string& s) { return s == "us" ? 1 : 0; }
    static const char* unitToStr(int u) { return u == 1 ? "us" : "ms"; }

    // Load persisted per-channel UI state from the instance's config group.
    void loadChannelConfig(json& cfg) {
        if (!cfg.contains("channels") || !cfg["channels"].is_array()) { return; }
        json& chs = cfg["channels"];
        for (int ci = 0; ci < 2; ci++) {
            if ((int)chs.size() <= ci || !chs[ci].is_object()) { continue; }
            json& ch = chs[ci];
            ChannelUi& ui = chUi[ci];

            auto sectionOpen = [&](const char* sec, bool& dst) {
                if (ch.contains(sec) && ch[sec].contains("enabled")) {
                    dst = ch[sec]["enabled"];
                }
            };
            sectionOpen("compass", ui.compassOpen);
            sectionOpen("status", ui.statusOpen);
            sectionOpen("manual", ui.manualOpen);
            sectionOpen("auto", ui.autoOpen);
            sectionOpen("plan", ui.planOpen);

            if (ch.contains("auto")) {
                json& a = ch["auto"];
                if (a.contains("delay_value")) { ui.autoInterval = a["delay_value"]; }
                if (a.contains("delay_unit")) { ui.autoUnit = unitFromStr(a["delay_unit"]); }
            }
            if (ch.contains("plan")) {
                json& p = ch["plan"];
                if (p.contains("delay_value")) { ui.planDelay = p["delay_value"]; }
                if (p.contains("delay_unit")) { ui.planUnit = unitFromStr(p["delay_unit"]); }
            }
        }
    }

    // Collapsible section header persisted as channels[ci].<cfgKey>.enabled.
    // Returns true when the section is expanded (content should be drawn).
    bool beginSection(const char* title, const char* cfgKey, int ci, const std::string& sfx,
                      bool& openState, const char* rightBtn = nullptr,
                      bool* rightBtnClicked = nullptr) {
        const float startX = ImGui::GetCursorPosX();
        const float availW = ImGui::GetContentRegionAvail().x;

        ImGui::SetNextItemOpen(openState, ImGuiCond_Once);
        char hid[160];
        snprintf(hid, sizeof(hid), "%s##antsw_sec_%s_%s", title, cfgKey, sfx.c_str());
        // Transparent header; only tint on hover/active.
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1.0f, 1.0f, 1.0f, 0.18f));
        const bool now = ImGui::CollapsingHeader(hid);
        ImGui::PopStyleColor(3);

        // Optional right-aligned button overlapping the header bar.
        if (rightBtn) {
            ImGui::SetItemAllowOverlap();
            char bid[80];
            snprintf(bid, sizeof(bid), "%s##antsw_secbtn_%s_%s", rightBtn, cfgKey, sfx.c_str());
            const float bw = ImGui::CalcTextSize(rightBtn).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SameLine();
            ImGui::SetCursorPosX(startX + availW - bw);
            const bool clicked = ImGui::SmallButton(bid);
            if (rightBtnClicked) { *rightBtnClicked = clicked; }
        }

        if (now != openState) {
            openState = now;
            saveChannelConfig(ci, cfgKey, "enabled", now);
        }
        return now;
    }

    // Editable UI state for one channel (Manual / Auto / Plan controls).
    struct ChannelUi {
        int manualSel = 0;                  // combo index, input = manualSel + 1
        int autoInterval = 100;
        int autoUnit = 0;                   // 0 = ms, 1 = us
        std::vector<int> autoOrder;  // auto-cycle inputs in the order they were clicked
        int planInputSel = 0;               // combo index, input = planInputSel + 1
        int planDelay = 100;
        int planUnit = 0;                   // 0 = ms, 1 = us
        bool planRepeat = false;
        std::vector<antenna_switcher::PlanStep> planSteps;

        bool compassFloating = false;  // compass shown in a floating window

        // Collapsible section states (persisted as channels[ci].<section>.enabled).
        bool compassOpen = true;
        bool statusOpen = true;
        bool manualOpen = true;
        bool autoOpen = true;
        bool planOpen = true;
    };

    std::string name;
    bool enabled = true;

    ChannelUi chUi[2];

    // Config-backed connection settings (grouped under the instance name).
    char host[1024];
    int port = 6053;
    char noisePsk[1024];
    char clientInfo[1024];

    // Module-level config (not part of the client options).
    bool autoConnect = false;
    int retryInterval = 5;  // seconds between reconnect attempts
    bool channel1Enabled = true;
    bool channel2Enabled = true;
    bool mockClient = true;  // use the server-less mock connection
    int mockStateUpdate = 1000;   // mock auto/plan step interval, ms (config-only)
    int mockBearingUpdate = 100;  // mock bearing spin interval, ms (config-only)

    // Client options assembled from the config above.
    antenna_switcher::Options options;

    // Connection worker + live state.
    std::unique_ptr<antsw::IConnection> conn;
    std::thread worker;
    std::mutex mtx;
    std::condition_variable cv;
    bool running = false;       // worker should keep running (guarded by mtx)
    bool wantConnected = false; // desired connection state (guarded by mtx)

    std::atomic<ConnState> connState{ConnState::Idle};
    std::atomic<int> retryRemaining{0};  // seconds left until the next retry
    std::chrono::steady_clock::time_point connectedSince;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/antenna_switcher_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new AntennaSwitcherModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete static_cast<AntennaSwitcherModule*>(instance);
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}