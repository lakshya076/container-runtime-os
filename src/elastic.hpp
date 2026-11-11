#pragma once
// Elastic memory controller: moves memory.high between a floor and a ceiling.
// Grow fast under pressure, shrink slowly when calm (inverse of TCP's AIMD), so the limit
// settles just above the container's real working set instead of a guessed static value.
// Pure logic: the caller samples pressure and writes memory.high.

constexpr double kGrowFactor = 1.25;   // pressured: high *= 1.25 ...
constexpr double kGrowHeadroom = 1.10; // ... and at least current * 1.10
constexpr double kShrinkFactor = 0.97; // calm: high *= 0.97 per control step
constexpr double kCollapseRatio = 1.5; // calm and high > 1.5 * current: jump down ...
constexpr double kCollapseTo = 1.2;    // ... to 1.2 * current
constexpr int kCooldownSteps = 8;      // no shrinking for this many steps after a grow

class ElasticController {
public:
    ElasticController(long long floor, long long ceiling);

    // One control step. Returns the new memory.high.
    long long step(bool pressured, long long current);

    void set_ceiling(long long ceiling);
    long long high() const { return high_; }
    long long floor() const { return floor_; }
    long long ceiling() const { return ceiling_; }
    int grows() const { return grows_; }
    int shrinks() const { return shrinks_; }

private:
    long long clamp(double v) const;
    long long floor_, ceiling_, high_;
    int cooldown_ = 0, grows_ = 0, shrinks_ = 0;
};

// Suggested static --memory: max(working_set * 1.2, peak), rounded up to 4 MiB.
// Never below the observed peak, since anon spikes cannot be reclaimed without swap.
// shortcut: p95-based heuristic; with PSI, a senpai-style search for the lowest stall-free limit is better.
long long recommend_limit(long long working_set, long long peak);
