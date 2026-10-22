#
# EasyTier ARMv7 Cross-Compilation Makefile
# Target Platform: RV1106 (arm-rockchip830-linux-uclibcgnueabihf)
# Source: https://github.com/EasyTier/EasyTier (main / 2.7.0)
#
# EasyTier 是 Rust 项目，交叉编译有四道坎（每一道都踩过，别再改）：
#
#   1. 目标用 armv7-unknown-linux-musleabihf（musl 静态链接）。
#      静态二进制不依赖目标机 libc，所以 uClibc 根文件系统可以直接跑；
#      换成 gnueabihf 会在设备上直接起不来。
#
#   2. C 依赖（ring / zstd / kcp-sys）用 zig 当交叉 C 编译器。
#      toolchain/ 下三个 wrapper 把 cc-rs 传来的 Rust triple
#      （--target=armv7-unknown-linux-musleabihf）改写成 zig 认的
#      arm-linux-musleabihf。
#
#   3. 链接器必须用 rust-lld，**不能**用 zig cc。
#      rustc 在 musl target 下会自带 crt1.o/crti.o/crtn.o + libc.a + libunwind.a，
#      zig cc 又会塞它自己那份 musl 启动文件 →
#        ld.lld: error: duplicate symbol: _start
#
#   4. proto3 的显式 optional 标签要求 protoc >= 3.12（Ubuntu 20.04 自带 3.6.1
#      会失败）。scripts/ensure-protoc.sh 负责检测/下载，通常不用管。
#
# 上游已发 v2.7.0（tag 8b7f1f01）。此处仍显式钉 commit 而不钉 tag：
# tag 可被上游移动，commit 不会。EASYTIER_COMMIT 必须与 v2.7.0 指向同一提交。
#

SHELL := /bin/bash
export LC_ALL=C

# Build output configuration
PKG_NAME := easytier
PKG_BIN ?= out
OUTPUT_DIR := $(PKG_BIN)
CURRENT_DIR := $(shell pwd)

# Upstream version / pinned commit
EASYTIER_VERSION ?= 2.7.0
EASYTIER_COMMIT ?= 8b7f1f0196edb5b580829ed5f135050fb5853478
EASYTIER_SRC := $(CURDIR)/easytier

# Rust toolchain (workspace 的 rust-version = "1.95"，低于它会被 cargo 拒绝)
RUST_TOOLCHAIN ?= 1.95
TARGET ?= armv7-unknown-linux-musleabihf
JOBS ?= $(shell nproc)

# 构建产物目录放在 submodule 之外，保持源码树干净
CARGO_TARGET_DIR ?= $(CURDIR)/target

# VARIANT:
#   client (默认) - 精简客户端，产出 easytier-core + easytier-cli
#   mini          - 上游官方最小节点 easytier-mini（无 CLI、无管理 API）
#   full          - 上游默认全功能，体积大，仅供对照/调试
VARIANT ?= client

# client 变体的 feature 集 = 上游 easytier-mini 的集合 + management + cli：
#   aes-gcm          加密（与全功能版默认配置互通）
#   dhcp-ipv4        自动获取虚拟 IP
#   proxy-cidr-monitor 子网路由变化监控
#   smoltcp          用户态 TCP/IP 栈
#   tun              TUN 网卡（连带 linux-netlink）
#   cli              easytier-cli + management（→ web-client + logging）
#   tcp-hole-punch   TCP 打洞（UDP 被封时的退路，设备实测仅 +58 KB）
#
# 刻意不含：
#   quic / websocket / kcp / faketcp / wireguard  —— 非 TCP/UDP 传输，砍掉约 60% 体积
#   zstd / upnp / socks5 / icmp-proxy / magic-dns —— 与 KVM 场景无关
#   endpoint-discovery                            —— 只影响 DNS TXT/SRV 记录与"按域名
#                                                    自动发现对端"；普通域名解析走系统
#                                                    getaddrinfo，不依赖它（+1.14 MiB）
#
# 注意 socks5 / vpn-portal：默认集不含 socks5、wireguard 两个 feature，但
# TOML 里的 socks5_proxy / [[port_forward]] / [vpn_portal_config] 仍会被**接受**：
#   - socks5_proxy / port_forward：静默不生效（不监听、不报错）
#   - vpn_portal_config：启动时明确报 "no host adapter exists" 退出
# 不要以为有 smoltcp 就有 socks5 网关——两者是独立 feature。
#
# **cli 已移除**：官方 easytier-cli 改由本目录 cli/easytier-cli.c 提供 C 版
# （体积 30 KB vs 官方 1.1 MB，功能覆盖 peer/route/node info/stun）。
# 但 `management` 必须保留——easytier-core 的 required-features 要它，
# 且 C 版 CLI 正是通过它暴露的本地 RPC（127.0.0.1:15888）通信。
CLIENT_FEATURES ?= aes-gcm,dhcp-ipv4,proxy-cidr-monitor,smoltcp,tun,management,tcp-hole-punch

