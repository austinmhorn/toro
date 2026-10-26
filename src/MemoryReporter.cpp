#include "toro/MemoryReporter.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace toro {
namespace {

std::string origin_text(const IROrigin& origin)
{
    return origin.module_name + ':' + std::to_string(origin.location.line)
        + ':' + std::to_string(origin.location.column);
}

std::string_view arc_operation_name(IRArcOperationKind kind)
{
    switch (kind) {
    case IRArcOperationKind::Retain: return "retain";
    case IRArcOperationKind::Release: return "release";
    case IRArcOperationKind::DestroyFinalize: return "destroy-finalize";
    case IRArcOperationKind::ManagedAssignment: return "managed-assignment";
    case IRArcOperationKind::OwnedTemporary: return "owned-temporary";
    case IRArcOperationKind::BorrowedTemporary: return "borrow";
    case IRArcOperationKind::OwnershipTransfer: return "transfer";
    }
    return "unknown";
}

MemoryArcOperationReport operation_report(const IRArcOperation& operation)
{
    return MemoryArcOperationReport{
        operation.kind,
        operation.value,
        operation.origin,
        operation.reason,
    };
}

void count_escape(IREscapeState state, std::size_t& non_escaping,
    std::size_t& escaping, std::size_t& unknown)
{
    if (state == IREscapeState::DoesNotEscape) {
        ++non_escaping;
    } else if (state == IREscapeState::Escapes) {
        ++escaping;
    } else {
        ++unknown;
    }
}

void count_arc_before(
    const IRArcOperation& operation, MemoryArcCounts& counts)
{
    if (operation.kind == IRArcOperationKind::Retain) {
        ++counts.retains_before;
    } else if (operation.kind == IRArcOperationKind::Release) {
        ++counts.releases_before;
    }
}

void count_arc_after(
    const IRArcOperation& operation, MemoryArcCounts& counts)
{
    if (operation.elided) {
        ++counts.elided_operations;
        return;
    }
    if (operation.kind == IRArcOperationKind::Retain) {
        ++counts.retains_after;
    } else if (operation.kind == IRArcOperationKind::Release) {
        ++counts.releases_after;
    } else if (operation.kind == IRArcOperationKind::OwnershipTransfer) {
        ++counts.ownership_transfers;
    } else if (operation.kind == IRArcOperationKind::BorrowedTemporary) {
        ++counts.inferred_borrows;
    }
}

void add_counts(MemoryArcCounts& total, const MemoryArcCounts& value)
{
    total.retains_before += value.retains_before;
    total.retains_after += value.retains_after;
    total.releases_before += value.releases_before;
    total.releases_after += value.releases_after;
    total.elided_operations += value.elided_operations;
    total.ownership_transfers += value.ownership_transfers;
    total.inferred_borrows += value.inferred_borrows;
}

auto origin_key(const IROrigin& origin)
{
    return std::tie(
        origin.module_name, origin.location.line, origin.location.column);
}

std::string cycle_key(const MemoryCycleReport& cycle)
{
    std::string key;
    for (const auto& edge : cycle.edges) {
        key += edge.owner_type + '\n' + edge.field_path + '\n'
            + edge.target_type + '\n' + origin_text(edge.origin) + '\n';
    }
    return key;
}

void append_operation(
    std::string& output, const MemoryArcOperationReport& operation,
    std::string_view indent)
{
    output += std::string(indent) + std::string(arc_operation_name(operation.kind));
    if (!operation.value.empty()) output += ' ' + operation.value;
    output += " @" + origin_text(operation.origin);
    if (!operation.reason.empty()) output += " ; " + operation.reason;
    output += '\n';
}

} // namespace

