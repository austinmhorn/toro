#pragma once

#include "toro/Diagnostic.hpp"
#include "toro/PerformanceContract.hpp"
#include "toro/SourceLocation.hpp"
#include "toro/Type.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace toro {

enum class IRValueCategory {
    Primitive,
    Value,
    ManagedReference,
    InterfaceValue,
    CollectionValue,
    ResultValue,
    Unknown,
};

struct IRType {
    Type type;
    IRValueCategory category{IRValueCategory::Unknown};
};

struct IROrigin {
    std::string module_name;
    SourceLocation location{1, 1};
};

enum class IROperationKind {
    Block,
    Constant,
    Load,
    Declare,
    Assign,
    MemberAssign,
    IndexAssign,
    Unary,
    Binary,
    Call,
    ForeignCall,
    VirtualCall,
    InterfaceCall,
    Construction,
    EnumConstruction,
    ResultConstruction,
    CollectionOperation,
    MemberAccess,
    IndexAccess,
    Conversion,
    PropagateResult,
    Branch,
    Loop,
    ForEach,
    Handle,
    Return,
    Stop,
    Continue,
};

struct IROwnershipTransfer {
    std::string source;
    std::string destination;
    IROrigin source_origin;
};

enum class IRReferenceMode : std::uint8_t {
    Own,
    Transfer,
    Borrow,
    Weak,
};

enum class IRBorrowKind : std::uint8_t {
    Parameter,
    Receiver,
    CallArgument,
    LocalAlias,
};

struct IRBorrowUse {
    IRBorrowKind kind{IRBorrowKind::CallArgument};
    std::size_t operand_index{0};
    std::string source;
    std::string destination;
    IROrigin origin;
    std::string reason;
};

enum class IRStorageClass : std::uint8_t {
    Heap,
    Stack,
};

struct IROperation {
    IROperationKind kind{IROperationKind::Block};
    IROrigin origin;
    std::optional<IRType> result_type;
    std::string name;
    std::string selected_declaration;
    std::vector<std::pair<std::string, IRType>> substitutions;
    std::vector<std::string> argument_names;
    std::vector<IROperation> children;
    std::optional<IROwnershipTransfer> ownership_transfer;
    std::vector<IRBorrowUse> borrows;
    std::string allocation_identity;
    std::optional<IRStorageClass> storage;
};

struct IRGenericParameter {
    std::string name;
    std::vector<IRType> constraints;
    IROrigin origin;
};

struct IRField {
    std::string name;
    IRType type;
    IROrigin origin;
    std::optional<IROperation> default_value;
    bool is_public{false};
    bool is_weak{false};
};

struct IREnumVariant {
    std::string name;
    std::optional<IRType> payload_type;
    IROrigin origin;
};

enum class IRDeclarationKind {
    Function,
    ForeignFunction,
    Struct,
    Class,
    Interface,
    Enum,
    Method,
    Conversion,
};

struct IRParameter {
    std::string name;
    IRType type;
    IROrigin origin;
};

struct IRDeclaration {
    IRDeclarationKind kind{IRDeclarationKind::Function};
    IROrigin origin;
    std::string name;
    std::string identity;
    std::string owner_identity;
    std::vector<IRGenericParameter> generic_parameters;
    std::vector<IRParameter> parameters;
    std::vector<IRField> fields;
    std::vector<IREnumVariant> variants;
    std::optional<IRType> return_type;
    std::vector<IRType> related_types;
    std::optional<IROperation> body;
    std::string calling_convention;
    bool is_virtual{false};
    bool is_override{false};
    bool is_abstract{false};
    bool is_public{false};
    PerformanceContract performance_contract{PerformanceContract::None};
};

struct IRModule {
    std::string name;
    std::vector<std::string> imports;
};

enum class IRLifetimeModel {
    LegacyArc,
    ExplicitArc,
};

enum class IRArcStage {
    Semantic,
    Materialized,
    TransfersAnalyzed,
    BorrowsAnalyzed,
    Optimized,
};

