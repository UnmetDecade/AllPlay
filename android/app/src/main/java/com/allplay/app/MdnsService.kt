package com.allplay.app

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.util.Log
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.Socket
import javax.jmdns.JmDNS
import javax.jmdns.ServiceEvent
import javax.jmdns.ServiceInfo
import javax.jmdns.ServiceListener

/**
 * Android implementation of IMdnsAdvertiser.
 *
 * Prefer JmDNS for advertising and discovery (in hotspot mode it must bind explicitly to the hotspot interface IP, or NsdManager binds the wrong NIC).
 * But on some ROMs (especially Android 4.4 TV boxes) the system netd already holds port 5353, so JmDNS bind fails with
 * EADDRINUSE; fall back to the system NsdManager then (it reuses netd's mDNS responder and does not contend for the port).
 */
class MdnsService(private val context: Context) {
    /** Hotspot interface IPv4 (injected by AllPlaySessionService before nativeStart). null/empty = auto-detect. */
    var advertiseIp: String? = null

    private val jmdnsList = mutableListOf<JmDNS>()
    private var serviceInfo: ServiceInfo? = null
    private val multicastLock: WifiManager.MulticastLock =
        (context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager)
            .createMulticastLock("allplay-mdns").apply { setReferenceCounted(false) }

    // Fall back to the system NsdManager when JmDNS cannot get 5353 (EADDRINUSE)
    private val nsdManager =
        context.applicationContext.getSystemService(Context.NSD_SERVICE) as NsdManager
    private var usingNsd = false
    private val nsdInfos = mutableListOf<NsdServiceInfo>()

    fun register(name: String, port: Int, txt: Array<String>): Boolean {
        if (tryJmDnsRegister(name, port, txt)) return true
        Log.w("AllPlay", "JmDNS registration failed, falling back to system NsdManager")
        usingNsd = true
        return tryNsdRegister(name, port, txt)
    }

    /// JmDNS path: bind the interface explicitly (needed in hotspot mode).
    private fun tryJmDnsRegister(name: String, port: Int, txt: Array<String>): Boolean {
        return try {
            multicastLock.acquire()
            val map = HashMap<String, String>()
            txt.forEach { kv ->
                val i = kv.indexOf('=')
                if (i > 0) map[kv.substring(0, i)] = kv.substring(i + 1)
            }
            val addrs = resolveAdvertiseAddresses()
            for (addr in addrs) {
                val dns = JmDNS.create(addr)
                jmdnsList.add(dns)
                if (addr is Inet4Address) {
                    dns.registerService(ServiceInfo.create("_airplay._tcp.local.", name, port, 0, 0, map))
                    dns.registerService(ServiceInfo.create("_carplay._tcp.local.", name, port, 0, 0, map))
                    Log.i("AllPlay", "JmDNS advertising _airplay/_carplay._tcp iface=$addr")
                } else {
                    Log.i("AllPlay", "JmDNS discovery only (no advertising) iface=$addr")
                }
            }
            if (addrs.isEmpty()) {
                val dns = JmDNS.create()
                jmdnsList.add(dns)
                dns.registerService(ServiceInfo.create("_airplay._tcp.local.", name, port, 0, 0, map))
                Log.i("AllPlay", "JmDNS advertising _airplay._tcp iface=(all)")
            }
            true
        } catch (e: Exception) {
            Log.w("AllPlay", "JmDNS registration failed ${e.javaClass.simpleName}: ${e.message}")
            false
        }
    }

