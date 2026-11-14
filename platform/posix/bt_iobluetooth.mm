// ============================================================================
// platform/posix/bt_iobluetooth.mm -- macOS transport (IOBluetooth RFCOMM)
// ============================================================================
// Key findings from real hardware:
//   - Since macOS 27, `pairedDevices`/`isPaired`/SDP queries silently return empty for processes
//     **without Bluetooth permission** -- the process must be launched from a desktop session (TCC attributed to Terminal).
//   - RFCOMM delegate callbacks run on the **initiating thread's run loop** -- both opening and
//     receiving data run an NSRunLoop on a dedicated thread.
//   - This file is compiled **without ARC** (CMake passes -fno-objc-arc), with explicit retain/release.
#include "posix/posix.h"

#include "common/log.h"

#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>
#import <IOBluetooth/IOBluetoothUserLib.h>   // IOBluetoothAddServiceDict
#import <CoreWLAN/CoreWLAN.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace allplay::posix {
class MacRfcommStream;   // forward declaration (for the delegate's owner property)
}

namespace allplay::posix {
/// Server-side wait: the observer fills stream when the phone connects (cross-thread, small lock + polling)
struct ServerWait {
    std::mutex       mu;
    MacRfcommStream* stream = nullptr;
};
}

// Observer for incoming RFCOMM channels (server side: wait for the phone to connect to our "Wireless iAPv2")
@interface RfcommServerObserver : NSObject
@property(nonatomic, assign) allplay::posix::ServerWait* wait;
@end

// ---------------------------------------------------------------------------
// delegate: forwards callbacks to the C++ object
// ---------------------------------------------------------------------------
@interface RfcommDelegate : NSObject <IOBluetoothRFCOMMChannelDelegate>
@property(nonatomic, assign) allplay::posix::MacRfcommStream* owner;
@end


namespace allplay::posix {

constexpr int kOpenTimeoutMs = 20000;

// ---------------------------------------------------------------------------
// RFCOMM byte stream
// ---------------------------------------------------------------------------
class MacRfcommStream : public core::IByteStream {
public:
    MacRfcommStream(IOBluetoothDevice* device, BluetoothRFCOMMChannelID channelId)
        : device_([device retain]), channelId_(channelId)
    {
        delegate_ = [[RfcommDelegate alloc] init];
        delegate_.owner = this;
    }

    /// Server side: the channel was established by the inbound connection (no open() needed)
    explicit MacRfcommStream(IOBluetoothRFCOMMChannel* opened)
    {
        channel_ = [opened retain];
        delegate_ = [[RfcommDelegate alloc] init];
        delegate_.owner = this;
        [channel_ setDelegate:delegate_];
        openDone_ = true;
        openedOk_ = true;
    }

    ~MacRfcommStream() override
    {
        if (channel_) {
            [channel_ closeChannel];
            [channel_ release];
            channel_ = nil;
        }
        if (delegate_) {
            delegate_.owner = nullptr;
            [delegate_ release];
            delegate_ = nil;
        }
        if (device_) {
            [device_ release];
            device_ = nil;
        }
    }

    /// Open asynchronously -> **pump the run loop while waiting** for openComplete (up to 20s).
    /// WARNING: the run loop must be pumped here: the completion callback is dispatched via the run loop, so blocking
    ///    on a condition variable on this thread means the callback never arrives (symptom: RFCOMM open timeout).
    bool open(std::string* err)
    {
        IOReturn r = [device_ openRFCOMMChannelAsync:&channel_
                                       withChannelID:channelId_
                                            delegate:delegate_];
        if (r != kIOReturnSuccess || !channel_) {
            if (err) *err = "openRFCOMMChannelAsync failed (ret=" + std::to_string((int)r) + ")";
            return false;
        }
        [channel_ retain];

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kOpenTimeoutMs);
        while (!openDone_ && std::chrono::steady_clock::now() < deadline) {
            @autoreleasepool {
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
            }
        }
        if (!openDone_) {
            if (err) *err = "RFCOMM open timed out";
            return false;
        }
        if (!openedOk_) {
            if (err) *err = "RFCOMM open rejected (peer refused?)";
            return false;
        }
        return true;
    }

    int read(uint8_t* buf, size_t n, int timeoutMs) override
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cvData_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                              [&] { return !queue_.empty() || closed_; })) {
            return 0;   // timeout
        }
        if (queue_.empty()) return -1;   // closed
        const size_t take = std::min(n, queue_.size());
        for (size_t i = 0; i < take; ++i) {
            buf[i] = queue_.front();
            queue_.pop_front();
        }
        return int(take);
    }

    bool writeAll(const uint8_t* buf, size_t n) override
    {
        if (!channel_) return false;
        size_t off = 0;
        while (off < n) {
            const size_t chunk = std::min<size_t>(n - off, 4096);
            IOReturn r = [channel_ writeSync:(void*)(buf + off) length:(UInt16)chunk];
            if (r != kIOReturnSuccess) return false;
            off += chunk;
        }
        return true;
    }

    void close() override
    {
        IOBluetoothRFCOMMChannel* ch = channel_;
        channel_ = nil;
        if (ch) {
            [ch closeChannel];
            [ch release];
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        cvData_.notify_all();
        cvOpen_.notify_all();
    }

    // ---- delegate callback entry points ----
    void onData(const void* data, size_t len)
    {
        std::lock_guard<std::mutex> lk(mu_);
        const uint8_t* p = static_cast<const uint8_t*>(data);
        queue_.insert(queue_.end(), p, p + len);
        cvData_.notify_all();
    }
    void onClosed()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        cvData_.notify_all();
    }
    void onOpenComplete(IOReturn status)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            openDone_ = true;
            openedOk_ = (status == kIOReturnSuccess);
        }
        cvOpen_.notify_all();
    }

    bool closed() const
    {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mu_));
        return closed_;
    }

