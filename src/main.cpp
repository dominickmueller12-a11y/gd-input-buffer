// Input Buffer
// ============
// Click a few ticks early and the click is moved to the tick where it works.
//
// How it works
// ------------
// Every jump press (and, in hold modes, release) that comes from the player is
// intercepted before the game sees it. The mod saves the level state with the
// game's own checkpoint system, then quietly runs the game's real physics a
// short way into the future with the input placed 0, 1, 2 ... N ticks later.
// It picks the smallest delay where you are still alive at the end of the
// look-ahead, rewinds, and plays your input for real on that tick. If you were
// too early even for the window, or the click was fine to begin with, the input
// goes through unchanged, exactly like vanilla.
//
// Because the real game physics are used for the preview (triggers, moving
// objects, orbs, portals, speed changes, dual mode, everything), there is no
// approximate "ghost" that can disagree with the real player.
//
// Every input that arrives while an earlier one is still waiting is shifted by
// the same amount, so taps stay taps and holds keep their length.

#include <Geode/Geode.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/ui/Notification.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr int kJumpButton = 1;
constexpr int kBaseTps = 240;

// ---------------------------------------------------------------------------
// Gamemodes
// ---------------------------------------------------------------------------

enum class Mode : int { Cube = 0, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };

constexpr const char* kModeSettingKey[] = {
    "cube-window", "ship-window", "ball-window", "ufo-window",
    "wave-window", "robot-window", "spider-window", "swing-window",
};
constexpr const char* kModeName[] = {
    "cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing",
};

Mode modeOf(PlayerObject* p) {
    if (!p) return Mode::Cube;
    if (p->m_isShip) return Mode::Ship;
    if (p->m_isBall) return Mode::Ball;
    if (p->m_isBird) return Mode::Ufo;
    if (p->m_isDart) return Mode::Wave;
    if (p->m_isRobot) return Mode::Robot;
    if (p->m_isSpider) return Mode::Spider;
    if (p->m_isSwing) return Mode::Swing;
    return Mode::Cube;
}