MemoryReport MemoryReporter::inspect(const IRProgram& program) const
{
    MemoryReport report;
    report.entry_module = program.entry_module;

    for (const auto& allocation : program.escape_allocations) {
        auto causes = allocation.causes;
        std::ranges::sort(causes,
            [](const auto& left, const auto& right) {
                if (origin_key(left.origin) != origin_key(right.origin)) {
                    return origin_key(left.origin) < origin_key(right.origin);
                }
                if (left.reason != right.reason) {
                    return left.reason < right.reason;
                }
                if (left.causes_escape != right.causes_escape) {
                    return left.causes_escape < right.causes_escape;
                }
                if (left.uncertain != right.uncertain) {
                    return left.uncertain < right.uncertain;
                }
                return left.detail < right.detail;
            });
        report.allocations.push_back(MemoryAllocationReport{
            allocation.identity,
            allocation.callable_identity,
            allocation.type,
            allocation.origin,
            allocation.state,
            allocation.storage,
            allocation.storage_reason,
            std::move(causes),
        });
    }
    std::ranges::sort(
        report.allocations, {}, &MemoryAllocationReport::identity);

    for (const auto& schedule : program.arc_schedules) {
        CallableMemoryReport callable;
        callable.callable_identity = schedule.callable_identity;
        for (const auto& operation : schedule.unoptimized_operations) {
            count_arc_before(operation, callable.arc);
        }
        for (const auto& operation : schedule.optimized_operations) {
            count_arc_after(operation, callable.arc);
            if (operation.elided) {
                callable.elided_arc_operations.push_back(
                    operation_report(operation));
            } else if (operation.kind
                == IRArcOperationKind::OwnershipTransfer) {
                callable.ownership_transfers.push_back(
                    operation_report(operation));
            } else if (operation.kind
                == IRArcOperationKind::BorrowedTemporary) {
                callable.inferred_borrows.push_back(
                    operation_report(operation));
            } else {
                callable.final_arc_operations.push_back(
                    operation_report(operation));
            }
        }
        report.callables.push_back(std::move(callable));
    }

    std::ranges::sort(
        report.callables, {}, &CallableMemoryReport::callable_identity);
    for (const auto& allocation : report.allocations) {
        ++report.summary.managed_allocation_sites;
        if (allocation.storage == IRStorageClass::Stack) {
            ++report.summary.stack_allocation_sites;
        } else {
            ++report.summary.heap_allocation_sites;
        }
        count_escape(allocation.escape_state,
            report.summary.non_escaping_sites,
            report.summary.escaping_sites,
            report.summary.unknown_escape_sites);
        auto callable = std::ranges::find_if(report.callables,
            [&](const auto& candidate) {
                return candidate.callable_identity
                    == allocation.callable_identity;
            });
        if (callable == report.callables.end()) {
            CallableMemoryReport created;
            created.callable_identity = allocation.callable_identity;
            report.callables.push_back(std::move(created));
            callable = std::prev(report.callables.end());
        }
        ++callable->allocation_sites;
        if (allocation.storage == IRStorageClass::Stack) {
            ++callable->stack_allocation_sites;
        } else {
            ++callable->heap_allocation_sites;
        }
        count_escape(allocation.escape_state, callable->non_escaping_sites,
            callable->escaping_sites, callable->unknown_escape_sites);
    }
    std::ranges::sort(
        report.callables, {}, &CallableMemoryReport::callable_identity);
    for (const auto& callable : report.callables) {
        add_counts(report.summary.arc, callable.arc);
    }

    for (const auto& declaration : program.declarations) {
        for (const auto& field : declaration.fields) {
            if (!field.is_weak) continue;
            report.weak_fields.push_back(MemoryWeakFieldReport{
                declaration.identity,
                field.name,
                field.type,
                field.origin,
            });
        }
    }
    std::ranges::sort(report.weak_fields,
        [](const auto& left, const auto& right) {
            return std::tie(left.owner_identity, left.field_name)
                < std::tie(right.owner_identity, right.field_name);
        });
    report.summary.weak_fields = report.weak_fields.size();

    for (const auto& cycle : program.strong_reference_cycles) {
        report.cycles.push_back(MemoryCycleReport{cycle.edges});
    }
    std::ranges::sort(report.cycles,
        [](const auto& left, const auto& right) {
            return cycle_key(left) < cycle_key(right);
        });
    report.summary.possible_strong_cycles = report.cycles.size();
    return report;
}

