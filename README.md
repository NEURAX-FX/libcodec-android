# libcodec2-gbinder

在 Fedora glibc 用户态用 `libgbinder` 直连 Android `/dev/hwbinder` 上的 Codec2 HIDL HAL。不启动 Android 代理，不调用 Java MediaCodec，不链接 Android/Bionic 库。当前实现针对 64 位小端 Linux；已在 Fedora aarch64、MT6989 原厂 HAL 上验证枚举。

## 构建与运行

```sh
sudo dnf install gcc-c++ meson ninja-build pkgconf-pkg-config glib2-devel libgbinder-devel kernel-headers
meson setup build --werror
meson compile -C build
meson test -C build --print-errorlogs

./build/c2-list
./build/c2-list --hardware-only
./build/c2-list --instance software
sh tests/mtk-list.sh ./build/c2-list
```

`c2-list` 优先发现 `android.hardware.media.c2@1.2::IComponentStore/default`，只有在找不到更高版本时才查找 1.1／1.0。用 `IBase::interfaceChain` 核对继承，再用 1.0 的 token 调用 `listComponents`。输出名称、domain、kind、MIME、HAL rank、标志和 HAL aliases。若 Binder、HIDL 或 Codec2 任一层报错，命令返回非零，不会把错误伪装成空列表。

HAL 的 `ComponentTraits` 没有 `isHardwareAccelerated` 字段。分类使用 AOSP `Codec2InfoBuilder` 的规则：`software`／`vendor-software` 是软件 store，`default` 要求名称、kind 和 MIME 在设备 codec XML 中匹配且没有 `software-codec` 属性，才标记为 `hardware`。`--codec-xml PATH` 可指定同一设备的 XML；默认读 `/vendor/etc/media_codecs_c2.xml`。默认文件缺失或没有匹配记录时，`default` 的条目标为 `unknown`。这里解析的是单个 XML 的 codec 属性，不重建 Android 的跨文件 `Include`、域和 variant 选择。`--hardware-only` 选择未禁用的视频 AVC/HEVC 解码条目，包含明确标出的 secure 条目；`c2-create` 排除 secure。这是元数据分类，不是硬件运行证明。

## 验收状态

| 阶段 | 本机结果 |
|---|---|
| `c2-list` | **真机通过**。确认 `1.2 → 1.1 → 1.0 → IBase`。`default` 返回 22 个组件、6 个硬件 AVC/HEVC 条目（含 secure 和 low-latency）；`software` 返回 32 个组件、0 个硬件条目。 |
| `c2-create` | **编译通过，真机未验收**。已实现 1.2／1.1／1.0 创建、HIDL 本地 listener、参数描述／查询、`raw.size` 配置及回读、`start`／`stop`／`release`。当前容器发出的创建请求能到达 HAL，但组件 Binder 引用在回复方向被内核拒绝，客户端无法执行后续操作。 |
| `c2-decode` | **未实现**。未提交 AU、未取得 `onWorkDone` 的解码帧、未验证 BufferPool、gralloc 映射或 H.264/H.265 解码。仓库没有软件解码回退或占位命令。 |

### 复现组件创建阻塞

```sh
sh tests/mtk-create.sh ./build/c2-create
# 单独查看请求结果，或在授权的环境中附带列出参数：
timeout 20s ./build/c2-create --name c2.mtk.avc.decoder --size 128x96 --params
```

失败发生在 `createComponent_1_2`，不是查找 store，也不是 `start()` 返回 Codec2 错误。对照同一次运行前后的 `/sys/kernel/debug/binder/{transaction_log,failed_transaction_log}`，请求方向成功；HAL 的回复有一个 Binder 对象（`size 32:8`），日志记录 `BR_FAILED_REPLY`（`29201`）和 `-EPERM`（`-1`）。同一时刻内核日志记录 `translate binder failed`。匹配的 GKI 驱动源码中，`binder_translate_binder()` 在 `security_binder_transfer_binder()` 拒绝时返回 `-EPERM`。当前 shell 标签为 `u:r:droidspacesd:s0`；没有抓到对应的 SELinux audit 记录，因此不把某条具体 vendor SELinux 规则当作已验证事实。未修改宿主策略或服务。