// In these modes the button is held, so a press is simulated as "held from now
// on". In the others a press is a tap (released on the next tick).
bool isHoldMode(Mode m) {
    return m == Mode::Ship || m == Mode::Wave || m == Mode::Swing || m == Mode::Robot;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

constexpr const char* kMainPresets[] = {
    "Off",
    "2 ticks (0.5 frame)",
    "4 ticks (1 frame)",
    "8 ticks (2 frames)",
    "12 ticks (3 frames)",
    "16 ticks (4 frames)",
    "24 ticks (6 frames)",
    "32 ticks (8 frames)",
};
constexpr int kMainPresetCount = sizeof(kMainPresets) / sizeof(kMainPresets[0]);

constexpr const char* kWaveModes[] = { "Off", "Press Only", "Grid (Press + Release)" };
constexpr int kWaveModeCount = 3;
enum class WaveMode : int { Off = 0, PressOnly, Grid };

template <class T>
T setting(const char* key) {
    return Mod::get()->getSettingValue<T>(key);
}

bool debugLogging() { return setting<bool>("debug-logging"); }

// Main window in 240-TPS ticks. 0 = whole mod off.
int mainWindowBaseTicks() {
    auto s = setting<std::string>("main-window");
    if (s == "Off") return 0;
    return std::max(0, std::atoi(s.c_str()));
}

int mainPresetIndex() {
    auto s = setting<std::string>("main-window");
    for (int i = 0; i < kMainPresetCount; ++i) {
        if (s == kMainPresets[i]) return i;
    }
    return 0;
}

WaveMode waveMode() {
    auto s = setting<std::string>("wave-mode");
    for (int i = 0; i < kWaveModeCount; ++i) {
        if (s == kWaveModes[i]) return static_cast<WaveMode>(i);
    }
    return WaveMode::Grid;
}

int tps() {
    auto v = static_cast<int>(setting<int64_t>("tps"));
    return std::clamp(v, kBaseTps, 1440);
}

// Windows and the look-ahead are written for 240 TPS. At a different tick rate
// they are scaled so they still cover the same amount of real time.
int scaleTicks(int baseTicks) {
    if (baseTicks <= 0) return 0;
    int t = tps();
    if (t == kBaseTps) return baseTicks;
    return std::max(1, static_cast<int>(std::lround(baseTicks * (double(t) / kBaseTps))));
}

// Window for a gamemode in 240-TPS ticks. 0 = no buffering.
int windowBaseTicks(Mode m) {
    int main = mainWindowBaseTicks();
    if (main <= 0) return 0; // master switch
    auto v = static_cast<int>(setting<int64_t>(kModeSettingKey[static_cast<int>(m)]));
    if (v < 0) return main;
    return v;
}

// Is this kind of input (press / release) buffered in this mode right now?
bool inputIsBuffered(Mode m, bool down) {
    if (windowBaseTicks(m) <= 0) return false;
    if (m == Mode::Wave) {
        switch (waveMode()) {
            case WaveMode::Off: return false;
            case WaveMode::PressOnly: return down;
            case WaveMode::Grid: return true;
        }
    }
    if (down) return true;
    // releases: only in hold modes, and only if enabled
    return isHoldMode(m) && setting<bool>("buffer-releases");
}

std::string statusText() {
    int main = mainWindowBaseTicks();
    if (main <= 0) return "Buffer: OFF";
    std::string s = fmt::format("Buffer: {}t", main);
    s += fmt::format(" | Wave: {}", kWaveModes[static_cast<int>(waveMode())]);
    if (tps() != kBaseTps) s += fmt::format(" | {} TPS", tps());
    return s;
}

void notify(std::string const& text) {
    Notification::create(text, NotificationIcon::Info, 1.2f)->show();
}

// ---------------------------------------------------------------------------
// Simulation flags shared with the side-effect hooks below
// ---------------------------------------------------------------------------

bool g_simulating = false;   // a look-ahead run is in progress
bool g_busy = false;         // a decision (checkpoint / rewinds) is in progress
bool g_simDied = false;      // the player died inside the look-ahead
bool g_simCompleted = false; // the level was completed inside the look-ahead
double g_stallSeconds = 0.0; // time spent deciding; removed from the next frame's delta
                             // so the game pauses for it instead of catching up

struct StepArgs {
    float dt;
    bool half;
    bool last;
};

struct RawInput {
    bool down;
    bool isPlayer1;
};

struct Queued {
    int ticksLeft;
    bool down;
    bool isPlayer1;
};

struct Slot {
    std::deque<Queued> queue; // inputs waiting to be played, in order
    int shift = 0;            // delay applied to the current run of inputs
};

} // namespace

// ---------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------

class $modify(IBLayer, GJBaseGameLayer) {
    struct Fields {
        Slot slot[2];
        std::vector<RawInput> pendingRaw; // inputs that arrived inside a physics step
        bool insideStep = false;
        std::vector<StepArgs> framePattern; // how the game stepped last frame
        std::vector<StepArgs> currentFrame;
        size_t stepInFrame = 0;
        double tpsAccum = 0.0;      // real time not yet turned into ticks
        float tpsRealDelta = 0.f;   // this frame's delta for the visual update
        bool tpsHide = false;       // true while running extra inner updates
        CCLabelBMFont* label = nullptr;
    };

    void setLabel(CCLabelBMFont* label) { m_fields->label = label; }

