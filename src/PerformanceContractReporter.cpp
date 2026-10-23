#include "toro/PerformanceContractReporter.hpp"

#include <algorithm>
#include <deque>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace toro {
namespace {

struct PathStep {
    std::string caller;
    const IREffectCallEdge* edge{nullptr};
};

struct Explanation {
    IRPerformanceContractStatus status{IRPerformanceContractStatus::Satisfied};
    IREffect effect{IREffect::Allocate};
    IROrigin origin;
    std::string detail;
    std::string terminal_callable;
    const IRLocalEffectSite* site{nullptr};
    const IRAllocationEscape* allocation{nullptr};
    std::vector<PathStep> path;
};

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

std::string status_name(IRPerformanceContractStatus status)
{
    switch (status) {
    case IRPerformanceContractStatus::Satisfied: return "satisfied";
    case IRPerformanceContractStatus::Violated: return "violated";
    case IRPerformanceContractStatus::Unproven: return "unproven";
    }
    return "unproven";
}

std::string effect_description(IREffect effect)
{
    switch (effect) {
    case IREffect::Allocate: return "general heap allocation";
    case IREffect::Escape: return "escaping temporary or reference";
    case IREffect::Block: return "blocking operation";
    case IREffect::FilesystemIO: return "blocking filesystem I/O";
    case IREffect::Synchronize: return "hidden synchronization";
    case IREffect::UnknownExternal:
        return "foreign or unresolved behavior has no performance contract";
    default: return std::string(ir_effect_name(effect));
    }
}

bool is_specialization_of(
    std::string_view candidate,
    std::string_view declaration_identity)
{
    return candidate.size() > declaration_identity.size()
        && candidate.starts_with(declaration_identity)
        && candidate[declaration_identity.size()] == '<';
}

class ReportBuilder {
public:
    explicit ReportBuilder(const IRProgram& program)
        : program_(program)
    {
        for (const auto& declaration : program.declarations) {
            declarations_.emplace(declaration.identity, &declaration);
        }
        for (const auto& summary : program.effect_summaries) {
            summaries_.emplace(summary.callable_identity, &summary);
        }
        for (const auto& schedule : program.arc_schedules) {
            schedules_.emplace(schedule.callable_identity, &schedule);
        }
    }

    std::string report() const
    {
        std::string output = "performance-contracts\n";
        auto results = sorted_results();
        if (results.empty()) return output + "  none\n";
        for (const auto* result : results) append_result(*result, output);
        return output;
    }

    std::vector<Diagnostic> diagnostics() const
    {
        std::vector<Diagnostic> output;
        for (const auto* result : sorted_results()) {
            if (result->status == IRPerformanceContractStatus::Satisfied) continue;
            const auto* declaration = declaration_for(result->callable_identity);
            if (!declaration) continue;
            Diagnostic diagnostic;
            diagnostic.severity = DiagnosticSeverity::Error;
            diagnostic.code = "performance-contract";
            diagnostic.message = result->status
                    == IRPerformanceContractStatus::Violated
                ? "performance callable '" + display_name(result->callable_identity)
                    + "' violates its contract"
                : "performance contract for callable '"
                    + display_name(result->callable_identity)
                    + "' cannot be proven";
            diagnostic.source = DiagnosticSource{
                declaration->origin.module_name, declaration->origin.location};
            diagnostic.notes.push_back(DiagnosticNote{
                "contract declared here",
                DiagnosticSource{declaration->origin.module_name,
                    declaration->origin.location},
            });
            for (const auto& explanation : explanations(*result)) {
                for (const auto& step : explanation.path) {
                    std::string message = "'" + display_name(step.caller)
                        + "' calls '"
                        + display_name(step.edge->callable_identity) + "'";
                    if (is_dynamic(*step.edge)) {
                        message = "dynamic call in '" + display_name(step.caller)
                            + "' may target '"
                            + display_name(step.edge->callable_identity) + "'";
                    }
                    diagnostic.notes.push_back(DiagnosticNote{
                        std::move(message),
                        DiagnosticSource{step.edge->origin.module_name,
                            step.edge->origin.location},
                    });
                }
                std::string reason = effect_description(explanation.effect);
                if (!explanation.detail.empty()) {
                    reason += " (" + explanation.detail + ')';
                }
                reason += explanation.status
                        == IRPerformanceContractStatus::Unproven
                    ? " prevents contract proof"
                    : " is prohibited in a performance function";
                diagnostic.notes.push_back(DiagnosticNote{
                    std::move(reason),
                    DiagnosticSource{explanation.origin.module_name,
                        explanation.origin.location},
                });
            }
            output.push_back(std::move(diagnostic));
        }
        return output;
    }

private:
    const IRDeclaration* declaration_for(std::string_view identity) const
    {
        if (const auto found = declarations_.find(std::string(identity));
            found != declarations_.end()) return found->second;
        for (const auto& declaration : program_.declarations) {
            if (is_specialization_of(identity, declaration.identity)) {
                return &declaration;
            }
        }
        return nullptr;
    }

