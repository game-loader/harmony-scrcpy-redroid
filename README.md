# harmony-scrcpy-redroid

在鸿蒙（HarmonyOS / OpenHarmony）设备上远程使用云端 Android：服务器用 [ReDroid](https://github.com/remote-android/redroid-doc) 跑 Android，鸿蒙端是原生 scrcpy 客户端，两边通过 [Tailscale](https://tailscale.com) 私网连接。

```
ReDroid (Android 12 + MindTheGapps + libndk)
   │ adb 127.0.0.1:5555
scrcpy gateway (启动 scrcpy-server v5.0.1，透传画面和控制)
   │ TCP <tailscale-ip>:27183
鸿蒙客户端 (AVCodec 硬件解码 + XComponent + 多点触控)
```

功能：

- H.264 硬件解码，收到画面立即渲染，不缓冲
- 多点触控，返回 / 主页 / 多任务键，文字输入
- 屏幕自动适配：折叠、展开、切换外屏或旋转时，Android 的分辨率和 dpi 会跟着当前屏幕改变

## 服务端

需要能加载 `binder_linux` 内核模块的 Linux x86_64 主机，以及 Docker 和 Tailscale。

1. 加载 binder，并设成开机自动加载：

   ```bash
   sudo modprobe binder_linux devices=binder,hwbinder,vndbinder
   echo binder_linux | sudo tee /etc/modules-load.d/redroid.conf
   echo 'options binder_linux devices=binder,hwbinder,vndbinder' | sudo tee /etc/modprobe.d/redroid.conf
   ```

2. 用 [redroid-script](https://github.com/ayasa520/redroid-script) 构建带 MindTheGapps 和 ARM 转译层的镜像，然后启动：

   ```bash
   python3 redroid.py -a 12.0.0 -mtg -n
   ./deploy/run-redroid.sh
   ```

3. 下载 [scrcpy v5.0.1](https://github.com/Genymobile/scrcpy/releases/tag/v5.0.1) 的 `scrcpy-linux-x86_64-v5.0.1.tar.gz` 并解压，然后启动网关。版本必须是 5.0.1，因为客户端实现的就是这个版本的协议。

   ```bash
   LISTEN_HOST=$(tailscale ip -4) SCRCPY_DIR=/path/to/scrcpy-linux-x86_64-v5.0.1 ./deploy/run-gateway.sh
   ```

   网关只监听 `LISTEN_HOST`，不设置会拒绝启动。可选环境变量：`MAX_SIZE`（默认 1920）、`BIT_RATE`（默认 8000000）、`MAX_FPS`（默认 60）。

4. 自检：`python3 gateway/test_client.py <tailscale-ip>`。正常会输出设备名、分辨率和帧数，并在屏幕中间点一下。

### Google 设备认证

登录 Google 账号后查 GSF ID，然后到 <https://www.google.com/android/uncertified> 登记：

```bash
adb -s 127.0.0.1:5555 root && adb connect 127.0.0.1:5555
adb -s 127.0.0.1:5555 shell 'sqlite3 /data/data/com.google.android.gsf/databases/gservices.db "select value from main where name=\"android_id\";"'
```

ReDroid 过不了 Play Integrity 的设备完整性检查，所以银行、支付、Wallet 这类应用不可用。Google 登录时“在设备上点击确认”的提示可以正常使用。

## 鸿蒙客户端

在 [Releases](../../releases) 下载未签名的 `entry-default-unsigned.hap`，自己签名后安装。也可以自己构建：

- DevEco Studio：打开 `harmony-scrcpy` 目录，配置签名后运行。
- 命令行：需要 OpenHarmony SDK API 12、hvigor 5.x（`@ohos/hvigor` 和 `@ohos/hvigor-ohos-plugin`，来自 `https://repo.harmonyos.com/npm/`）和 Java 17。

  ```bash
  cd harmony-scrcpy
  echo "sdk.dir=/path/to/ohos-sdk" > local.properties   # 目录下有 12/ets、12/native 等
  node node_modules/@ohos/hvigor/bin/hvigor.js --mode module \
    -p module=entry@default -p product=default -p buildMode=release assembleHap --no-daemon
  ```

使用时在 App 里填写网关的 Tailscale IP 和端口（默认 27183）。鸿蒙设备要加入同一个 tailnet。

### 屏幕适配协议

scrcpy 不使用视频连接的“客户端到服务器”方向，所以客户端借用这个方向发送调整请求：`"RSZ1"` 加 u16 宽、u16 高、u16 dpi，大端序。网关等 0.4 秒，只取最后一个请求，然后执行 `wm size` 和 `wm density`。scrcpy-server 发现屏幕变化后开始新的画面会话。

## 限制

- 同一时间只支持一个客户端连接，新的连接会顶掉旧的
- 没有声音
- 中文输入依赖 Android 端当前的输入法

## 许可

Apache-2.0。客户端实现的是 [scrcpy](https://github.com/Genymobile/scrcpy) 协议（Apache-2.0）。图标来自 scrcpy 项目。