    static void onModify(auto& self) {
        // Run AFTER every other mod's hook. Other mods then see the player's
        // real input exactly once, and the inputs / ticks we inject for the
        // preview go straight to the game without passing through their hooks
        // (no inflated CPS counters, no "speedhack" false positives).
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::LastPre);
        (void)self.setHookPriority("GJBaseGameLayer::processCommands", Priority::LastPre);
    }

    // ---- helpers ----------------------------------------------------------

    PlayLayer* asPlayLayer() {
        auto pl = PlayLayer::get();
        return (pl && static_cast<GJBaseGameLayer*>(pl) == this) ? pl : nullptr;
    }

    bool isTwoPlayer() {
        return m_levelSettings && m_levelSettings->m_twoPlayerMode;
    }

    int slotFor(bool isPlayer1) {
        return (isTwoPlayer() && !isPlayer1) ? 1 : 0;
    }

    PlayerObject* playerFor(int slot) {
        return (isTwoPlayer() && slot == 1) ? m_player2 : m_player1;
    }

    StepArgs simArgs(size_t k) {
        auto& pat = m_fields->framePattern;
        if (pat.empty()) return { 1.f / kBaseTps, false, true };
        return pat[(m_fields->stepInFrame + k) % pat.size()];
    }

    // One vanilla physics step, bypassing our own processCommands logic.
    // Simulated steps are never "the last tick of a frame", which skips the
    // once-per-frame visual work.
    void rawStep(StepArgs const& a) {
        GJBaseGameLayer::processCommands(a.dt, a.half, false);
    }

    void inject(bool down, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, kJumpButton, isPlayer1);
    }

    // ---- look-ahead -------------------------------------------------------

    // Place the input `delay` steps from now and run `horizon` steps.
    // True if the player is still alive afterwards.
    bool survives(int delay, bool down, bool isPlayer1, bool tap, int horizon, int& stepsUsed) {
        g_simDied = false;
        g_simCompleted = false;
        g_simulating = true;
        for (int k = 0; k < horizon; ++k) {
            if (k == delay) inject(down, isPlayer1);
            if (tap && k == delay + 1) inject(false, isPlayer1);
            rawStep(simArgs(k));
            ++stepsUsed;
            if (g_simDied || g_simCompleted) break;
        }
        g_simulating = false;
        return !g_simDied;
    }

    // Decide how many steps to hold this input back. 0 = play it now.
    int chooseDelay(bool down, bool isPlayer1, Mode mode) {
        auto pl = asPlayLayer();
        if (!pl || m_playerDied) return 0;

        int window = scaleTicks(windowBaseTicks(mode));
        if (window <= 0) return 0;
        int horizon = scaleTicks(static_cast<int>(setting<int64_t>("look-ahead")));
        int budget = static_cast<int>(setting<int64_t>("sim-budget"));
        bool tap = down && !isHoldMode(mode);

        auto t0 = std::chrono::steady_clock::now();

        // Anything the game does with buttons while we save / rewind must not
        // come back into the buffer.
        struct BusyGuard { BusyGuard() { g_busy = true; } ~BusyGuard() { g_busy = false; } } busy;

        auto checkpoint = pl->createCheckpoint();
        if (!checkpoint) return 0;
        checkpoint->retain();
        auto tCheckpoint = std::chrono::steady_clock::now();
        double rewindUs = 0.0;

        auto savedQueue = m_queuedButtons;
        bool savedDied = m_playerDied;

        int sims = 0;
        int steps = 0;

        auto rewind = [&]() {
            auto r0 = std::chrono::steady_clock::now();
            pl->loadFromCheckpoint(checkpoint);
            m_queuedButtons = savedQueue;
            m_playerDied = savedDied;
            rewindUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - r0).count();
        };

        // Runs one candidate. Returns false (dead) when out of budget so the
        // search gives up instead of freezing the game.
        auto alive = [&](int d) -> bool {
            if (steps + horizon + d > budget) return false;
            if (sims > 0) rewind();
            ++sims;
            return survives(d, down, isPlayer1, tap, horizon + d, steps);
        };

        int chosen = 0;
        bool found = false;

        if (alive(0)) {
            // The input is fine as it is.
            found = true;
            chosen = 0;
        }
        else if (alive(window)) {
            // Too early, but the end of the window survives: find the first
            // surviving tick. (hi always survives, so the answer is hi.)
            int lo = 1, hi = window;
            while (lo < hi) {
                int mid = lo + (hi - lo) / 2;
                if (alive(mid)) hi = mid;
                else lo = mid + 1;
            }
            found = true;
            chosen = hi;
        }
        else {
            // Neither end survives (for example the window is so long that the
            // end of it is already past the gap). Coarse scan, then refine
            // downward to the first surviving tick of that run.
            int stride = std::max(1, window / 8);
            for (int d = stride; d < window; d += stride) {
                if (alive(d)) {
                    int best = d;
                    for (int e = d - 1; e >= 1 && e > d - stride; --e) {
                        if (alive(e)) best = e;
                        else break;
                    }
                    found = true;
                    chosen = best;
                    break;
                }
                if (steps + horizon + d + stride > budget) break;
            }
        }

        rewind();
        checkpoint->release();

        auto t1 = std::chrono::steady_clock::now();
        double totalSec = std::chrono::duration<double>(t1 - t0).count();
        g_stallSeconds += totalSec;

        if (debugLogging()) {
            auto us = static_cast<long long>(totalSec * 1e6);
            auto cpUs = std::chrono::duration_cast<std::chrono::microseconds>(tCheckpoint - t0).count();
            log::info("[InputBuffer] {} {} | window {}t | delay {} ({}) | {} sims, {} ticks | {} us total (checkpoint {} us, rewinds {} us, ~{} us/tick)",
                kModeName[static_cast<int>(mode)], down ? "press" : "release", window,
                found ? chosen : 0, found ? (chosen == 0 ? "already safe" : "shifted") : "nothing survives",
                sims, steps, us, cpUs, static_cast<long long>(rewindUs),
                steps > 0 ? static_cast<long long>((us - cpUs - rewindUs) / steps) : 0);
        }

        return found ? chosen : 0;
    }

    // ---- input queue ------------------------------------------------------

    void enqueue(Slot& s, bool down, bool isPlayer1, int ticks) {
        if (!s.queue.empty()) {
            auto& back = s.queue.back();
            // keep order, and never put a release on the same tick as its press
            ticks = std::max(ticks, back.ticksLeft);
            if (back.down && !down && ticks == back.ticksLeft) ticks = back.ticksLeft + 1;
        }
        s.queue.push_back({ ticks, down, isPlayer1 });
    }

    // A real input from the player, handled outside a physics step.
    void processRaw(bool down, bool isPlayer1) {
        int idx = slotFor(isPlayer1);
        auto& s = m_fields->slot[idx];

        // Something earlier is still waiting: shift this one by the same amount.
        if (!s.queue.empty()) {
            enqueue(s, down, isPlayer1, s.shift);
            return;
        }

        auto player = playerFor(idx);
        Mode mode = modeOf(player);
        bool eligible = player && !player->m_isDashing && inputIsBuffered(mode, down);
        // A press while standing on the ground in a landing mode always
        // registers right away, so there is nothing to buffer: skip the
        // simulation entirely (this is most cube clicks).
        if (eligible && down && !isHoldMode(mode) && mode != Mode::Ufo
            && (player->m_isOnGround || player->m_isOnGround2)) {
            eligible = false;
        }
        if (eligible && mode == Mode::Robot && down && (player->m_isOnGround || player->m_isOnGround2)) {
            eligible = false;
        }
        auto pl = asPlayLayer();
        if (!eligible || !pl || pl->m_isPaused || m_playerDied) {
            inject(down, isPlayer1);
            return;
        }

        int d = chooseDelay(down, isPlayer1, mode);
        if (d <= 0) {
            inject(down, isPlayer1);
            return;
        }
        s.shift = d;
        enqueue(s, down, isPlayer1, d);
    }

    void fireDue(Slot& s) {
        while (!s.queue.empty() && s.queue.front().ticksLeft <= 0) {
            auto q = s.queue.front();
            s.queue.pop_front();
            inject(q.down, q.isPlayer1);
        }
        if (s.queue.empty()) s.shift = 0;
    }

    void clearBuffers() {
        g_stallSeconds = 0.0;
        auto& f = m_fields;
        for (auto& s : f->slot) {
            s.queue.clear();
            s.shift = 0;
        }
        f->pendingRaw.clear();
    }

    // ---- status label -----------------------------------------------------

    void refreshLabel() {
        auto& f = m_fields;
        if (!f->label) return;
        bool show = setting<bool>("show-label");
        f->label->setVisible(show);
        if (show) f->label->setString(statusText().c_str());
    }

    // ---- hooks ------------------------------------------------------------

    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_simulating || g_busy || button != kJumpButton || !asPlayLayer()) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        if (mainWindowBaseTicks() <= 0 && m_fields->slot[0].queue.empty() && m_fields->slot[1].queue.empty()) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        if (m_fields->insideStep) {
            // Can't rewind in the middle of a step: decide right after it ends.
            m_fields->pendingRaw.push_back({ down, isPlayer1 });
            return;
        }
        processRaw(down, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g_simulating || g_busy || !asPlayLayer()) {
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }

        auto& f = m_fields;

        // Remember how the game steps so simulations step the same way.
        f->currentFrame.push_back({ dt, isHalfTick, isLastTick });
        if (isLastTick) {
            f->framePattern = f->currentFrame;
            f->currentFrame.clear();
        }

        for (auto& s : f->slot) fireDue(s);

        f->insideStep = true;
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        f->insideStep = false;

        f->stepInFrame = isLastTick ? 0 : f->stepInFrame + 1;

        for (auto& s : f->slot) {
            for (auto& q : s.queue) --q.ticksLeft;
        }

        if (!f->pendingRaw.empty()) {
            auto pending = std::move(f->pendingRaw);
            f->pendingRaw.clear();
            for (auto& r : pending) processRaw(r.down, r.isPlayer1);
        }
    }

    // ---- Game Speed (TPS) -------------------------------------------------
    // Same approach as the well-known "physics bypass" hacks: the game's delta
    // quantiser is re-implemented with our tick length instead of 1/240, and
    // update() is called several times per frame with that tick length. The
    // once-per-frame work (postUpdate: camera, visibility, labels) only runs on
    // the last call. At 240 TPS nothing is touched.

    double customDelta(float dt, double tpsValue, bool applyExtra) {
        double spt = 1.0 / tpsValue;
        if (applyExtra && m_resumeTimer > 0) {
            --m_resumeTimer;
            dt = 0.f;
        }
        double total = dt + m_extraDelta;
        double timestep = std::min(m_gameState.m_timeWarp, 1.f) * spt;
        double steps = std::round(total / timestep);
        double newDelta = steps * timestep;
        if (applyExtra) m_extraDelta = total - newDelta;
        return newDelta;
    }

    double getModifiedDelta(float dt) {
        int t = tps();
        if (t == kBaseTps || !asPlayLayer()) return GJBaseGameLayer::getModifiedDelta(dt);
        return customDelta(dt, t, true);
    }

    void update(float dt) {
        // Time spent deciding a click was a frame stall. Take it out of this
        // frame's delta so the game pauses for it instead of catching up with
        // a visible jump.
        if (g_stallSeconds > 0.0 && asPlayLayer() && !g_simulating) {
            float comp = static_cast<float>(std::min<double>(g_stallSeconds, dt));
            g_stallSeconds = std::max(0.0, g_stallSeconds - comp);
            dt = std::max(0.f, dt - comp);
        }
        int t = tps();
        if (g_simulating || t == kBaseTps || !asPlayLayer()) {
            GJBaseGameLayer::update(dt);
            return;
        }
        auto& f = m_fields;
        f->tpsAccum += dt;
        f->tpsRealDelta = static_cast<float>(customDelta(dt, kBaseTps, false));

        double tick = 1.0 / t;
        if (f->tpsAccum < tick) return;

        size_t steps = static_cast<size_t>(f->tpsAccum / tick);
        f->tpsAccum -= steps * tick;

        auto start = std::chrono::steady_clock::now();
        auto frameBudget = std::chrono::duration<double>(dt);

        f->tpsHide = true;
        while (steps > 1) {
            GJBaseGameLayer::update(static_cast<float>(tick));
            --steps;
            // If updating takes longer than a frame, stop and lump the rest
            // into the final call so the game can't spiral.
            if (std::chrono::steady_clock::now() - start > frameBudget) break;
        }
        f->tpsHide = false;
        GJBaseGameLayer::update(static_cast<float>(tick * steps));
    }

    bool tpsHiding() { return m_fields->tpsHide; }
    float realFrameDelta() { return m_fields->tpsRealDelta; }

    // 2.2 levels measure progress in 240-TPS ticks. At another tick rate the
    // counters run too fast, so they are rebuilt from the level time around the
    // calls that read them.
    unsigned int ticksAt240() {
        return static_cast<unsigned int>(std::lround(m_gameState.m_levelTime * kBaseTps));
    }
};