下一项运行验收需要设备管理员提供允许 HAL 向 Fedora 客户端交付 `IComponent` Binder 引用的受支持运行环境。在该环境中先让 `tests/mtk-create.sh` 通过，再验证 DMA-heap 输入的 `nativeBlock`、AU／SPS／PPS、EOS 和 `onWorkDone`。可先验证 HAL 是否接受 `BASIC_GRAPHIC` 输出池；如果实际返回 `pooledBlock`，必须完成 BufferPool 握手和回收。此机声明了 passthrough Mapper 4 和本地 Mapper 5，不是可直接调用的远程 mapper。即使拿到 graphic handle，也必须验证 fence、可 CPU 锁定性和 YUV plane/crop 布局，再保存原始帧；不能从 stride 或第一个 FD 推定 NV12。

已在 `/dev/dma_heap/system` 和 `/dev/dma_heap/mtk_mm-uncached` 分别验证 4 KiB 分配、`mmap` 与 `DMA_BUF_IOCTL_SYNC` 的 CPU 写入／回读。尚未验证 HAL 能导入这些分配；这不是帧解码或输出映射的验收。

## ABI 与许可证

- [AOSP Codec2 HIDL 1.0](https://android.googlesource.com/platform/hardware/interfaces/+/refs/tags/android-14.0.0_r1/media/c2/1.0/)、[1.1](https://android.googlesource.com/platform/hardware/interfaces/+/refs/tags/android-14.0.0_r1/media/c2/1.1/)、[1.2](https://android.googlesource.com/platform/hardware/interfaces/+/refs/tags/android-14.0.0_r1/media/c2/1.2/)，以及 [`hidl-gen` 的编号](https://android.googlesource.com/platform/system/tools/hidl/+/refs/tags/android-14.0.0_r1/Interface.cpp#464)与[代理／桩生成逻辑](https://android.googlesource.com/platform/system/tools/hidl/+/refs/tags/android-14.0.0_r1/generateCpp.cpp)：按声明接口分配 token；`listComponents = 3`，`createComponent{,_1_1,_1_2} = 1/9/10`。同时交叉检查了设备 `/vendor/lib64/android.hardware.media.c2@1.{0,1,2}.so` 中的生成代理。`listComponents` 回复依次是 HIDL 状态、Codec2 状态、`vec<ComponentTraits>`；组件 trait 的大小为 64 字节，名称／MIME／别名向量的偏移为 0／32／48。
- [AOSP `Parcel.cpp`](https://android.googlesource.com/platform/system/libhwbinder/+/refs/tags/android-14.0.0_r1/Parcel.cpp) 与 [GKI Binder 驱动](https://android.googlesource.com/kernel/common/+/2d2543bebccf/drivers/android/binder.c)：使用 HwBinder scatter-gather 对象及父索引、父偏移；空向量的 null 子对象可能只存在于 inline parcel，不在 offsets 表中。GLib 驱动异步 `getService`、接口方法及本地 listener；单次异步回复等待上限 5 秒，不含 libgbinder 初始化。超时取消本地等待，不撤回内核中已送出的事务；诊断脚本另设进程级上限。
- [AOSP HIDL 客户端](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-14.0.0_r1/media/codec2/hal/client/client.cpp)、[转换逻辑](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-14.0.0_r1/media/codec2/hal/hidl/1.0/utils/types.cpp)、[`C2Param.h`](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-14.0.0_r1/media/codec2/core/include/C2Param.h) 与 [`C2Config.h`](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-14.0.0_r1/media/codec2/core/include/C2Config.h)：按实际结构写入 `size → index → payload`，每条参数补齐至 8 字节；沿 `IComponent → IComponentInterface → IConfigurable` 查询，不复制 Framework 二进制或头文件。
- [AOSP `Codec2InfoBuilder`](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-14.0.0_r1/media/codec2/sfplugin/Codec2InfoBuilder.cpp#634) 提供 store/XML 分类参考。[libgbinder 1.1.47](https://github.com/mer-hybris/libgbinder/tree/1.1.47) 提供 IPC、设备和 callback 生命周期；构建最低要求为 libgbinder 1.1.43。只使用系统的 Linux UAPI、GLib 和 libgbinder；本项目未引入 Android Framework、AOSP 源文件或 MediaTek 二进制。项目代码许可证见 `LICENSES/Apache-2.0.txt`，外部 libgbinder 的 BSD-3-Clause 声明见 `LICENSES/libgbinder.txt`。
