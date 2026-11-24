#include "acr_controller.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void near(double value, double target, double tolerance = 1e-12) {
    require(std::abs(value - target) <= tolerance, "numerical assertion failed");
}

template<class Test>
void rejects(Test test) {
    try {
        test();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("invalid input was accepted");
}

} // namespace

int main() try {
    using namespace acr;
    const auto forecast = estimate_mass({70, 20, 10});
    require(forecast.source == MassSource::group_forecast && forecast.samples == 100, "forecast metadata");
    near(forecast.z_point, 0.7);
    near(forecast.unsafe_fraction, 0.2);
    near(forecast.unresolved_fraction, 0.1);
    near(forecast.z_lower, 0.6201678660558755);
    near(forecast.z_possible_upper, 0.8574981763397121);
    const auto all_safe = estimate_mass({100, 0, 0}, MassSource::action_audit);
    near(all_safe.z_lower, 100.0 / (100.0 + 1.6448536269514727 * 1.6448536269514727));
    near(all_safe.z_possible_upper, 1);
    const auto all_unknown = estimate_mass({0, 0, 10});
    near(all_unknown.z_lower, 0);
    near(all_unknown.z_possible_upper, 1);
    near(estimate_mass({}).z_possible_upper, 1);
    require(choose_plan(all_unknown).k == 0, "all unknown should use witness directly");

    const auto direct = plan_for_k(make_exact_mass(0.6), 0);
    require(direct.k == 0 && !direct.meets_forecast_target, "direct witness should not meet the proposal target");
    near(direct.tv_proxy, 1);
    near(direct.fallback_probability, 1);
    near(direct.expected_proposals, 0);
    near(direct.normalized_work, 0);
    near(direct.objective, 0.8);

    const auto medium = choose_plan(make_exact_mass(0.6));
    require(medium.k == 4 && medium.meets_forecast_target, "0.60 needs four draws");
    near(medium.fallback_probability, 0.0256);
    near(medium.expected_proposals, 1.624);
    near(medium.tv_proxy, 0.4);
    near(plan_for_k(make_exact_mass(0.6), 1).tv_proxy, medium.tv_proxy);
    const auto high = choose_plan(make_exact_mass(0.9));
    require(high.k == 2 && high.meets_forecast_target, "0.90 needs two draws");
    near(high.fallback_probability, 0.01);
    near(high.expected_proposals, 1.1);
    require(choose_plan(make_exact_mass(0.95)).k == 1, "0.95 is on the one-draw threshold");
    require(choose_plan(make_exact_mass(0.10)).k == 6, "balanced objective for unattainable target");
    require(choose_plan(make_exact_mass(0.01)).k == 0, "very low support should avoid futile proposals");
    require(choose_plan(make_exact_mass(1)).k == 1, "full safe support needs one draw");

    const auto exhausted = plan_for_k(make_exact_mass(0), hard_max_proposals);
    near(exhausted.fallback_probability, 1);
    near(exhausted.expected_proposals, hard_max_proposals);
    OnlineBudget budget;
    require(budget.candidate_checks == 512 && budget.refill_nodes == 8192 && budget.states == 128 &&
            budget.soft_thread_cpu_microseconds == 2000, "default online limits");

    rejects([] { (void)make_exact_mass(std::numeric_limits<double>::quiet_NaN()); });
    rejects([] { (void)make_exact_mass(.8, .7); });
    rejects([] { (void)estimate_mass({std::numeric_limits<std::uint64_t>::max(), 1, 0}); });
    rejects([] { (void)estimate_mass({1, 0, 0}, MassSource::exact_support); });
    rejects([] { (void)plan_for_k(make_exact_mass(.5), 7); });
    rejects([] { (void)choose_plan(make_exact_mass(.5), {.max_k = 0}); });

    std::cout << "ACR numerical smoke PASS: Wilson bounds, fixed support, K planning and online caps\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
