# flexkvm-easytier

EasyTier 客户端 ARMv7 交叉编译（RV1106 / uClibc 根文件系统）。

## 版本

- EasyTier: **v2.7.0**（tag 提交 `8b7f1f01`）
  > 上游已发 v2.7.0。仍显式钉 commit 而不钉 tag：tag 可被上游移动，commit 不会。
  > **注意**：早期版本钉的是 `0a783c8e`，那是 v2.7.0 之前 14 个提交，并非 tag 本身，
  > 已修正。其中包含与本项目直接相关的修复
  > `fix(peers): keep TCP hole-punched connections alive with 1s pings`。
- Rust: **1.95**（workspace `rust-version = "1.95"`）
- 目标: `armv7-unknown-linux-musleabihf`（**musl 静态**，不依赖目标机 libc）

## 编译

```bash
make            # 默认构建 client 变体并做 upx 压缩
make VARIANT=mini
make UPX=0
```

依赖（`make check-tools` 会检查）：

| 依赖 | 用途 |
|---|---|
| `zig` | 交叉 C 编译器，编 `ring` / `zstd` / `kcp-sys`（见 `toolchain/` 的 wrapper） |
| Rust 1.95 + 对应 target | `rustup toolchain install 1.95 && rustup target add --toolchain 1.95 armv7-unknown-linux-musleabihf` |
| `rust-lld` | 链接器（随 Rust 工具链分发）。**不能用 zig cc 链接**，否则 CRT 重复定义 |
| `protoc >= 3.12` | `easytier-proto` 每次构建都重新生成代码。系统版本过旧时 `scripts/ensure-protoc.sh` 会自动下载到 `.tools/` |
| `upx`（可选） | 体积压缩 |

## 产物

```
out/
├── easytier-core    # 守护进程
└── easytier-cli     # 管理 CLI（通过本地 RPC 查询/控制）
```

`file` 结果：`ELF 32-bit LSB executable, ARM, EABI5, statically linked, stripped`，
**0 个动态依赖、无 interp**。

## 构建变体

| VARIANT | 产物 | 未压缩 | UPX 后 | 空闲 RSS |
|---|---|---|---|---|
| **`client`（默认）** | `easytier-core` | 5,822,408 B | 1,904,056 B | 6.1 MB |
| | `easytier-cli` | 3,097,864 B | 1,118,116 B | — |
| `mini` | `easytier-mini` | 4,465,728 B | 1,537,432 B | — |
| `full` | `easytier-core` + `easytier-cli` | 20,169,036 B | 5.3 MB | — |

> 默认变体的 RSS 是设备实测：加入 `tcp-hole-punch` 前 5,992 kB、之后 6,064 kB
> （都是空载、单实例）。UPX 后两个文件合计约 3.0 MB。

对照：tailscaled 29.8 MB / ZeroTier 7.3 MB / NetBird 33.7 MB（同一块板子实测）。

## feature 取舍（为什么默认是 client）

默认变体 = **上游 `easytier-mini` 的 feature 集合 + `management` + `cli` + `tcp-hole-punch`**：

```
aes-gcm  dhcp-ipv4  proxy-cidr-monitor  smoltcp  tun  cli  tcp-hole-punch
                                            (cli → management → web-client + logging)
```

这样既有 mini 的体积，又有 `easytier-cli` 和本地 RPC 管理面；`tcp-hole-punch`
设备实测只 +58 KB（5,764,128 → 5,822,408 B），换 UDP 被封时的一条退路，值得。

> **注意 `socks5` 与 `smoltcp` 是两回事**：默认集有 `smoltcp`（所以
> `[[proxy_network]]` 子网代理可用），但没有 `socks5` 特性，所以
> `socks5_proxy` / `[[port_forward]]` 这类 gateway 代理不可用。
> 更要当心的是，这两种配置**仍然会被解析并接受**：`socks5_proxy` 和
> `[[port_forward]]` 不监听也不报错（静默失效），`[vpn_portal_config]` 则会
> 在启动时明确报 `VPN portal is configured but no host adapter exists` 退出。

