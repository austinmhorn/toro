#include "toro/PerformanceContractAnalyzer.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toro {
namespace {

constexpr std::array prohibited_effects{
    IREffect::FilesystemIO,
    IREffect::Block,
    IREffect::Synchronize,
    IREffect::Allocate,
    IREffect::Escape,
};

bool same_origin(const IROrigin& left, const IROrigin& right)
{
    return left.module_name == right.module_name
        && left.location.line == right.location.line
        && left.location.column == right.location.column;
}

bool is_specialization_of(
    std::string_view candidate,
    std::string_view declaration_identity)
{
    return candidate.size() > declaration_identity.size()
        && candidate.starts_with(declaration_identity)
        && candidate[declaration_identity.size()] == '<';
}

bool is_prohibited(IREffect effect)
{
    return std::ranges::find(prohibited_effects, effect)
        != prohibited_effects.end();
}

std::size_t effect_rank(IREffect effect)
{
    const auto found = std::ranges::find(prohibited_effects, effect);
    if (found != prohibited_effects.end()) {
        return static_cast<std::size_t>(found - prohibited_effects.begin());
    }
    return prohibited_effects.size();
}

std::string effect_description(IREffect effect)
{
    switch (effect) {
    case IREffect::Allocate: return "general heap allocation";
    case IREffect::Escape: return "an escaping temporary or reference";
    case IREffect::Block: return "a blocking operation";
    case IREffect::FilesystemIO: return "blocking filesystem I/O";
    case IREffect::Synchronize: return "hidden synchronization";
    case IREffect::UnknownExternal: return "unknown external behavior";
    default: return std::string(ir_effect_name(effect));
    }
}

DiagnosticSource diagnostic_source(const IROrigin& origin)
{
    return DiagnosticSource{origin.module_name, origin.location};
}

struct PolicyNode {
    const IRCallableEffectSummary* summary{nullptr};
    const IRDeclaration* declaration{nullptr};
    std::vector<IRPerformanceContractCause> causes;
    std::vector<IRPerformanceContractRefinement> refinements;
};

class Analysis {
public:
    explicit Analysis(IRProgram& program)
        : program_(program)
    {
        for (const auto& declaration : program_.declarations) {
            declarations_.emplace(declaration.identity, &declaration);
        }
    }

    void run()
    {
        if (program_.arc_stage != IRArcStage::Optimized) {
            throw std::runtime_error(
                "performance-contract analysis requires the final optimized runtime plan");
        }
        program_.performance_contract_results.clear();
        std::erase_if(program_.diagnostics, [](const Diagnostic& diagnostic) {
            return diagnostic.code == "performance-contract";
        });

        collect_nodes();
        derive_local_policy();
        propagate_policy();
        publish_results();
    }

private:
    const IRDeclaration* declaration_for(std::string_view identity) const
    {
        if (const auto exact = declarations_.find(std::string(identity));
            exact != declarations_.end()) {
            return exact->second;
        }
        for (const auto& declaration : program_.declarations) {
            if (is_specialization_of(identity, declaration.identity)) {
                return &declaration;
            }
        }
        return nullptr;
    }

    void collect_nodes()
    {
        nodes_.reserve(program_.effect_summaries.size());
        for (const auto& summary : program_.effect_summaries) {
            node_indices_.emplace(summary.callable_identity, nodes_.size());
            nodes_.push_back(PolicyNode{
                &summary,
                declaration_for(summary.callable_identity),
                {},
                {},
            });
        }
    }

    bool same_cause(
        const IRPerformanceContractCause& left,
        const IRPerformanceContractCause& right) const
    {
        return left.status == right.status && left.effect == right.effect
            && same_origin(left.origin, right.origin)
            && left.via_callable == right.via_callable
            && left.transitive == right.transitive
            && left.detail == right.detail;
    }

    bool add_cause(
        PolicyNode& node,
        IRPerformanceContractCause cause)
    {
        if (std::ranges::any_of(node.causes, [&](const auto& existing) {
                return same_cause(existing, cause);
            })) {
            return false;
        }
        node.causes.push_back(std::move(cause));
        return true;
    }

    bool add_refinement(
        PolicyNode& node,
        IRPerformanceContractRefinement refinement)
    {
        if (std::ranges::any_of(node.refinements, [&](const auto& existing) {
                return existing.effect == refinement.effect
                    && existing.allocation_identity
                        == refinement.allocation_identity
                    && existing.via_callable == refinement.via_callable
                    && existing.transitive == refinement.transitive;
            })) {
            return false;
        }
        node.refinements.push_back(std::move(refinement));
        return true;
    }

