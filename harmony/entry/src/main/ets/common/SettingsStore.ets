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

export class WifiConfig {
  ssid: string = '';
  passphrase: string = '';
  /// 2 = WPA2（iAP2 的 securityType；0 开放 / 1 WEP / 2 WPA2 / 3 WPA3）
  securityType: number = 2;

  isUsable(): boolean {
    return this.ssid.length > 0;
  }
}

export async function loadWifiConfig(ctx: common.Context): Promise<WifiConfig> {
  const cfg = new WifiConfig();
  try {
    const opt: preferences.Options = { name: STORE_NAME };
    const store: preferences.Preferences = await preferences.getPreferences(ctx, opt);
    cfg.ssid = await store.get(KEY_SSID, '') as string;
    cfg.passphrase = await store.get(KEY_PASSPHRASE, '') as string;
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
  await store.flush();
}