enum class IRArcOperationKind {
    Retain,
    Release,
    DestroyFinalize,
    ManagedAssignment,
    OwnedTemporary,
    BorrowedTemporary,
    OwnershipTransfer,
};

enum class IRArcElisionKind : std::uint8_t {
    InferredBorrow,
    OwnershipTransfer,
};

struct IRArcElision {
    IRArcElisionKind kind{IRArcElisionKind::InferredBorrow};
    std::string related_value;
    IROrigin origin;
    std::string reason;
};

struct IRArcRetention {
    IROrigin origin;
    std::string reason;
};

struct IRArcOperation {
    IRArcOperationKind kind{IRArcOperationKind::Retain};
    std::string value;
    IROrigin origin;
    std::string reason;
    std::size_t pair_id{0};
    bool elided{false};
    std::optional<IRArcElision> elision{};
    std::optional<IRArcRetention> retention{};
};

struct IRCallableArcSchedule {
    std::string callable_identity;
    std::vector<IRArcOperation> unoptimized_operations;
    std::vector<IRArcOperation> optimized_operations;
};

struct IRArcMetrics {
    std::size_t retains_before{0};
    std::size_t retains_after{0};
    std::size_t releases_before{0};
    std::size_t releases_after{0};
    std::size_t elided_pairs{0};
    std::size_t ownership_transfers{0};
    std::size_t borrows_inferred{0};
};

enum class IROwnershipBehavior : std::uint8_t {
    Observe,
    Retain,
    Store,
    Return,
    Consume,
    Escape,
    WeakStore,
    Unknown,
};

struct IROwnershipCause {
    IROwnershipBehavior behavior{IROwnershipBehavior::Observe};
    IROrigin origin;
    std::string via_callable;
    std::string reason;
    bool transitive{false};
};

struct IRParameterOwnershipSummary {
    std::string parameter_name;
    std::size_t parameter_index{0};
    IROrigin origin;
    bool receiver{false};
    std::vector<IROwnershipBehavior> behaviors;
    std::vector<IROwnershipCause> causes;
};

enum class IRReturnOwnership : std::uint8_t {
    None,
    NewOwned,
    ParameterAlias,
    ReceiverAlias,
    FieldAlias,
    Unknown,
};

struct IRReturnOwnershipSummary {
    IRReturnOwnership ownership{IRReturnOwnership::None};
    std::optional<std::size_t> parameter_index;
    IROrigin origin;
    std::string reason;
};

struct IRCallableOwnershipSummary {
    std::string callable_identity;
    std::vector<IRParameterOwnershipSummary> parameters;
    IRReturnOwnershipSummary return_value;
};

struct IRStrongOwnershipEdge {
    std::string owner_type;
    std::string owner_display;
    std::string field_path;
    std::string target_type;
    std::string target_display;
    IROrigin origin;
    bool target_may_be_deferred{false};
};

struct IRStrongReferenceCycle {
    std::vector<IRStrongOwnershipEdge> edges;
};

enum class IREffect : std::uint8_t {
    Allocate,
    Retain,
    ReleaseDestroy,
    Escape,
    Block,
    FilesystemIO,
    EnvironmentAccess,
    TimeQuery,
    ProcessIO,
    Synchronize,
    ForeignCall,
    NativeCall,
    DynamicDispatch,
    Terminate,
    UnknownExternal,
    Count,
};

struct IREffectCause {
    IREffect effect{IREffect::Allocate};
    IROrigin origin;
    std::string via_callable;
    bool transitive{false};
};

struct IRLocalEffectSite {
    IREffect effect{IREffect::Allocate};
    IROrigin origin;
    std::optional<IROperationKind> operation_kind;
    std::string operation_name;
};

struct IREffectCallEdge {
    std::string callable_identity;
    IROrigin origin;
    IROperationKind operation_kind{IROperationKind::Call};
    std::string operation_name;
};

struct IRCallableEffectSummary {
    std::string callable_identity;
    std::vector<IREffect> effects;
    std::vector<IREffectCause> causes;
    std::vector<IRLocalEffectSite> local_effect_sites;
    std::vector<IREffectCallEdge> call_edges;
};