    std::string display_name(std::string_view identity) const
    {
        const auto* declaration = declaration_for(identity);
        if (!declaration) return std::string(identity);
        std::string result;
        if (!declaration->owner_identity.empty()) {
            if (const auto* owner = declaration_for(declaration->owner_identity)) {
                result = owner->name + ".";
            }
        }
        result += declaration->name;
        if (identity.size() > declaration->identity.size()) {
            result += identity.substr(declaration->identity.size());
        }
        return result;
    }

    std::string type_name(const IRType& type) const
    {
        if (const auto* declaration = declaration_for(type.type.name)) {
            return declaration->name;
        }
        return type.type.name;
    }

    std::vector<const IRPerformanceContractResult*> sorted_results() const
    {
        std::vector<const IRPerformanceContractResult*> results;
        for (const auto& result : program_.performance_contract_results) {
            results.push_back(&result);
        }
        std::ranges::sort(results, {}, [](const auto* result) {
            return result->callable_identity;
        });
        return results;
    }

    const IRAllocationEscape* allocation_at(
        std::string_view callable,
        const IROrigin& origin) const
    {
        const auto* declaration = declaration_for(callable);
        for (const auto& allocation : program_.escape_allocations) {
            if (same_origin(allocation.origin, origin)
                && (!declaration
                    || allocation.callable_identity == declaration->identity)) {
                return &allocation;
            }
        }
        return nullptr;
    }

    std::string escape_detail(const IROrigin& origin) const
    {
        for (const auto& allocation : program_.escape_allocations) {
            for (const auto& cause : allocation.causes) {
                if (cause.causes_escape && same_origin(cause.origin, origin)) {
                    return cause.detail.empty()
                        ? std::string(ir_escape_reason_name(cause.reason))
                        : cause.detail;
                }
            }
        }
        return {};
    }

    bool site_explains(
        std::string_view callable,
        const IRLocalEffectSite& site,
        const IRPerformanceContractCause& cause) const
    {
        if (site.effect != cause.effect) return false;
        if (site.effect != IREffect::Allocate) return true;
        if (site.operation_kind != IROperationKind::Construction) return true;
        const auto* allocation = allocation_at(callable, site.origin);
        return !allocation || allocation->storage == IRStorageClass::Heap;
    }

    std::optional<Explanation> find_explanation(
        std::string_view root,
        const IRPerformanceContractCause& cause) const
    {
        struct Pending {
            std::string callable;
            std::vector<PathStep> path;
        };
        std::deque<Pending> pending;
        std::unordered_set<std::string> visited;
        visited.emplace(root);
        if (cause.transitive && !cause.via_callable.empty()) {
            const auto root_summary = summaries_.find(std::string(root));
            if (root_summary != summaries_.end()) {
                for (const auto& edge : root_summary->second->call_edges) {
                    if (edge.callable_identity != cause.via_callable) continue;
                    visited.emplace(edge.callable_identity);
                    pending.push_back(Pending{edge.callable_identity,
                        {PathStep{std::string(root), &edge}}});
                    break;
                }
            }
        }
        if (pending.empty()) {
            pending.push_back(Pending{std::string(root), {}});
        }
        while (!pending.empty()) {
            auto current = std::move(pending.front());
            pending.pop_front();
            const auto found = summaries_.find(current.callable);
            if (found == summaries_.end()) continue;
            for (const auto& site : found->second->local_effect_sites) {
                if (!site_explains(current.callable, site, cause)) continue;
                Explanation result;
                result.status = cause.status;
                result.effect = cause.effect;
                result.origin = site.origin;
                result.detail = cause.detail;
                result.terminal_callable = current.callable;
                result.site = &site;
                result.path = std::move(current.path);
                if (site.effect == IREffect::Allocate) {
                    result.allocation = allocation_at(current.callable, site.origin);
                    if (result.detail.empty() && result.allocation) {
                        result.detail = result.allocation->storage_reason;
                    }
                } else if (site.effect == IREffect::Escape
                    && result.detail.empty()) {
                    result.detail = escape_detail(site.origin);
                }
                return result;
            }
            for (const auto& edge : found->second->call_edges) {
                if (!visited.emplace(edge.callable_identity).second) continue;
                auto path = current.path;
                path.push_back(PathStep{current.callable, &edge});
                pending.push_back(Pending{edge.callable_identity, std::move(path)});
            }
        }
        return std::nullopt;
    }

