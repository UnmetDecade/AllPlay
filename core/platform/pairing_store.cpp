#include "platform/pairing_store.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace core {

namespace fs = std::filesystem;

namespace {

std::string hexOf(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(d[p[i] >> 4]);
        out.push_back(d[p[i] & 0xF]);
    }
    return out;
}

bool hexToBytes(const std::string& s, std::vector<uint8_t>* out)
{
    if (s.size() % 2 != 0) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out->resize(s.size() / 2);
    for (size_t i = 0; i < out->size(); ++i) {
        int hi = nib(s[2 * i]), lo = nib(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        (*out)[i] = uint8_t((hi << 4) | lo);
    }
    return true;
}

std::string readLine(const fs::path& p, bool* ok)
{
    std::ifstream in(p);
    if (!in) { *ok = false; return {}; }
    std::string s;
    std::getline(in, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    *ok = true;
    return s;
}

bool writeFile0600(const fs::path& p, const std::string& data)
{
    // .string().c_str(): on Windows path::value_type is wchar_t, but fopen wants char*
    FILE* f = std::fopen(p.string().c_str(), "w");
    if (!f) return false;
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

}  // namespace

bool PairingStore::load(const std::string& stateDir)
{
    bool ok = false;
    std::string id = readLine(fs::path(stateDir) / "controller_id", &ok);
    if (!ok) return false;
    std::string pkhex = readLine(fs::path(stateDir) / "controller_ltpk", &ok);
    if (!ok) return false;
    std::vector<uint8_t> pk;
    if (!hexToBytes(pkhex, &pk) || pk.size() != 32) return false;
    controllerId_ = std::move(id);
    controllerLtpk_ = std::move(pk);
    return true;
}

bool PairingStore::save(const std::string& stateDir, const std::string& id,
                        const std::vector<uint8_t>& ltpk32)
{
    if (ltpk32.size() != 32) return false;
    std::error_code ec;
    fs::create_directories(stateDir, ec);
    if (ec) return false;

    controllerId_ = id;
    controllerLtpk_ = ltpk32;
    return writeFile0600(fs::path(stateDir) / "controller_id", id + "\n") &&
           writeFile0600(fs::path(stateDir) / "controller_ltpk", hexOf(ltpk32.data(), 32) + "\n");
}

}  // namespace core