砍掉的部分与理由：

| 砍掉 | 省掉什么 | 影响 |
|---|---|---|
| `quic` / `websocket` / `kcp` / `faketcp` | quinn + **rustls** + tokio-websockets + kcp-sys | 只剩 TCP/UDP 传输，不能走 ws/wss、没有 KCP 抗丢包 |
| `wireguard` | boringtun + **ring** | 标准 WireGuard 客户端接不进来 |
| `zstd` / `upnp` / `socks5` / `icmp-proxy` | zstd-sys、hyper、… | 压缩、自动端口映射、免 root SOCKS5、ICMP 代理 |
| `magic-dns` | hickory-server/client | 不能按 `.et.net` 主机名互访，只能记 IP |
| `endpoint-discovery` | **hickory DNS（+1.14 MiB / +1.15 MB 内存）** | 见下 |

> `tcp-hole-punch` 已加入默认集（原表里列为"按需加回"，现已常开）。
> 它不依赖 TXT DNS：TCP STUN 默认服务器（`stun.hot-chilli.net` /
> `turn.cloudflare.com` 等）全是普通域名，设备实测能拿到映射地址
> （`113.77.235.62:20353`），punch 路径确实可用。

### 关于 DNS 的两点澄清

1. **`endpoint-discovery` 依赖的 `dns-resolver` 不是给用户解析域名的。**
   EasyTier 的普通域名解析（A/AAAA）走 `tokio::net::lookup_host`，即系统
   `getaddrinfo`、读 `/etc/resolv.conf`，**和这个 feature 无关**——所以填
   `[[peer]] uri = "tcp://你的域名:11010"` 照样能解析。
   被砍掉的只是 **TXT / SRV 记录**解析，影响：
   - STUN 服务器的 `txt:` 条目展开（日志会刷 `this build does not include TXT DNS resolution`）
   - `endpoint-discovery`：按 DNS SRV 记录或 HTTP 服务**自动发现对端**

   即：**用户手填对端地址（IP 或域名都行）→ 不需要它；只填一个域名靠自动发现对端 → 需要它。**

2. **留着 `dns-resolver` 有个兜底好处**：内建 hickory resolver 硬编码了
   `223.5.5.5` / `180.184.1.1`，系统解析失败时会重试一次。

### 不会动系统 DNS

`--accept-dns`（magic dns）在 **Linux 上是空实现**：`get_system_config()` 只对
Windows/macOS 返回实现（`dns_server/system_config/` 下只有 `darwin.rs` 和
`windows.rs`），Linux 直接 `Ok(None)`。仓库里 `resolv.conf` 只出现在 `darwin.rs`。

设备实测（`--accept-dns true`，最激进参数）：`/etc/resolv.conf` 的 md5 在启动前 /
运行中 / 停止后**三次完全一致**，`:53` 零监听。

## 运行时

```bash
# 设备上（配置格式见 examples/easytier.toml，注意别用仓库里那份过时的 android 示例）
./easytier-core --config-file /userdata/easytier/easytier.toml

# 查询状态
./easytier-cli peer
./easytier-cli route
```

- 需要 `/dev/net/tun` 和 root（要建 TUN、装路由）
- 本地 RPC 默认 `127.0.0.1:15888`
- 默认只监听 `11010`(tcp/udp)。上游那套 `11011`(wg/ws)、`11012`(quic/wss)、
  `11013`(faketcp) 在本裁剪版里没有对应 feature，配置里写上会打
  `listener add failed ... maybe not supported` 后跳过（不影响启动）

## 实测记录（RV1106，212 MB RAM，uClibc）

```
版本          v2.7.0-8b7f1f01
TUN           tun0，MTU 1360，静态二进制直接起
控制面        easytier-cli peer / route 正常
连通          设备 ↔ x86_64 对端：双向 ping 0% 丢包，2.6 ~ 5.4 ms
隧道          p2p / tcp
内存          空闲 6,132 kB，跑流量 7,292 kB
DNS           全程未改动（md5 三次一致）
```