    std::vector<PathStep> path_to_callable(
        std::string_view root,
        std::string_view target) const
    {
        struct Pending {
            std::string callable;
            std::vector<PathStep> path;
        };
        std::deque<Pending> pending;
        std::unordered_set<std::string> visited;
        pending.push_back(Pending{std::string(root), {}});
        visited.emplace(root);
        while (!pending.empty()) {
            auto current = std::move(pending.front());
            pending.pop_front();
            const auto* declaration = declaration_for(current.callable);
            if (current.callable == target
                || (declaration && declaration->identity == target)) {
                return current.path;
            }
            const auto found = summaries_.find(current.callable);
            if (found == summaries_.end()) continue;
            for (const auto& edge : found->second->call_edges) {
                if (!visited.emplace(edge.callable_identity).second) continue;
                auto path = current.path;
                path.push_back(PathStep{current.callable, &edge});
                pending.push_back(Pending{edge.callable_identity, std::move(path)});
            }
        }
        return {};
    }

    std::vector<Explanation> explanations(
        const IRPerformanceContractResult& result) const
    {
        std::vector<Explanation> output;
        for (const auto& cause : result.causes) {
            auto explanation = find_explanation(result.callable_identity, cause);
            if (!explanation) {
                explanation = Explanation{cause.status, cause.effect,
                    cause.origin, cause.detail, result.callable_identity,
                    nullptr, nullptr, {}};
            }
            const bool duplicate = std::ranges::any_of(output,
                [&](const Explanation& existing) {
                    return existing.status == explanation->status
                        && existing.effect == explanation->effect
                        && same_origin(existing.origin, explanation->origin)
                        && existing.detail == explanation->detail;
                });
            if (!duplicate) output.push_back(std::move(*explanation));
        }
        return output;
    }

    static bool is_dynamic(const IREffectCallEdge& edge)
    {
        return edge.operation_kind == IROperationKind::VirtualCall
            || edge.operation_kind == IROperationKind::InterfaceCall;
    }

    void append_path(const std::vector<PathStep>& path, std::string& output) const
    {
        if (path.empty()) return;
        output += "        path\n";
        output += "          " + display_name(path.front().caller) + "\n";
        for (const auto& step : path) {
            output += "          -> "
                + display_name(step.edge->callable_identity);
            if (is_dynamic(*step.edge)) output += " [dynamic target]";
            output += " @" + origin_text(step.edge->origin) + "\n";
        }
    }

    void append_dynamic_proofs(
        const IRPerformanceContractResult& result,
        std::string& output) const
    {
        const auto found = summaries_.find(result.callable_identity);
        if (found == summaries_.end()) return;
        for (std::size_t index = 0; index < found->second->call_edges.size();) {
            const auto& first = found->second->call_edges[index];
            if (!is_dynamic(first)) {
                ++index;
                continue;
            }
            std::size_t count = 1;
            while (index + count < found->second->call_edges.size()
                && is_dynamic(found->second->call_edges[index + count])
                && same_origin(first.origin,
                    found->second->call_edges[index + count].origin)) {
                ++count;
            }
            output += "      "
                + std::string(first.operation_kind == IROperationKind::VirtualCall
                    ? "virtual" : "interface")
                + " dispatch " + first.operation_name + " @"
                + origin_text(first.origin) + "\n";
            output += "        reachable-targets " + std::to_string(count) + "\n";
            output += "        all-targets contract-safe\n";
            index += count;
        }
    }

