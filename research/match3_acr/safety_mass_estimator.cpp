#include "acr_controller.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace acr {
namespace {

long double wilson_lower(std::uint64_t events, std::uint64_t samples) {
    if (events == 0 || samples == 0) {
        return 0.0L;
    }
    constexpr long double z = 1.6448536269514727L;
    const long double n = samples;
    const long double proportion = static_cast<long double>(events) / n;
    const long double square = z * z / n;
    const long double centre = proportion + square / 2;
    const long double spread = z * std::sqrt(proportion * (1 - proportion) / n + square / (4 * n));
    return std::clamp((centre - spread) / (1 + square), 0.0L, 1.0L);
}

bool probability(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

}  // namespace

MassEstimate estimate_mass(const SafetyCounts& counts, MassSource source) {
    if (source != MassSource::action_audit && source != MassSource::group_forecast) {
        throw std::invalid_argument("sample counts require an audit or forecast source");
    }
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    if (counts.safe > max - counts.unsafe || counts.safe + counts.unsafe > max - counts.unresolved) {
        throw std::invalid_argument("mass sample count overflow");
    }
    const auto n = counts.safe + counts.unsafe + counts.unresolved;
    if (n == 0) {
        return {source, 0, 0.0, 0.0, 1.0, 0.0, 0.0};
    }
    const double sample_count = static_cast<double>(n);
    return {
        source,
        n,
        static_cast<double>(counts.safe) / sample_count,
        static_cast<double>(wilson_lower(counts.safe, n)),
        static_cast<double>(1.0L - wilson_lower(counts.unsafe, n)),
        static_cast<double>(counts.unsafe) / sample_count,
        static_cast<double>(counts.unresolved) / sample_count,
    };
}

MassEstimate make_exact_mass(double certified_mass, double possible_safe_upper) {
    if (!probability(certified_mass) || !probability(possible_safe_upper) || certified_mass > possible_safe_upper) {
        throw std::invalid_argument("invalid exact safety mass bounds");
    }
    return {MassSource::exact_support, 0, certified_mass, certified_mass, possible_safe_upper,
            1.0 - possible_safe_upper, possible_safe_upper - certified_mass};
}

std::string_view mass_source_name(MassSource source) {
    switch (source) {
        case MassSource::exact_support: return "exact_support";
        case MassSource::action_audit: return "action_audit";
        case MassSource::group_forecast: return "group_forecast";
    }
    throw std::invalid_argument("unknown mass source");
}

}  // namespace acr
