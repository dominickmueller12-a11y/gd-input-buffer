// Input Buffer
// ============
// Click a few ticks early and the click is moved to the tick where it works.
//
// How it works
// ------------
// Every jump press (and, in flying modes, release) from the player is
// intercepted before the game sees it. A hidden copy of the player (a "ghost")
// is given the real player's exact state and run forward a short way with the
// input placed 0, 1, 2 ... N ticks later, using the game's own player physics
// and collision code against the level. The smallest delay where the ghost is
// still alive at the end of the look-ahead wins; the real input is then played
// on that tick. If the input was already fine, or nothing in the window
// survives, it goes through unchanged, exactly like vanilla.
//
// Only the ghost moves during the preview. The level itself (triggers, moving
// objects, music, trail, counters, other mods) is never stepped or rewound.
//
// Every input that arrives while an earlier one is still waiting is shifted by
// the same amount, so taps stay taps and holds keep their length.

#include <Geode/Geode.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/ui/Notification.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <string>
#include <unordered_set>
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

// In these modes the button is held. In the others a press is a tap.
bool isHoldMode(Mode m) {
    return m == Mode::Ship || m == Mode::Wave || m == Mode::Swing || m == Mode::Robot;
}

// Modes where "safe" can't be decided by landing on the ground.
bool isFlyingMode(Mode m) {
    return m == Mode::Ship || m == Mode::Wave || m == Mode::Ufo || m == Mode::Swing;
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
    "48 ticks (12 frames)",
    "64 ticks (16 frames)",
    "96 ticks (24 frames)",
    "120 ticks (0.5 second)",
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
// Preview state shared with the sandbox hooks below
// ---------------------------------------------------------------------------

bool g_simulating = false;    // a ghost preview is running
bool g_simDied = false;       // the ghost died
bool g_simHitPortal = false;  // the ghost reached something we don't simulate
bool g_smart = true;          // ghost may use orbs / pads / gravity portals
int g_simObjectsSeen = 0;     // objects the ghost's collision pass looked at (debug)
double g_stallSeconds = 0.0;  // time spent deciding; removed from the next frame

PlayerObject* g_ghost1 = nullptr;
PlayerObject* g_ghost2 = nullptr;

bool isGhost(PlayerObject* p) {
    return p && (p == g_ghost1 || p == g_ghost2);
}

// Objects whose activation state the ghost changed during the preview; put
// back exactly as they were afterwards.
struct ObjSnapshot {
    GameObject* object;
    bool isActivated;
    EffectGameObject* effect;
    bool activated;
    bool byP1;
    bool byP2;
};
std::vector<ObjSnapshot> g_snapshots;
std::unordered_set<GameObject*> g_simUsed;

void snapshotObject(GameObject* object) {
    if (!object || g_simUsed.contains(object)) return;
    g_simUsed.insert(object);
    ObjSnapshot s { object, object->m_isActivated, nullptr, false, false, false };
    if (auto effect = typeinfo_cast<EffectGameObject*>(object)) {
        s.effect = effect;
        s.activated = effect->m_activated;
        s.byP1 = effect->m_activatedByPlayer1;
        s.byP2 = effect->m_activatedByPlayer2;
    }
    g_snapshots.push_back(s);
}

void restoreObjects() {
    for (auto const& s : g_snapshots) {
        s.object->m_isActivated = s.isActivated;
        if (s.effect) {
            s.effect->m_activated = s.activated;
            s.effect->m_activatedByPlayer1 = s.byP1;
            s.effect->m_activatedByPlayer2 = s.byP2;
        }
    }
    g_snapshots.clear();
    g_simUsed.clear();
}

// Object classes the ghost interacts with.
constexpr uint64_t typeBit(GameObjectType type) {
    return 1ull << static_cast<int>(type);
}

constexpr uint64_t kCollidingTypes =
    typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::Slope) |
    typeBit(GameObjectType::CollisionObject) | typeBit(GameObjectType::AnimatedHazard);

