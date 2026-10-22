#include "toro/PerformanceContractAnalyzer.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <unordered_map>

namespace toro {
namespace {

constexpr std::array prohibited_effects{
    IREffect::FilesystemIO,
    IREffect::Block,
    IREffect::Synchronize,
    IREffect::Allocate,
    IREffect::Escape,
};

bool contains_effect(
    const IRCallableEffectSummary& summary,
    IREffect effect)
{
    return std::ranges::find(summary.effects, effect) != summary.effects.end();
}

const IREffectCause* find_cause(
    const IRCallableEffectSummary& summary,
    IREffect effect)
{
    const auto found = std::ranges::find_if(
        summary.causes,
        [effect](const IREffectCause& cause) { return cause.effect == effect; });
    return found == summary.causes.end() ? nullptr : &*found;
}

bool is_specialization_of(
    std::string_view candidate,
    std::string_view declaration_identity)
{
    return candidate.size() > declaration_identity.size()
        && candidate.starts_with(declaration_identity)
        && candidate[declaration_identity.size()] == '<';
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
        program_.performance_contract_results.clear();
        std::erase_if(program_.diagnostics, [](const Diagnostic& diagnostic) {
            return diagnostic.code == "performance-contract";
        });

        for (const auto& summary : program_.effect_summaries) {
            const auto* declaration = contracted_declaration(summary);
            if (!declaration || skip_symbolic_generic(*declaration, summary)) {
                continue;
            }
            auto result = evaluate(*declaration, summary);
            if (result.status != IRPerformanceContractStatus::Satisfied) {
                publish_diagnostic(*declaration, result);
            }
            program_.performance_contract_results.push_back(std::move(result));
        }
    }

private:
    const IRDeclaration* contracted_declaration(
        const IRCallableEffectSummary& summary) const
    {
        for (const auto& declaration : program_.declarations) {
            if (declaration.performance_contract
                    != PerformanceContract::Performance
                || (declaration.kind != IRDeclarationKind::Function
                    && declaration.kind != IRDeclarationKind::Method)) {
                continue;
            }
            if (summary.callable_identity == declaration.identity
                || is_specialization_of(
                    summary.callable_identity, declaration.identity)) {
                return &declaration;
            }
        }
        return nullptr;
    }

    bool skip_symbolic_generic(
        const IRDeclaration& declaration,
        const IRCallableEffectSummary& summary) const
    {
        if (declaration.generic_parameters.empty()
            || summary.callable_identity != declaration.identity) {
            return false;
        }
        return std::ranges::any_of(
            program_.effect_summaries,
            [&](const IRCallableEffectSummary& candidate) {
                return is_specialization_of(
                    candidate.callable_identity, declaration.identity);
            });
    }

    IRPerformanceContractResult evaluate(
        const IRDeclaration& declaration,
        const IRCallableEffectSummary& summary) const
    {
        IRPerformanceContractResult result;
        result.callable_identity = summary.callable_identity;
        result.status = IRPerformanceContractStatus::Satisfied;

        for (const auto effect : prohibited_effects) {
            if (!contains_effect(summary, effect)) continue;
            const auto* cause = find_cause(summary, effect);
            result.causes.push_back(IRPerformanceContractCause{
                IRPerformanceContractStatus::Violated,
                effect,
                cause ? cause->origin : declaration.origin,
                cause ? cause->via_callable : std::string{},
                cause && cause->transitive,
            });
            result.status = IRPerformanceContractStatus::Violated;
        }

        if (contains_effect(summary, IREffect::UnknownExternal)) {
            const auto* cause = find_cause(summary, IREffect::UnknownExternal);
            result.causes.push_back(IRPerformanceContractCause{
                IRPerformanceContractStatus::Unproven,
                IREffect::UnknownExternal,
                cause ? cause->origin : declaration.origin,
                cause ? cause->via_callable : std::string{},
                cause && cause->transitive,
            });
            if (result.status == IRPerformanceContractStatus::Satisfied) {
                result.status = IRPerformanceContractStatus::Unproven;
            }
        }
        return result;
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
        for (const auto& declaration : program_.declarations) {
            if (identity == declaration.identity
                || is_specialization_of(identity, declaration.identity)) {
                return callable_name(declaration, identity);
            }
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
            diagnostic.notes.push_back(DiagnosticNote{
                std::move(message),
                diagnostic_source(cause.origin),
            });
        }
        program_.diagnostics.push_back(std::move(diagnostic));
    }

    IRProgram& program_;
    std::unordered_map<std::string, const IRDeclaration*> declarations_;
};

} // namespace

void PerformanceContractAnalyzer::analyze(IRProgram& program) const
{
    Analysis(program).run();
}

} // namespace toro
