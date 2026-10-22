#!/bin/bash
#
# ensure-protoc.sh - 保证有一个能编译 proto3 optional 字段的 protoc
#
# 为什么需要：easytier-proto/build/main.rs 每次构建都会调用 prost-build 重新生成
# 代码，而 EasyTier 的 .proto 里用了 proto3 的显式 `optional` 标签，这要求
# protoc >= 3.12。Ubuntu 20.04 自带的 protoc 是 3.6.1，会直接报
#   "Explicit 'optional' labels are disallowed in the Proto3 syntax"
#
# 行为：
#   1. $PROTOC 指定且可用 → 用它
#   2. 系统 protoc 版本 >= 3.12 → 用它
#   3. 否则下载官方 release 到 .tools/protoc（只需一次）
#
# 输出：protoc 可执行文件的绝对路径（供 Makefile 的 $(shell ...) 使用）
#
set -euo pipefail

MIN_MAJOR=3
MIN_MINOR=12
PROTOC_VERSION=36.2
CACHE_DIR="$(cd "$(dirname "$0")/.." && pwd)/.tools/protoc"

version_ok() {
	local bin="$1"
	[ -x "$bin" ] || command -v "$bin" >/dev/null 2>&1 || return 1
	local raw
	raw=$("$bin" --version 2>/dev/null | awk '{print $2}') || return 1
	local major minor
	major=$(echo "$raw" | cut -d. -f1)
	minor=$(echo "$raw" | cut -d. -f2)
	[ -n "$major" ] && [ -n "$minor" ] || return 1
	if [ "$major" -gt "$MIN_MAJOR" ]; then return 0; fi
	if [ "$major" -eq "$MIN_MAJOR" ] && [ "$minor" -ge "$MIN_MINOR" ]; then return 0; fi
	return 1
}

# 0. 自我保护：PROTOC 指向本脚本自身时，下面 version_ok "$PROTOC" 会执行
#    `ensure-protoc.sh --version`，而子进程又继承同一个 PROTOC，导致无限递归 fork。
self="$(readlink -f "$0")"
if [ -n "${PROTOC:-}" ] && [ "$(readlink -f "$PROTOC" 2>/dev/null)" = "$self" ]; then
	echo "warning: PROTOC 指向 ensure-protoc.sh 自身，已忽略；应写 PROTOC=\$(ensure-protoc.sh) 取其输出" >&2
	unset PROTOC
fi

# 1. 显式指定
if [ -n "${PROTOC:-}" ]; then
	if version_ok "$PROTOC"; then
		echo "$PROTOC"
		exit 0
	fi
	echo "warning: PROTOC=$PROTOC 版本过低或不可执行（需要 >= ${MIN_MAJOR}.${MIN_MINOR}）" >&2
fi

# 2. 系统 protoc
if version_ok protoc; then
	command -v protoc
	exit 0
fi

# 3. 下载缓存
cached="$CACHE_DIR/bin/protoc"
if version_ok "$cached"; then
	echo "$cached"
	exit 0
fi

case "$(uname -m)" in
	x86_64)  arch=x86_64 ;;
	aarch64|arm64) arch=aarch_64 ;;
	*)
		echo "error: 不支持的构建机架构 $(uname -m)，请安装 protoc >= ${MIN_MAJOR}.${MIN_MINOR} 后用 PROTOC=... 指定" >&2
		exit 1
		;;
esac

url="https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOC_VERSION}/protoc-${PROTOC_VERSION}-linux-${arch}.zip"
echo "info: 系统 protoc 过旧，下载 protoc ${PROTOC_VERSION} → $CACHE_DIR" >&2
mkdir -p "$CACHE_DIR"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
if ! curl -fsSL -o "$tmp/protoc.zip" "$url"; then
	echo "error: 下载失败：$url" >&2
	exit 1
fi
unzip -oq "$tmp/protoc.zip" -d "$CACHE_DIR"

if ! version_ok "$cached"; then
	echo "error: 下载后的 protoc 不可用：$cached" >&2
	exit 1
fi
echo "$cached"