# ---- C 控制台（替代官方 easytier-cli）----
#
# 官方 easytier-cli 是独立二进制（源码 crate `easytier` 的 bin target），
# 所以能真正删掉——上面的 CLIENT_FEATURES 去掉了 `cli` 特性后 cargo 不再
# 产出它，省下约 1.1 MB（UPX 后）。
#
# C 版的通信不走 HTTP：easytier 的控制面是自定义 RPC（16 字节二进制头 +
# protobuf），详见 cli/easytier-cli.c 的文件头注释。
CLI_ENABLE ?= 1
LIB_DIR    := $(CURDIR)/../lib
LIB_A      := $(LIB_DIR)/out/libvpnctl.a
CLI_BIN    := $(OUTPUT_DIR)/easytier-cli
CLI_SRC    := $(CURDIR)/cli/easytier-cli.c
CLI_CFLAGS := -std=c17 -O2 -Wall -Wextra -DET_VERSION='"$(EASYTIER_VERSION)"'
CLI_LDLIBS := -ljansson
# C 版用 uClibc 工具链（动态链接，与 musl 静态的 core 并存没冲突：
# 设备上两者都能跑，jansson 设备已有）
CROSS_COMPILE ?= arm-rockchip830-linux-uclibcgnueabihf-
CC_CLI := $(CROSS_COMPILE)gcc
ET_SYSROOT ?= $(shell ls -d /root/rv1106/flexkvm_sdk/sysdrv/source/buildroot/buildroot-*/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot 2>/dev/null | head -1)

# upx 压缩（与 tailscale/zerotier/frp 一致）；UPX=0 关闭
UPX ?= 1

ifeq ($(VARIANT),client)
  CARGO_PKG := easytier
  CARGO_PROFILE := mini
  CARGO_EXTRA := --no-default-features --features "$(CLIENT_FEATURES)"
  # 只产出 core；CLI 由 make cli 用 C 版补齐
  BINARIES := easytier-core
else ifeq ($(VARIANT),mini)
  CARGO_PKG := easytier-mini
  CARGO_PROFILE := mini
  CARGO_EXTRA :=
  BINARIES := easytier-mini
else ifeq ($(VARIANT),full)
  CARGO_PKG := easytier
  CARGO_PROFILE := release
  CARGO_EXTRA :=
  BINARIES := easytier-core easytier-cli
else
  $(error VARIANT 必须是 client / mini / full，当前为 "$(VARIANT)")
endif

# 交叉工具链：zig 包装的 C/CXX/AR + rust-lld 链接器
HOST_TRIPLE := $(shell rustc +$(RUST_TOOLCHAIN) -vV 2>/dev/null | awk '/^host:/{print $$2}')
RUST_SYSROOT := $(shell rustc +$(RUST_TOOLCHAIN) --print sysroot 2>/dev/null)
RUST_LLD := $(RUST_SYSROOT)/lib/rustlib/$(HOST_TRIPLE)/bin/rust-lld
PROTOC ?= $(shell $(CURDIR)/scripts/ensure-protoc.sh)

export CC_armv7_unknown_linux_musleabihf := $(CURDIR)/toolchain/armv7-musl-cc
export CXX_armv7_unknown_linux_musleabihf := $(CURDIR)/toolchain/armv7-musl-cxx
export AR_armv7_unknown_linux_musleabihf := $(CURDIR)/toolchain/armv7-musl-ar
export CARGO_TARGET_ARMV7_UNKNOWN_LINUX_MUSLEABIHF_LINKER := $(RUST_LLD)
export CARGO_TARGET_ARMV7_UNKNOWN_LINUX_MUSLEABIHF_AR := $(CURDIR)/toolchain/armv7-musl-ar

.PHONY: all clean clone build cli lib install info help check-tools

all: info check-tools clone build cli install
	@echo "Build $(PKG_NAME) done"

info:
	@echo "=========================================="
	@echo "EasyTier ARMv7 Cross-Compilation"
	@echo "=========================================="
	@echo "Version: $(EASYTIER_VERSION) (commit $(EASYTIER_COMMIT))"
	@echo "Variant: $(VARIANT)  ->  $(BINARIES)"
	@echo "Profile: $(CARGO_PROFILE)"
	@echo "Rust: $(RUST_TOOLCHAIN)  Target: $(TARGET)"
	@echo "Source: $(EASYTIER_SRC)"
	@echo "Output Dir: $(OUTPUT_DIR)"
	@echo "UPX: $(UPX)"
ifeq ($(VARIANT),client)
	@echo "Features: $(CLIENT_FEATURES)"
endif
	@echo "=========================================="