    /// NsdManager path: reuse the system netd mDNS responder and advertise _airplay/_carplay._tcp.
    private fun tryNsdRegister(name: String, port: Int, txt: Array<String>): Boolean {
        val types = arrayOf("_airplay._tcp", "_carplay._tcp")
        return try {
            types.forEach { type ->
                val info = NsdServiceInfo().apply {
                    serviceName = name
                    serviceType = type
                    setPort(port)
                }
                setTxtRecordCompat(info, txt)
                nsdManager.registerService(info, NsdManager.PROTOCOL_DNS_SD,
                    object : NsdManager.RegistrationListener {
                        override fun onServiceRegistered(i: NsdServiceInfo) {
                            Log.i("AllPlay", "NsdManager advertising ${i.serviceType} name=${i.serviceName}")
                        }
                        override fun onRegistrationFailed(i: NsdServiceInfo, errorCode: Int) {
                            Log.w("AllPlay", "NsdManager advertising failed ${i.serviceType} code=$errorCode")
                        }
                        override fun onServiceUnregistered(i: NsdServiceInfo) {}
                        override fun onUnregistrationFailed(i: NsdServiceInfo, errorCode: Int) {}
                    })
                nsdInfos.add(info)
            }
            true
        } catch (e: Exception) {
            Log.w("AllPlay", "NsdManager registration failed ${e.javaClass.simpleName}: ${e.message}")
            false
        }
    }

    /// Set TXT: API 21+ uses setAttribute; API 19 skips it (setTxtRecord(byte[]) does not exist, and
    /// setTxtRecord(DnsSdTxtRecord) makes NsdServiceInfo Binder serialization fail → mDNS registration fails).
    /// Measured on API 19: with TXT null, the iPhone still connects via Bluetooth 0x4301 + RTSP /info, so TXT is not required.
    private fun setTxtRecordCompat(info: NsdServiceInfo, txt: Array<String>) {
        if (Build.VERSION.SDK_INT >= 21) {
            txt.forEach { kv ->
                val i = kv.indexOf('=')
                if (i > 0) info.setAttribute(kv.substring(0, i), kv.substring(i + 1))
            }
        }
    }

    /// Discover the iPhone's _carplay-ctrl._tcp and invoke the callback once host/port are resolved.
    fun discoverCarPlayCtrl(onResolved: (host: String, port: Int) -> Unit) {
        if (usingNsd) { discoverNsd(onResolved); return }
        if (jmdnsList.isEmpty()) return
        val listener = object : ServiceListener {
            override fun serviceAdded(event: ServiceEvent) {
                Log.i("AllPlay", "JmDNS discovered _carplay-ctrl serviceAdded name=${event.name}")
                event.dns.requestServiceInfo(event.type, event.name, true)
            }
            override fun serviceRemoved(event: ServiceEvent) {}
            override fun serviceResolved(event: ServiceEvent) {
                val info = event.info
                Log.i("AllPlay", "JmDNS _carplay-ctrl resolved name=${event.name} port=${info.port} addrs=${info.inetAddresses?.size}")
                val a = info.inetAddresses?.firstOrNull { (it is Inet4Address) == (event.dns.inetAddress is Inet4Address) }
                    ?: info.inet4Addresses?.firstOrNull()
                    ?: info.inetAddresses?.firstOrNull()
                a?.let { onResolved(it.hostAddress, info.port) }
            }
        }
        jmdnsList.forEach { dns ->
            dns.addServiceListener("_carplay-ctrl._tcp.local.", listener)
            Log.i("AllPlay", "JmDNS discovered _carplay-ctrl on ${dns.inetAddress}")
        }
    }

