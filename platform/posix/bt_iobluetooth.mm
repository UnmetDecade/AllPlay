// ============================================================================
// platform/posix/bt_iobluetooth.mm —— macOS 传输层（IOBluetooth RFCOMM）
// ============================================================================
// 实测要点：
//   · macOS 27 起 `pairedDevices`/`isPaired`/SDP 查询对**无蓝牙权限**的进程
//     静默返回空 —— 必须从桌面会话启动（TCC 归属到 Terminal）。
//   · RFCOMM 的 delegate 回调走**发起线程的 run loop** —— 打开与收数据都在
//     一条专用线程上跑 NSRunLoop。
//   · 本文件按**非 ARC** 编译（CMake 给 -fno-objc-arc），显式 retain/release。
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
class MacRfcommStream;   // 前向声明（delegate 的 owner 属性用）
}

namespace allplay::posix {
/// 服务端等待：手机连进来时由观察者填 stream（跨线程，用小锁+轮询）
struct ServerWait {
    std::mutex       mu;
    MacRfcommStream* stream = nullptr;
};
}

// 入站 RFCOMM 通道的观察者（服务端方向：等手机连我们的 "Wireless iAPv2"）
@interface RfcommServerObserver : NSObject
@property(nonatomic, assign) allplay::posix::ServerWait* wait;
@end

// ---------------------------------------------------------------------------
// delegate：回调转发到 C++ 对象
// ---------------------------------------------------------------------------
@interface RfcommDelegate : NSObject <IOBluetoothRFCOMMChannelDelegate>
@property(nonatomic, assign) allplay::posix::MacRfcommStream* owner;
@end


namespace allplay::posix {

constexpr int kOpenTimeoutMs = 20000;

// ---------------------------------------------------------------------------
// RFCOMM 字节流
// ---------------------------------------------------------------------------
class MacRfcommStream : public core::IByteStream {
public:
    MacRfcommStream(IOBluetoothDevice* device, BluetoothRFCOMMChannelID channelId)
        : device_([device retain]), channelId_(channelId)
    {
        delegate_ = [[RfcommDelegate alloc] init];
        delegate_.owner = this;
    }

    /// 服务端方向：通道已由入站连接建立（不需要 open()）
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

    /// 异步打开 → **边泵 run loop 边等** openComplete（最多 20s）。
    /// ⚠️ 这里必须泵 run loop：完成回调是通过 run loop 派发的，若在这条线程上
    ///    阻塞等条件变量，回调永远到不了（症状：RFCOMM 打开超时）。
    bool open(std::string* err)
    {
        IOReturn r = [device_ openRFCOMMChannelAsync:&channel_
                                       withChannelID:channelId_
                                            delegate:delegate_];
        if (r != kIOReturnSuccess || !channel_) {
            if (err) *err = "openRFCOMMChannelAsync 失败（ret=" + std::to_string((int)r) + "）";
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
            if (err) *err = "RFCOMM 打开超时";
            return false;
        }
        if (!openedOk_) {
            if (err) *err = "RFCOMM 打开被拒绝（对端不接受？）";
            return false;
        }
        return true;
    }

    int read(uint8_t* buf, size_t n, int timeoutMs) override
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cvData_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                              [&] { return !queue_.empty() || closed_; })) {
            return 0;   // 超时
        }
        if (queue_.empty()) return -1;   // 关闭
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

    // ---- delegate 回调入口 ----
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
            *err = "已配对设备列表为空（macOS 27：本进程可能没有蓝牙权限 —— "
                   "要从桌面会话的 Terminal 启动，别走 ssh）";
        }
        return out;
    }

    core::IByteStream* connectRFCOMM(const std::string& address, const std::string& uuid,
                                    std::string* err) override
    {
        // UUID 字符串 → 16 字节
        uint8_t raw[16] = {0};
        int bi = 0;
        for (size_t i = 0; i < uuid.size() && bi < 16;) {
            if (uuid[i] == '-') { ++i; continue; }
            raw[bi++] = uint8_t(strtoul(uuid.substr(i, 2).c_str(), nullptr, 16));
            i += 2;
        }
        if (bi != 16) {
            if (err) *err = "UUID 形态不对（要 8-4-4-4-12）";
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

        // IOBluetooth 打开+回调都要 run loop → 放专用线程
        std::thread worker([res, &address, raw, &mu, &cv] {
            @autoreleasepool {
                NSString* addr = [NSString stringWithUTF8String:address.c_str()];
                IOBluetoothDevice* device = [IOBluetoothDevice deviceWithAddressString:addr];
                IOBluetoothSDPUUID* u = [IOBluetoothSDPUUID uuidWithBytes:raw length:16];
                MacRfcommStream* stream = nullptr;

                if (!device) {
                    res->err = "deviceWithAddressString 失败（地址形态？）";
                } else {
                    IOBluetoothSDPServiceRecord* record = [device getServiceRecordForUUID:u];
                    BluetoothRFCOMMChannelID chid = 0;
                    if (!record) {
                        res->err = "SDP 查不到 iAP2 SPP 服务（未配对/无权限/手机没广播）";
                    } else if ([record getRFCOMMChannelID:&chid] != kIOReturnSuccess) {
                        res->err = "取 RFCOMM channel 号失败";
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
                    // 常驻：跑 run loop 收数据；通道关闭后退出
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
            if (err) *err = "连接流程超时";
            return nullptr;
        }
        lk.unlock();
        worker.detach();   // 线程继续服务该通道（对象生命周期=进程）

        if (!res->stream) {
            if (err) *err = res->err.empty() ? "连接失败" : res->err;
            return nullptr;
        }
        return res->stream;
    }

    core::IByteStream* waitForPhoneConnection(int timeoutMs, std::string* err) override
    {
        @autoreleasepool {
            // UUID → 16 字节
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
            // macOS 27：发布本地 SDP 记录用 publishedServiceRecordWithDictionary
            // （老的 IOBluetoothAddServiceDict 已被删除）
            IOBluetoothSDPServiceRecord* record =
                [IOBluetoothSDPServiceRecord publishedServiceRecordWithDictionary:svc];
            if (!record) {
                if (err) *err = "发布会话 SDP 'Wireless iAPv2' 失败（字典格式？权限？）";
                return nullptr;
            }
            static IOBluetoothSDPServiceRecord* keepRecord = nil;   // 非 ARC：要持有
            keepRecord = [record retain];
            LOG_INFO("[bt] 已注册 SDP 'Wireless iAPv2'（UUID %s，RFCOMM ch%d），等手机连接…",
                     core::kWirelessIapUuid, chid);

            static ServerWait wait;                    // 进程生命周期
            static RfcommServerObserver* obs = nil;    // 保持注册
            static IOBluetoothUserNotification* note = nil;
            obs = [[RfcommServerObserver alloc] init];
            obs.wait = &wait;
            note = [IOBluetoothRFCOMMChannel
                        registerForChannelOpenNotifications:obs
                                                    selector:@selector(newRFCOMMChannelOpened:channel:)
                                               withChannelID:chid
                                                   direction:kIOBluetoothUserNotificationChannelDirectionIncoming];
            if (!note) {
                if (err) *err = "注册入站通道通知失败";
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
            if (err) *err = "等待手机连接超时（手机上把本机重新配对一次？或 iPhone 未把它当车）";
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
        return s ? s : "";   // macOS 27 常拿不到（系统打码）→ 用 --ssid
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

// delegate 实现要放在 MacRfcommStream 完整定义之后（成员调用需要完整类型）
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

// 入站通道观察者实现（要放在 MacRfcommStream 完整定义之后）
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
