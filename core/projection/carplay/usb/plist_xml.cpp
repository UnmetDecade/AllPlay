/**
 * @file   plist_xml.cpp
 * @brief  Implementation of the minimal XML plist
 * @author blt
 */

#include "usb/plist_xml.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace core::usb {
namespace {

// ---------------------------------------------------------------------------
// base64 (plist <data> uses base64; hand-rolled to avoid digging into EVP's edge behavior for this)
// ---------------------------------------------------------------------------
const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::vector<uint8_t>& in)
{
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t b0 = in[i];
        const uint32_t b1 = i + 1 < in.size() ? in[i + 1] : 0;
        const uint32_t b2 = i + 2 < in.size() ? in[i + 2] : 0;
        const uint32_t n = (b0 << 16) | (b1 << 8) | b2;
        out += kB64[(n >> 18) & 0x3f];
        out += kB64[(n >> 12) & 0x3f];
        out += i + 1 < in.size() ? kB64[(n >> 6) & 0x3f] : '=';
        out += i + 2 < in.size() ? kB64[n & 0x3f] : '=';
    }
    return out;
}

int b64Value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool base64Decode(const std::string& in, std::vector<uint8_t>* out)
{
    out->clear();
    uint32_t acc = 0;
    int      bits = 0;
    for (char c : in) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (c == '=') break;
        const int v = b64Value(c);
        if (v < 0) return false;
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(uint8_t((acc >> bits) & 0xff));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// XML escaping
// ---------------------------------------------------------------------------
std::string escapeXml(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string unescapeXml(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '&') {
            out += s[i++];
            continue;
        }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos) {
            out += s[i++];
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            const long cp = std::strtol(ent.c_str() + 1, nullptr, 10);
            // Only handle within BMP; lockdown needs nothing larger
            if (cp > 0 && cp < 0x10000) {
                if (cp < 0x80) {
                    out += char(cp);
                } else if (cp < 0x800) {
                    out += char(0xc0 | (cp >> 6));
                    out += char(0x80 | (cp & 0x3f));
                } else {
                    out += char(0xe0 | (cp >> 12));
                    out += char(0x80 | ((cp >> 6) & 0x3f));
                    out += char(0x80 | (cp & 0x3f));
                }
            }
        } else {
            out += '&';  // unknown entity: keep as-is, don't swallow the data
            out += ent;
            out += ';';
        }
        i = semi + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
void indent(std::string& out, int depth)
{
    for (int i = 0; i < depth; ++i) out += "\t";
}

void writeValue(std::string& out, const Plist& v, int depth)
{
    switch (v.type()) {
        case Plist::Type::Dict:
            if (v.items().empty()) {
                out += "<dict/>\n";
                break;
            }
            out += "<dict>\n";
            for (const auto& kv : v.items()) {
                indent(out, depth + 1);
                out += "<key>" + escapeXml(kv.first) + "</key>\n";
                indent(out, depth + 1);
                writeValue(out, kv.second, depth + 1);
            }
            indent(out, depth);
            out += "</dict>\n";
            break;

        case Plist::Type::Array:
            if (v.elements().empty()) {
                out += "<array/>\n";
                break;
            }
            out += "<array>\n";
            for (const auto& e : v.elements()) {
                indent(out, depth + 1);
                writeValue(out, e, depth + 1);
            }
            indent(out, depth);
            out += "</array>\n";
            break;

        case Plist::Type::Bool:
            out += v.asBool() ? "<true/>\n" : "<false/>\n";
            break;

        case Plist::Type::Integer: {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v.asInteger()));
            out += std::string("<integer>") + buf + "</integer>\n";
            break;
        }

        case Plist::Type::Real: {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.17g", v.asReal());
            out += std::string("<real>") + buf + "</real>\n";
            break;
        }

        case Plist::Type::String:
            out += "<string>" + escapeXml(v.asString()) + "</string>\n";
            break;

        case Plist::Type::Data: {
            const std::string b64 = base64Encode(v.asData());
            // Wrap at 68 characters: close to what Apple outputs, easier to diff
            out += "<data>\n";
            for (size_t i = 0; i < b64.size(); i += 68) {
                indent(out, depth + 1);
                out += b64.substr(i, 68) + "\n";
            }
            indent(out, depth);
            out += "</data>\n";
            break;
        }

        case Plist::Type::Null:
        default:
            out += "<string></string>\n";
            break;
    }
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
struct Cursor {
    const std::string& s;
    size_t             i = 0;