    std::vector<const IRAllocationEscape*> allocations_for(
        const PolicyNode& node,
        const IRLocalEffectSite& site) const
    {
        std::vector<const IRAllocationEscape*> result;
        if (!node.declaration) return result;
        for (const auto& allocation : program_.escape_allocations) {
            if (allocation.callable_identity == node.declaration->identity
                && same_origin(allocation.origin, site.origin)) {
                result.push_back(&allocation);
            }
        }
        return result;
    }

    void evaluate_allocation_site(
        PolicyNode& node,
        const IRLocalEffectSite& site)
    {
        if (site.operation_kind != IROperationKind::Construction) {
            add_cause(node, IRPerformanceContractCause{
                IRPerformanceContractStatus::Violated,
                IREffect::Allocate,
                site.origin,
                {},
                false,
                {},
            });
            return;
        }

        const auto allocations = allocations_for(node, site);
        if (allocations.empty()) {
            add_cause(node, IRPerformanceContractCause{
                IRPerformanceContractStatus::Violated,
                IREffect::Allocate,
                site.origin,
                {},
                false,
                "no stack-storage proof for this construction",
            });
            return;
        }

        for (const auto* allocation : allocations) {
            if (allocation->storage == IRStorageClass::Stack) {
                add_refinement(node, IRPerformanceContractRefinement{
                    IREffect::Allocate,
                    allocation->origin,
                    allocation->identity,
                    allocation->storage,
                    allocation->storage_reason,
                    {},
                    false,
                });
            } else {
                add_cause(node, IRPerformanceContractCause{
                    IRPerformanceContractStatus::Violated,
                    IREffect::Allocate,
                    allocation->origin,
                    {},
                    false,
                    allocation->storage_reason,
                });
            }
        }
    }

    void evaluate_local_site(
        PolicyNode& node,
        const IRLocalEffectSite& site)
    {
        if (site.effect == IREffect::UnknownExternal) {
            add_cause(node, IRPerformanceContractCause{
                IRPerformanceContractStatus::Unproven,
                site.effect,
                site.origin,
                {},
                false,
                {},
            });
        } else if (site.effect == IREffect::Allocate) {
            evaluate_allocation_site(node, site);
        } else if (is_prohibited(site.effect)) {
            add_cause(node, IRPerformanceContractCause{
                IRPerformanceContractStatus::Violated,
                site.effect,
                site.origin,
                {},
                false,
                {},
            });
        }
    }

    void derive_local_policy()
    {
        for (auto& node : nodes_) {
            if (!node.summary->local_effect_sites.empty()) {
                for (const auto& site : node.summary->local_effect_sites) {
                    evaluate_local_site(node, site);
                }
                continue;
            }
            for (const auto& cause : node.summary->causes) {
                if (cause.transitive) continue;
                evaluate_local_site(node, IRLocalEffectSite{
                    cause.effect,
                    cause.origin,
                    std::nullopt,
                    {},
                });
            }
        }
    }

