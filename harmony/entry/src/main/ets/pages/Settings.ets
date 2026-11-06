/**
 * 设置页 —— 填 Wi-Fi 名和密码
 *
 * 这两项会被原样填进 iAP2 的两个报文：
 *   · 0x5703 AccessoryWiFiConfiguration（ssid / passphrase / securityType）
 *   · 0x4301 CarPlayStartSession 的 group 1（同一个网络 + 本机 IP + AirPlay 端口）
 * 手机据此切到同一个 Wi-Fi，再连本机的 7000 端口。
 *
 * 不写死任何值、不进仓库：存在应用自己的 preferences 里（见 common/SettingsStore）。
 */

import { router } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import {
  SEC_OPEN, SEC_WEP, SEC_WPA2, SEC_WPA3_TRANSITION,
  WifiConfig, loadWifiConfig, saveWifiConfig
} from '../common/SettingsStore';
import { localIpv4 } from '../common/NetInfo';
import { ChevronLeft } from '../components/Glyphs';
import {
  C_BLUE, C_CARD_LINE, C_EYEBROW, C_HINT, C_INK, C_MUTED, C_RULE, C_WHITE
} from '../common/Theme';

@Entry
@Component
struct Settings {
  @State ssid: string = '';
  @State passphrase: string = '';
  @State securityType: number = SEC_WPA2;
  @State localIp: string = '';
  @State saveHint: string = '';
  @State loaded: boolean = false;

  async aboutToAppear(): Promise<void> {
    const cfg: WifiConfig = await loadWifiConfig(getContext(this));
    this.ssid = cfg.ssid;
    this.passphrase = cfg.passphrase;
    this.securityType = cfg.securityType;
    this.localIp = localIpv4();
    this.loaded = true;
  }

  private async save(): Promise<void> {
    const cfg = new WifiConfig();
    cfg.ssid = this.ssid.trim();
    cfg.passphrase = this.passphrase;
    cfg.securityType = this.securityType;
    // 加密网络没密码发出去也没用，手机连不上；开放网络则允许空密码
    if (!cfg.isComplete()) {
      this.saveHint = cfg.ssid.length === 0 ? '先填网络名称' : '这个加密方式需要密码';
      return;
    }
    try {
      await saveWifiConfig(getContext(this), cfg);
      this.saveHint = '已保存';
      setTimeout(() => {
        router.back();
      }, 250);
    } catch (e) {
      const err = e as BusinessError;
      this.saveHint = '保存失败：' + err.code + ' ' + err.message;
    }
  }

  @Builder
  fieldLabel(text: string) {
    Text(text)
      .fontSize(12)
      .fontWeight(FontWeight.Bold)
      .fontColor(C_EYEBROW)
      .width('100%')
      .margin({ top: 18, bottom: 8 })
  }

  @Builder
  securityChip(value: number, label: string) {
    Text(label)
      .fontSize(14)
      .fontWeight(this.securityType === value ? FontWeight.Medium : FontWeight.Normal)
      .fontColor(this.securityType === value ? C_WHITE : C_INK)
      .backgroundColor(this.securityType === value ? C_BLUE : '#F2F3F5')
      .borderRadius(10)
      .padding({ left: 16, right: 16, top: 10, bottom: 10 })
      .margin({ right: 8, bottom: 8 })
      .onClick(() => {
        this.securityType = value;
      })
  }

