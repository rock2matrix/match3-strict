#include "acr_controller.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace acr {
namespace {

bool nonnegative(double value) {
    return std::isfinite(value) && value >= 0;
}

void validate(const MassEstimate& mass, const BalancedConfig& config) {
    if (!nonnegative(mass.z_lower) || !nonnegative(mass.z_point) || !nonnegative(mass.z_possible_upper) ||
        mass.z_lower > mass.z_point || mass.z_point > mass.z_possible_upper || mass.z_possible_upper > 1) {
        throw std::invalid_argument("invalid conservative certified mass");
    }
    if (config.max_k == 0 || config.max_k > hard_max_proposals || !nonnegative(config.fallback_target) ||
        config.fallback_target > 1 || !nonnegative(config.tv_weight) || !nonnegative(config.cpu_weight) ||
        !nonnegative(config.fallback_weight) || !nonnegative(config.relative_proposal_cost)) {
        throw std::invalid_argument("invalid balanced planning configuration");
    }
}

Plan calculate(const MassEstimate& mass, std::uint32_t k, const BalancedConfig& config) {
    if (k == 0) {
        // The mass estimate does not state P0({witness}). Use the conservative
        // upper proxy for distortion of the deterministic witness policy.
        return {0, 1.0, 1.0, 0.0, 0.0, config.tv_weight + config.fallback_weight, false};
    }
    const double failure = 1.0 - mass.z_lower;
    double expected_proposals = 0.0;
    double fallback = 1.0;
    for (std::uint32_t attempt = 0; attempt < k; ++attempt) {
        expected_proposals += fallback;
        fallback *= failure;
    }
    const double work = (expected_proposals / config.max_k) * config.relative_proposal_cost;
    const double objective = config.tv_weight * failure + config.cpu_weight * work +
                             config.fallback_weight * fallback;
    // Allow roundoff from 1-z, e.g. a represented z of 0.95 for K=1.
    // A zero target still requires zero modeled fallback.
    const double tolerance = config.fallback_target > 0 ? 4 * std::numeric_limits<double>::epsilon() : 0;
    return {k, failure, fallback, expected_proposals, work, objective,
            fallback - config.fallback_target <= tolerance};
}

}  // namespace

Plan plan_for_k(const MassEstimate& mass, std::uint32_t k, const BalancedConfig& config) {
    validate(mass, config);
    if (k > config.max_k) {
        throw std::invalid_argument("proposal count exceeds planning bounds");
    }
    return calculate(mass, k, config);
}

Plan choose_plan(const MassEstimate& mass, const BalancedConfig& config) {
    validate(mass, config);
    auto best = calculate(mass, 0, config);
    for (std::uint32_t k = 1; k <= config.max_k; ++k) {
        const auto candidate = calculate(mass, k, config);
        if (candidate.meets_forecast_target) {
            // Expected work never decreases with K. Take the first feasible
            // plan, leaving additional fallback reduction to the offline bank.
            return candidate;
        }
        if (candidate.objective < best.objective) {
            best = candidate;
        }
    }
    return best;
}

}  // namespace acr
