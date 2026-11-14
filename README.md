# libcodec2-gbinder

在 Fedora glibc 用户态用 `libgbinder` 直连 Android `/dev/hwbinder` 上的 Codec2 HIDL HAL。不启动 Android 代理，不调用 Java MediaCodec，不链接 Android/Bionic 库。当前实现针对 64 位小端 Linux；已在 Fedora aarch64、MT6989 原厂 HAL 上验证枚举。

## 构建与运行

```sh
sudo dnf install gcc-c++ meson ninja-build pkgconf-pkg-config glib2-devel libgbinder-devel kernel-headers ffmpeg-devel
meson setup build --werror
meson compile -C build
meson test -C build --print-errorlogs

./build/c2-list
./build/c2-list --hardware-only
./build/c2-list --instance software
sh tests/mtk-list.sh ./build/c2-list
```

`c2-list` 优先发现 `android.hardware.media.c2@1.2::IComponentStore/default`，只有在找不到更高版本时才查找 1.1／1.0。用 `IBase::interfaceChain` 核对继承，再用 1.0 的 token 调用 `listComponents`。输出名称、domain、kind、MIME、HAL rank、标志和 HAL aliases。若 Binder、HIDL 或 Codec2 任一层报错，命令返回非零，不会把错误伪装成空列表。

HAL 的 `ComponentTraits` 没有 `isHardwareAccelerated` 字段。分类使用 AOSP `Codec2InfoBuilder` 的规则：`software`／`vendor-software` 是软件 store，`default` 要求名称、kind 和 MIME 在设备 codec XML 中匹配且没有 `software-codec` 属性，才标记为 `hardware`。`--codec-xml PATH` 可指定同一设备的 XML；默认读 `/vendor/etc/media_codecs_c2.xml`。默认文件缺失或没有匹配记录时，`default` 的条目标为 `unknown`。这里解析的是单个 XML 的 codec 属性，不重建 Android 的跨文件 `Include`、域和 variant 选择。`--hardware-only` 选择未禁用的 H.264、HEVC 和 AV1 硬件解码条目，包含明确标出的 secure 条目；`c2-create` 和 `c2-decode` 排除 secure。这是元数据分类，不是硬件运行证明。

## 验收状态

| 阶段 | 本机结果 |
|---|---|
| `c2-list` | **真机通过**。确认 `1.2 → 1.1 → 1.0 → IBase`。`default` 返回 22 个组件、9 个硬件 H.264／HEVC／AV1 解码条目（含 secure 和 low-latency）。 |
| `c2-create` | **真机通过（临时授权）**。宿主全局为 `Enforcing`，`c2.mtk.avc.decoder` 返回 68 个支持参数，成功配置并回读 `128×96`，完成启动、停止和释放。 |
| `c2-decode` | **普通 Fedora 容器真机通过**。H.264、HEVC、AV1 均与参考逐字节一致；AV1 验证了 MP4、Matroska、IVF 和低开销 OBU。 |
| `codec2_drv_video.so` | **普通 Fedora 容器真机通过**。X11、Wayland、DRM 均可初始化。FFmpeg 强制 VA-API 解码 H.264 High 和 HEVC Main，各 29 帧（`162×106`，含 B 帧），直接映射 DRM PRIME 后读回与参考逐字节一致。 |
| `libamfrt64.so.1` | **普通 Fedora 容器真机通过**。原版 FFmpeg 9.0.2 的 `av1_amf` 直连 MTK Codec2。两组各 40 帧，含仅在 `av1C` 中提供序列头的 MP4；I420 像素哈希、时间戳和帧时长均与 dav1d 一致。 |

### MTK Codec2 授权（ReSukiSU）

`modules/mtk_codec2_droidspaces/` 是独立的 ReSukiSU 模块。v1.1.0 仅在 `mtk_hal_c2 → droidspacesd` 方向授予 `binder transfer`（回传组件引用）、`binder call`（向容器发起回调）和 `fd use`（接收容器传来的输入 DMA-BUF）。这些权限按 SELinux 域生效，不限于特定 Binder 接口或 FD 类型。规则在 DroidSpaces 的 `sepolicy.rule` 加载后由 `post-fs-data` 应用；不改变 SELinux 模式，不修改 DroidSpaces 的模块文件，也不挂载系统分区。