    private fun discoverNsd(onResolved: (host: String, port: Int) -> Unit) {
        nsdManager.discoverServices("_carplay-ctrl._tcp", NsdManager.PROTOCOL_DNS_SD,
            object : NsdManager.DiscoveryListener {
                override fun onDiscoveryStarted(serviceType: String) {
                    Log.i("AllPlay", "NsdManager started discovery $serviceType")
                }
                override fun onServiceFound(serviceInfo: NsdServiceInfo) {
                    Log.i("AllPlay", "NsdManager discovered _carplay-ctrl name=${serviceInfo.serviceName}")
                    nsdManager.resolveService(serviceInfo, object : NsdManager.ResolveListener {
                        override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                            Log.w("AllPlay", "NsdManager resolve failed code=$errorCode")
                        }
                        override fun onServiceResolved(serviceInfo: NsdServiceInfo) {
                            val host = serviceInfo.host?.hostAddress
                            Log.i("AllPlay", "NsdManager _carplay-ctrl resolved host=$host port=${serviceInfo.port}")
                            if (host != null) onResolved(host, serviceInfo.port)
                        }
                    })
                }
                override fun onServiceLost(serviceInfo: NsdServiceInfo) {}
                override fun onDiscoveryStopped(serviceType: String) {}
                override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                    Log.w("AllPlay", "NsdManager start discovery failed code=$errorCode")
                }
                override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
            })
    }

    /// Probe the iPhone: GET /ctrl-int/1/connect (per the protocol's CarPlayBonjour request format)
    fun probeCarPlay(host: String, port: Int, deviceId: String, sourceVersion: String): Boolean {
        return try {
            val sock = Socket()
            sock.connect(InetSocketAddress(host, port), 3000)
            sock.soTimeout = 3000
            val deviceIdNoColon = deviceId.replace(":", "")
            val hostHeader = if (':' in host) "[$host]:$port" else "$host:$port"
            val req = "GET /ctrl-int/1/connect HTTP/1.1\r\n" +
                "Host: $hostHeader\r\n" +
                "User-Agent: AirPlay/$sourceVersion\r\n" +
                "AirPlay-Receiver-Device-ID: $deviceIdNoColon\r\n" +
                "Connection: close\r\n\r\n"
            sock.getOutputStream().write(req.toByteArray(Charsets.UTF_8))
            sock.getOutputStream().flush()
            sock.close()
            true
        } catch (e: Exception) {
            false
        }
    }

    fun unregister() {
        jmdnsList.forEach { dns ->
            try { dns.unregisterService(serviceInfo) } catch (_: Exception) {}
            try { dns.close() } catch (_: Exception) {}
        }
        jmdnsList.clear()
        serviceInfo = null
        nsdInfos.forEach { info ->
            try { nsdManager.unregisterService(object : NsdManager.RegistrationListener {
                override fun onServiceUnregistered(i: NsdServiceInfo) {}
                override fun onUnregistrationFailed(i: NsdServiceInfo, errorCode: Int) {}
                override fun onServiceRegistered(i: NsdServiceInfo) {}
                override fun onRegistrationFailed(i: NsdServiceInfo, errorCode: Int) {}
            }) } catch (_: Exception) {}
        }
        nsdInfos.clear()
        usingNsd = false
        if (multicastLock.isHeld) multicastLock.release()
    }

    /// Addresses to bind for advertising/discovery: explicit config first; otherwise detect IPv4+IPv6 on hotspot interfaces (wlan/ap/softap).
    private fun resolveAdvertiseAddresses(): List<InetAddress> {
        val ip = advertiseIp?.takeIf { it.isNotBlank() }
        if (ip != null) {
            try { return listOf(InetAddress.getByName(ip)) } catch (_: Exception) {}
        }
        val out = mutableListOf<InetAddress>()
        var fallback = mutableListOf<InetAddress>()
        return try {
            val ifs = NetworkInterface.getNetworkInterfaces()
            while (ifs.hasMoreElements()) {
                val ni = ifs.nextElement()
                if (!ni.isUp || ni.isLoopback) continue
                val apLike = ni.name.startsWith("wlan") || ni.name.startsWith("ap") ||
                    ni.name.startsWith("softap") || ni.name.startsWith("p2p")
                for (a in ni.inetAddresses) {
                    if (a.isLoopbackAddress) continue
                    if (apLike) out.add(a) else fallback.add(a)
                }
            }
            if (out.isEmpty()) out.addAll(fallback)
            out.sortBy { if (it is Inet4Address) 0 else 1 }
            out
        } catch (_: Exception) {
            out
        }
    }
}