private:
    IOBluetoothDevice*        device_;      // retained
    IOBluetoothRFCOMMChannel* channel_ = nil;
    BluetoothRFCOMMChannelID  channelId_;
    RfcommDelegate*           delegate_ = nil;   // +1

    std::mutex              mu_;
    std::condition_variable cvData_, cvOpen_;
    std::deque<uint8_t>     queue_;
    bool                    closed_ = false;
    bool                    openDone_ = false;
    bool                    openedOk_ = false;
};

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------
namespace {

class MacTransport : public core::IBluetooth {
public:
    std::vector<core::BtDevice> pairedDevices(std::string* err) override
    {
        std::vector<core::BtDevice> out;
        @autoreleasepool {
            NSArray* paired = [IOBluetoothDevice pairedDevices];
            for (IOBluetoothDevice* d in paired) {
                core::BtDevice dev;
                const char* n = [[d name] UTF8String];
                dev.name = n ? n : "";
                const char* a = [[d addressString] UTF8String];
                dev.address = a ? a : "";
                dev.connected = [d isConnected];
                out.push_back(std::move(dev));
            }
        }
        if (out.empty() && err) {
            *err = "paired device list is empty (macOS 27: this process may lack Bluetooth permission -- "
                   "launch it from Terminal in the desktop session, not over ssh)";
        }
        return out;
    }

    core::IByteStream* connectRFCOMM(const std::string& address, const std::string& uuid,
                                    std::string* err) override
    {
        // UUID string -> 16 bytes
        uint8_t raw[16] = {0};
        int bi = 0;
        for (size_t i = 0; i < uuid.size() && bi < 16;) {
            if (uuid[i] == '-') { ++i; continue; }
            raw[bi++] = uint8_t(strtoul(uuid.substr(i, 2).c_str(), nullptr, 16));
            i += 2;
        }
        if (bi != 16) {
            if (err) *err = "malformed UUID (expected 8-4-4-4-12)";
            return nullptr;
        }

        struct Result {
            MacRfcommStream* stream = nullptr;
            std::string      err;
            bool             done = false;
        };
        auto res = std::make_shared<Result>();
        std::mutex              mu;
        std::condition_variable cv;

        // IOBluetooth open and callbacks both need a run loop -> use a dedicated thread
        std::thread worker([res, &address, raw, &mu, &cv] {
            @autoreleasepool {
                NSString* addr = [NSString stringWithUTF8String:address.c_str()];
                IOBluetoothDevice* device = [IOBluetoothDevice deviceWithAddressString:addr];
                IOBluetoothSDPUUID* u = [IOBluetoothSDPUUID uuidWithBytes:raw length:16];
                MacRfcommStream* stream = nullptr;

                if (!device) {
                    res->err = "deviceWithAddressString failed (bad address format?)";
                } else {
                    IOBluetoothSDPServiceRecord* record = [device getServiceRecordForUUID:u];
                    BluetoothRFCOMMChannelID chid = 0;
                    if (!record) {
                        res->err = "SDP lookup found no iAP2 SPP service (not paired / no permission / phone not advertising)";
                    } else if ([record getRFCOMMChannelID:&chid] != kIOReturnSuccess) {
                        res->err = "failed to get RFCOMM channel number";
                    } else {
                        stream = new MacRfcommStream(device, chid);
                        if (!stream->open(&res->err)) {
                            delete stream;
                            stream = nullptr;
                        }
                    }
                }
                res->stream = stream;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    res->done = true;
                }
                cv.notify_all();

                if (stream) {
                    // Stay resident: run the run loop to receive data; exit once the channel closes
                    while (!stream->closed()) {
                        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
                    }
                }
            }
        });

        std::unique_lock<std::mutex> lk(mu);
        if (!cv.wait_for(lk, std::chrono::milliseconds(kOpenTimeoutMs + 5000),
                         [&] { return res->done; })) {
            worker.detach();
            if (err) *err = "connection flow timed out";
            return nullptr;
        }
        lk.unlock();
        worker.detach();   // the thread keeps serving this channel (object lifetime = process)

        if (!res->stream) {
            if (err) *err = res->err.empty() ? "connection failed" : res->err;
            return nullptr;
        }
        return res->stream;
    }