从仓库根目录打包：

```sh
mkdir -p build
(cd modules/mtk_codec2_droidspaces && zip -X - module.prop post-fs-data.sh) \
  > build/mtk-codec2-droidspaces-v1.1.0.zip
```

在 ReSukiSU 管理器中选择 ZIP 安装并重启 Android。用宿主的 `getenforce` 确认全局仍为 `Enforcing`，再在 Fedora 中验收。停用或卸载模块后需要重启，才能撤销本模块加载的授权。

```sh
sh tests/mtk-create.sh ./build/c2-create
# 如需同时列出参数：
timeout 20s ./build/c2-create --name c2.mtk.avc.decoder --size 128x96 --params
```

未加授权时，失败发生在 `createComponent_1_2`，不是查找 store，也不是 `start()` 返回 Codec2 错误。Binder 日志显示请求成功到达 HAL，但包含一个 Binder 对象的回复（`size 32:8`）被内核以 `-EPERM` 拒绝，客户端收到 `BR_FAILED_REPLY`。加载上述单条授权后，同一生命周期测试通过。

### 实际解码（`c2-decode`）

加载 v1.1.0 模块的授权后，在 Fedora 中运行：

```sh
./build/c2-decode input.mp4 output.yuv
./build/c2-decode --format h264 --size 128x96 input.h264 output.yuv
./build/c2-decode av1.mkv output-av1.yuv
./build/c2-decode --format obu --size 162x106 input.obu output-obu.yuv
```

FFmpeg 库只负责本地解复用和码流格式处理：H.264／HEVC 为 Annex B，AV1 为低开销 OBU；没有软件解码或 Android 代理。AV1 可用 MP4、Matroska/WebM、IVF 或含时间单元分隔符的原始 OBU，不支持 AV1 Annex B。码流经 DMA-heap 原生块提交给 HAL，等待 `onWorkDone` 和 EOS，按实际 crop 和 plane stride 写紧凑的 8-bit I420（Y、U、V），不会覆盖已存在的输出。读回限定为已验证的 MT6989 Mapper build `7ce7291c0c8a57912de5a13b19e89137` 的线性 YV12；其他布局、位深、压缩、BufferPool 输出和分辨率变化会报错。

v1.0.0 的 `binder transfer` 只解决组件引用回传。缺少 `fd use` 时，带输入 FD 的 `IComponent::queue` 在 Binder 层被拒绝；只补 `fd use` 时可以提交输入，但缺少 `binder call`，等不到 `onWorkDone`。补齐两项后，普通 Fedora 容器通过了上面的 AVC、HEVC 像素对比。

### FFmpeg AMF（AV1）

已编入 `av1_amf` 的 FFmpeg 不需要打补丁。用 [AMD AMF SDK 头文件](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/releases/tag/v1.5.2) 构建可选的 `libamfrt64.so.1`；`-Damf_sdk` 指向包含 `AMF/core/Factory.h` 的绝对路径。这里验证的是 SDK v1.5.2；若 FFmpeg 请求更高版本的 SDK，用对应版本的头文件重建。未设置该选项时不构建 AMF 库。

```sh
meson setup build-amf --werror -Damf_sdk=/path/to/include
meson compile -C build-amf
LD_LIBRARY_PATH="$(realpath build-amf)" \
  ffmpeg -xerror -c:v av1_amf -i input.mp4 -map 0:v:0 \
  -fps_mode passthrough -c:v rawvideo -pix_fmt yuv420p -threads:v 1 -f rawvideo output.yuv

sh tests/mtk-amf.sh build-amf
```

`-c:v av1_amf` 要放在 `-i` 前；不加 `-hwaccel amf`，直接接收 CPU I420。库只在这条命令的进程里加载，不安装或覆盖系统 AMF，也不使用 AMD 驱动或 Vulkan 解码。沿用上面的 Codec2 权限，整份低开销 AV1 OBU 包交给 `c2.mtk.av1.decoder`，需要时从 `av1C` 补序列头。

符合对齐和边界要求的独立线性 DMA-BUF 直接只读映射，按实际 crop 和 stride 把 Y、U、V 平面交给 FFmpeg，不复制像素。最后一个帧引用释放时结束 CPU 访问、解除映射并关闭 FD。每个进程最多保留 8 张直映帧；超出上限，或布局不能安全直接映射时，复制成独立 I420 帧。两种帧都可以跨后续解码或解码器销毁保留。

