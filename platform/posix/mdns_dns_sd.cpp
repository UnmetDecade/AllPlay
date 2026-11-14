// ============================================================================
// platform/posix/mdns_dns_sd.cpp -- macOS backend: system-provided mDNSResponder (dns_sd.h)
// ============================================================================
// On macOS mDNSResponder is always running, so dns_sd is directly usable: no avahi-like service is
// needed, nor any password/permissions. TXT records are properly encoded with the TXTRecord* API (length-prefixed).
//
// Event loop: get the fd from DNSServiceRefSockFD and poll it with select, calling DNSServiceProcessResult on timeout.
// (DNSServiceSetDispatchQueue is not used, avoiding a libdispatch dependency.)
#include "posix/posix.h"

#include "common/log.h"

#include <dns_sd.h>

#include <arpa/inet.h>   // htons
#include <sys/select.h>
#include <unistd.h>

#include <atomic>
#include <thread>

namespace allplay::posix {

namespace {

class DnsSdBonjour : public core::IMdnsAdvertiser {
public:
    ~DnsSdBonjour() override { stop(); }

    bool start(const std::string& name, uint16_t port,
               const std::vector<std::string>& txt, std::string* err) override
    {
        // "k=v" -> TXT (v may contain '=', split only on the first '=')
        TXTRecordRef rec;
        TXTRecordCreate(&rec, 0, nullptr);
        for (const auto& kv : txt) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            std::string k = kv.substr(0, eq);
            std::string v = kv.substr(eq + 1);
            TXTRecordSetValue(&rec, k.c_str(), (uint8_t)v.size(), v.data());
        }

        DNSServiceErrorType e = DNSServiceRegister(
            &ref_, /*flags=*/0, /*ifIndex=*/0,
            name.c_str(), "_airplay._tcp", /*domain=*/nullptr, /*host=*/nullptr,
            htons(port),
            TXTRecordGetLength(&rec), TXTRecordGetBytesPtr(&rec),
            /*callback=*/nullptr, /*context=*/nullptr);
        TXTRecordDeallocate(&rec);

        if (e != kDNSServiceErr_NoError) {
            // dns_sd has no public error->string; logging the error code is enough
            if (err) *err = "DNSServiceRegister failed: errno=" + std::to_string(int(e));
            ref_ = nullptr;
            return false;
        }

        fd_ = DNSServiceRefSockFD(ref_);
        if (fd_ < 0) {
            if (err) *err = "DNSServiceRefSockFD failed";
            DNSServiceRefDeallocate(ref_);
            ref_ = nullptr;
            return false;
        }

        name_ = name;
        port_ = port;
        stop_.store(false);
        thread_ = std::thread([this] { loop(); });
        LOG_INFO("Bonjour advertised (dns_sd): %s._airplay._tcp :%u", name_.c_str(), port_);
        return true;
    }

    void stop() override
    {
        if (!ref_) return;
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        DNSServiceRefDeallocate(ref_);
        ref_ = nullptr;
        fd_ = -1;
    }

private:
    void loop()
    {
        while (!stop_.load()) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(fd_, &set);
            timeval tv{0, 200 * 1000};   // 200ms
            int r = ::select(fd_ + 1, &set, nullptr, nullptr, &tv);
            if (r > 0 && FD_ISSET(fd_, &set)) {
                if (DNSServiceProcessResult(ref_) != kDNSServiceErr_NoError) break;
            }
        }
    }

    DNSServiceRef     ref_ = nullptr;
    int               fd_  = -1;
    std::string       name_;
    uint16_t          port_ = 0;
    std::atomic<bool> stop_{true};
    std::thread       thread_;
};

}  // namespace

std::unique_ptr<core::IMdnsAdvertiser> makeMdnsAdvertiser()
{
    return std::unique_ptr<core::IMdnsAdvertiser>(new DnsSdBonjour);
}

}  // namespace allplay::posix
