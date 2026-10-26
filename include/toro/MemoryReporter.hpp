#pragma once

#include "toro/IR.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace toro {

struct MemoryArcCounts {
    std::size_t retains_before{0};
    std::size_t retains_after{0};
    std::size_t releases_before{0};
    std::size_t releases_after{0};
    std::size_t elided_operations{0};
    std::size_t ownership_transfers{0};
    std::size_t inferred_borrows{0};
};

struct MemorySummary {
    std::size_t managed_allocation_sites{0};
    std::size_t stack_allocation_sites{0};
    std::size_t heap_allocation_sites{0};
    std::size_t non_escaping_sites{0};
    std::size_t escaping_sites{0};
    std::size_t unknown_escape_sites{0};
    MemoryArcCounts arc;
    std::size_t possible_strong_cycles{0};
    std::size_t weak_fields{0};
    std::size_t captures{0};
};

struct MemoryAllocationReport {
    std::string identity;
    std::string callable_identity;
    IRType type;
    IROrigin origin;
    IREscapeState escape_state{IREscapeState::Unknown};
    IRStorageClass storage{IRStorageClass::Heap};
    std::string storage_reason;
    std::vector<IREscapeCause> escape_causes;
};

struct MemoryArcOperationReport {
    IRArcOperationKind kind{IRArcOperationKind::Retain};
    std::string value;
    IROrigin origin;
    std::string reason;
};

struct CallableMemoryReport {
    std::string callable_identity;
    std::size_t allocation_sites{0};
    std::size_t stack_allocation_sites{0};
    std::size_t heap_allocation_sites{0};
    std::size_t non_escaping_sites{0};
    std::size_t escaping_sites{0};
    std::size_t unknown_escape_sites{0};
    MemoryArcCounts arc;
    std::vector<MemoryArcOperationReport> final_arc_operations;
    std::vector<MemoryArcOperationReport> elided_arc_operations;
    std::vector<MemoryArcOperationReport> ownership_transfers;
    std::vector<MemoryArcOperationReport> inferred_borrows;
};

struct MemoryWeakFieldReport {
    std::string owner_identity;
    std::string field_name;
    IRType type;
    IROrigin origin;
};

struct MemoryCycleReport {
    std::vector<IRStrongOwnershipEdge> edges;
};

struct MemoryReport {
    std::string entry_module;
    MemorySummary summary;
    std::vector<MemoryAllocationReport> allocations;
    std::vector<CallableMemoryReport> callables;
    std::vector<MemoryWeakFieldReport> weak_fields;
    std::vector<MemoryCycleReport> cycles;
};

class MemoryReporter {
public:
    [[nodiscard]] MemoryReport inspect(const IRProgram& program) const;
    [[nodiscard]] std::string format(const MemoryReport& report) const;
    [[nodiscard]] std::string report(const IRProgram& program) const;
};

} // namespace toro