    core::IByteStream* waitForPhoneConnection(int timeoutMs, std::string* err) override
    {
        @autoreleasepool {
            // UUID -> 16 bytes
            uint8_t raw[16] = {0};
            const std::string uuid = core::kWirelessIapUuid;
            int bi = 0;
            for (size_t i = 0; i < uuid.size() && bi < 16;) {
                if (uuid[i] == '-') { ++i; continue; }
                raw[bi++] = uint8_t(strtoul(uuid.substr(i, 2).c_str(), nullptr, 16));
                i += 2;
            }
            IOBluetoothSDPUUID* u = [IOBluetoothSDPUUID uuidWithBytes:raw length:16];
            const BluetoothRFCOMMChannelID chid = 3;

            NSDictionary* svc = @{
                @"Name" : @"Wireless iAPv2",
                @"ServiceClassList" : @[ @{@"UUID" : u} ],
                @"RFCOMMChannelID" : @(chid),
            };
            // macOS 27: publishing a local SDP record uses publishedServiceRecordWithDictionary
            // (the old IOBluetoothAddServiceDict has been removed)
            IOBluetoothSDPServiceRecord* record =
                [IOBluetoothSDPServiceRecord publishedServiceRecordWithDictionary:svc];
            if (!record) {
                if (err) *err = "failed to publish SDP 'Wireless iAPv2' (dictionary format? permission?)";
                return nullptr;
            }
            static IOBluetoothSDPServiceRecord* keepRecord = nil;   // non-ARC: must be retained
            keepRecord = [record retain];
            LOG_INFO("[bt] registered SDP 'Wireless iAPv2' (UUID %s, RFCOMM ch%d), waiting for phone to connect...",
                     core::kWirelessIapUuid, chid);

            static ServerWait wait;                    // process lifetime
            static RfcommServerObserver* obs = nil;    // keeps the registration alive
            static IOBluetoothUserNotification* note = nil;
            obs = [[RfcommServerObserver alloc] init];
            obs.wait = &wait;
            note = [IOBluetoothRFCOMMChannel
                        registerForChannelOpenNotifications:obs
                                                    selector:@selector(newRFCOMMChannelOpened:channel:)
                                               withChannelID:chid
                                                   direction:kIOBluetoothUserNotificationChannelDirectionIncoming];
            if (!note) {
                if (err) *err = "failed to register inbound channel notification";
                return nullptr;
            }

            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeoutMs);
            while (std::chrono::steady_clock::now() < deadline) {
                {
                    std::lock_guard<std::mutex> lk(wait.mu);
                    if (wait.stream) return wait.stream;
                }
                @autoreleasepool {
                    [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                             beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
                }
            }
            if (err) *err = "timed out waiting for phone to connect (re-pair this host on the phone? or the iPhone doesn't treat it as a car)";
            return nullptr;
        }
    }
};

}  // namespace

std::unique_ptr<core::IBluetooth> makeBluetooth()
{
    return std::unique_ptr<core::IBluetooth>(new MacTransport);
}

std::string controllerAddress()
{
    @autoreleasepool {
        IOBluetoothHostController* c = [IOBluetoothHostController defaultController];
        const char* a = [[c addressAsString] UTF8String];
        return a ? a : "";
    }
}

std::string currentSSID()
{
    @autoreleasepool {
        CWInterface* iface = [[CWWiFiClient sharedWiFiClient] interface];
        const char* s = [[iface ssid] UTF8String];
        return s ? s : "";   // often unavailable on macOS 27 (masked by the system) -> use --ssid
    }
}

std::string bluetoothAdapterName()
{
    @autoreleasepool {
        IOBluetoothHostController* c = [IOBluetoothHostController defaultController];
        const char* n = [[c nameAsString] UTF8String];
        return n ? n : "";
    }
}

}  // namespace allplay::posix

// The delegate implementation must come after MacRfcommStream's full definition (member calls need the complete type)
@implementation RfcommDelegate
- (void)rfcommChannelData:(IOBluetoothRFCOMMChannel*)channel
                     data:(void*)dataPointer
                   length:(size_t)dataLength
{
    (void)channel;
    if (self.owner) self.owner->onData(dataPointer, dataLength);
}
- (void)rfcommChannelClosed:(IOBluetoothRFCOMMChannel*)channel
{
    (void)channel;
    if (self.owner) self.owner->onClosed();
}
- (void)rfcommChannelOpenComplete:(IOBluetoothRFCOMMChannel*)channel status:(IOReturn)error
{
    (void)channel;
    if (self.owner) self.owner->onOpenComplete(error);
}
@end

// Incoming-channel observer implementation (must come after MacRfcommStream's full definition)
@implementation RfcommServerObserver
- (void)newRFCOMMChannelOpened:(IOBluetoothUserNotification*)notification
                       channel:(IOBluetoothRFCOMMChannel*)newChannel
{
    (void)notification;
    auto* w = self.wait;
    if (!w) return;
    std::lock_guard<std::mutex> lk(w->mu);
    if (!w->stream) {
        w->stream = new allplay::posix::MacRfcommStream(newChannel);
    }
}
@end
