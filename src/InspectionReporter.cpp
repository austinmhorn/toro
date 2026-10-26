#include "toro/InspectionReporter.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>

namespace toro {
namespace {

bool same_origin(const IROrigin& left, const IROrigin& right)
{
    return left.module_name == right.module_name
        && left.location.line == right.location.line
        && left.location.column == right.location.column;
}

std::string origin_text(const IROrigin& origin)
{
    return origin.module_name + ':' + std::to_string(origin.location.line)
        + ':' + std::to_string(origin.location.column);
}

bool inspectable_effect(IREffect effect)
{
    return effect == IREffect::Block || effect == IREffect::FilesystemIO
        || effect == IREffect::Synchronize;
}

std::size_t effect_rank(IREffect effect)
{
    switch (effect) {
    case IREffect::Block: return 0;
    case IREffect::FilesystemIO: return 1;
    case IREffect::Synchronize: return 2;
    default: return 3;
    }
}

std::string complete_type_name(const IRType& type)
{
    return type_name(type.type);
}

} // namespace

IRInspectionReport InspectionReporter::inspect(const IRProgram& program) const
{
    IRInspectionReport report;
    report.entry_module = program.entry_module;

    for (const auto& allocation : program.escape_allocations) {
        report.allocations.push_back(IRInspectionAllocation{
            allocation.identity,
            allocation.callable_identity,
            allocation.type,
            allocation.origin,
            allocation.state,
            allocation.storage,
            allocation.storage_reason,
        });
    }

    for (const auto& summary : program.effect_summaries) {
        for (const auto& site : summary.local_effect_sites) {
            if (!inspectable_effect(site.effect)) continue;
            report.effects.push_back(IRInspectionEffect{
                summary.callable_identity,
                site.effect,
                site.origin,
                site.operation_name,
            });
        }
        for (const auto& edge : summary.call_edges) {
            if (edge.operation_kind != IROperationKind::VirtualCall
                && edge.operation_kind != IROperationKind::InterfaceCall) {
                continue;
            }
            auto existing = std::ranges::find_if(report.dispatches,
                [&](const IRInspectionDispatch& dispatch) {
                    return dispatch.callable_identity == summary.callable_identity
                        && dispatch.operation_kind == edge.operation_kind
                        && dispatch.operation_name == edge.operation_name
                        && same_origin(dispatch.origin, edge.origin);
                });
            if (existing == report.dispatches.end()) {
                report.dispatches.push_back(IRInspectionDispatch{
                    summary.callable_identity,
                    edge.origin,
                    edge.operation_kind,
                    edge.operation_name,
                    {edge.callable_identity},
                });
            } else {
                existing->target_identities.push_back(edge.callable_identity);
            }
        }
    }

    for (const auto& schedule : program.arc_schedules) {
        IRInspectionArc fact;
        fact.callable_identity = schedule.callable_identity;
        for (const auto& operation : schedule.unoptimized_operations) {
            if (operation.kind == IRArcOperationKind::Retain) {
                ++fact.retains_before;
            } else if (operation.kind == IRArcOperationKind::Release) {
                ++fact.releases_before;
            }
        }
        for (const auto& operation : schedule.optimized_operations) {
            if (operation.elided) ++fact.elided_operations;
            if (operation.kind == IRArcOperationKind::Retain
                && !operation.elided) {
                ++fact.retains_after;
            } else if (operation.kind == IRArcOperationKind::Release
                && !operation.elided) {
                ++fact.releases_after;
            } else if (operation.kind
                == IRArcOperationKind::OwnershipTransfer) {
                ++fact.ownership_transfers;
            } else if (operation.kind
                == IRArcOperationKind::BorrowedTemporary) {
                ++fact.inferred_borrows;
            }
        }
        if (fact.retains_before != 0 || fact.releases_before != 0
            || fact.elided_operations != 0 || fact.ownership_transfers != 0
            || fact.inferred_borrows != 0) {
            report.arc.push_back(std::move(fact));
        }
    }

    std::ranges::sort(report.allocations, {},
        &IRInspectionAllocation::identity);
    std::ranges::sort(report.dispatches,
        [](const IRInspectionDispatch& left,
            const IRInspectionDispatch& right) {
            if (left.callable_identity != right.callable_identity) {
                return left.callable_identity < right.callable_identity;
            }
            if (left.origin.module_name != right.origin.module_name) {
                return left.origin.module_name < right.origin.module_name;
            }
            if (left.origin.location.line != right.origin.location.line) {
                return left.origin.location.line < right.origin.location.line;
            }
            if (left.origin.location.column != right.origin.location.column) {
                return left.origin.location.column < right.origin.location.column;
            }
            if (left.operation_kind != right.operation_kind) {
                return left.operation_kind < right.operation_kind;
            }
            return left.operation_name < right.operation_name;
        });
    for (auto& dispatch : report.dispatches) {
        std::ranges::sort(dispatch.target_identities);
        dispatch.target_identities.erase(
            std::ranges::unique(dispatch.target_identities).begin(),
            dispatch.target_identities.end());
    }
    std::ranges::sort(report.arc, {}, &IRInspectionArc::callable_identity);
    std::ranges::sort(report.effects,
        [](const IRInspectionEffect& left, const IRInspectionEffect& right) {
            if (left.callable_identity != right.callable_identity) {
                return left.callable_identity < right.callable_identity;
            }
            if (effect_rank(left.effect) != effect_rank(right.effect)) {
                return effect_rank(left.effect) < effect_rank(right.effect);
            }
            if (left.origin.module_name != right.origin.module_name) {
                return left.origin.module_name < right.origin.module_name;
            }
            if (left.origin.location.line != right.origin.location.line) {
                return left.origin.location.line < right.origin.location.line;
            }
            if (left.origin.location.column != right.origin.location.column) {
                return left.origin.location.column < right.origin.location.column;
            }
            return left.operation_name < right.operation_name;
        });
    return report;
}

std::string InspectionReporter::format(const IRInspectionReport& report) const
{
    std::size_t stack_allocations = 0;
    std::size_t heap_allocations = 0;
    for (const auto& allocation : report.allocations) {
        allocation.storage == IRStorageClass::Stack
            ? ++stack_allocations : ++heap_allocations;
    }
    std::size_t blocking_effects = 0;
    std::size_t synchronization_effects = 0;
    for (const auto& effect : report.effects) {
        if (effect.effect == IREffect::Synchronize) {
            ++synchronization_effects;
        } else {
            ++blocking_effects;
        }
    }

    std::string output = "toro-inspect 1\nentry " + report.entry_module + "\n";
    output += "summary\n";
    output += "  allocations stack " + std::to_string(stack_allocations)
        + " heap " + std::to_string(heap_allocations) + "\n";
    output += "  dynamic-dispatch-sites "
        + std::to_string(report.dispatches.size()) + "\n";
    output += "  captures " + std::to_string(report.capture_count) + "\n";
    output += "  blocking-effects " + std::to_string(blocking_effects) + "\n";
    output += "  synchronization-effects "
        + std::to_string(synchronization_effects) + "\n";

    output += "allocations\n";
    if (report.allocations.empty()) output += "  none\n";
    for (const auto& allocation : report.allocations) {
        output += "  allocation " + allocation.identity + " @"
            + origin_text(allocation.origin) + "\n";
        output += "    callable " + allocation.callable_identity + "\n";
        output += "    type " + complete_type_name(allocation.type) + "\n";
        output += "    escape "
            + std::string(ir_escape_state_name(allocation.escape_state)) + "\n";
        output += "    storage "
            + std::string(ir_storage_class_name(allocation.storage));
        if (!allocation.reason.empty()) output += " ; " + allocation.reason;
        output += '\n';
    }

    output += "dynamic-dispatch\n";
    if (report.dispatches.empty()) output += "  none\n";
    for (const auto& dispatch : report.dispatches) {
        output += "  "
            + std::string(dispatch.operation_kind == IROperationKind::VirtualCall
                ? "virtual" : "interface")
            + " " + dispatch.operation_name + " in "
            + dispatch.callable_identity + " @" + origin_text(dispatch.origin)
            + "\n";
        for (const auto& target : dispatch.target_identities) {
            output += "    target " + target + "\n";
        }
    }

    output += "arc\n";
    if (report.arc.empty()) output += "  none\n";
    for (const auto& arc : report.arc) {
        output += "  callable " + arc.callable_identity + "\n";
        output += "    retains " + std::to_string(arc.retains_before) + " -> "
            + std::to_string(arc.retains_after) + "\n";
        output += "    releases " + std::to_string(arc.releases_before) + " -> "
            + std::to_string(arc.releases_after) + "\n";
        output += "    elided-operations "
            + std::to_string(arc.elided_operations) + "\n";
        output += "    ownership-transfers "
            + std::to_string(arc.ownership_transfers) + "\n";
        output += "    inferred-borrows "
            + std::to_string(arc.inferred_borrows) + "\n";
    }

    output += "captures\n";
    output += report.capture_count == 0 ? "  none\n" : "  represented\n";

    output += "blocking-and-synchronization\n";
    if (report.effects.empty()) output += "  none\n";
    for (const auto& effect : report.effects) {
        output += "  " + std::string(ir_effect_name(effect.effect)) + " in "
            + effect.callable_identity + " @" + origin_text(effect.origin);
        if (!effect.operation_name.empty()) {
            output += " ; operation " + effect.operation_name;
        }
        output += '\n';
    }
    return output;
}

std::string InspectionReporter::report(const IRProgram& program) const
{
    return format(inspect(program));
}

} // namespace toro