enum class IRPerformanceContractStatus : std::uint8_t {
    Satisfied,
    Violated,
    Unproven,
};

struct IRPerformanceContractCause {
    IRPerformanceContractStatus status{IRPerformanceContractStatus::Satisfied};
    IREffect effect{IREffect::Allocate};
    IROrigin origin;
    std::string via_callable;
    bool transitive{false};
    std::string detail;
};

struct IRPerformanceContractRefinement {
    IREffect effect{IREffect::Allocate};
    IROrigin origin;
    std::string allocation_identity;
    IRStorageClass storage{IRStorageClass::Heap};
    std::string detail;
    std::string via_callable;
    bool transitive{false};
};

struct IRPerformanceContractResult {
    std::string callable_identity;
    IRPerformanceContractStatus status{IRPerformanceContractStatus::Satisfied};
    std::vector<IRPerformanceContractCause> causes;
    std::vector<IRPerformanceContractRefinement> refinements;
};

enum class IREscapeState : std::uint8_t {
    DoesNotEscape,
    Escapes,
    Unknown,
};

enum class IREscapeReason : std::uint8_t {
    Return,
    GlobalStorage,
    StrongField,
    CollectionStorage,
    CallArgument,
    DynamicCall,
    ForeignCall,
    UnknownCall,
    WeakObservation,
    InterfaceAlias,
    ResultPayload,
    EnumPayload,
};

struct IREscapeCause {
    IREscapeReason reason{IREscapeReason::Return};
    IROrigin origin;
    std::string detail;
    bool causes_escape{true};
    bool uncertain{false};
};

struct IRAllocationEscape {
    std::string identity;
    std::string callable_identity;
    IRType type;
    IROrigin origin;
    IREscapeState state{IREscapeState::DoesNotEscape};
    std::vector<IREscapeCause> causes;
    IRStorageClass storage{IRStorageClass::Heap};
    std::string storage_reason;
};

struct IREscapeMetrics {
    std::size_t managed_allocations{0};
    std::size_t non_escaping{0};
    std::size_t escaping{0};
    std::size_t unknown{0};
    std::size_t stack_allocations{0};
    std::size_t heap_allocations{0};
};

struct IRProgram {
    std::vector<IRModule> modules;
    std::vector<IRDeclaration> declarations;
    std::vector<IROperation> top_level_operations;
    std::string entry_module;
    IRLifetimeModel lifetime_model{IRLifetimeModel::LegacyArc};
    std::vector<IRCallableEffectSummary> effect_summaries;
    std::vector<IRPerformanceContractResult> performance_contract_results;
    std::vector<IRAllocationEscape> escape_allocations;
    IREscapeMetrics escape_metrics;
    IRArcStage arc_stage{IRArcStage::Semantic};
    std::vector<IRCallableArcSchedule> arc_schedules;
    IRArcMetrics arc_metrics;
    std::vector<IRCallableOwnershipSummary> ownership_summaries;
    std::vector<IRStrongOwnershipEdge> strong_ownership_edges;
    std::vector<IRStrongReferenceCycle> strong_reference_cycles;
    std::vector<Diagnostic> diagnostics;
    bool borrowing_enabled{true};
};

[[nodiscard]] std::string_view ir_effect_name(IREffect effect);
[[nodiscard]] std::string_view ir_escape_state_name(IREscapeState state);
[[nodiscard]] std::string_view ir_escape_reason_name(IREscapeReason reason);
[[nodiscard]] std::string_view ir_storage_class_name(IRStorageClass storage);
[[nodiscard]] std::string_view ir_reference_mode_name(IRReferenceMode mode);
[[nodiscard]] std::string_view ir_borrow_kind_name(IRBorrowKind kind);
[[nodiscard]] std::string_view ir_ownership_behavior_name(
    IROwnershipBehavior behavior);
[[nodiscard]] std::string_view ir_return_ownership_name(
    IRReturnOwnership ownership);
[[nodiscard]] std::string dump_ir(
    const IRProgram& program,
    bool include_effects = false,
    bool include_arc = false,
    bool include_escape = false,
    bool include_ownership = false);

} // namespace toro