// ---------------------------------------------------------------------------
// PlayLayer: deaths, completion, resets, safe mode, status label
// ---------------------------------------------------------------------------

class $modify(IBPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        // Simulated deaths must be caught before any other mod sees them.
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::FirstPre);
        (void)self.setHookPriority("PlayLayer::levelComplete", Priority::FirstPre);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", Priority::FirstPre);
    }

    IBLayer* engine() {
        return static_cast<IBLayer*>(static_cast<GJBaseGameLayer*>(this));
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        if (mainWindowBaseTicks() > 0 && setting<bool>("safe-mode")) {
            m_isTestMode = true;
        }

        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setAnchorPoint({ 0.f, 0.f });
        label->setPosition({ 4.f, 4.f });
        label->setScale(0.3f);
        label->setOpacity(140);
        label->setZOrder(1000);
        label->setID("status-label"_spr);
        this->addChild(label);
        engine()->setLabel(label);
        engine()->refreshLabel();
        return true;
    }

    bool tpsActive() { return tps() != kBaseTps; }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_simulating) {
            // The hidden anti-cheat spike is not a real death.
            if (object != m_anticheatSpike) g_simDied = true;
            return;
        }
        engine()->clearBuffers();
        if (!tpsActive()) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }
        auto saved = m_gameState.m_currentProgress;
        if (m_level && m_level->m_timestamp > 0) m_gameState.m_currentProgress = engine()->ticksAt240();
        PlayLayer::destroyPlayer(player, object);
        m_gameState.m_currentProgress = saved;
    }

    void levelComplete() {
        if (g_simulating) {
            g_simCompleted = true;
            return;
        }
        if (!tpsActive()) {
            PlayLayer::levelComplete();
            return;
        }
        // levelComplete stores the tick count as the completion timestamp.
        auto saved = m_gameState.m_commandIndex;
        m_gameState.m_commandIndex = engine()->ticksAt240();
        PlayLayer::levelComplete();
        m_gameState.m_commandIndex = saved;
    }

    void updateProgressbar() {
        if (!tpsActive()) {
            PlayLayer::updateProgressbar();
            return;
        }
        auto saved = m_gameState.m_currentProgress;
        if (m_level && m_level->m_timestamp > 0) m_gameState.m_currentProgress = engine()->ticksAt240();
        PlayLayer::updateProgressbar();
        m_gameState.m_currentProgress = saved;
    }

    // Camera, visibility and labels: once per rendered frame, not once per tick.
    void postUpdate(float dt) {
        if (!tpsActive()) {
            PlayLayer::postUpdate(dt);
            return;
        }
        if (engine()->tpsHiding()) return;
        PlayLayer::postUpdate(engine()->realFrameDelta());
    }

    void playEndAnimationToPos(CCPoint position) {
        if (g_simulating) return;
        PlayLayer::playEndAnimationToPos(position);
    }

    void resetLevel() {
        engine()->clearBuffers();
        PlayLayer::resetLevel();
    }
};