constexpr uint64_t kInteractTypes =
    typeBit(GameObjectType::YellowJumpPad) | typeBit(GameObjectType::PinkJumpPad) | typeBit(GameObjectType::RedJumpPad) |
    typeBit(GameObjectType::GravityPad) | typeBit(GameObjectType::SpiderPad) | typeBit(GameObjectType::YellowJumpRing) |
    typeBit(GameObjectType::PinkJumpRing) | typeBit(GameObjectType::RedJumpRing) | typeBit(GameObjectType::GravityRing) |
    typeBit(GameObjectType::GreenRing) | typeBit(GameObjectType::DropRing) | typeBit(GameObjectType::CustomRing) |
    typeBit(GameObjectType::DashRing) | typeBit(GameObjectType::GravityDashRing) | typeBit(GameObjectType::SpiderOrb) |
    typeBit(GameObjectType::InverseGravityPortal) | typeBit(GameObjectType::NormalGravityPortal) |
    typeBit(GameObjectType::GravityTogglePortal);

// Portals that change too much to preview: reaching one ends the preview as safe.
constexpr uint64_t kStopTypes =
    typeBit(GameObjectType::ShipPortal) | typeBit(GameObjectType::CubePortal) | typeBit(GameObjectType::BallPortal) |
    typeBit(GameObjectType::UfoPortal) | typeBit(GameObjectType::WavePortal) | typeBit(GameObjectType::RobotPortal) |
    typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal) | typeBit(GameObjectType::DualPortal) |
    typeBit(GameObjectType::SoloPortal) | typeBit(GameObjectType::RegularSizePortal) | typeBit(GameObjectType::MiniSizePortal) |
    typeBit(GameObjectType::InverseMirrorPortal) | typeBit(GameObjectType::NormalMirrorPortal) |
    typeBit(GameObjectType::TeleportPortal) | typeBit(GameObjectType::TeleportOrb);

