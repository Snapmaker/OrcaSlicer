#include "LocalZSchedule.hpp"
#include "../LocalZOrderOptimizer.hpp"
#include "../libslic3r.h"

#include <cmath>
#include <limits>
#include <map>
#include <tuple>

namespace Slic3r {
namespace {

bool contains(const std::vector<unsigned int>& tools, unsigned int tool)
{
    return std::find(tools.begin(), tools.end(), tool) != tools.end();
}

LocalZSchedule schedule_by_height(const std::vector<LocalZSchedulePass>& passes, const std::vector<size_t>& order, int starting_tool)
{
    LocalZSchedule result;
    result.final_extruder = starting_tool;
    auto group_end        = [&](size_t begin) {
        size_t end = begin + 1;
        while (end < order.size() && std::abs(passes[order[begin]].print_z - passes[order[end]].print_z) <= EPSILON)
            ++end;
        return end;
    };
    auto group_tools = [&](size_t begin, size_t end) {
        std::vector<unsigned int> tools;
        for (size_t i = begin; i < end; ++i)
            for (unsigned int tool : passes[order[i]].extruders)
                if (!contains(tools, tool))
                    tools.push_back(tool);
        return tools;
    };
    for (size_t begin = 0; begin < order.size();) {
        const size_t end            = group_end(begin);
        const auto   tools          = group_tools(begin, end);
        const auto   next_tools     = end < order.size() ? group_tools(end, group_end(end)) : std::vector<unsigned int>();
        int          preferred_last = -1;
        for (unsigned int tool : tools)
            if (contains(next_tools, tool)) {
                preferred_last = int(tool);
                break;
            }
        for (unsigned int tool : LocalZOrderOptimizer::order_bucket_extruders(tools, result.final_extruder, preferred_last)) {
            LocalZScheduledPhase phase{tool, passes[order[begin]].print_z, {}};
            for (size_t i = begin; i < end; ++i)
                if (contains(passes[order[i]].extruders, tool))
                    phase.passes.push_back(order[i]);
            result.phases.push_back(std::move(phase));
            result.final_extruder = int(tool);
        }
        begin = end;
    }
    return result;
}

} // namespace

LocalZSchedule build_local_z_schedule(const std::vector<LocalZSchedulePass>& passes, int starting_extruder)
{
    std::vector<size_t> order;
    for (size_t i = 0; i < passes.size(); ++i)
        if (!passes[i].extruders.empty())
            order.push_back(i);
    const auto ordered_before = [&](size_t lhs, size_t rhs) {
        const auto& a = passes[lhs];
        const auto& b = passes[rhs];
        return std::tie(a.print_z, a.object_layer, a.pass_index) < std::tie(b.print_z, b.object_layer, b.pass_index);
    };
    std::sort(order.begin(), order.end(), ordered_before);
    if (std::any_of(order.begin(), order.end(), [&](size_t i) { return passes[i].dependency_group == 0; }))
        return schedule_by_height(passes, order, starting_extruder);

    struct PassState
    {
        size_t                    source;
        std::vector<unsigned int> remaining;
        size_t                    successor = size_t(-1);
        bool                      ready     = false;
        bool                      completed = false;
    };
    std::vector<PassState>                                   states;
    std::map<std::pair<size_t, size_t>, std::vector<size_t>> chains;
    for (size_t source : order) {
        const auto& pass = passes[source];
        chains[{pass.object_layer, pass.dependency_group}].push_back(states.size());
        states.push_back({source, pass.extruders});
    }
    for (auto& entry : chains) {
        auto& chain = entry.second;
        std::sort(chain.begin(), chain.end(), [&](size_t lhs, size_t rhs) {
            const auto& a = passes[states[lhs].source];
            const auto& b = passes[states[rhs].source];
            if (a.dependency_order != b.dependency_order)
                return a.dependency_order < b.dependency_order;
            if (std::abs(a.print_z - b.print_z) > EPSILON)
                return a.print_z < b.print_z;
            return a.pass_index < b.pass_index;
        });
        states[chain.front()].ready = true;
        for (size_t i = 1; i < chain.size(); ++i)
            states[chain[i - 1]].successor = chain[i];
    }

    LocalZSchedule result;
    result.final_extruder = starting_extruder;
    size_t completed      = 0;
    while (completed < states.size()) {
        double ready_z = std::numeric_limits<double>::infinity();
        for (const auto& state : states)
            if (state.ready && !state.completed)
                ready_z = std::min(ready_z, passes[state.source].print_z);
        const auto ready = [&](const PassState& state) {
            return state.ready && !state.completed && std::abs(passes[state.source].print_z - ready_z) <= EPSILON;
        };
        std::vector<unsigned int> tools;
        for (const auto& state : states)
            if (ready(state))
                for (unsigned int tool : state.remaining)
                    if (!contains(tools, tool))
                        tools.push_back(tool);
        if (tools.empty()) {
            result                          = schedule_by_height(passes, order, starting_extruder);
            result.used_dependency_fallback = true;
            return result;
        }

        int    chosen     = -1;
        size_t best_ready = 0, best_future = 0;
        if (result.final_extruder >= 0 && contains(tools, unsigned(result.final_extruder)))
            chosen = result.final_extruder;
        else
            for (unsigned int tool : tools) {
                size_t ready_count = 0, future_count = 0;
                for (const auto& state : states)
                    if (!state.completed && contains(state.remaining, tool)) {
                        ++future_count;
                        if (ready(state))
                            ++ready_count;
                    }
                if (chosen < 0 || ready_count > best_ready ||
                    (ready_count == best_ready &&
                     (future_count > best_future || (future_count == best_future && tool < unsigned(chosen))))) {
                    chosen      = int(tool);
                    best_ready  = ready_count;
                    best_future = future_count;
                }
            }

        LocalZScheduledPhase phase{unsigned(chosen), ready_z, {}};
        std::vector<size_t>  newly_completed;
        for (size_t i = 0; i < states.size(); ++i) {
            auto& state = states[i];
            if (!ready(state))
                continue;
            auto found = std::find(state.remaining.begin(), state.remaining.end(), unsigned(chosen));
            if (found == state.remaining.end())
                continue;
            phase.passes.push_back(state.source);
            state.remaining.erase(found);
            if (state.remaining.empty())
                newly_completed.push_back(i);
        }
        std::sort(phase.passes.begin(), phase.passes.end(), [&](size_t lhs, size_t rhs) {
            const auto& a = passes[lhs];
            const auto& b = passes[rhs];
            if (std::abs(a.print_z - b.print_z) > EPSILON)
                return a.print_z < b.print_z;
            return std::tie(a.object_layer, a.pass_index) < std::tie(b.object_layer, b.pass_index);
        });
        // Match emission's phase Z, including the existing epsilon grouping.
        phase.print_z = passes[phase.passes.front()].print_z;
        result.phases.push_back(std::move(phase));
        result.final_extruder = chosen;
        for (size_t i : newly_completed) {
            auto& state     = states[i];
            state.ready     = false;
            state.completed = true;
            ++completed;
            if (state.successor != size_t(-1))
                states[state.successor].ready = true;
        }
    }
    return result;
}

} // namespace Slic3r
