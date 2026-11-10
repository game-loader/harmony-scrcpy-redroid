import scrcpy from 'libscrcpy.so';
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { display } from '@kit.ArkUI';

// Android keycodes used by the navigation bar.
const KEYCODE_HOME = 3;
const KEYCODE_BACK = 4;
const KEYCODE_APP_SWITCH = 187;
const KEYCODE_DEL = 67;
const KEYCODE_ENTER = 66;
// Cap the long side so the stream stays within the server's encoder limits.
const MAX_LONG_SIDE = 1920; // matches the gateway MAX_SIZE, avoids a second rescale

@Entry
@Component
struct Index {
  @State host: string = '';
  @State port: string = '27183';
  @State status: string = 'idle';
  @State connected: boolean = false;
  @State videoW: number = 1080;
  @State videoH: number = 1920;
  @State textInput: string = '';
  @State showKeyboard: boolean = false;
  @State autoFit: boolean = true;
  // Size of the area available for the remote screen, in vp.
  private areaW: number = 0;
  private areaH: number = 0;
  private store: preferences.Preferences | null = null;

  aboutToAppear(): void {
    const ctx = getContext(this) as common.UIAbilityContext;
    this.store = preferences.getPreferencesSync(ctx, { name: 'scrcpy' });
    this.host = this.store.getSync('host', this.host) as string;
    this.port = this.store.getSync('port', this.port) as string;
    this.autoFit = this.store.getSync('autoFit', this.autoFit) as boolean;
  }

  aboutToDisappear(): void {
    scrcpy.disconnect();
  }

  connect(): void {
    this.store?.putSync('host', this.host);
    this.store?.putSync('port', this.port);
    this.store?.putSync('autoFit', this.autoFit);
    this.store?.flush();
    this.connected = true;
    scrcpy.connect(this.host.trim(), Number(this.port), (status: string, w: number, h: number) => {
      this.status = status;
      if (w > 0 && h > 0) {
        this.videoW = w;
        this.videoH = h;
      }
      if (status.startsWith('connected')) {
        this.requestFit();
      }
      if (status === 'disconnected' || status.startsWith('error')) {
        this.connected = false;
      }
    });
  }

  // Resize the Android display to this device's free area, keeping the same
  // density so apps lay out exactly as native ones would on this screen.
  requestFit(): void {
    if (!this.autoFit || this.areaW <= 0 || this.areaH <= 0) {
      return;
    }
    const density = display.getDefaultDisplaySync().densityPixels;
    let w = vp2px(this.areaW);
    let h = vp2px(this.areaH);
    let dpi = density * 160;
    const scale = Math.min(1, MAX_LONG_SIDE / Math.max(w, h));
    w = Math.round(w * scale);
    h = Math.round(h * scale);
    dpi = Math.round(dpi * scale);
    scrcpy.requestResize(w, h, dpi);
  }

  @Builder
  NavButton(label: string, action: () => void) {
    Button(label)
      .type(ButtonType.Normal)
      .borderRadius(8)
      .fontSize(14)
      .backgroundColor('#33FFFFFF')
      .fontColor(Color.White)
      .height(36)
      .layoutWeight(1)
      .onClick(action)
  }

  build() {
    Stack() {
      // The XComponent is always mounted so the surface exists before connecting.
      Column() {
        // The remote screen fills this area; its size drives auto-fit, so
        // folding, unfolding or switching to the cover screen resizes Android.
        Column() {
          XComponent({ id: 'scrcpy_surface', type: XComponentType.SURFACE, libraryname: 'scrcpy' })
            .aspectRatio(this.videoW / this.videoH)
            .width('100%')
            .constraintSize({ maxHeight: '100%' })
        }
        .width('100%')
        .layoutWeight(1)
        .justifyContent(FlexAlign.Center)
        .visibility(this.connected ? Visibility.Visible : Visibility.Hidden)
        .onAreaChange((_old: Area, area: Area) => {
          this.areaW = area.width as number;
          this.areaH = area.height as number;
          if (this.connected) {
            this.requestFit();
          }
        })

        if (this.showKeyboard && this.connected) {
          Row({ space: 6 }) {
            TextInput({ placeholder: '输入文字后发送', text: this.textInput })
              .layoutWeight(1)
              .fontColor(Color.White)
              .placeholderColor('#88FFFFFF')
              .backgroundColor('#22FFFFFF')
              .onChange((v: string) => { this.textInput = v; })
              .onSubmit(() => {
                scrcpy.sendText(this.textInput);
                this.textInput = '';
              })
            this.NavButton('发送', () => {
              scrcpy.sendText(this.textInput);
              this.textInput = '';
            })
            this.NavButton('⌫', (): void => { scrcpy.sendKey(KEYCODE_DEL); })
            this.NavButton('↵', (): void => { scrcpy.sendKey(KEYCODE_ENTER); })
          }
          .padding(6)
          .width('100%')
        }

        if (this.connected) {
          Row({ space: 6 }) {
            this.NavButton('◁', (): void => { scrcpy.sendKey(KEYCODE_BACK); })
            this.NavButton('○', (): void => { scrcpy.sendKey(KEYCODE_HOME); })
            this.NavButton('▢', (): void => { scrcpy.sendKey(KEYCODE_APP_SWITCH); })
            this.NavButton('⌨', () => { this.showKeyboard = !this.showKeyboard; })
            this.NavButton('断开', () => {
              scrcpy.disconnect();
              this.connected = false;
            })
          }
          .padding(6)
          .width('100%')
        }
      }
      .width('100%')
      .height('100%')
      .justifyContent(FlexAlign.Center)

      if (!this.connected) {
        Column({ space: 12 }) {
          Text('远程安卓').fontSize(28).fontColor(Color.White).fontWeight(FontWeight.Bold)
          Text('通过 Tailscale 连接 scrcpy 网关').fontSize(14).fontColor('#AAFFFFFF')
          TextInput({ placeholder: '网关地址（Tailscale IP）', text: this.host })
            .fontColor(Color.White)
            .backgroundColor('#22FFFFFF')
            .onChange((v: string) => { this.host = v; })
          TextInput({ placeholder: '端口', text: this.port })
            .type(InputType.Number)
            .fontColor(Color.White)
            .backgroundColor('#22FFFFFF')
            .onChange((v: string) => { this.port = v; })
          Row() {
            Text('安卓分辨率自动适配本机屏幕').fontSize(14).fontColor(Color.White).layoutWeight(1)
            Toggle({ type: ToggleType.Switch, isOn: this.autoFit })
              .onChange((on: boolean) => { this.autoFit = on; })
          }
          .width('100%')
          Button('连接').width('100%').onClick(() => this.connect())
          Text(this.status).fontSize(12).fontColor('#88FFFFFF')
        }
        .padding(24)
        .width('100%')
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(Color.Black)
  }
}
