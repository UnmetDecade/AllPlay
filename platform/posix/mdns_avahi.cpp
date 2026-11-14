// ============================================================================
// platform/posix/mdns_avahi.cpp -- Linux backend: talks to the system avahi-daemon over D-Bus
// ============================================================================
// A missing avahi-daemon is not fatal: start() returns false with a reason (RTSP keeps listening as usual).
#include "posix/posix.h"

#include "common/log.h"

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/strlst.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace allplay::posix {

namespace {

class AvahiBonjour : public core::IMdnsAdvertiser {
public:
    ~AvahiBonjour() override { stop(); }

    bool start(const std::string& name, uint16_t port,
               const std::vector<std::string>& txt, std::string* err) override
    {
        name_ = name;
        port_ = port;
        txt_  = txt;

        poll_ = avahi_simple_poll_new();
        if (!poll_) {
            if (err) *err = "avahi_simple_poll_new failed";
            return false;
        }
        int aerr = 0;
        // NO_FAIL: do not fail immediately when the daemon is absent (we do not commit after entering FAILURE, see the callback)
        client_ = avahi_client_new(avahi_simple_poll_get(poll_), AVAHI_CLIENT_NO_FAIL,
                                   &AvahiBonjour::clientCb, this, &aerr);
        if (!client_) {
            if (err) *err = std::string("failed to connect to avahi-daemon: ") + avahi_strerror(aerr);
            avahi_simple_poll_free(poll_);
            poll_ = nullptr;
            return false;
        }

        stop_.store(false);
        thread_ = std::thread([this] {
            while (!stop_.load()) {
                if (avahi_simple_poll_iterate(poll_, 200) != 0) break;
            }
        });

        // Wait for the advertisement to actually commit (up to ~0.5s) so the result can be reported truthfully to the caller
        for (int i = 0; i < 25; ++i) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (published_) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (err) *err = "avahi did not reach RUNNING within 0.5s (daemon not running?)";
        return false;
    }

    void stop() override
    {
        stop_.store(true);
        if (poll_) avahi_simple_poll_quit(poll_);
        if (thread_.joinable()) thread_.join();

        std::lock_guard<std::mutex> lk(mu_);
        if (group_)  { avahi_entry_group_free(group_); group_ = nullptr; }
        if (client_) { avahi_client_free(client_);     client_ = nullptr; }
        if (poll_)   { avahi_simple_poll_free(poll_);  poll_ = nullptr; }
    }

private:
    static void clientCb(AvahiClient* c, AvahiClientState state, void* userdata)
    {
        auto* self = static_cast<AvahiBonjour*>(userdata);
        if (state != AVAHI_CLIENT_S_RUNNING) return;

        std::lock_guard<std::mutex> lk(self->mu_);
        if (!self->group_) {
            self->group_ = avahi_entry_group_new(c, nullptr, nullptr);
            if (!self->group_) {
                LOG_WARN("avahi: failed to create entry group: %s", avahi_strerror(avahi_client_errno(c)));
                return;
            }
        }
        if (avahi_entry_group_is_empty(self->group_)) {
            // NOTE: avahi_entry_group_add_service() is a variadic interface; it treats `...` as
            // a NULL-terminated sequence of const char*. Passing cstr.data() (a const char**) makes
            // avahi read **the pointer array's own memory** as a string, so the TXT record becomes a few bytes of binary garbage
            // -- and the iPhone never connects to AirPlay without features/flags/deviceid.
            // Must use the strlst variant with AvahiStringList.
            AvahiStringList* lst = nullptr;
            for (auto it = self->txt_.rbegin(); it != self->txt_.rend(); ++it)
                lst = avahi_string_list_add(lst, it->c_str());  // add prepends, so adding in reverse yields the original order

            int r = avahi_entry_group_add_service_strlst(
                self->group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, (AvahiPublishFlags)0,
                self->name_.c_str(), "_airplay._tcp", nullptr, nullptr,
                self->port_, lst);
            avahi_string_list_free(lst);
            if (r < 0) {
                LOG_WARN("avahi: add_service failed: %s", avahi_strerror(r));
                return;
            }
            if ((r = avahi_entry_group_commit(self->group_)) < 0) {
                LOG_WARN("avahi: commit failed: %s", avahi_strerror(r));
                return;
            }
            self->published_ = true;
            LOG_INFO("Bonjour advertised (avahi): %s._airplay._tcp :%u",
                     self->name_.c_str(), self->port_);
        }
    }

    AvahiSimplePoll* poll_   = nullptr;
    AvahiClient*     client_ = nullptr;
    AvahiEntryGroup* group_  = nullptr;

    std::string              name_;
    uint16_t                 port_ = 0;
    std::vector<std::string> txt_;

    std::thread       thread_;
    std::atomic<bool> stop_{true};
    std::mutex        mu_;
    bool              published_ = false;
};

}  // namespace

std::unique_ptr<core::IMdnsAdvertiser> makeMdnsAdvertiser()
{
    return std::unique_ptr<core::IMdnsAdvertiser>(new AvahiBonjour);
}

}  // namespace allplay::posix