    bool eof() const { return i >= s.size(); }
    bool startsWith(const char* p) const
    {
        const size_t n = std::strlen(p);
        return s.compare(i, n, p) == 0;
    }
    void skipSpace()
    {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    /// Move past c from the current position (used to skip <?...?> / <!...>)
    bool skipPast(char c)
    {
        const size_t p = s.find(c, i);
        if (p == std::string::npos) return false;
        i = p + 1;
        return true;
    }
    /// Skip the start tag (with attributes), stopping after '>'; selfClosing may be null, if non-null reports whether it was <x/>
    bool skipStartTag(bool* selfClosing)
    {
        const size_t gt = s.find('>', i);
        if (gt == std::string::npos) return false;
        if (selfClosing != nullptr) *selfClosing = (gt > 0 && s[gt - 1] == '/');
        i = gt + 1;
        return true;
    }
    bool skipEndTag()
    {
        const size_t gt = s.find('>', i);
        if (gt == std::string::npos) return false;
        i = gt + 1;
        return true;
    }
    /// Read all text up to closeTag and consume closeTag
    bool readText(const char* closeTag, std::string* out)
    {
        const size_t p = s.find(closeTag, i);
        if (p == std::string::npos) return false;
        *out = s.substr(i, p - i);
        i = p + std::strlen(closeTag);
        return true;
    }
};

bool parseValue(Cursor& c, Plist* out, std::string* err);

bool parseDict(Cursor& c, Plist* out, std::string* err)
{
    bool selfClosing = false;
    if (!c.skipStartTag(&selfClosing)) {
        *err = "dict start tag is not closed";
        return false;
    }
    *out = Plist::dict();
    if (selfClosing) return true;

    for (;;) {
        c.skipSpace();
        if (c.startsWith("</dict>")) {
            c.skipEndTag();
            return true;
        }
        if (!c.startsWith("<key>")) {
            *err = "Expected <key> inside dict";
            return false;
        }
        c.skipStartTag(nullptr);
        std::string key;
        if (!c.readText("</key>", &key)) {
            *err = "key is not closed";
            return false;
        }
        c.skipSpace();
        Plist v;
        if (!parseValue(c, &v, err)) return false;
        out->set(unescapeXml(key), std::move(v));
    }
}

bool parseArray(Cursor& c, Plist* out, std::string* err)
{
    bool selfClosing = false;
    if (!c.skipStartTag(&selfClosing)) {
        *err = "array start tag is not closed";
        return false;
    }
    *out = Plist::array();
    if (selfClosing) return true;

    for (;;) {
        c.skipSpace();
        if (c.startsWith("</array>")) {
            c.skipEndTag();
            return true;
        }
        Plist v;
        if (!parseValue(c, &v, err)) return false;
        out->elements().push_back(std::move(v));
    }
}

bool parseValue(Cursor& c, Plist* out, std::string* err)
{
    c.skipSpace();
    if (c.eof()) {
        *err = "Unexpected end of XML";
        return false;
    }

    if (c.startsWith("<dict")) return parseDict(c, out, err);
    if (c.startsWith("<array")) return parseArray(c, out, err);

    if (c.startsWith("<true")) {
        bool sc = false;
        if (!c.skipStartTag(&sc)) { *err = "<true> is not closed"; return false; }
        *out = Plist::boolean(true);
        return true;
    }
    if (c.startsWith("<false")) {
        bool sc = false;
        if (!c.skipStartTag(&sc)) { *err = "<false> is not closed"; return false; }
        *out = Plist::boolean(false);
        return true;
    }

    struct TextTag {
        const char* open;
        const char* close;
    };
    // NOTE: should /integer come before /int...? No prefix conflict here, so just scan linearly
    static const TextTag kTextTags[] = {
        {"<string", "</string>"}, {"<integer", "</integer>"}, {"<real", "</real>"},
        {"<data", "</data>"},     {"<date", "</date>"},
    };
    for (const TextTag& t : kTextTags) {
        if (!c.startsWith(t.open)) continue;
        bool selfClosing = false;
        if (!c.skipStartTag(&selfClosing)) {
            *err = std::string(t.open) + " is not closed";
            return false;
        }
        std::string text;
        if (!selfClosing && !c.readText(t.close, &text)) {
            *err = std::string(t.close) + " not found";
            return false;
        }
        if (std::strcmp(t.open, "<integer") == 0) {
            *out = Plist::integer(std::strtoll(text.c_str(), nullptr, 10));
        } else if (std::strcmp(t.open, "<real") == 0) {
            *out = Plist::real(std::strtod(text.c_str(), nullptr));
        } else if (std::strcmp(t.open, "<data") == 0) {
            std::vector<uint8_t> bin;
            if (!base64Decode(text, &bin)) {
                *err = "data is not valid base64";
                return false;
            }
            *out = Plist::data(std::move(bin));
        } else {
            // <string> and <date> (date kept as a string; we never send it back)
            *out = Plist::string(unescapeXml(text));
        }
        return true;
    }

    *err = "Unknown plist tag: " + c.s.substr(c.i, 24);
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
Plist Plist::boolean(bool v) { Plist p; p.type_ = Type::Bool; p.bool_ = v; return p; }
Plist Plist::integer(int64_t v) { Plist p; p.type_ = Type::Integer; p.int_ = v; return p; }
Plist Plist::real(double v) { Plist p; p.type_ = Type::Real; p.real_ = v; return p; }
Plist Plist::string(std::string v) { Plist p; p.type_ = Type::String; p.str_ = std::move(v); return p; }
Plist Plist::data(std::vector<uint8_t> v) { Plist p; p.type_ = Type::Data; p.data_ = std::move(v); return p; }
Plist Plist::dict() { Plist p; p.type_ = Type::Dict; return p; }
Plist Plist::array() { Plist p; p.type_ = Type::Array; return p; }

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
bool Plist::asBool(bool def) const
{
    if (type_ == Type::Bool) return bool_;
    if (type_ == Type::Integer) return int_ != 0;
    return def;
}

int64_t Plist::asInteger(int64_t def) const
{
    if (type_ == Type::Integer) return int_;
    if (type_ == Type::Bool) return bool_ ? 1 : 0;
    if (type_ == Type::String) return std::strtoll(str_.c_str(), nullptr, 10);
    return def;
}

double Plist::asReal(double def) const
{
    if (type_ == Type::Real) return real_;
    if (type_ == Type::Integer) return double(int_);
    return def;
}

const std::string& Plist::asString() const
{
    static const std::string kEmpty;
    return type_ == Type::String ? str_ : kEmpty;
}

const std::vector<uint8_t>& Plist::asData() const
{
    static const std::vector<uint8_t> kEmpty;
    return type_ == Type::Data ? data_ : kEmpty;
}

const Plist* Plist::find(const std::string& key) const
{
    if (type_ != Type::Dict) return nullptr;
    for (const auto& kv : dict_) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

std::string Plist::stringValue(const std::string& key, const std::string& def) const
{
    const Plist* v = find(key);
    return v != nullptr ? v->asString() : def;
}

int64_t Plist::intValue(const std::string& key, int64_t def) const
{
    const Plist* v = find(key);
    return v != nullptr ? v->asInteger(def) : def;
}

bool Plist::boolValue(const std::string& key, bool def) const
{
    const Plist* v = find(key);
    return v != nullptr ? v->asBool(def) : def;
}

void Plist::set(const std::string& key, Plist v)
{
    if (type_ != Type::Dict) {
        type_ = Type::Dict;
        dict_.clear();
    }
    for (auto& kv : dict_) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    }
    dict_.emplace_back(key, std::move(v));
}

// ---------------------------------------------------------------------------
// XML
// ---------------------------------------------------------------------------
std::string Plist::toXml() const
{
    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    out += "<plist version=\"1.0\">\n";
    writeValue(out, *this, 0);
    out += "</plist>\n";
    return out;
}

bool Plist::parse(const std::string& xml, Plist* out, std::string* err)
{
    std::string dummy;
    if (err == nullptr) err = &dummy;

    Cursor c{xml, 0};
    c.skipSpace();
    std::string pi;
    while (c.startsWith("<?")) {
        // Must search for **"?>"** (two chars): searching only for '?' would hit the '?' of "<?"
        // itself, so the declaration could never be skipped (the phone's plist includes an XML declaration; hit this in practice).
        if (!c.readText("?>", &pi)) { *err = "XML declaration is not closed"; return false; }
        c.skipSpace();
    }
    while (c.startsWith("<!")) {
        if (!c.skipPast('>')) { *err = "DOCTYPE is not closed"; return false; }
        c.skipSpace();
    }
    if (!c.startsWith("<plist")) {
        *err = "Not a plist (starts with " + xml.substr(0, 20) + ")";
        return false;
    }
    bool selfClosing = false;
    if (!c.skipStartTag(&selfClosing)) {
        *err = "plist start tag is not closed";
        return false;
    }
    if (!parseValue(c, out, err)) return false;
    return true;
}

}  // namespace core::usb