// ---------------------------------------------------------------------------
// Side effects that must not leak out of a simulated step
// ---------------------------------------------------------------------------

class $modify(IBPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::incrementJumps", Priority::FirstPre);
        (void)self.setHookPriority("PlayerObject::playSpawnEffect", Priority::FirstPre);
    }

    void incrementJumps() {
        if (g_simulating) return; // don't count simulated jumps in stats
        PlayerObject::incrementJumps();
    }

    void playSpawnEffect() {
        if (g_simulating) return;
        PlayerObject::playSpawnEffect();
    }
};

class $modify(IBHardStreak, HardStreak) {
    void addPoint(CCPoint point) {
        if (g_simulating) return; // no duplicate wave trail from the preview
        HardStreak::addPoint(point);
    }
};

// Saving / rewinding a checkpoint re-seeks the music; FMOD seeks can block for
// tens of milliseconds, and they are pointless because we restore the exact
// same state. Skip all music work while a decision is in progress.
class $modify(IBAudioLayer, GJBaseGameLayer) {
    void resetAudio() {
        if (g_busy) return;
        GJBaseGameLayer::resetAudio();
    }
    void updateTimeWarp(float timeWarp) {
        if (g_busy) return;
        GJBaseGameLayer::updateTimeWarp(timeWarp);
    }
};