float speedPortalValue(int id) {
    switch (id) {
        case 200: return 0.7f;
        case 201: return 0.9f;
        case 202: return 1.1f;
        case 203: return 1.3f;
        case 1334: return 1.6f;
        default: return 0.f;
    }
}

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
        float tickDt = 1.f / kBaseTps;    // dt of the last real physics step
        int sinceInput = 999;             // ticks since the last real input
        int rapid = 0;                    // consecutive fast inputs (wave spam)
        bool haveLastRaw = false;
        bool lastRawDown = false;
        double tpsAccum = 0.0;
        float tpsRealDelta = 0.f;
        bool tpsHide = false;
        CCLabelBMFont* label = nullptr;

        // Ghosts live in a layer that is never added to the scene: cocos never
        // draws or schedules them, so they cost nothing between clicks.
        CCLayer* ghostLayer = nullptr;
        PlayerObject* ghost1 = nullptr;
        PlayerObject* ghost2 = nullptr;

        ~Fields() { dropGhosts(); }

        void dropGhosts() {
            if (g_ghost1 == ghost1) g_ghost1 = nullptr;
            if (g_ghost2 == ghost2) g_ghost2 = nullptr;
            for (auto** g : { &ghost1, &ghost2 }) {
                if (*g) {
                    (*g)->removeFromParentAndCleanup(true);
                    (*g)->release();
                    *g = nullptr;
                }
            }
            if (ghostLayer) {
                ghostLayer->removeAllChildrenWithCleanup(true);
                ghostLayer->release();
                ghostLayer = nullptr;
            }
        }
    };

    static void onModify(auto& self) {
        // Run after every other mod's hook: they see the player's real input
        // exactly once, and the delayed input we play goes straight to the game.
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::LastPre);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", Priority::FirstPre);
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", Priority::FirstPre);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", Priority::FirstPre);
        (void)self.setHookPriority("GJBaseGameLayer::flipGravity", Priority::FirstPre);
        (void)self.setHookPriority("GJBaseGameLayer::gameEventTriggered", Priority::FirstPre);
        (void)self.setHookPriority("GJBaseGameLayer::playerWillSwitchMode", Priority::FirstPre);
    }

    // ---- helpers ----------------------------------------------------------

    PlayLayer* asPlayLayer() {
        auto pl = PlayLayer::get();
        return (pl && static_cast<GJBaseGameLayer*>(pl) == this) ? pl : nullptr;
    }

    bool isTwoPlayer() {
        return m_levelSettings && m_levelSettings->m_twoPlayerMode;
    }

    bool isDual() {
        return m_gameState.m_isDualMode && m_player2;
    }

    int slotFor(bool isPlayer1) {
        return (isTwoPlayer() && !isPlayer1) ? 1 : 0;
    }

    PlayerObject* playerFor(int slot) {
        return (isTwoPlayer() && slot == 1) ? m_player2 : m_player1;
    }

    void setLabel(CCLabelBMFont* label) { m_fields->label = label; }
    void dropGhostsNow() { m_fields->dropGhosts(); }

    void inject(bool down, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, kJumpButton, isPlayer1);
    }

    // ---- ghosts -----------------------------------------------------------

    PlayerObject* makeGhost() {
        auto& f = m_fields;
        if (!f->ghostLayer) {
            f->ghostLayer = CCLayer::create();
            f->ghostLayer->retain();
        }
        auto ghost = PlayerObject::create(1, 1, this, f->ghostLayer, true);
        ghost->retain();
        ghost->setPosition({ 0.f, 105.f });
        ghost->setVisible(false);
        f->ghostLayer->addChild(ghost);
        return ghost;
    }

    void ensureGhosts() {
        auto& f = m_fields;
        if (!f->ghost1) f->ghost1 = makeGhost();
        if (!f->ghost2) f->ghost2 = makeGhost();
        g_ghost1 = f->ghost1;
        g_ghost2 = f->ghost2;
    }

    static void copyState(PlayerObject* ghost, PlayerObject* real) {
        ghost->setVisible(false);
        ghost->resetTouchedRings(true);
        ghost->copyAttributes(real);
        ghost->setScale(real->getScale());
        ghost->setContentSize(real->getContentSize());
        ghost->m_width = real->m_width;
        ghost->m_height = real->m_height;
        ghost->m_gravityMod = real->m_gravityMod;
        ghost->m_isOnGround = real->m_isOnGround;
        ghost->m_isUpsideDown = real->m_isUpsideDown;
        ghost->m_vehicleSize = real->m_vehicleSize;
        ghost->m_playerSpeed = real->m_playerSpeed;
        ghost->m_isDashing = real->m_isDashing;
        ghost->m_lastActivatedPortal = real->m_lastActivatedPortal;
        // button / jump-buffer state, so the ghost reacts to the preview input
        // exactly like the real player would
        ghost->m_holdingButtons = real->m_holdingButtons;
        ghost->m_jumpBuffered = real->m_jumpBuffered;
        ghost->m_wasJumpBuffered = real->m_wasJumpBuffered;
        ghost->m_stateJumpBuffered = real->m_stateJumpBuffered;
        ghost->m_stateRingJump = real->m_stateRingJump;
        ghost->m_stateRingJump2 = real->m_stateRingJump2;
        ghost->m_wasRobotJump = real->m_wasRobotJump;
        ghost->m_touchedRing = real->m_touchedRing;
        ghost->m_touchedPad = real->m_touchedPad;
        ghost->m_touchedGravityPortal = real->m_touchedGravityPortal;
        // position / motion, in case copyAttributes leaves any of it out
        ghost->setPosition(real->getPosition());
        ghost->setRotation(real->getRotation());
        ghost->m_position = real->m_position;
        ghost->m_lastGroundedPos = real->m_lastGroundedPos;
        ghost->m_yVelocity = real->m_yVelocity;
        ghost->m_fallSpeed = real->m_fallSpeed;
        ghost->m_yVelocityBeforeSlope = real->m_yVelocityBeforeSlope;
        ghost->m_groundYVelocity = real->m_groundYVelocity;
        ghost->m_speedMultiplier = real->m_speedMultiplier;
        ghost->m_gravity = real->m_gravity;
        ghost->m_isGoingLeft = real->m_isGoingLeft;
        ghost->m_rotationSpeed = real->m_rotationSpeed;
        ghost->m_isRotating = real->m_isRotating;
        ghost->m_totalTime = real->m_totalTime;
        ghost->m_objectSnappedTo = real->m_objectSnappedTo;
        ghost->m_maybeLastGroundObject = real->m_maybeLastGroundObject;
        ghost->m_isDead = false;
        ghost->m_isLocked = real->m_isLocked;
    }

    static void pressGhost(PlayerObject* ghost, bool down) {
        if (down) ghost->pushButton(PlayerButton::Jump);
        else ghost->releaseButton(PlayerButton::Jump);
    }

    void stepGhost(PlayerObject* ghost, float dt) {
        ghost->update(dt);      // physics: velocities, jumps, gravity
        ghost->updateMove(dt);  // actually travel (2.2 moved this out of update)
        ghost->updateRotation(dt);
        ghost->m_collisionLogTop->removeAllObjects();
        ghost->m_collisionLogBottom->removeAllObjects();
        ghost->m_collisionLogLeft->removeAllObjects();
        ghost->m_collisionLogRight->removeAllObjects();
        this->checkCollisions(ghost, dt, false);
    }

    // ---- look-ahead -------------------------------------------------------

    // The input is modelled as a gesture: the input itself, then the opposite
    // input `gesture` ticks later (a tap for landing modes, a short hold /
    // short release for flying modes).
    // True if the ghost is still alive after `horizon` ticks.
    bool survives(int delay, bool down, int gesture, int horizon, bool flying, int& stepsUsed) {
        auto& f = m_fields;
        bool dual = isDual();
        copyState(f->ghost1, m_player1);
        if (dual) copyState(f->ghost2, m_player2);

        float dt = f->tickDt;
        if (!(dt > 0.0005f && dt < 0.1f)) dt = 1.f / kBaseTps;

        g_simulating = true;
        g_simDied = false;
        g_simHitPortal = false;

        bool safe = false;

        for (int k = 0; k < horizon; ++k) {
            if (k == delay) {
                pressGhost(f->ghost1, down);
                if (dual) pressGhost(f->ghost2, down);
            }
            if (k == delay + gesture) {
                pressGhost(f->ghost1, !down);
                if (dual) pressGhost(f->ghost2, !down);
            }
            stepGhost(f->ghost1, dt);
            if (!g_simDied && dual) stepGhost(f->ghost2, dt);
            ++stepsUsed;
            if (g_simDied) break;
            if (g_simHitPortal) { safe = true; break; }
        }

        g_simulating = false;
        restoreObjects();
        return safe || !g_simDied;
    }

    // Decide how many steps to hold this input back. 0 = play it now.
    int chooseDelay(bool down, Mode mode) {
        auto pl = asPlayLayer();
        if (!pl || m_playerDied || !m_player1) return 0;
        ensureGhosts();

        int window = scaleTicks(windowBaseTicks(mode));
        if (window <= 0) return 0;
        // The look-ahead must reach past the whole window, or a big window
        // could never find the danger it is supposed to dodge.
        int horizon = std::max(scaleTicks(static_cast<int>(setting<int64_t>("look-ahead"))), window + scaleTicks(16));
        int budget = static_cast<int>(setting<int64_t>("sim-budget"));
        int gesture = isHoldMode(mode)
            ? std::max(1, scaleTicks(static_cast<int>(setting<int64_t>("hold-length"))))
            : 1;
        bool flying = isFlyingMode(mode);
        g_smart = setting<bool>("simulate-interactables");

        auto t0 = std::chrono::steady_clock::now();
        int sims = 0, steps = 0;
        g_simObjectsSeen = 0;
        CCPoint realPos = m_player1->getPosition();
        copyState(m_fields->ghost1, m_player1);
        CCPoint ghostPos = m_fields->ghost1->getPosition();

        auto alive = [&](int d) -> bool {
            if (steps + horizon + d > budget) return false;
            ++sims;
            return survives(d, down, gesture, horizon + d, flying, steps);
        };

        int chosen = 0;
        bool found = false;

        if (alive(0)) {
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
            // Neither end survives: coarse scan, then refine downward.
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

        double totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        g_stallSeconds += totalSec;

        if (debugLogging()) {
            log::info("[InputBuffer] {} {} | window {}t | delay {} ({}) | {} sims, {} ticks, {} us | real ({:.1f},{:.1f}) ghost ({:.1f},{:.1f}) yvel {:.2f} | objs seen {}",
                kModeName[static_cast<int>(mode)], down ? "press" : "release", window,
                found ? chosen : 0, found ? (chosen == 0 ? "already safe" : "shifted") : "nothing survives",
                sims, steps, static_cast<long long>(totalSec * 1e6),
                realPos.x, realPos.y, ghostPos.x, ghostPos.y, m_player1->m_yVelocity, g_simObjectsSeen);
        }

        return found ? chosen : 0;
    }

    // ---- input queue ------------------------------------------------------

    void enqueue(Slot& s, bool down, bool isPlayer1, int ticks) {
        if (!s.queue.empty()) {
            auto& back = s.queue.back();
            ticks = std::max(ticks, back.ticksLeft);
            if (back.down && !down && ticks == back.ticksLeft) ticks = back.ticksLeft + 1;
        }
        s.queue.push_back({ ticks, down, isPlayer1 });
    }

    // A real input from the player, handled outside a physics step.
    void processRaw(bool down, bool isPlayer1) {
        auto& f = m_fields;
        int idx = slotFor(isPlayer1);
        auto& s = f->slot[idx];

        // The game hands us the same input twice in one tick (once mid-step,
        // once after). The second copy must not undo a decision: drop it if
        // something is queued, otherwise just pass it through.
        bool duplicate = f->haveLastRaw && f->sinceInput == 0 && f->lastRawDown == down;
        f->haveLastRaw = true;
        f->lastRawDown = down;
        if (duplicate) {
            if (s.queue.empty()) inject(down, isPlayer1);
            return;
        }

        // Something earlier is still waiting: shift this one by the same amount.
        if (!s.queue.empty()) {
            enqueue(s, down, isPlayer1, s.shift);
            f->sinceInput = 0;
            return;
        }

        auto player = playerFor(idx);
        Mode mode = modeOf(player);
        bool eligible = player && !player->m_isDashing && inputIsBuffered(mode, down);

        // Fast spam (wave): don't fight the player's rhythm.
        if (f->sinceInput < 8) {
            if (++f->rapid >= 3) eligible = false;
        }
        else {
            f->rapid = 0;
        }
        f->sinceInput = 0;

        auto pl = asPlayLayer();
        if (!eligible || !pl || pl->m_isPaused || m_playerDied) {
            if (debugLogging()) {
                log::info("[InputBuffer] raw {} p1={} mode={} ground={}/{} dashing={} buffered={} rapid={} -> passthrough",
                    down ? "press" : "release", isPlayer1, kModeName[static_cast<int>(mode)],
                    player ? player->m_isOnGround : false, player ? player->m_isOnGround2 : false,
                    player ? player->m_isDashing : false, inputIsBuffered(mode, down), f->rapid);
            }
            inject(down, isPlayer1);
            return;
        }

        int d = chooseDelay(down, mode);
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
        f->sinceInput = 999;
        f->rapid = 0;
        f->haveLastRaw = false;
    }

    // ---- status label -----------------------------------------------------

    void refreshLabel() {
        auto& f = m_fields;
        if (!f->label) return;
        bool show = setting<bool>("show-label");
        f->label->setVisible(show);
        if (show) f->label->setString(statusText().c_str());
    }

    // ---- input hooks ------------------------------------------------------

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!g_simulating && debugLogging() && asPlayLayer()) {
            log::info("[InputBuffer] handleButton down={} button={} p1={} insideStep={} queued={}",
                down, button, isPlayer1, m_fields->insideStep, m_fields->slot[0].queue.size());
        }
        if (g_simulating || button != kJumpButton || !asPlayLayer()) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        if (mainWindowBaseTicks() <= 0 && m_fields->slot[0].queue.empty() && m_fields->slot[1].queue.empty()) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        if (m_fields->insideStep) {
            // Decide right after the current step ends.
            m_fields->pendingRaw.push_back({ down, isPlayer1 });
            return;
        }
        processRaw(down, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g_simulating || !asPlayLayer()) {
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }

        auto& f = m_fields;
        if (dt > 0.f) f->tickDt = dt;
        if (f->sinceInput < 998) ++f->sinceInput;

        for (auto& s : f->slot) fireDue(s);

        f->insideStep = true;
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        f->insideStep = false;

        for (auto& s : f->slot) {
            for (auto& q : s.queue) --q.ticksLeft;
        }

        if (!f->pendingRaw.empty()) {
            auto pending = std::move(f->pendingRaw);
            f->pendingRaw.clear();
            for (auto& r : pending) processRaw(r.down, r.isPlayer1);
        }
    }

    // ---- sandbox: the ghost may only touch what we allow -------------------

    // The ghost collides with solids and hazards, may use orbs / pads /
    // gravity portals (restored afterwards), reacts to speed portals, and
    // stops at portals that change too much to preview.
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!g_simulating || !isGhost(player)) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }
        g_simObjectsSeen += objectCount;
        static gd::vector<GameObject*> relevant;
        relevant.clear();
        if (static_cast<int>(relevant.capacity()) < objectCount) relevant.reserve(objectCount);

        uint64_t keep = g_smart ? (kCollidingTypes | kInteractTypes) : kCollidingTypes;
        bool haveRect = false;
        CCRect ghostRect;
        auto& list = *objects;
        for (int i = 0; i < objectCount; ++i) {
            auto object = list[i];
            if (!object) continue;
            auto type = static_cast<unsigned>(object->m_objectType);
            uint64_t bit = type < 64 ? (1ull << type) : 0;
            if (keep & bit) {
                relevant.push_back(object);
                continue;
            }
            bool stop = (kStopTypes & bit) != 0;
            // the portal the player is already in / just used doesn't end the preview
            if (stop && object == player->m_lastActivatedPortal) continue;
            float speed = stop ? 0.f : speedPortalValue(object->m_objectID);
            if (!stop && speed <= 0.f) continue;
            if (!haveRect) {
                ghostRect = player->getObjectRect();
                haveRect = true;
            }
            if (ghostRect.intersectsRect(object->getObjectRect())) {
                if (stop) g_simHitPortal = true;
                else player->m_playerSpeed = speed;
            }
        }
        GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (g_simulating) {
            if (!g_smart || !isGhost(player) || !object || g_simUsed.contains(object)) return false;
            snapshotObject(object);
            return true;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (g_simulating) {
            if (!g_smart || !isGhost(player)) return;
            snapshotObject(ring);
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (g_simulating) {
            if (g_smart && isGhost(player)) player->flipGravity(flip, true);
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (g_simulating) return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (g_simulating) {
            g_simHitPortal = true;
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }

    // ---- Game Speed (TPS) -------------------------------------------------
    // Same approach as the well-known "physics bypass" hacks. At 240 TPS
    // nothing is touched.

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
        // frame's delta so the game pauses for it instead of catching up.
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
            if (std::chrono::steady_clock::now() - start > frameBudget) break;
        }
        f->tpsHide = false;
        GJBaseGameLayer::update(static_cast<float>(tick * steps));
    }

    bool tpsHiding() { return m_fields->tpsHide; }
    float realFrameDelta() { return m_fields->tpsRealDelta; }

    unsigned int ticksAt240() {
        return static_cast<unsigned int>(std::lround(m_gameState.m_levelTime * kBaseTps));
    }
};