兼容范围是固定分辨率、8-bit 4:2:0 的 MTK AV1 解码，不是通用 AMF runtime；不提供其它编解码器、Vulkan 帧或 DRM PRIME 导出。测试的第二组输入移除包内序列头，并用 `reverse` 持有全部解码帧到 EOS，覆盖配置注入、排空和帧生命周期。

### FFmpeg VA-API（vgem）

装有 `libva-devel`（VA-API 1.16 或更新）和 `libdrm-devel` 时，Meson 会额外构建 `build/codec2_drv_video.so`。沿用上面的 Codec2 授权；`vgem` 仅提供 DRM 入口，不参与解码或输出缓冲分配。

要让 libva 在打开 `vgem` 时自动加载本驱动，在 Fedora 容器中按设备名安装，不覆盖同名文件：

```sh
driver="$(pkg-config --variable=driverdir libva)/vgem_drv_video.so"
[ ! -e "$driver" ] && [ ! -L "$driver" ] &&
  sudo install -m 0755 build/codec2_drv_video.so "$driver"
```

本机驱动路径为 `/usr/lib64/dri/vgem_drv_video.so`。安装后，新启动的应用选择 `vgem` 设备时无需设置 `LIBVA_DRIVER_NAME` 或 `LIBVA_DRIVERS_PATH`；要撤销，只删除本次安装的驱动文件。不安装也可用下例的环境变量显式选择构建目录里的驱动。

在有 DRI3 的 X11 会话中用 `vainfo --display x11`；Wayland 下用 `vainfo --display wayland`；无图形会话用 `vainfo --display drm --device /dev/dri/renderD128`。三种 `VADisplay` 都可初始化，不提供 `vaGetSurfaceBufferWl` 或 `vaPutSurface`，显示仍由应用负责。

```sh
LIBVA_DRIVER_NAME=codec2 LIBVA_DRIVERS_PATH="$(realpath build)" \
  ffmpeg -xerror -threads:v 1 -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi -i input.mp4 -map 0:v:0 \
  -vf 'hwdownload,format=yuv420p' -fps_mode passthrough \
  -c:v rawvideo -threads:v 1 -f rawvideo output.yuv

sh tests/mtk-vaapi.sh ./build /dev/dri/renderD128
```

驱动将 VA 参数和完整切片组装为 Annex B，由 `c2.mtk.avc.decoder` 或 `c2.mtk.hevc.decoder` 解码。内部参数集请求 HAL 按解码顺序返回图片，保留原切片的 POC；按 Codec2 work index 匹配 `VASurfaceID`，显示顺序仍由 FFmpeg 管理。`hwdownload` 要求硬件帧，不能把软件回退误当成通过。

VA 后端限逐行 8-bit 4:2:0，从 IDR 开始：H.264 Constrained Baseline／Main／High（POC 0/2），以及 HEVC Main（单层、temporal ID 0、切片内短期参考集；不支持 SPS RPS 模板、长期参考或 Main 10）。

