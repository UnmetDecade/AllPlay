/**
 * @file   bplist.h
 * @brief  Binary plist serialization (own implementation, no third-party deps)
 * @author blt
 */

// ============================================================================
// core/bplist/bplist.h — binary plist serialization (own implementation, no third-party deps)
// ============================================================================
// AirPlay's /info, SETUP responses, etc. are all binary plists ("bplist00").
// This implements the **write** half (needed for /info in M1); the **read** half comes in M3 when we parse phone requests,
// kept in the same file group for symmetry.
//
// Why implement it ourselves: it is part of the protocol (codec is self-contained, no third-party deps),
// and we only use a tiny subset: dict/array/string/data/integer/bool/float.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <cstddef>

namespace core::bplist {

class Value;
using Ptr = std::shared_ptr<Value>;

// ---- Construction (all return shared pointers, composed into a tree) ----
Ptr boolean(bool v);
Ptr u(uint64_t v);
Ptr i(int64_t v);
Ptr real(double v);
Ptr str(std::string v);
Ptr data(std::vector<uint8_t> v);
Ptr array(std::vector<Ptr> v);
/// Dictionary: preserves insertion order (plist readers do not require ordering)
Ptr dict(std::vector<std::pair<std::string, Ptr>> v);

/// Serialize to a "bplist00" byte stream (internally: flatten objects + offset table + trailer)
std::vector<uint8_t> serialize(const Ptr& root);

// ---------------------------------------------------------------------------
// Read side (from M3: parse bplist request bodies sent by the phone, e.g. SETUP / /command)
// ---------------------------------------------------------------------------
// NOTE (read side, matching the bplist encoding): read 8-byte integers as **unsigned** — the phone's
// streamConnectionID / seed would otherwise (wrongly) decode as a negative Long when the high bit is set; treat them as u64.
struct Node;
using NodePtr = std::shared_ptr<Node>;

enum class NodeType { Dict, Array, Str, Data, Uint, Int, Real, Bool, Null };

struct Node {
    NodeType type = NodeType::Null;

    std::vector<std::pair<std::string, NodePtr>> dict;   ///< Dict (order-preserving)
    std::vector<NodePtr>                              array; ///< Array / Set
    std::string        str;
    std::vector<uint8_t> data;
    uint64_t           u = 0;
    int64_t            i = 0;
    double             r = 0.0;
    bool               b = false;
};

/// Parse "bplist00"; returns nullptr on failure (bad magic/trailer/out-of-bounds).
NodePtr parse(const uint8_t* data, size_t len);

/// Look up a dict by key; returns nullptr if not a dict or key absent.
NodePtr dictGet(const NodePtr& n, const char* key);
/// Index an array; returns nullptr if out of range or not an array.
NodePtr arrGet(const NodePtr& n, size_t idx);

std::string         strOf(const NodePtr& n);   ///< Str; otherwise ""
std::vector<uint8_t> dataOf(const NodePtr& n);  ///< Data; otherwise empty
uint64_t            uintOf(const NodePtr& n);   ///< Uint/Int; otherwise 0
bool                boolOf(const NodePtr& n);   ///< Bool; otherwise false

}  // namespace core::bplist
