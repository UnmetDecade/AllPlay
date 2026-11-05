/**
 * 本机网络信息（只要一个 IPv4 地址）
 *
 * 为什么需要：无线 CarPlay 的 0x5703（Wi-Fi 配置）要把车机自己的地址告诉手机，
 * 手机才会连到 airPlayPort。鸿蒙没有"取本机 IP"的一行 API，得走默认网络的
 * ConnectionProperties，从链路地址里挑第一个 IPv4。
 */

import { connection } from '@kit.NetworkKit';
import { wifiManager } from '@kit.ConnectivityKit';

export function localIpv4(): string {
  try {
    const net: connection.NetHandle = connection.getDefaultNetSync();
    if (!net || net.netId === 0) return '';
    const props: connection.ConnectionProperties = connection.getConnectionPropertiesSync(net);
    const addrs: connection.LinkAddress[] = props.linkAddresses;
    for (let i = 0; i < addrs.length; i++) {
      const ip: string = addrs[i].address.address;
      if (ip.length > 0 && ip.indexOf(':') < 0 && ip.indexOf('.') > 0) return ip;
    }
  } catch (e) {
    // 没联网 / 没权限都当"取不到"，界面上显示空即可
  }
  return '';
}

/**
 * 把所有网络的「网卡名:IPv4」串成一行，用来在日志里看清本机到底挂在哪些网上。
 * 排查"0x4301 报错地址"时，这一行比任何猜测都快。
 */
export function netSummary(): string {
  const out: string[] = [];
  try {
    const nets: connection.NetHandle[] = connection.getAllNetsSync();
    for (let i = 0; i < nets.length; i++) {
      const props: connection.ConnectionProperties = connection.getConnectionPropertiesSync(nets[i]);
      let ip: string = '';
      const addrs: connection.LinkAddress[] = props.linkAddresses;
      for (let j = 0; j < addrs.length; j++) {
        const a: string = addrs[j].address.address;
        if (a.indexOf(':') < 0 && a.indexOf('.') > 0) { ip = a; break; }
      }
      out.push(props.interfaceName + ':' + (ip.length > 0 ? ip : '无IPv4'));
    }
  } catch (e) {
    return '读取网络列表失败';
  }
  return out.length > 0 ? out.join('  ') : '没有网络';
}

/**
 * 本机热点（AP）当前是否开着。
 *
 * 为什么在意：手机只对「配件用凭据带出来的那次入网」拉起 CarPlay 栈。
 * 如果车机和手机本来就在同一个 Wi-Fi 上，手机没有"入网"动作，0x4301 发过去
 * 它也不连 7000。让手机连本机热点，才是和真车机一样的那条路。
 */
export function hotspotActive(): boolean {
  try {
    return wifiManager.isHotspotActive();
  } catch (e) {
    return false;
  }
}

/**
 * 本机热点的 IPv4（AP 网卡的地址，鸿蒙上是 192.168.43.1）。
 *
 * 默认网络不一定是热点网卡 —— 手机连着 Wi-Fi 又开热点时，默认路由还挂在 STA 上，
 * 直接拿默认网络的地址，0x4301 报出去的会是手机根本没加入的那个网段的地址。
 */
export function apIpv4(): string {
  try {
    const nets: connection.NetHandle[] = connection.getAllNetsSync();
    for (let i = 0; i < nets.length; i++) {
      const props: connection.ConnectionProperties = connection.getConnectionPropertiesSync(nets[i]);
      // 鸿蒙热点网卡叫 wlan1（实测 Mate 60）；别的实现可能叫 softap0 / ap0
      if (!/^(wlan[1-9]|softap|ap[0-9])/i.test(props.interfaceName)) continue;
      const addrs: connection.LinkAddress[] = props.linkAddresses;
      for (let j = 0; j < addrs.length; j++) {
        const ip: string = addrs[j].address.address;
        if (ip.length > 0 && ip.indexOf(':') < 0 && ip.indexOf('.') > 0) return ip;
      }
    }
  } catch (e) {
    // 没权限 / 没热点都当"没有"
  }
  return '';
}

/**
 * 本机当前连着的 Wi-Fi 名。
 *
 * 为什么必须核对：0x5703 让手机去连**设置页里填的** SSID，0x4301 又让手机来连
 * **本机当前 IP**。两者不在同一个 Wi-Fi 上，手机就会"连上了网却怎么也找不到车机"。
 * 这种错误在协议日志里完全看不出来，只能拿实际 SSID 和配置比。
 */
export function linkedSsid(): string {
  try {
    return wifiManager.getLinkedInfoSync().ssid;
  } catch (e) {
    return '';
  }
}
