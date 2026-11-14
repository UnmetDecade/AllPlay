/**
 * @file   plist_xml.h
 * @brief  Minimal XML plist (the lockdown channel only accepts XML)
 * @author blt
 */

// ============================================================================
// core/usb/plist_xml.h — minimal XML plist
// ============================================================================
// The lockdown plist channel uses **XML plist**, not the binary plist in core/bplist;
// the two are not interchangeable. This implements only the types lockdown needs (dict/array/string/integer/
// data/bool/real) and a minimal XML subset: no namespaces, no CDATA, no DTD validation.
// Just enough — pulling in an XML library is not worth it.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace core::usb {

class Plist {
public:
    enum class Type { Null, Bool, Integer, Real, String, Data, Dict, Array };

    using Dict = std::vector<std::pair<std::string, Plist>>;  // order-preserving: outgoing requests must be human-readable
    using Array = std::vector<Plist>;

    Plist() = default;

    static Plist boolean(bool v);
    static Plist integer(int64_t v);
    static Plist real(double v);
    static Plist string(std::string v);
    static Plist data(std::vector<uint8_t> v);
    static Plist dict();
    static Plist array();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isDict() const { return type_ == Type::Dict; }

    bool                        asBool(bool def = false) const;
    int64_t                     asInteger(int64_t def = 0) const;
    double                      asReal(double def = 0.0) const;
    const std::string&          asString() const;  ///< returns an empty string when not a string
    const std::vector<uint8_t>& asData() const;

    const Dict&  items() const { return dict_; }
    Dict&        items() { return dict_; }
    const Array& elements() const { return arr_; }
    Array&       elements() { return arr_; }

    /// Look up a key in a dict; returns nullptr if not a dict or the key is absent
    const Plist* find(const std::string& key) const;

    std::string stringValue(const std::string& key, const std::string& def = "") const;
    int64_t     intValue(const std::string& key, int64_t def = 0) const;
    bool        boolValue(const std::string& key, bool def = false) const;

    /// Only valid on dicts: overwrite an existing key, append if absent
    void set(const std::string& key, Plist v);

    std::string toXml() const;
    static bool parse(const std::string& xml, Plist* out, std::string* err);

private:
    Type                 type_ = Type::Null;
    bool                 bool_ = false;
    int64_t              int_ = 0;
    double               real_ = 0.0;
    std::string          str_;
    std::vector<uint8_t> data_;
    Dict                 dict_;
    Array                arr_;
};

}  // namespace core::usb