    void propagate_policy()
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& node : nodes_) {
                for (const auto& edge : node.summary->call_edges) {
                    const auto found = node_indices_.find(edge.callable_identity);
                    if (found == node_indices_.end()) continue;
                    const auto target_causes = nodes_[found->second].causes;
                    const auto target_refinements
                        = nodes_[found->second].refinements;
                    for (const auto& target : target_causes) {
                        changed |= add_cause(node, IRPerformanceContractCause{
                            target.status,
                            target.effect,
                            edge.origin,
                            edge.callable_identity,
                            true,
                            target.detail,
                        });
                    }
                    for (const auto& target : target_refinements) {
                        changed |= add_refinement(
                            node, IRPerformanceContractRefinement{
                                target.effect,
                                target.origin,
                                target.allocation_identity,
                                target.storage,
                                target.detail,
                                edge.callable_identity,
                                true,
                            });
                    }
                }
            }
        }
    }

    bool skip_symbolic_generic(const PolicyNode& node) const
    {
        if (!node.declaration || node.declaration->generic_parameters.empty()
            || node.summary->callable_identity != node.declaration->identity) {
            return false;
        }
        return std::ranges::any_of(nodes_, [&](const PolicyNode& candidate) {
            return is_specialization_of(
                candidate.summary->callable_identity,
                node.declaration->identity);
        });
    }

    IRPerformanceContractStatus status(const PolicyNode& node) const
    {
        if (std::ranges::any_of(node.causes, [](const auto& cause) {
                return cause.status == IRPerformanceContractStatus::Violated;
            })) {
            return IRPerformanceContractStatus::Violated;
        }
        if (!node.causes.empty()) return IRPerformanceContractStatus::Unproven;
        return IRPerformanceContractStatus::Satisfied;
    }

    void normalize(PolicyNode& node) const
    {
        std::ranges::stable_sort(node.causes,
            [](const auto& left, const auto& right) {
                if (left.status != right.status) {
                    return left.status
                        == IRPerformanceContractStatus::Violated;
                }
                return effect_rank(left.effect) < effect_rank(right.effect);
            });
    }

    void publish_results()
    {
        for (auto& node : nodes_) {
            if (!node.declaration
                || node.declaration->performance_contract
                    != PerformanceContract::Performance
                || (node.declaration->kind != IRDeclarationKind::Function
                    && node.declaration->kind != IRDeclarationKind::Method)
                || skip_symbolic_generic(node)) {
                continue;
            }
            normalize(node);
            IRPerformanceContractResult result{
                node.summary->callable_identity,
                status(node),
                node.causes,
                node.refinements,
            };
            if (result.status != IRPerformanceContractStatus::Satisfied) {
                publish_diagnostic(*node.declaration, result);
            }
            program_.performance_contract_results.push_back(std::move(result));
        }
    }

    std::string callable_name(
        const IRDeclaration& declaration,
        std::string_view identity) const
    {
        std::string result;
        if (!declaration.owner_identity.empty()) {
            if (const auto owner = declarations_.find(declaration.owner_identity);
                owner != declarations_.end()) {
                result = owner->second->name + ".";
            }
        }
        result += declaration.name;
        if (identity.size() > declaration.identity.size()) {
            result += identity.substr(declaration.identity.size());
        }
        return result;
    }

    std::string callee_name(std::string_view identity) const
    {
        if (const auto* declaration = declaration_for(identity)) {
            return callable_name(*declaration, identity);
        }
        return "unknown callable";
    }

    void publish_diagnostic(
        const IRDeclaration& declaration,
        const IRPerformanceContractResult& result)
    {
        const bool violated = result.status
            == IRPerformanceContractStatus::Violated;
        Diagnostic diagnostic;
        diagnostic.severity = DiagnosticSeverity::Error;
        diagnostic.code = "performance-contract";
        diagnostic.message = violated
            ? "performance callable '"
                + callable_name(declaration, result.callable_identity)
                + "' violates its contract"
            : "performance contract for callable '"
                + callable_name(declaration, result.callable_identity)
                + "' cannot be proven";
        diagnostic.source = diagnostic_source(
            result.causes.empty() ? declaration.origin : result.causes.front().origin);
        diagnostic.notes.push_back(DiagnosticNote{
            "contract declared here",
            diagnostic_source(declaration.origin),
        });
        for (const auto& cause : result.causes) {
            std::string message;
            if (cause.status == IRPerformanceContractStatus::Unproven) {
                message = cause.transitive
                    ? "call to '" + callee_name(cause.via_callable)
                        + "' reaches unknown external behavior, so the contract cannot be proven"
                    : "unknown external behavior prevents contract proof";
            } else {
                message = cause.transitive
                    ? "call to '" + callee_name(cause.via_callable)
                        + "' reaches " + effect_description(cause.effect)
                        + ", which is not allowed by this performance contract"
                    : effect_description(cause.effect)
                        + " is not allowed by this performance contract";
            }
            if (!cause.detail.empty()) message += " (" + cause.detail + ")";
            diagnostic.notes.push_back(DiagnosticNote{
                std::move(message),
                diagnostic_source(cause.origin),
            });
        }
        program_.diagnostics.push_back(std::move(diagnostic));
    }

    IRProgram& program_;
    std::unordered_map<std::string, const IRDeclaration*> declarations_;
    std::unordered_map<std::string, std::size_t> node_indices_;
    std::vector<PolicyNode> nodes_;
};

} // namespace

void PerformanceContractAnalyzer::analyze(IRProgram& program) const
{
    Analysis(program).run();
}

} // namespace toro