## 注意事项

- **官方免费共享节点 `public.easytier.cn` 在部分网络下解析不到（NXDOMAIN）**，
  而官方文档《快速组网》正是让用户填它。生产建议自建共享节点
  （`easytier-core` 不带参数启动即为公共服务器，不需要 root）。
- 社区 issue [#719](https://github.com/EasyTier/EasyTier/issues/719) 报"子网代理在
  OpenWrt 上跑超一天必须重启路由器"，另有 [#1849](https://github.com/EasyTier/EasyTier/issues/1849)、
  [#1006](https://github.com/EasyTier/EasyTier/issues/1006)。**上产品前需要 7×24 长跑验证。**
- 本项目只做裁剪与交叉编译，**不改上游源码**，全部靠 cargo feature + profile 实现。

## 工具链

- 目标平台: RV1106 (arm-rockchip830-linux-uclibcgnueabihf)
- Rust target: `armv7-unknown-linux-musleabihf`
- C 交叉编译器: `zig cc -target arm-linux-musleabihf`（`toolchain/armv7-musl-*`）
- 链接器: `rust-lld`

## C 控制台（替代官方 `easytier-cli`）

官方 `easytier-cli` 是**独立二进制**，构建时已从产物中移除
（`CLIENT_FEATURES` 去掉 `cli` 特性，cargo 不再产出），省约 1.1 MB。
改由 `cli/easytier-cli.c` 提供：

```bash
make cli      # → out/easytier-cli
```

| | |
|---|---|
| 体积 | **22 KB**（官方 1,118,892 B 的 1/51）|
| 通信 | easytier-core 的本地 RPC（`127.0.0.1:15888`）|

命令面（5 个，全部实测）：`peer` / `route` / `node info` / `stun` / `version`

### 协议要点（自己实现的，没有 C 客户端库）

easytier 的控制面**不是 HTTP**，是自定义 RPC：

```
┌──────────────┬────────────────────┬─────────────────┐
│ TCPTunnelHdr │ PeerManagerHeader  │ RpcPacket       │
│ 4B LE len    │ 16B (packed)       │ protobuf        │
└──────────────┴────────────────────┴─────────────────┘
```

`lib/src/vpnctl_proto.c` 提供了 protobuf wire format 的最小实现。踩过的坑：

| 坑 | 现象 |
|---|---|
| 所有 u32 是**小端**（`DefaultEndian = LittleEndian`）| 用 htonl 会报 `body too long` 断连 |
| 单片消息 `total_pieces` 必须是 **0** | 设 1 会走进分片路径，报 `descriptor is missing` |
| `RpcPacket.body` 要包一层 `RpcRequest` | 直接塞 ListRouteRequest 会超时（`Elapsed(())`）|
| `RpcPacket.descriptor` 字段必须填 | 缺了服务端静默丢包 |
| 响应帧 `packet_type` 在偏移 **8**（不是 12）| 偏移 12 是 `len`，会把有效响应判成畸形 |
| 空响应没有 `response` 字段（proto3 不序列化零值）| 单机场景 `peer`/`route` 会误报错 |

### 多实例：CLI 要带 `-p`

`easytier-cli` 支持 `-p/--rpc-portal <ip:port>`（与官方同名选项）：

```bash
./easytier-cli -p 127.0.0.1:15900 node info
```

不带时连默认 `127.0.0.1:15888`。设备上如果同时跑了多个实例，
不指定端口会读到"另一个"实例的状态——这个错很隐蔽，因为输出看起来完全正常。

### 运行时注意

**`rpc_portal` 只能通过命令行参数设置，TOML 里写的不生效**（实测）：

```bash
./easytier-core --config-file x.toml --rpc-portal 127.0.0.1:15888
```

多实例部署时如果都挤在默认 15888 上，第二个会绑定失败。
