/**
 * @file   plist_xml.h
 * @brief  极简 XML plist（lockdown 那条通道只吃 XML）
 * @author blt
 */

// ============================================================================
// core/usb/plist_xml.h —— 极简 XML plist
// ============================================================================
// lockdown 的 plist 通道用的是 **XML plist**，不是本项目 core/bplist 那套二进制 plist，
// 两者不能混用。这里只实现 lockdown 用得到的那几个类型（dict/array/string/integer/
// data/bool/real）与最小的 XML 子集：不处理命名空间、不处理 CDATA、不做 DTD 校验。
// 够用就好 —— 多引一个 XML 库不划算。
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace core::usb {

class Plist {
public:
    enum class Type { Null, Bool, Integer, Real, String, Data, Dict, Array };

    using Dict = std::vector<std::pair<std::string, Plist>>;  // 保序：发出去的请求要人能读
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
    const std::string&          asString() const;  ///< 不是字符串时返回空串
    const std::vector<uint8_t>& asData() const;

    const Dict&  items() const { return dict_; }
    Dict&        items() { return dict_; }
    const Array& elements() const { return arr_; }
    Array&       elements() { return arr_; }

    /// dict 查 key；不是 dict 或没这个 key 返回 nullptr
    const Plist* find(const std::string& key) const;

    std::string stringValue(const std::string& key, const std::string& def = "") const;
    int64_t     intValue(const std::string& key, int64_t def = 0) const;
    bool        boolValue(const std::string& key, bool def = false) const;

    /// 只在 dict 上有效：已有同 key 就覆盖，没有就追加
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