class $modify(IBAudioPlay, PlayLayer) {
    void startMusic() {
        if (g_busy) return;
        PlayLayer::startMusic();
    }
    void prepareMusic(bool dontWait) {
        if (g_busy) return;
        PlayLayer::prepareMusic(dontWait);
    }
};

class $modify(IBAudio, FMODAudioEngine) {
    void setMusicTimeMS(unsigned int time, bool dontWait, int musicID) {
        if (g_busy) return;
        FMODAudioEngine::setMusicTimeMS(time, dontWait, musicID);
    }
    void fadeInMusic(float duration, int channel) {
        if (g_busy) return;
        FMODAudioEngine::fadeInMusic(duration, channel);
    }
    void fadeOutMusic(float duration, int channel) {
        if (g_busy) return;
        FMODAudioEngine::fadeOutMusic(duration, channel);
    }
    int playEffectAdvanced(gd::string path, float speed, float p3, float volume, float pitch,
                           bool fft, bool reverb, int startMillis, int endMillis, int fadeIn,
                           int fadeOut, bool loop, int effectID, bool overrideSound, bool noPreload,
                           int channelID, int uniqueID, float minInterval, int sfxGroup) {
        if (g_busy) return 0; // no sound effects from the preview
        return FMODAudioEngine::playEffectAdvanced(path, speed, p3, volume, pitch, fft, reverb,
            startMillis, endMillis, fadeIn, fadeOut, loop, effectID, overrideSound, noPreload,
            channelID, uniqueID, minInterval, sfxGroup);
    }
};