std::string MemoryReporter::format(const MemoryReport& report) const
{
    const auto& summary = report.summary;
    std::string output = "toro-memory-report 1\nentry "
        + report.entry_module + "\n";
    output += "scope static-compiler-analysis\n";
    output += "summary\n";
    output += "  managed-allocation-sites total "
        + std::to_string(summary.managed_allocation_sites) + " stack "
        + std::to_string(summary.stack_allocation_sites) + " heap "
        + std::to_string(summary.heap_allocation_sites) + "\n";
    output += "  escape-sites non-escaping "
        + std::to_string(summary.non_escaping_sites) + " escaping "
        + std::to_string(summary.escaping_sites) + " unknown "
        + std::to_string(summary.unknown_escape_sites) + "\n";
    output += "  static-arc-retains "
        + std::to_string(summary.arc.retains_before) + " -> "
        + std::to_string(summary.arc.retains_after) + "\n";
    output += "  static-arc-releases "
        + std::to_string(summary.arc.releases_before) + " -> "
        + std::to_string(summary.arc.releases_after) + "\n";
    output += "  elided-static-arc-operations "
        + std::to_string(summary.arc.elided_operations) + "\n";
    output += "  static-ownership-transfers "
        + std::to_string(summary.arc.ownership_transfers) + "\n";
    output += "  static-inferred-borrows "
        + std::to_string(summary.arc.inferred_borrows) + "\n";
    output += "  possible-strong-cycles "
        + std::to_string(summary.possible_strong_cycles) + "\n";
    output += "  weak-fields " + std::to_string(summary.weak_fields) + "\n";
    output += "  captures " + std::to_string(summary.captures) + "\n";

    output += "allocations\n";
    if (report.allocations.empty()) output += "  none\n";
    for (const auto& allocation : report.allocations) {
        output += "  allocation " + allocation.identity + " @"
            + origin_text(allocation.origin) + "\n";
        output += "    callable " + allocation.callable_identity + "\n";
        output += "    concrete-type " + type_name(allocation.type.type) + "\n";
        output += "    semantic-managed-construction yes\n";
        output += "    escape "
            + std::string(ir_escape_state_name(allocation.escape_state)) + "\n";
        output += "    runtime-storage "
            + std::string(ir_storage_class_name(allocation.storage)) + "\n";
        output += "    general-heap-allocation "
            + std::string(allocation.storage == IRStorageClass::Heap
                    ? "yes" : "no")
            + "\n";
        if (!allocation.storage_reason.empty()) {
            output += "    storage-reason " + allocation.storage_reason + "\n";
        }
        output += "    escape-causes\n";
        if (allocation.escape_causes.empty()) output += "      none\n";
        for (const auto& cause : allocation.escape_causes) {
            output += "      " + std::string(ir_escape_reason_name(cause.reason))
                + " @" + origin_text(cause.origin);
            if (!cause.causes_escape) output += " non-escaping";
            if (cause.uncertain) output += " uncertain";
            if (!cause.detail.empty()) output += " ; " + cause.detail;
            output += '\n';
        }
    }

    output += "callables\n";
    if (report.callables.empty()) output += "  none\n";
    for (const auto& callable : report.callables) {
        output += "  callable " + callable.callable_identity + "\n";
        output += "    allocation-sites total "
            + std::to_string(callable.allocation_sites) + " stack "
            + std::to_string(callable.stack_allocation_sites) + " heap "
            + std::to_string(callable.heap_allocation_sites) + "\n";
        output += "    escape-sites non-escaping "
            + std::to_string(callable.non_escaping_sites) + " escaping "
            + std::to_string(callable.escaping_sites) + " unknown "
            + std::to_string(callable.unknown_escape_sites) + "\n";
        output += "    static-arc-retains "
            + std::to_string(callable.arc.retains_before) + " -> "
            + std::to_string(callable.arc.retains_after) + "\n";
        output += "    static-arc-releases "
            + std::to_string(callable.arc.releases_before) + " -> "
            + std::to_string(callable.arc.releases_after) + "\n";
        output += "    elided-static-arc-operations "
            + std::to_string(callable.arc.elided_operations) + "\n";
        output += "    final-static-arc-plan\n";
        if (callable.final_arc_operations.empty()) output += "      none\n";
        for (const auto& operation : callable.final_arc_operations) {
            append_operation(output, operation, "      ");
        }
        output += "    elided-static-arc\n";
        if (callable.elided_arc_operations.empty()) output += "      none\n";
        for (const auto& operation : callable.elided_arc_operations) {
            append_operation(output, operation, "      ");
        }
        output += "    ownership-transfers\n";
        if (callable.ownership_transfers.empty()) output += "      none\n";
        for (const auto& operation : callable.ownership_transfers) {
            append_operation(output, operation, "      ");
        }
        output += "    inferred-borrows\n";
        if (callable.inferred_borrows.empty()) output += "      none\n";
        for (const auto& operation : callable.inferred_borrows) {
            append_operation(output, operation, "      ");
        }
    }

    output += "weak-fields\n";
    if (report.weak_fields.empty()) output += "  none\n";
    for (const auto& field : report.weak_fields) {
        output += "  " + field.owner_identity + '.' + field.field_name
            + ": " + type_name(field.type.type) + " @"
            + origin_text(field.origin) + " ; non-owning\n";
    }

    output += "possible-strong-cycles\n";
    if (report.cycles.empty()) output += "  none\n";
    for (std::size_t index = 0; index < report.cycles.size(); ++index) {
        output += "  cycle " + std::to_string(index + 1) + " advisory\n";
        for (const auto& edge : report.cycles[index].edges) {
            output += "    " + edge.owner_display + '.' + edge.field_path
                + " -> " + edge.target_display + " @"
                + origin_text(edge.origin) + "\n";
        }
    }
    return output;
}

std::string MemoryReporter::report(const IRProgram& program) const
{
    return format(inspect(program));
}

} // namespace toro
