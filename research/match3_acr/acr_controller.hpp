#pragma once

#include <cstdint>
#include <string_view>

namespace acr {

inline constexpr std::uint32_t hard_max_proposals = 6;

// These limits constrain the caller's online work. They do not increase safety
// mass and do not grant permission to use a proposal.
struct OnlineBudget {
    std::uint32_t candidate_checks = 512;
    std::uint32_t refill_nodes = 8192;
    std::uint32_t states = 128;
    std::uint32_t soft_thread_cpu_microseconds = 2000;
};

enum class MassSource { exact_support, action_audit, group_forecast };

struct SafetyCounts {
    std::uint64_t safe = 0;
    std::uint64_t unsafe = 0;
    std::uint64_t unresolved = 0;
};

struct MassEstimate {
    MassSource source;
    std::uint64_t samples;
    double z_point;
    double z_lower;
    double z_possible_upper;
    double unsafe_fraction;
    double unresolved_fraction;
};

// One-sided Wilson 95% lower bound on proven-safe mass. The reported upper
// bound is 1 minus the corresponding lower bound on proven-unsafe mass. The
// two bounds are separate one-sided statements. A group forecast does not give
// per-state coverage or a guarantee on held-out state mixtures.
[[nodiscard]] MassEstimate estimate_mass(
    const SafetyCounts& counts, MassSource source = MassSource::group_forecast);

// Exact mass of the lookup support under P0 for this state/action. Supplying
// that mass alone leaves the upper bound on the true safe mass at 1.
[[nodiscard]] MassEstimate make_exact_mass(double certified_mass, double possible_safe_upper = 1.0);
[[nodiscard]] std::string_view mass_source_name(MassSource source);

struct BalancedConfig {
    std::uint32_t max_k = hard_max_proposals;
    double fallback_target = 0.05;
    double tv_weight = 0.35;
    double cpu_weight = 0.20;
    double fallback_weight = 0.45;
    double relative_proposal_cost = 1.0;
};

struct Plan {
    std::uint32_t k;
    double tv_proxy;
    double fallback_probability;
    double expected_proposals;
    double normalized_work;
    double objective;
    bool meets_forecast_target;
};

// First proved-safe proposal wins. After K unsuccessful admissibility checks,
// the runtime must use its verified witness. K=0 uses the witness directly.
// Forecasts assume a fixed support and sufficient time to expend the K checks;
// earlier stops also use witness.
[[nodiscard]] Plan plan_for_k(const MassEstimate& mass, std::uint32_t k,
                              const BalancedConfig& config = {});
[[nodiscard]] Plan choose_plan(const MassEstimate& mass, const BalancedConfig& config = {});

}  // namespace acr