// ---------------------------------------------------------------------------
// Hotkeys and live setting changes
// ---------------------------------------------------------------------------

namespace {

void refreshLiveLabel() {
    if (auto pl = PlayLayer::get()) {
        static_cast<IBLayer*>(static_cast<GJBaseGameLayer*>(pl))->refreshLabel();
    }
}

void cycleMainWindow() {
    int next = (mainPresetIndex() + 1) % kMainPresetCount;
    Mod::get()->setSettingValue<std::string>("main-window", kMainPresets[next]);
    notify(fmt::format("Buffer window: {}", kMainPresets[next]));
}

void cycleWaveMode() {
    int next = (static_cast<int>(waveMode()) + 1) % kWaveModeCount;
    Mod::get()->setSettingValue<std::string>("wave-mode", kWaveModes[next]);
    notify(fmt::format("Wave mode: {}", kWaveModes[next]));
}

void changeTps(int delta) {
    int next = std::clamp(tps() + delta, kBaseTps, 1440);
    Mod::get()->setSettingValue<int64_t>("tps", next);
    notify(fmt::format("Game speed: {} TPS", next));
}

void toggleDebug() {
    bool next = !debugLogging();
    Mod::get()->setSettingValue<bool>("debug-logging", next);
    notify(fmt::format("Debug logging: {}", next ? "ON" : "OFF"));
}

} // namespace

$on_mod(Loaded) {
    listenForKeybindSettingPresses("key-cycle-window", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) cycleMainWindow();
    });
    listenForKeybindSettingPresses("key-cycle-wave", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) cycleWaveMode();
    });
    listenForKeybindSettingPresses("key-tps-up", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) changeTps(+60);
    });
    listenForKeybindSettingPresses("key-tps-down", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) changeTps(-60);
    });
    listenForKeybindSettingPresses("key-debug", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) toggleDebug();
    });

    // Keep the on-screen label in sync however a setting gets changed.
    listenForSettingChanges<std::string_view>("main-window", [](std::string_view) { refreshLiveLabel(); });
    listenForSettingChanges<std::string_view>("wave-mode", [](std::string_view) { refreshLiveLabel(); });
    listenForSettingChanges<int64_t>("tps", [](int64_t) { refreshLiveLabel(); });
    listenForSettingChanges<bool>("show-label", [](bool) { refreshLiveLabel(); });
}
