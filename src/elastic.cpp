#include "elastic.hpp"

#include <algorithm>

ElasticController::ElasticController(long long floor, long long ceiling)
    : floor_(floor), ceiling_(std::max(floor, ceiling)), high_(ceiling_) {}

long long ElasticController::clamp(double v) const {
    return std::clamp(static_cast<long long>(v), floor_, ceiling_);
}

long long ElasticController::step(bool pressured, long long current) {
    const long long before = high_;
    const double cur = static_cast<double>(current);
    if (pressured) {
        high_ = clamp(std::max(static_cast<double>(high_) * kGrowFactor, cur * kGrowHeadroom));
        cooldown_ = kCooldownSteps;
        if (high_ > before) ++grows_;
    } else if (cooldown_ > 0) {
        --cooldown_;
    } else {
        double target = static_cast<double>(high_) > cur * kCollapseRatio ? cur * kCollapseTo
                                                                          : static_cast<double>(high_) * kShrinkFactor;
        high_ = clamp(std::min(target, static_cast<double>(high_)));
        if (high_ < before) ++shrinks_;
    }
    return high_;
}

void ElasticController::set_ceiling(long long ceiling) {
    ceiling_ = std::max(floor_, ceiling);
    high_ = std::min(high_, ceiling_);
}

long long recommend_limit(long long working_set, long long peak) {
    constexpr long long kStep = 4LL << 20;
    long long want = std::max(working_set + working_set / 5, peak);
    return (want + kStep - 1) / kStep * kStep;
}
