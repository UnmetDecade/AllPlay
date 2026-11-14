#include "bplist/bplist.h"

#include <cstring>
#include <map>

namespace core::bplist {

// ---------------------------------------------------------------------------
// Value model
// ---------------------------------------------------------------------------
class Value {
public:
    enum class Type { Bool, Uint, Int, Real, Str, Data, Array, Dict };

    explicit Value(Type t) : type(t) {}

    Type         type;
    bool         b = false;
    uint64_t     u = 0;
    int64_t      i = 0;
    double       r = 0.0;
    std::string  s;                              // Str
    std::vector<uint8_t> d;                      // Data
    std::vector<Ptr>     a;                      // Array
    std::vector<std::pair<std::string, Ptr>> m;  // Dict
};

Ptr boolean(bool v) { auto p = std::make_shared<Value>(Value::Type::Bool); p->b = v; return p; }
Ptr u(uint64_t v)   { auto p = std::make_shared<Value>(Value::Type::Uint); p->u = v; return p; }
Ptr i(int64_t v)    { auto p = std::make_shared<Value>(Value::Type::Int);  p->i = v; return p; }
Ptr real(double v)  { auto p = std::make_shared<Value>(Value::Type::Real); p->r = v; return p; }
Ptr str(std::string v) { auto p = std::make_shared<Value>(Value::Type::Str); p->s = std::move(v); return p; }
Ptr data(std::vector<uint8_t> v) { auto p = std::make_shared<Value>(Value::Type::Data); p->d = std::move(v); return p; }
Ptr array(std::vector<Ptr> v) { auto p = std::make_shared<Value>(Value::Type::Array); p->a = std::move(v); return p; }
Ptr dict(std::vector<std::pair<std::string, Ptr>> v)
{
    auto p = std::make_shared<Value>(Value::Type::Dict);
    p->m = std::move(v);
    return p;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------
// bplist layout: magic + object table (objects linked by "object references") + offset table + 32-byte trailer.
// Key point: reference width (refSize) depends on the total object count, offset width (offsetSize) on the largest offset,
// so flatten the whole tree into an **object array** first, then write.
//
// "Long containers/strings" (length >= 15): marker 0x?F followed by an **inline integer token**
// (0x10|log2(size) plus a big-endian value), not "reference a separate count object" —
// getting this wrong makes parsers report Invalid file (plistlib/CoreFoundation both read it inline).
// References and the offset table are **big-endian**.
namespace {

class Writer {
public:
    std::vector<uint8_t> run(const Ptr& root)
    {
        collect(root.get(), /*top=*/true);

        std::vector<uint8_t> out;
        const char magic[8] = {'b', 'p', 'l', 'i', 's', 't', '0', '0'};
        out.insert(out.end(), magic, magic + 8);

        offsets_.clear();
        offsets_.reserve(objs_.size());
        for (const Value* v : objs_) {
            offsets_.push_back(out.size());
            writeObject(v, out);
        }

        // Offset table + trailer
        const uint64_t numObjects = objs_.size();
        const uint64_t maxOffset  = offsets_.back();
        const int      offsetSize = intSize(maxOffset);
        const int      refSize    = intSize(numObjects - 1);
        const uint64_t tableOff   = out.size();
        for (uint64_t off : offsets_) writeBE(out, off, offsetSize);   // big-endian

        // trailer: 6 unused bytes + 1 byte offsetSize + 1 byte refSize + 3 u64s
        out.insert(out.end(), 6, 0);
        out.push_back(uint8_t(offsetSize));
        out.push_back(uint8_t(refSize));
        writeBE(out, numObjects, 8);
        writeBE(out, 0, 8);            // top object = 0
        writeBE(out, tableOff, 8);
        return out;
    }

private:
    std::vector<const Value*>          objs_;        // index → object
    std::map<const Value*, uint32_t>   index_;       // object → index
    std::vector<Ptr>                   extra_;       // synthesized objects (dict key strings)
    std::vector<uint64_t>              offsets_;

    static int intSize(uint64_t v)
    {
        if (v <= 0xFF) return 1;
        if (v <= 0xFFFF) return 2;
        if (v <= 0xFFFFFFFFu) return 4;
        return 8;
    }

    static void writeBE(std::vector<uint8_t>& out, uint64_t v, int bytes)
    {
        for (int k = bytes - 1; k >= 0; --k) out.push_back(uint8_t(v >> (8 * k)));
    }

    /// Synthesized objects (dict key strings) also go into the object table
    void addExtra(const Ptr& v) { extra_.push_back(v); }

    uint32_t collect(const Value* v, bool top = false)
    {
        (void)top;
        auto it = index_.find(v);
        if (it != index_.end()) return it->second;

        const uint32_t idx = uint32_t(objs_.size());
        index_[v] = idx;
        objs_.push_back(v);

        switch (v->type) {
        case Value::Type::Array:
            for (const auto& c : v->a) collect(c.get());
            break;
        case Value::Type::Dict:
            for (const auto& kv : v->m) {
                auto key = str(kv.first);          // key is a standalone string object
                addExtra(key);
                collect(key.get());
                collect(kv.second.get());
            }
            break;
        default: break;   // leaf (including long strings/data) has no children; count is inline
        }
        return idx;
    }

    void writeRef(std::vector<uint8_t>& out, uint32_t objIndex) const
    {
        // bplist references and the offset table are **big-endian** (wrong endianness makes parsers report "Invalid file")
        const int refSize = intSize(objs_.size() - 1);
        writeBE(out, objIndex, refSize);
    }

    /// Length <15: encoded in the marker; >=15: marker 0x?F + **inline integer token** (not a reference!)
    void writeCountOrInline(std::vector<uint8_t>& out, size_t count,
                            uint8_t shortMarker) const
    {
        if (count < 15) {
            out.push_back(uint8_t(shortMarker | count));
            return;
        }
        out.push_back(uint8_t(shortMarker | 0x0F));
        const int bytes = intSize(count);
        static const int kLog2[9] = {0, 0, 1, 0, 2, 0, 0, 0, 3};
        out.push_back(uint8_t(0x10 | kLog2[bytes]));
        writeBE(out, count, bytes);
    }

    void writeObject(const Value* v, std::vector<uint8_t>& out) const
    {
        switch (v->type) {
        case Value::Type::Bool:
            out.push_back(v->b ? 0x09 : 0x08);
            break;
        case Value::Type::Uint: {
            const int bytes = intSize(v->u);
            static const int kLog2[9] = {0, 0, 1, 0, 2, 0, 0, 0, 3};
            out.push_back(uint8_t(0x10 | kLog2[bytes]));
            writeBE(out, v->u, bytes);
            break;
        }
        case Value::Type::Int:
            // Negative numbers are rarely used; write them uniformly as signed 8-byte (bplist allows it)
            out.push_back(0x13);
            writeBE(out, uint64_t(v->i), 8);
            break;
        case Value::Type::Real: {
            out.push_back(0x23);
            uint64_t bits;
            std::memcpy(&bits, &v->r, 8);
            writeBE(out, bits, 8);
            break;
        }
        case Value::Type::Str: {
            // Handle pure ASCII only (all strings we generate are); non-ASCII goes to UTF-16BE
            bool ascii = true;
            for (unsigned char c : v->s) if (c >= 0x80) { ascii = false; break; }
            if (ascii) {
                writeCountOrInline(out, v->s.size(), 0x50);
                out.insert(out.end(), v->s.begin(), v->s.end());
            } else {
                std::vector<uint8_t> u16;
                u16.reserve(v->s.size() * 2);
                for (unsigned char c : v->s) { u16.push_back(0); u16.push_back(c); }  // fallback: treat each byte as U+00xx
                writeCountOrInline(out, v->s.size(), 0x60);
                out.insert(out.end(), u16.begin(), u16.end());
            }
            break;
        }
        case Value::Type::Data:
            writeCountOrInline(out, v->d.size(), 0x40);
            out.insert(out.end(), v->d.begin(), v->d.end());
            break;
        case Value::Type::Array:
            writeCountOrInline(out, v->a.size(), 0xA0);
            for (const auto& c : v->a) writeRef(out, index_.at(c.get()));
            break;
        case Value::Type::Dict:
            writeCountOrInline(out, v->m.size(), 0xD0);
            for (const auto& kv : v->m) {
                writeRef(out, findKeyObject(kv.first));
            }
            for (const auto& kv : v->m) writeRef(out, index_.at(kv.second.get()));
            break;
        }
    }

    /// Dict keys were synthesized during collect; recover their index by string content
    uint32_t findKeyObject(const std::string& key) const
    {
        // Key string objects have no other references, so find them by "equal content + is a key string":
        // collect adds one extra copy per key, so matching content is enough to locate it.
        for (const auto& ex : extra_) {
            if (ex->type == Value::Type::Str && ex->s == key) {
                auto it = index_.find(ex.get());
                if (it != index_.end()) return it->second;
            }
        }
        return 0;   // should not happen (every dict key was registered in collect)
    }
};

}  // namespace

std::vector<uint8_t> serialize(const Ptr& root)
{
    Writer w;
    return w.run(root);
}

// ---------------------------------------------------------------------------
// Read side (parse bplist request bodies sent by the phone)
// ---------------------------------------------------------------------------
namespace {

uint64_t readBE(const uint8_t* p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

class Reader {
public:
    Reader(const uint8_t* d, size_t len) : d_(d), len_(len) {}

    NodePtr run()
    {
        if (len_ < 40) return nullptr;
        if (std::memcmp(d_, "bplist00", 8) != 0) return nullptr;

        const size_t tr = len_ - 32;                 // trailer
        offsetSize_  = d_[tr + 6];
        refSize_     = d_[tr + 7];
        numObjects_  = readBE(d_ + tr + 8, 8);
        topObject_   = readBE(d_ + tr + 16, 8);
        offsetTable_ = readBE(d_ + tr + 24, 8);
        if (offsetSize_ < 1 || offsetSize_ > 8 || refSize_ < 1 || refSize_ > 8) return nullptr;
        if (topObject_ >= numObjects_) return nullptr;

        offsets_.resize(size_t(numObjects_));
        for (uint64_t i = 0; i < numObjects_; ++i) {
            size_t pos = offsetTable_ + size_t(i) * offsetSize_;
            if (pos + offsetSize_ > len_) return nullptr;
            offsets_[size_t(i)] = readBE(d_ + pos, offsetSize_);
        }
        return objectAt(offsets_[size_t(topObject_)]);
    }

private:
    NodePtr objectAt(uint64_t off)
    {
        if (off >= len_) return nullptr;
        auto it = cache_.find(off);
        if (it != cache_.end()) return it->second;

        auto n = std::make_shared<Node>();
        cache_[off] = n;                              // placeholder first to break cycles

        const uint8_t marker = d_[off];
        const uint8_t type  = marker >> 4;
        const uint8_t info  = marker & 0x0F;

        switch (type) {
        case 0x0:   // null / false / true
            if (info == 0x08)      { n->type = NodeType::Bool; n->b = false; }
            else if (info == 0x09) { n->type = NodeType::Bool; n->b = true; }
            else n->type = NodeType::Null;
            break;
        case 0x1: { // integer (1/2/4/8 bytes; >8 unsupported → fallback)
            if (info <= 3) {
                n->type = NodeType::Uint;
                n->u = readBE(d_ + off + 1, 1 << info);
            }
            break;
        }
        case 0x2: { // real
            n->type = NodeType::Real;
            if (info == 3) {
                uint64_t bits = readBE(d_ + off + 1, 8);
                std::memcpy(&n->r, &bits, 8);
            } else if (info == 2) {
                uint32_t bits = uint32_t(readBE(d_ + off + 1, 4));
                float f; std::memcpy(&f, &bits, 4); n->r = f;
            }
            break;
        }
        case 0x4: { // data
            uint64_t cnt; size_t consumed;
            readCount(off, info, &cnt, &consumed);
            size_t start = off + 1 + consumed;
            if (start + cnt <= len_) {
                n->type = NodeType::Data;
                n->data.assign(d_ + start, d_ + start + cnt);
            }
            break;
        }
        case 0x5: { // ASCII string
            uint64_t cnt; size_t consumed;
            readCount(off, info, &cnt, &consumed);
            size_t start = off + 1 + consumed;
            if (start + cnt <= len_) {
                n->type = NodeType::Str;
                n->str.assign(reinterpret_cast<const char*>(d_ + start), size_t(cnt));
            }
            break;
        }
        case 0x6: { // UTF-16BE string
            uint64_t cnt; size_t consumed;
            readCount(off, info, &cnt, &consumed);
            size_t start = off + 1 + consumed;
            if (start + cnt * 2 <= len_) {
                n->type = NodeType::Str;
                n->str = utf16beToUtf8(d_ + start, size_t(cnt));
            }
            break;
        }
        case 0x8: { // UID → integer
            n->type = NodeType::Uint;
            n->u = readBE(d_ + off + 1, info + 1);
            break;
        }
        case 0xA:
        case 0xC: { // array / set
            uint64_t cnt; size_t consumed;
            readCount(off, info, &cnt, &consumed);
            n->type = NodeType::Array;
            size_t start = off + 1 + consumed;
            for (uint64_t i = 0; i < cnt; ++i) {
                size_t pos = start + size_t(i) * refSize_;
                if (pos + refSize_ > len_) break;
                uint64_t ref = readBE(d_ + pos, refSize_);
                if (ref >= numObjects_) break;
                NodePtr c = objectAt(offsets_[size_t(ref)]);
                if (c) n->array.push_back(c);
            }
            break;
        }
        case 0xD: { // dict (keys first, then values)
            uint64_t cnt; size_t consumed;
            readCount(off, info, &cnt, &consumed);
            n->type = NodeType::Dict;
            size_t start = off + 1 + consumed;
            std::vector<NodePtr> keys, vals;
            for (uint64_t i = 0; i < cnt; ++i) {
                size_t pos = start + size_t(i) * refSize_;
                if (pos + refSize_ > len_) return n;
                uint64_t ref = readBE(d_ + pos, refSize_);
                if (ref >= numObjects_) return n;
                keys.push_back(objectAt(offsets_[size_t(ref)]));
            }
            for (uint64_t i = 0; i < cnt; ++i) {
                size_t pos = start + size_t(cnt + i) * refSize_;
                if (pos + refSize_ > len_) return n;
                uint64_t ref = readBE(d_ + pos, refSize_);
                if (ref >= numObjects_) return n;
                vals.push_back(objectAt(offsets_[size_t(ref)]));
            }
            for (uint64_t i = 0; i < cnt; ++i) {
                std::string key = keys[size_t(i)] ? keys[size_t(i)]->str : std::string();
                n->dict.emplace_back(std::move(key), vals[size_t(i)]);
            }
            break;
        }
        default:
            break;
        }
        return n;
    }

    void readCount(size_t markerPos, uint8_t info, uint64_t* cnt, size_t* consumed)
    {
        if (info < 0x0F) { *cnt = info; *consumed = 0; return; }
        uint8_t m = d_[markerPos + 1];
        int nbytes = 1 << (m & 0x0F);
        *cnt = readBE(d_ + markerPos + 2, nbytes);
        *consumed = 1 + size_t(nbytes);
    }

    static std::string utf16beToUtf8(const uint8_t* p, size_t units)
    {
        std::string out;
        for (size_t i = 0; i < units; ++i) {
            uint32_t cp = (uint32_t(p[2 * i]) << 8) | p[2 * i + 1];
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units) {
                uint32_t lo = (uint32_t(p[2 * i + 2]) << 8) | p[2 * i + 3];
                if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); ++i; }
            }
            if (cp < 0x80) out.push_back(char(cp));
            else if (cp < 0x800) { out.push_back(char(0xC0 | (cp >> 6))); out.push_back(char(0x80 | (cp & 0x3F))); }
            else if (cp < 0x10000) { out.push_back(char(0xE0 | (cp >> 12))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F))); }
            else { out.push_back(char(0xF0 | (cp >> 18))); out.push_back(char(0x80 | ((cp >> 12) & 0x3F))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F))); }
        }
        return out;
    }

    const uint8_t* d_;
    size_t len_;
    uint8_t  offsetSize_ = 0, refSize_ = 0;
    uint64_t numObjects_ = 0, topObject_ = 0, offsetTable_ = 0;
    std::vector<uint64_t> offsets_;
    std::map<uint64_t, NodePtr> cache_;
};

}  // namespace

NodePtr parse(const uint8_t* data, size_t len)
{
    if (!data || len == 0) return nullptr;
    Reader r(data, len);
    return r.run();
}

NodePtr dictGet(const NodePtr& n, const char* key)
{
    if (!n || n->type != NodeType::Dict) return nullptr;
    for (const auto& kv : n->dict)
        if (kv.first == key) return kv.second;
    return nullptr;
}

NodePtr arrGet(const NodePtr& n, size_t idx)
{
    if (!n || n->type != NodeType::Array || idx >= n->array.size()) return nullptr;
    return n->array[idx];
}

std::string strOf(const NodePtr& n)
{
    return (n && n->type == NodeType::Str) ? n->str : std::string();
}

std::vector<uint8_t> dataOf(const NodePtr& n)
{
    return (n && n->type == NodeType::Data) ? n->data : std::vector<uint8_t>();
}

uint64_t uintOf(const NodePtr& n)
{
    if (!n) return 0;
    if (n->type == NodeType::Uint) return n->u;
    if (n->type == NodeType::Int) return uint64_t(n->i);
    return 0;
}

bool boolOf(const NodePtr& n)
{
    return n && n->type == NodeType::Bool && n->b;
}

}  // namespace core::bplist