不通过 VA-API 声明 AV1。原版 [FFmpeg 的 AV1 VA 后端](https://github.com/FFmpeg/FFmpeg/blob/n8.0/libavcodec/vaapi_av1.c) 提交图片参数和 tile 数据，不提供 Codec2 整码流解码所需的原始 OBU 帧头及完整参考刷新信息。AV1 走 `c2-decode` 或 `av1_amf` 的原始码流路径；驱动无编码入口。

#### DRM PRIME 零拷贝交付

应用先调用 `vaSyncSurface`，再用 `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2` 和 `VA_EXPORT_SURFACE_READ_ONLY` 导出，支持 separate 或 composed layers。每次导出都返回 HAL `BASIC_GRAPHIC` 原始像素 DMA-BUF 的一个独立 FD，由调用方关闭。底层为线性 YV12，描述符只用实际 offset 和 pitch 按 I420 的 Y、U、V 顺序引用同一块内存，不拷贝像素。只读是调用约定，不是 FD 的访问控制。只在 `vaGetImage`／`hwdownload` 读回时做 CPU 拷贝。

FFmpeg 可在上面的命令中加 `-init_hw_device drm=prime:/dev/dri/renderD128 -filter_hw_device prime`，并把过滤链换成 `hwmap=mode=read+direct,format=drm_prime`，把帧交给支持 DRM PRIME 的下游。如需只读 CPU 像素，在后面接 `,hwmap=mode=read+direct,format=yuv420p` 可直接映射该 DMA-BUF，不必先 `hwdownload`；写文件或软件编码仍会读取像素。

`tests/mtk-vaapi.sh` 验证直接映射和原有下载路径都与参考逐字节一致；同时把 29 个帧的 FD 导入 `vgem`，核对导回的是同一 DMA-BUF，并在继续解码及销毁 VA 解码器后复查像素。这个测试验证 DRM 导入与 FD 生命周期，不等同于播放器或显示设备的零拷贝验收。

## ABI 与许可证

- [AOSP Codec2 HIDL 1.0](https://android.googlesource.com/platform/hardware/interfaces/+/refs/tags/android-14.0.0_r1/media/c2/1.0/)、[1.1](https://android.googlesource.com/platform/hardware/interfaces/+/refs/tags/android-14.0.0_r1/media/c2/1.1/)、[1.2](https://android.googlesource.com/platform/hardware/interfaces/+/refs/tags/android-14.0.0_r1/media/c2/1.2/)，以及 [`hidl-gen` 的编号](https://android.googlesource.com/platform/system/tools/hidl/+/refs/tags/android-14.0.0_r1/Interface.cpp#464)与[代理／桩生成逻辑](https://android.googlesource.com/platform/system/tools/hidl/+/refs/tags/android-14.0.0_r1/generateCpp.cpp)：按声明接口分配 token；`listComponents = 3`，`createComponent{,_1_1,_1_2} = 1/9/10`。同时交叉检查了设备 `/vendor/lib64/android.hardware.media.c2@1.{0,1,2}.so` 中的生成代理。`listComponents` 回复依次是 HIDL 状态、Codec2 状态、`vec<ComponentTraits>`；组件 trait 的大小为 64 字节，名称／MIME／别名向量的偏移为 0／32／48。
- [AOSP `Parcel.cpp`](https://android.googlesource.com/platform/system/libhwbinder/+/refs/tags/android-14.0.0_r1/Parcel.cpp) 与 [GKI Binder 驱动](https://android.googlesource.com/kernel/common/+/2d2543bebccf/drivers/android/binder.c)：使用 HwBinder scatter-gather 对象及父索引、父偏移；空向量的 null 子对象可能只存在于 inline parcel，不在 offsets 表中。GLib 驱动异步 `getService`、接口方法及本地 listener；单次异步回复等待上限 5 秒，不含 libgbinder 初始化。超时取消本地等待，不撤回内核中已送出的事务；诊断脚本另设进程级上限。
- [AOSP HIDL 客户端](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-14.0.0_r1/media/codec2/hal/client/client.cpp)、[转换逻辑](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-14.0.0_r1/media/codec2/hal/hidl/1.0/utils/types.cpp)、[`C2Param.h`](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-14.0.0_r1/media/codec2/core/include/C2Param.h) 与 [`C2Config.h`](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-14.0.0_r1/media/codec2/core/include/C2Config.h)：按实际结构写入 `size → index → payload`，每条参数补齐至 8 字节；沿 `IComponent → IComponentInterface → IConfigurable` 查询，不复制 Framework 二进制或头文件。
- [AOSP `Codec2InfoBuilder`](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-14.0.0_r1/media/codec2/sfplugin/Codec2InfoBuilder.cpp#634) 提供 store/XML 分类参考。[libgbinder 1.1.47](https://github.com/mer-hybris/libgbinder/tree/1.1.47) 提供 IPC、设备和 callback 生命周期；构建最低要求为 libgbinder 1.1.43，原生 FD 数组读回适配其 1.1.x reader 布局。`c2-decode` 另用系统 FFmpeg 的 libavformat、libavcodec bitstream filter 和 libavutil，不链接 Android 库。本项目未引入 Android Framework、AOSP 源文件或 MediaTek 二进制。项目代码许可证见 `LICENSES/Apache-2.0.txt`，外部 libgbinder 的 BSD-3-Clause 声明见 `LICENSES/libgbinder.txt`。