// ---------------------------------------------------------------------------
// PlayLayer: ghost deaths, resets, safe mode, status label
// ---------------------------------------------------------------------------

class $modify(IBPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::FirstPre);
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
            // Only the ghost may die in a preview; the anti-cheat spike is not
            // a real death.
            if (isGhost(player) && object != m_anticheatSpike) g_simDied = true;
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
        if (g_simulating) return;
        if (!tpsActive()) {
            PlayLayer::levelComplete();
            return;
        }
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

    void onQuit() {
        engine()->clearBuffers();
        engine()->dropGhostsNow();
        PlayLayer::onQuit();
    }
};

// ---------------------------------------------------------------------------
// Side effects the ghost must not cause
// ---------------------------------------------------------------------------

class $modify(IBPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::incrementJumps", Priority::FirstPre);
        (void)self.setHookPriority("PlayerObject::ringJump", Priority::FirstPre);
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", Priority::FirstPre);
    }

    void incrementJumps() {
        if (g_simulating) return; // ghost jumps don't count in stats
        PlayerObject::incrementJumps();
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (g_simulating) {
            if (!g_smart || !isGhost(this)) return;
            snapshotObject(ring);
        }
        PlayerObject::ringJump(ring, skipCheck);
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (g_simulating) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }
};

class $modify(IBEffectObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (g_simulating) return; // the ghost never fires triggers
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(IBGameObject, GameObject) {
    void playShineEffect() {
        if (g_simulating) return;
        GameObject::playShineEffect();
    }
};

class $modify(IBAudio, FMODAudioEngine) {
    int playEffectAdvanced(gd::string path, float speed, float p3, float volume, float pitch,
                           bool fft, bool reverb, int startMillis, int endMillis, int fadeIn,
                           int fadeOut, bool loop, int effectID, bool overrideSound, bool noPreload,
                           int channelID, int uniqueID, float minInterval, int sfxGroup) {
        if (g_simulating) return 0; // no sound effects from the preview
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

    listenForSettingChanges<std::string_view>("main-window", [](std::string_view) { refreshLiveLabel(); });
    listenForSettingChanges<std::string_view>("wave-mode", [](std::string_view) { refreshLiveLabel(); });
    listenForSettingChanges<int64_t>("tps", [](int64_t) { refreshLiveLabel(); });
    listenForSettingChanges<bool>("show-label", [](bool) { refreshLiveLabel(); });
}