check-tools:
	@command -v zig >/dev/null 2>&1 || { \
		echo "错误：找不到 zig。EasyTier 的 C 依赖（ring/zstd/kcp-sys）需要它当交叉 C 编译器。"; \
		echo "      安装：https://ziglang.org/download/ （本仓库验证过 0.16.0-dev）"; exit 1; }
	@rustc +$(RUST_TOOLCHAIN) --version >/dev/null 2>&1 || { \
		echo "错误：找不到 Rust $(RUST_TOOLCHAIN) 工具链。"; \
		echo "      安装：rustup toolchain install $(RUST_TOOLCHAIN)"; exit 1; }
	@test -x "$(RUST_LLD)" || { \
		echo "错误：找不到 rust-lld（$(RUST_LLD)）。"; \
		echo "      它随 Rust 工具链分发；若缺失请检查 rustup 安装是否完整。"; exit 1; }
	@test -n "$(PROTOC)" || { \
		echo "错误：没有可用的 protoc >= 3.12。"; \
		echo "      排查：直接执行 scripts/ensure-protoc.sh 看报错"; \
		echo "      绕过：make PROTOC=/path/to/protoc"; exit 1; }
	@rustup target list --installed --toolchain $(RUST_TOOLCHAIN) 2>/dev/null | grep -qx "$(TARGET)" || \
		rustup target add --toolchain $(RUST_TOOLCHAIN) $(TARGET)

clone:
	@echo "Initializing/Updating easytier submodule (commit $(EASYTIER_COMMIT))..."
	@git submodule update --init --force easytier
	@cd $(EASYTIER_SRC) && git checkout -q $(EASYTIER_COMMIT) 2>/dev/null || \
		{ echo "错误：无法切到 $(EASYTIER_COMMIT)，请先 git submodule update --init --force easytier"; exit 1; }

build: check-tools clone
	@echo "Building $(CARGO_PKG) [$(VARIANT)] ..."
	@cd $(EASYTIER_SRC) && \
		CARGO_TARGET_DIR=$(CARGO_TARGET_DIR) \
		PROTOC=$(PROTOC) \
		cargo +$(RUST_TOOLCHAIN) build \
			--profile $(CARGO_PROFILE) \
			-p $(CARGO_PKG) $(CARGO_EXTRA) \
			--target $(TARGET) -j$(JOBS)

lib:
	@$(MAKE) -C $(LIB_DIR)

cli: lib
	@echo "Building C console (easytier-cli)..."
	@mkdir -p $(OUTPUT_DIR)
	@$(CC_CLI) $(CLI_CFLAGS) \
		$(if $(ET_SYSROOT),--sysroot=$(ET_SYSROOT) -I$(ET_SYSROOT)/usr/include) \
		-I$(LIB_DIR)/include \
		$(CLI_SRC) $(LIB_A) \
		$(if $(ET_SYSROOT),-L$(ET_SYSROOT)/usr/lib) \
		$(CLI_LDLIBS) -o $(CLI_BIN)
	@echo "  -> $(CLI_BIN)"

install: build cli
	@echo "Installing to $(OUTPUT_DIR)/ ..."
	@mkdir -p $(OUTPUT_DIR)
	@for b in $(BINARIES); do \
		cp -f $(CARGO_TARGET_DIR)/$(TARGET)/$(CARGO_PROFILE)/$$b $(OUTPUT_DIR)/ || exit 1; \
	done
ifeq ($(UPX),1)
	@echo ""
	@echo "Compressing with upx..."
	@if command -v upx >/dev/null 2>&1; then \
		upx --best --lzma $(addprefix $(OUTPUT_DIR)/,$(BINARIES)); \
	else \
		echo "  upx not found, skipping compression"; \
	fi
endif
	@# 注意：CLI 现在就叫 easytier-cli（与官方同名），这里**不能**再
	@# rm 它——那会把刚生成的产物删掉。早期带 -c 后缀的遗留文件已清理完毕。
	@test -f $(CLI_BIN) && { which $(CROSS_COMPILE)strip >/dev/null 2>&1 && $(CROSS_COMPILE)strip $(CLI_BIN) || true; } || true
	@echo ""
	@echo "Installed files:"
	@ls -lh $(OUTPUT_DIR)/

clean:
	@echo "Clean done (removed $(PKG_BIN)/ and $(CARGO_TARGET_DIR)/)"
	@rm -rf $(PKG_BIN) $(CARGO_TARGET_DIR)

help:
	@echo "EasyTier Build Targets:"
	@echo ""
	@echo "  make               - Build default variant (client) and compress"
	@echo "  make clone         - Checkout pinned easytier submodule commit"
	@echo "  make check-tools   - Verify zig / rust / rust-lld / protoc"
	@echo "  make build         - Build only"
	@echo "  make install       - Build and compress with upx"
	@echo "  make clean         - Clean build artifacts"
	@echo "  make help          - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  VARIANT=client|mini|full  - what to build (default: client)"
	@echo "  UPX=0                     - skip upx compression"
	@echo "  RUST_TOOLCHAIN=1.95       - rustup toolchain"
	@echo "  PROTOC=/path/to/protoc    - override protoc"
	@echo "  CLIENT_FEATURES=...       - override cargo feature set (VARIANT=client)"
