// FrameFix - practice-only input correction using a hidden trajectory predictor.
// "Frame" here = one physics step (GD runs 240 steps/sec by default).
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <deque>
#include <random>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Settings helpers
// ---------------------------------------------------------------------------
static bool  cfgBool(char const* k) { return Mod::get()->getSettingValue<bool>(k); }
static int   cfgInt(char const* k)  { return (int)Mod::get()->getSettingValue<int64_t>(k); }

static std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}

// ---------------------------------------------------------------------------
// Predictor
// ---------------------------------------------------------------------------
// Question answered: "if I apply this input after `delay` steps (and do nothing
// else afterwards), does player 1 survive the next HORIZON steps?"
//
// It runs on a throwaway PlayerObject so the real player is never touched.
// This is the same idea the "Show Trajectory" style mods use. The copy/step
// part is the piece that depends most on GD version internals, so it is
// isolated here for easy fixing.
constexpr int HORIZON = 90; // steps to look ahead

struct SimInput {
    bool pressDown;   // true = simulate a press, false = simulate a release
    int  delay;       // steps from now until the input is applied
};

class Predictor {
public:
    // Returns true if the player survives HORIZON steps with this input.
    static bool survives(GJBaseGameLayer* gl, PlayerObject* real, SimInput in) {
        auto sim = PlayerObject::create(1, 1, gl, gl->m_objectLayer, false);
        if (!sim) return false;
        sim->retain();

        copyState(real, sim);

        bool alive = true;
        for (int step = 0; step < HORIZON && alive; ++step) {
            if (step == in.delay) {
                if (in.pressDown) sim->pushButton(PlayerButton::Jump);
                else              sim->releaseButton(PlayerButton::Jump);
            }
            alive = stepOnce(gl, sim);
        }

        sim->release();
        return alive;
    }

private:
    static void copyState(PlayerObject* from, PlayerObject* to) {
        to->setPosition(from->getPosition());
        to->m_yVelocity        = from->m_yVelocity;
        to->m_isUpsideDown     = from->m_isUpsideDown;
        to->m_isShip           = from->m_isShip;
        to->m_isBall           = from->m_isBall;
        to->m_isBird           = from->m_isBird;
        to->m_isDart           = from->m_isDart;
        to->m_isRobot          = from->m_isRobot;
        to->m_isSpider         = from->m_isSpider;
        to->m_isOnGround       = from->m_isOnGround;
        to->m_playerSpeed      = from->m_playerSpeed;
        to->m_gravityMod       = from->m_gravityMod;
        to->m_isHolding        = from->m_isHolding;
        to->m_touchingRings    = from->m_touchingRings;
        // TODO: copy any extra fields your target GD version needs
        // (gravity flip state, size/mini, platformer velocity, etc.).
    }

    // Advance the sim player one physics step and report whether it is alive.
    // Collision against level objects goes through the real layer's section
    // lookup; death is detected via the player's own collision result.
    static bool stepOnce(GJBaseGameLayer* gl, PlayerObject* sim) {
        float dt = 1.f / 240.f;
        sim->update(dt * 60.f);          // physics step
        gl->checkCollisions(sim, dt, true);
        return !sim->m_isDead;
    }
};

// ---------------------------------------------------------------------------
// Input queue + hook
// ---------------------------------------------------------------------------
struct Pending {
    int  fireAt;     // physics-step index when this should be applied
    bool down;
    int  button;
    bool isPlayer1;
};

class $modify(FFLayer, GJBaseGameLayer) {
    struct Fields {
        int step = 0;
        std::deque<Pending> queue;
        bool replaying = false;
    };

    bool active() {
        if (!cfgBool("enabled")) return false;
        if (cfgBool("practice-only") && !m_isPracticeMode) return false;
        return true;
    }

    bool rollCorrection() {
        if (!cfgBool("random-mode")) return true;
        int chance = cfgInt("random-chance");
        return std::uniform_int_distribution<int>(1, 100)(rng()) <= chance;
    }

    // Count physics steps and fire any due delayed inputs.
    void processCommands(float dt) {
        GJBaseGameLayer::processCommands(dt);
        auto f = m_fields.self();
        f->step++;

        while (!f->queue.empty() && f->queue.front().fireAt <= f->step) {
            auto p = f->queue.front();
            f->queue.pop_front();
            f->replaying = true;
            GJBaseGameLayer::handleButton(p.down, p.button, p.isPlayer1);
            f->replaying = false;
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto f = m_fields.self();

        // Only touch player 1's jump button, and never our own replays.
        if (f->replaying || !isPlayer1 || button != 1 || !active() || !m_player1) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        int maxFrames = down ? cfgInt("click-max-frames") : cfgInt("release-max-frames");
        if (maxFrames <= 0) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // 1. Would the input as-is (delay 0) already be fine? Then leave it alone.
        if (Predictor::survives(this, m_player1, {down, 0})) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // 2. Random mode: sometimes refuse to help.
        if (!rollCorrection()) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // 3. Find the smallest delay (up to the limit) that makes it work.
        for (int d = 1; d <= maxFrames; ++d) {
            if (Predictor::survives(this, m_player1, {down, d})) {
                f->queue.push_back({f->step + d, down, button, isPlayer1});
                return; // swallowed now, replayed later
            }
        }

        // 4. Too far off: pass it through unchanged (you still die).
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};