  build() {
    Column() {
      // ---------------- 顶栏 ----------------
      Row() {
        Row() {
          ChevronLeft({ diameter: 20, color: C_INK })
        }
        .width(40)
        .height(40)
        .justifyContent(FlexAlign.Center)
        .borderRadius(20)
        .onClick(() => router.back())

        Text('设置')
          .fontSize(21)
          .fontWeight(FontWeight.Bold)
          .fontColor(C_INK)
          .margin({ left: 10 })

        Blank()

        Text('保存')
          .fontSize(15)
          .fontWeight(FontWeight.Medium)
          .fontColor(C_BLUE)
          .padding({ left: 12, right: 12, top: 8, bottom: 8 })
          .onClick(() => this.save())
      }
      .width('100%')
      .height(60)
      .padding({ left: 12, right: 8 })

      Divider().color(C_RULE).strokeWidth(1)

      // ---------------- 表单 ----------------
      Scroll() {
        Column() {
          Text('Wi-Fi')
            .fontSize(20)
            .fontWeight(FontWeight.Bold)
            .fontColor(C_INK)
            .width('100%')
            .margin({ top: 22 })

          Text('手机要连到和本机同一个 Wi-Fi，才能把画面送到这块屏幕。')
            .fontSize(13)
            .fontColor(C_HINT)
            .width('100%')
            .lineHeight(20)
            .margin({ top: 6 })

          this.fieldLabel('网络名称（SSID）')
          TextInput({ text: this.ssid, placeholder: '例如 my-home-wifi' })
            .height(48)
            .fontSize(16)
            .fontColor(C_INK)
            .backgroundColor(C_WHITE)
            .borderRadius(12)
            .border({ width: 1, color: C_CARD_LINE })
            .onChange((v: string) => {
              this.ssid = v;
            })

          this.fieldLabel('加密方式')
          Flex({ wrap: FlexWrap.Wrap }) {
            this.securityChip(SEC_OPEN, '无密码')
            this.securityChip(SEC_WEP, 'WEP')
            this.securityChip(SEC_WPA2, 'WPA2')
            this.securityChip(SEC_WPA3_TRANSITION, 'WPA3')
          }
          .width('100%')

          Text(this.securityType === SEC_OPEN
            ? '开放网络：手机连上去不用密码。'
            : (this.securityType === SEC_WPA2
              ? '家用路由器最常见的一项。WPA2/WPA3 混合模式也选这个。'
              : (this.securityType === SEC_WPA3_TRANSITION
                ? '路由器只开 WPA3（或 WPA3 过渡模式）时选这个。'
                : 'WEP 已淘汰，只在老设备上用。')))
            .fontSize(12)
            .fontColor(C_HINT)
            .width('100%')
            .lineHeight(18)
            .margin({ bottom: 4 })

          if (this.securityType !== SEC_OPEN) {
            this.fieldLabel('密码')
            TextInput({ text: this.passphrase, placeholder: 'WPA2 密码' })
              .type(InputType.Password)
              .height(48)
              .fontSize(16)
              .fontColor(C_INK)
              .backgroundColor(C_WHITE)
              .borderRadius(12)
              .border({ width: 1, color: C_CARD_LINE })
              .onChange((v: string) => {
                this.passphrase = v;
              })
          }

          this.fieldLabel('本机地址')
          Row() {
            Text(this.localIp.length > 0 ? this.localIp : '未联网')
              .fontSize(16)
              .fontColor(this.localIp.length > 0 ? C_INK : C_MUTED)
            Blank()
            Text('AirPlay 端口 7000')
              .fontSize(12)
              .fontColor(C_HINT)
          }
          .width('100%')
          .height(48)
          .padding({ left: 14, right: 14 })
          .backgroundColor('#F7F8FA')
          .borderRadius(12)

          Text('本机地址由系统自动读取 —— 手机连上同一 Wi-Fi 后，会用这个地址连回本机。')
            .fontSize(12)
            .fontColor(C_HINT)
            .width('100%')
            .lineHeight(18)
            .margin({ top: 8 })

          if (this.saveHint.length > 0) {
            Text(this.saveHint)
              .fontSize(13)
              .fontColor(C_MUTED)
              .width('100%')
              .margin({ top: 14 })
          }

          Blank().layoutWeight(1)
        }
        .width('100%')
        .padding({ left: 20, right: 20, bottom: 24 })
      }
      .layoutWeight(1)
      .align(Alignment.Top)
    }
    .width('100%')
    .height('100%')
    .backgroundColor(C_WHITE)
  }
}
