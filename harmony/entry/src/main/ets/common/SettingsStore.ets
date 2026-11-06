/**
 * 本机设置的持久化（目前只有 Wi-Fi 名 / 密码）
 *
 * 为什么不在代码里写死：SSID 和密码是用户家里的东西，写进仓库就是泄露。
 * 设置页让用户自己填，落在应用的 preferences 里（随 App 卸载消失）。
 * 原生侧只通过 start(config) 的 ssid / passphrase 字段见到它们，
 * 再原样填进 0x5703（Wi-Fi 配置）和 0x4301（CarPlayStartSession）。
 */

import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';

const STORE_NAME: string = 'allplay_settings';
const KEY_SSID: string = 'wifi_ssid';
const KEY_PASSPHRASE: string = 'wifi_passphrase';
const KEY_SECURITY: string = 'wifi_security_type';

/// iAP2 的 securityType 取值（0x5703 参数 3、0x4301 group 1 参数 4 用的是同一套）
export const SEC_OPEN: number = 0;
export const SEC_WEP: number = 1;
export const SEC_WPA2: number = 2;
export const SEC_WPA3_TRANSITION: number = 3;

export function securityTypeLabel(t: number): string {
  if (t === SEC_OPEN) return '无密码';
  if (t === SEC_WEP) return 'WEP';
  if (t === SEC_WPA2) return 'WPA2';
  if (t === SEC_WPA3_TRANSITION) return 'WPA3';
  return '未知(' + t + ')';
}

export class WifiConfig {
  ssid: string = '';
  passphrase: string = '';
  /// 2 = WPA2（iAP2 的 securityType；0 开放 / 1 WEP / 2 WPA2 / 3 WPA3）
  securityType: number = SEC_WPA2;

  isUsable(): boolean {
    return this.ssid.length > 0;
  }

  /// 开放网络不带密码是合法的；加密网络没密码就没法连
  isComplete(): boolean {
    return this.ssid.length > 0 &&
      (this.securityType === SEC_OPEN || this.passphrase.length > 0);
  }
}

export async function loadWifiConfig(ctx: common.Context): Promise<WifiConfig> {
  const cfg = new WifiConfig();
  try {
    const opt: preferences.Options = { name: STORE_NAME };
    const store: preferences.Preferences = await preferences.getPreferences(ctx, opt);
    cfg.ssid = await store.get(KEY_SSID, '') as string;
    cfg.passphrase = await store.get(KEY_PASSPHRASE, '') as string;
    const sec: number = await store.get(KEY_SECURITY, SEC_WPA2) as number;
    // 老版本没存过这一项 → 用默认；越界值也收回默认，别把脏值发进协议报文
    cfg.securityType = (sec >= SEC_OPEN && sec <= SEC_WPA3_TRANSITION) ? sec : SEC_WPA2;
  } catch (e) {
    // 读不到就当没配过：连接流程里会明确提示
  }
  return cfg;
}

export async function saveWifiConfig(ctx: common.Context, cfg: WifiConfig): Promise<void> {
  const opt: preferences.Options = { name: STORE_NAME };
  const store: preferences.Preferences = await preferences.getPreferences(ctx, opt);
  await store.put(KEY_SSID, cfg.ssid);
  await store.put(KEY_PASSPHRASE, cfg.passphrase);
  await store.put(KEY_SECURITY, cfg.securityType);
  await store.flush();
}