    void append_ownership_proofs(
        const IRPerformanceContractResult& result,
        std::string& output) const
    {
        const auto found = schedules_.find(result.callable_identity);
        if (found == schedules_.end()) return;
        for (const auto& operation : found->second->optimized_operations) {
            if (operation.kind != IRArcOperationKind::BorrowedTemporary
                && operation.kind != IRArcOperationKind::OwnershipTransfer) {
                continue;
            }
            output += operation.kind == IRArcOperationKind::BorrowedTemporary
                ? "      borrow " : "      ownership-transfer ";
            output += operation.value + " @" + origin_text(operation.origin);
            if (!operation.reason.empty()) output += " ; " + operation.reason;
            output += '\n';
        }
    }

    bool has_ownership_proof(const IRPerformanceContractResult& result) const
    {
        const auto found = schedules_.find(result.callable_identity);
        if (found == schedules_.end()) return false;
        return std::ranges::any_of(found->second->optimized_operations,
            [](const IRArcOperation& operation) {
                return operation.kind == IRArcOperationKind::BorrowedTemporary
                    || operation.kind == IRArcOperationKind::OwnershipTransfer;
            });
    }

    bool has_dynamic_proof(const IRPerformanceContractResult& result) const
    {
        const auto found = summaries_.find(result.callable_identity);
        return found != summaries_.end()
            && std::ranges::any_of(found->second->call_edges,
                [](const IREffectCallEdge& edge) { return is_dynamic(edge); });
    }

    void append_result(
        const IRPerformanceContractResult& result,
        std::string& output) const
    {
        output += "  callable " + display_name(result.callable_identity) + "\n";
        output += "    identity " + result.callable_identity + "\n";
        output += "    contract " + status_name(result.status) + "\n";
        if (result.status == IRPerformanceContractStatus::Satisfied) {
            output += "    proofs\n";
            for (const auto& refinement : result.refinements) {
                const auto allocation = std::ranges::find_if(
                    program_.escape_allocations, [&](const auto& candidate) {
                        return candidate.identity == refinement.allocation_identity
                            || same_origin(candidate.origin, refinement.origin);
                    });
                output += "      construction ";
                output += allocation == program_.escape_allocations.end()
                    ? refinement.allocation_identity
                    : type_name(allocation->type);
                output += " @" + origin_text(refinement.origin) + "\n";
                if (!refinement.via_callable.empty()) {
                    if (allocation != program_.escape_allocations.end()) {
                        append_path(path_to_callable(result.callable_identity,
                            allocation->callable_identity), output);
                    } else {
                        output += "        via "
                            + display_name(refinement.via_callable) + "\n";
                    }
                }
                output += "        semantic-effect allocation\n";
                output += "        runtime-storage "
                    + std::string(ir_storage_class_name(refinement.storage)) + "\n";
                output += "        general-heap-allocation discharged\n";
            }
            append_ownership_proofs(result, output);
            append_dynamic_proofs(result, output);
            if (result.refinements.empty() && !has_ownership_proof(result)
                && !has_dynamic_proof(result)) {
                output += "      none required\n";
            }
            return;
        }
        output += "    reasons\n";
        for (const auto& explanation : explanations(result)) {
            output += "      " + effect_description(explanation.effect)
                + " @" + origin_text(explanation.origin) + "\n";
            if (explanation.allocation) {
                output += "        construction "
                    + type_name(explanation.allocation->type) + "\n";
                output += "        runtime-storage "
                    + std::string(ir_storage_class_name(
                        explanation.allocation->storage)) + "\n";
            }
            if (!explanation.detail.empty()) {
                output += "        detail " + explanation.detail + "\n";
            }
            append_path(explanation.path, output);
        }
    }

    const IRProgram& program_;
    std::unordered_map<std::string, const IRDeclaration*> declarations_;
    std::unordered_map<std::string, const IRCallableEffectSummary*> summaries_;
    std::unordered_map<std::string, const IRCallableArcSchedule*> schedules_;
};

} // namespace

void PerformanceContractReporter::annotate_diagnostics(IRProgram& program) const
{
    std::erase_if(program.diagnostics, [](const Diagnostic& diagnostic) {
        return diagnostic.code == "performance-contract";
    });
    auto diagnostics = ReportBuilder(program).diagnostics();
    program.diagnostics.insert(program.diagnostics.end(),
        std::make_move_iterator(diagnostics.begin()),
        std::make_move_iterator(diagnostics.end()));
}

std::string PerformanceContractReporter::report(const IRProgram& program) const
{
    return ReportBuilder(program).report();
}

} // namespace toro
