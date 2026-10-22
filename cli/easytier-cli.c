/*
 * easytier-cli.c - easytier-core 的 C 控制台（替代官方 easytier-cli）。
 *
 * 与其余四家的关键差别：easytier 的控制面**不是 HTTP**，而是自定义 RPC：
 *
 *   ┌──────────────┬────────────────────┬─────────────────┐
 *   │ TCPTunnelHdr │ PeerManagerHeader  │ RpcPacket       │
 *   │ 4B LE len    │ 16B (packed)       │ protobuf        │
 *   └──────────────┴────────────────────┴─────────────────┘
 *
 *   TCPTunnelHeader  : uint32 len = 16 + payload 长度（LE）
 *   PeerManagerHeader: from_peer(u32 LE) to_peer(u32 LE)
 *                      packet_type(u8) flags(u8) forward(u8) reserved(u8)
 *                      len(u32 LE = payload 长度)
 *   packet_type      : 8 = RpcReq，9 = RpcResp（easytier-core/src/packet/mod.rs）
 *
 * 协议细节来自读源码 + 设备实测。踩过的坑写在对应代码旁。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <jansson.h>

#include "vpnctl.h"
#include "vpnctl_proto.h"

#ifndef ET_VERSION
#define ET_VERSION "2.7.0"
#endif
#ifndef ET_RPC_HOST
#define ET_RPC_HOST "127.0.0.1"
#endif
#ifndef ET_RPC_PORT
#define ET_RPC_PORT 15888
#endif

/*
 * 运行时可覆盖的 RPC 端点。官方 easytier-cli 有 -p/--rpc-portal，
 * 多实例部署时必须能指定（否则永远连默认 15888，接上别的实例）。
 */
static char g_rpc_host[64] = ET_RPC_HOST;
static int g_rpc_port = ET_RPC_PORT;

#define PROG "easytier-cli"

#define ET_PKT_RPC_REQ 8
#define ET_PKT_RPC_RESP 9

/* RpcPacket 字段号（common.proto:149） */
#define F_FROM_PEER 1
#define F_TO_PEER 2
#define F_TXN_ID 3
#define F_DESCRIPTOR 4
#define F_BODY 5
#define F_IS_REQUEST 6
#define F_TOTAL_PIECES 7
#define F_PIECE_IDX 8
#define F_COMPRESSION 10

/* RpcDescriptor 字段号（common.proto:93） */
#define D_DOMAIN 1
#define D_PROTO 2
#define D_SERVICE 3
#define D_METHOD_IDX 4

/*
 * RpcCompressionInfo（common.proto:140）：algo=1 accepted_algo=2
 * 枚举 CompressionAlgoPb { Invalid=0, None=1, Zstd=2 }
 *
 * **坑**：必须显式填 None(1)。填 0(Invalid) 服务端回
 * "invalid compression algorithm: Invalid"，请求直接失败（实测）。
 */
#define COMP_NONE 1

/* InstanceIdentifier（api_instance.proto:9）：空 selector = 唯一默认实例 */
#define INST_SELECTOR 2

/* ListRouteResponse 里 Route 的字段号（api_instance.proto:64） */
#define R_PEER_ID 1
#define R_IPV4_ADDR 2
#define R_NEXT_HOP 3
#define R_COST 4
#define R_PROXY_CIDRS 5
#define R_HOSTNAME 6
#define R_STUN_INFO 7
#define R_VERSION 9
#define R_PATH_LATENCY 11
#define R_IPV6_ADDR 15

/* Ipv4Inet { Ipv4Addr address=1; uint32 network_length=2 }
 * Ipv4Addr { uint32 addr=1 } —— addr 是主机字节序的 u32 */
#define IPV4_ADDR 1
#define IPV4_NETLEN 2
#define IPV4_ADDR_VAL 1

/* StunInfo { NatType udp_nat_type=1; tcp=2; last_update=3; public_ip=4; ... } */
#define SI_UDP_NAT 1
#define SI_TCP_NAT 2
#define SI_PUBLIC_IP 4

/* NodeInfo（api_instance.proto:93） */
#define N_PEER_ID 1
#define N_IPV4_ADDR 2
#define N_PROXY_CIDRS 3
#define N_HOSTNAME 4
#define N_STUN_INFO 5
#define N_INST_ID 6
#define N_LISTENERS 7
#define N_VERSION 9

/* ---------------- 传输层 ---------------- */

static int read_full(int fd, void *buf, size_t n) {
    size_t off = 0;
    uint8_t *p = buf;

    while (off < n) {
        ssize_t r = recv(fd, p + off, n - off, 0);

        if (r > 0) {
            off += (size_t)r;
            continue;
        }
        if (r == 0) {
            return -ECONNRESET;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -ETIMEDOUT;
        }
        return -errno;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t n) {
    size_t off = 0;
    const uint8_t *p = buf;

    while (off < n) {
        ssize_t r = send(fd, p + off, n - off, MSG_NOSIGNAL);

        if (r > 0) {
            off += (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return -ETIMEDOUT;
        }
        return r == 0 ? -EPIPE : -errno;
    }
    return 0;
}

/* 发一次 RPC，响应正文拷进 *resp_body（malloc） */
static int et_rpc(uint32_t method_index, const char *service,
                  const uint8_t *req_body, size_t req_len, uint8_t **resp_body,
                  size_t *resp_len) {
    int fd = -1;
    int ret;
    uint32_t txn = (uint32_t)rand();
    vpnctl_pb_writer_t desc, pkt, comp;
    uint8_t hdr[20];
    uint32_t net32;

    if (resp_body) {
        *resp_body = NULL;
    }
    if (resp_len) {
        *resp_len = 0;
    }

    vpnctl_pb_writer_init(&desc);
    vpnctl_pb_writer_init(&pkt);
    vpnctl_pb_writer_init(&comp);

    vpnctl_pb_put_bytes(&desc, D_DOMAIN, "", 0);
    vpnctl_pb_put_bytes(&desc, D_PROTO, service, strlen(service));
    vpnctl_pb_put_bytes(&desc, D_SERVICE, service, strlen(service));
    vpnctl_pb_put_uint(&desc, D_METHOD_IDX, method_index);

    vpnctl_pb_put_uint(&comp, 1, COMP_NONE);
    vpnctl_pb_put_uint(&comp, 2, COMP_NONE);

    vpnctl_pb_put_uint(&pkt, F_FROM_PEER, 1);
    vpnctl_pb_put_uint(&pkt, F_TO_PEER, 1);
    vpnctl_pb_put_uint(&pkt, F_TXN_ID, txn);
    /*
     * **关键**：RpcPacket.body 不是裸的请求消息，而是
     * `RpcRequest { RpcDescriptor descriptor=1; bytes request=2; int32
     *  timeout_ms=3 }`（common.proto:102）的 protobuf。
     * 服务端 easytier-core/src/rpc/server.rs:225 先
     * `RpcRequest::decode(body)` 再 dispatch。直接把 ListRouteRequest 塞进
     * body 会解出一个空 RpcRequest，descriptor 为空 → 服务端不认 →
     * 直到 rx_timeout 才回 `Timeout`（实测表现为 body 里 `Elapsed(())`）。
     */
    {
        vpnctl_pb_writer_t rpc_req;

        vpnctl_pb_writer_init(&rpc_req);
        vpnctl_pb_put_msg(&rpc_req, 1, &desc);        /* RpcRequest.descriptor */
        vpnctl_pb_put_bytes(&rpc_req, 2, req_body, req_len); /* .request */
        vpnctl_pb_put_uint(&rpc_req, 3, 30000);       /* .timeout_ms */
        /*
         * RpcPacket 自身的 descriptor（与 RpcRequest 内层那个是两回事）：
         * rpc/server.rs:238 `let Some(desc) = packet.descriptor.clone() else
         * { warn!("received RPC request without a descriptor"); return; }`
         * —— 缺了它服务端直接丢包，客户端只能等超时。
         */
        vpnctl_pb_put_msg(&pkt, F_DESCRIPTOR, &desc);
        vpnctl_pb_put_bytes(&pkt, F_BODY, rpc_req.buf, rpc_req.len);
        vpnctl_pb_writer_free(&rpc_req);
    }
    vpnctl_pb_put_uint(&pkt, F_IS_REQUEST, 1);
    /*
     * **坑**：单片消息的 total_pieces 必须是 **0**，表示"未分片"。
     * 设成 1 会走进分片合并路径（packet.rs:102 的 feed）：
     * 那里 piece_idx==0 且 descriptor 缺失就报 "descriptor is missing"；
     * 而我们的 descriptor 是有的，但 total_pieces==1 又会被
     * `piece_idx >= total_pieces` 之类校验绕进去，最终表现为服务端
     * 不回包、客户端 recv 超时（实测踩过两次）。
     *
     * packet.rs:106：`if total_pieces == 0 && piece_idx == 0 { return 单片 }`
     */
    vpnctl_pb_put_uint(&pkt, F_TOTAL_PIECES, 0);
    vpnctl_pb_put_uint(&pkt, F_PIECE_IDX, 0);
    vpnctl_pb_put_msg(&pkt, F_COMPRESSION, &comp);

    if (pkt.oom) {
        ret = -ENOMEM;
        goto out;
    }

    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        ret = -errno;
        goto out;
    }
    {
        struct sockaddr_in addr;
        struct timeval tv = {.tv_sec = 40, .tv_usec = 0};

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)g_rpc_port);
        inet_pton(AF_INET, g_rpc_host, &addr.sin_addr);

        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            ret = (errno == ECONNREFUSED) ? -EHOSTDOWN : -errno;
            close(fd);
            fd = -1;
            goto out;
        }
        {
            int one = 1;

            (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }
    }

    /*
     * TCPTunnelHeader(4B = 16 + payload) + PeerManagerHeader(16B) + payload
     *
     * **所有 u32 都是小端**：源码 packet/mod.rs:18 定义 `type DefaultEndian =
     * LittleEndian;`，两个头里的 len/from_peer_id/to_peer_id 都是
     * `U32<DefaultEndian>`。用 htonl（大端）会被服务端读成天文数字，
     * 报 `InvalidPacket("body too long")` 后直接断连——实测踩过。
     */
    net32 = (uint32_t)(16u + (uint32_t)pkt.len); /* 已经是主机序 = 小端 */
    memcpy(hdr, &net32, 4);
    net32 = 1;
    memcpy(hdr + 4, &net32, 4); /* from_peer_id */
    net32 = 1;
    memcpy(hdr + 8, &net32, 4); /* to_peer_id */
    hdr[12] = ET_PKT_RPC_REQ;
    hdr[13] = 0; /* flags：不能带 ENCRYPTED，否则服务端要求解密 */
    hdr[14] = 0;
    hdr[15] = 0;
    net32 = (uint32_t)pkt.len;
    memcpy(hdr + 16, &net32, 4);

    ret = write_all(fd, hdr, sizeof(hdr));
    if (ret == 0) {
        ret = write_all(fd, pkt.buf, pkt.len);
    }
    if (ret != 0) {
        close(fd);
        fd = -1;
        goto out;
    }

    {
        uint8_t lenbuf[4];
        uint32_t total;
        uint8_t *frame;
        vpnctl_pb_reader_t rd, sub;
        const uint8_t *body_ptr = NULL;
        size_t body_len = 0;
        int rc;

        ret = read_full(fd, lenbuf, 4);
        if (ret != 0) {
            close(fd);
            fd = -1;
            goto out;
        }
        memcpy(&net32, lenbuf, 4);
        total = net32; /* 小端，与写入一致 */
        if (total < 16 || total > 16u * 1024 * 1024) {
            close(fd);
            fd = -1;
            ret = -EBADMSG;
            goto out;
        }

        frame = malloc(total);
        if (!frame) {
            close(fd);
            fd = -1;
            ret = -ENOMEM;
            goto out;
        }
        ret = read_full(fd, frame, total);
        close(fd);
        fd = -1;
        if (ret != 0) {
            free(frame);
            goto out;
        }

        /*
         * 收到的 total 字节 = PeerManagerHeader(16) + payload，
         * **不含** TCPTunnelHeader（那个 4 字节长度已经被先读掉了）。
         * PeerManagerHeader 布局（packet/mod.rs:127，repr(C, packed)）：
         *   [0..4)  from_peer_id  u32 LE
         *   [4..8)  to_peer_id    u32 LE
         *   [8]     packet_type
         *   [9]     flags
         *   [10]    forward_counter
         *   [11]    reserved
         *   [12..16) len           u32 LE
         * 所以 packet_type 在**偏移 8**；偏移 12 是 len（曾错当 type 判定，
         * 导致所有响应都被判成畸形）。发送侧因为前面多 4 字节 tunnel header，
         * packet_type 才落在 12。
         */
        if (frame[8] != ET_PKT_RPC_RESP) {
            free(frame);
            ret = -EBADMSG;
            goto out;
        }

        vpnctl_pb_reader_init(&rd, frame + 16, total - 16);
        rc = vpnctl_pb_get_msg(&rd, F_BODY, &sub);
        if (rc <= 0) {
            free(frame);
            ret = rc < 0 ? rc : -EBADMSG;
            goto out;
        }

        /*
         * body 是 `RpcResponse { bytes response=1; error.Error error=2;
         * uint64 runtime_us=3 }`（common.proto:127）。错误时 error 被置位，
         * response 为空——这里把 error 翻译成人话，避免上层只看到空数据。
         */
        {
            vpnctl_pb_reader_t resp_rd;
            vpnctl_pb_field_t rf;

            vpnctl_pb_reader_init(&resp_rd, sub.buf, sub.len);
            while (vpnctl_pb_next(&resp_rd, &rf) > 0) {
                if (rf.field == 2 && rf.wire_type == 2) {
                    /* error.Error 是 oneof，字段 8 = Timeout */
                    vpnctl_pb_reader_t er;
                    vpnctl_pb_field_t ef;

                    vpnctl_pb_reader_init(&er, rf.data, rf.data_len);
                    while (vpnctl_pb_next(&er, &ef) > 0) {
                        if (ef.field == 8) {
                            free(frame);
                            ret = vpnctl_error(
                                PROG, -ETIMEDOUT,
                                "easytier-core did not answer in time "
                                "(RPC timeout; is the instance running?)");
                            goto out;
                        }
                    }
                    free(frame);
                    ret = vpnctl_error(PROG, -EIO,
                                       "easytier-core returned an RPC error");
                    goto out;
                }
            }
        }

        /* 取 RpcResponse.response（字段 1） */
        {
            vpnctl_pb_reader_t resp_rd;

            vpnctl_pb_reader_init(&resp_rd, sub.buf, sub.len);
            rc = vpnctl_pb_get_bytes(&resp_rd, 1, (const uint8_t **)&body_ptr,
                                     &body_len);
            if (rc <= 0) {
                /*
                 * proto3 不序列化零值：**成功的空响应**（比如没有任何对端时的
                 * ListRouteResponse）连 response 字段都不存在，只留
                 * runtime_us。此时 rc==0 不是错误，要当成"空 body"返回成功，
                 * 否则 `peer`/`route` 在单机场景必然报错（实测踩过）。
                 */
                if (rc == 0) {
                    body_ptr = (const uint8_t *)"";
                    body_len = 0;
                } else {
                    free(frame);
                    ret = rc;
                    goto out;
                }
            }
        }

        if (resp_body && resp_len) {
            *resp_body = malloc(body_len + 1);
            if (!*resp_body) {
                free(frame);
                ret = -ENOMEM;
                goto out;
            }
            memcpy(*resp_body, body_ptr, body_len);
            (*resp_body)[body_len] = '\0';
            *resp_len = body_len;
        }
        free(frame);
        ret = 0;
    }

out:
    vpnctl_pb_writer_free(&desc);
    vpnctl_pb_writer_free(&pkt);
    vpnctl_pb_writer_free(&comp);
    if (ret == -EHOSTDOWN) {
        return vpnctl_error(PROG, ret,
                            "cannot connect to easytier-core at %s:%d\n"
                            "  note: rpc_portal in TOML is IGNORED; start it "
                            "with --rpc-portal %s:%d, and pass -p to this CLI "
                            "if it uses a non-default port",
                            g_rpc_host, g_rpc_port, g_rpc_host, g_rpc_port);
    }
    return ret;
}

/* 组 ListXxxRequest{instance:{instance_selector:{}}} */
static int et_inst_request(uint32_t method_index, const char *service,
                           uint8_t **resp, size_t *resp_len) {
    vpnctl_pb_writer_t req, inst, sel;
    int ret;

    vpnctl_pb_writer_init(&req);
    vpnctl_pb_writer_init(&inst);
    vpnctl_pb_writer_init(&sel);

    vpnctl_pb_put_msg(&inst, INST_SELECTOR, &sel);
    vpnctl_pb_put_msg(&req, 1, &inst);

    ret = et_rpc(method_index, service, req.buf, req.len, resp, resp_len);

    vpnctl_pb_writer_free(&req);
    vpnctl_pb_writer_free(&inst);
    vpnctl_pb_writer_free(&sel);
    return ret;
}

/* 把 Ipv4Inet 子消息格式化成 "a.b.c.d/len" */
static void fmt_ipv4_inet(const uint8_t *data, size_t len, char *dst,
                          size_t dst_len) {
    vpnctl_pb_reader_t r, addr_sub;
    uint64_t netlen = 32;
    uint64_t addr = 0;

    dst[0] = '\0';
    vpnctl_pb_reader_init(&r, data, len);

    if (vpnctl_pb_get_uint(&r, IPV4_NETLEN, &netlen) <= 0) {
        netlen = 32;
    }
    {
        vpnctl_pb_reader_t r2;

        vpnctl_pb_reader_init(&r2, data, len);
        if (vpnctl_pb_get_msg(&r2, IPV4_ADDR, &addr_sub) > 0) {
            (void)vpnctl_pb_get_uint(&addr_sub, IPV4_ADDR_VAL, &addr);
        }
    }
    /* addr 是主机序的 u32（网络号），按大端拆成四段 */
    snprintf(dst, dst_len, "%u.%u.%u.%u/%u", (unsigned)((addr >> 24) & 0xff),
             (unsigned)((addr >> 16) & 0xff), (unsigned)((addr >> 8) & 0xff),
             (unsigned)(addr & 0xff), (unsigned)netlen);
}

/* NAT 类型枚举名（common.proto 的 NatType） */
static const char *nat_type_name(uint64_t v) {
    switch (v) {
    case 0:
        return "Unknown";
    case 1:
        return "OpenInternet";
    case 2:
        return "NoPat";
    case 3:
        return "FullCone";
    case 4:
        return "Restricted";
    case 5:
        return "PortRestricted";
    case 6:
        return "Symmetric";
    case 7:
        return "SymUdpFirewall";
    case 8:
        return "SymmetricEasyInc";
    case 9:
        return "SymmetricEasyDec";
    default:
        return "?";
    }
}

/* ---------------- list peer ---------------- */

/*
 * 注意：ListPeer 的响应只给 peer_id 与连接统计，**没有 ipv4/hostname**——
 * 那些在 ListRoute 里。官方 CLI 的 peer 表实际是两者拼起来的，所以这里
 * 也调两次：先用 ListRoute 拿 id→(ipv4,hostname) 的映射，再调 ListPeer 补统计。
 */
typedef struct {
    uint32_t peer_id;
    char ipv4[64];
    char hostname[128];
    char version[32];
    uint64_t rx;
    uint64_t tx;
    int64_t latency_us;
    char tunnel[32];
    char nat[32];
} et_peer_t;

#define ET_MAX_PEERS 64

static int fetch_routes(et_peer_t *peers, int *n_peers) {
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    int ret = et_inst_request(3, "PeerManageRpc", &resp, &resp_len);

    if (ret != 0) {
        return ret;
    }

    /* ListRouteResponse { repeated Route routes = 1 } */
    {
        vpnctl_pb_reader_t r;
        vpnctl_pb_field_t f;

        vpnctl_pb_reader_init(&r, resp, resp_len);
        while (vpnctl_pb_next(&r, &f) > 0 && *n_peers < ET_MAX_PEERS) {
            vpnctl_pb_reader_t sub;
            uint64_t v;
            et_peer_t *p;

            if (f.field != 1 || f.wire_type != 2) {
                continue;
            }
            p = &peers[*n_peers];
            memset(p, 0, sizeof(*p));
            vpnctl_pb_reader_init(&sub, f.data, f.data_len);

            {
                vpnctl_pb_reader_t r2;

                vpnctl_pb_reader_init(&r2, f.data, f.data_len);
                if (vpnctl_pb_get_uint(&r2, R_PEER_ID, &v) > 0) {
                    p->peer_id = (uint32_t)v;
                }
            }
            {
                vpnctl_pb_reader_t r2;
                vpnctl_pb_field_t f2;

                vpnctl_pb_reader_init(&r2, f.data, f.data_len);
                while (vpnctl_pb_next(&r2, &f2) > 0) {
                    if (f2.field == R_IPV4_ADDR && f2.wire_type == 2) {
                        fmt_ipv4_inet(f2.data, f2.data_len, p->ipv4,
                                      sizeof(p->ipv4));
                    } else if (f2.field == R_HOSTNAME && f2.wire_type == 2) {
                        size_t n = f2.data_len < sizeof(p->hostname) - 1
                                       ? f2.data_len
                                       : sizeof(p->hostname) - 1;

                        memcpy(p->hostname, f2.data, n);
                        p->hostname[n] = '\0';
                    } else if (f2.field == R_VERSION && f2.wire_type == 2) {
                        size_t n = f2.data_len < sizeof(p->version) - 1
                                       ? f2.data_len
                                       : sizeof(p->version) - 1;

                        memcpy(p->version, f2.data, n);
                        p->version[n] = '\0';
                    } else if (f2.field == R_STUN_INFO &&
                               f2.wire_type == 2) {
                        vpnctl_pb_reader_t si;
                        uint64_t nat = 0;

                        vpnctl_pb_reader_init(&si, f2.data, f2.data_len);
                        if (vpnctl_pb_get_uint(&si, SI_UDP_NAT, &nat) > 0) {
                            snprintf(p->nat, sizeof(p->nat), "%s",
                                     nat_type_name(nat));
                        }
                    }
                }
            }
            (*n_peers)++;
        }
    }
    free(resp);
    return 0;
}

/* ListPeer 只补统计字段（按 peer_id 对齐） */
static void fetch_peer_stats(et_peer_t *peers, int n_peers) {
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    int ret = et_inst_request(1, "PeerManageRpc", &resp, &resp_len);

    if (ret != 0) {
        return;
    }

    /* ListPeerResponse { repeated PeerInfo peer_infos=1; NodeInfo my_info=2 }
     * PeerInfo { peer_id=1; repeated PeerConnInfo conns=2; ... }
     * PeerConnInfo { ... tunnel=5; stats=6; ... }
     * PeerConnStats { rx_bytes=1; tx_bytes=2; rx_packets=3; tx_packets=4;
     *                 latency_us=5 } */
    {
        vpnctl_pb_reader_t r;
        vpnctl_pb_field_t f;

        vpnctl_pb_reader_init(&r, resp, resp_len);
        while (vpnctl_pb_next(&r, &f) > 0) {
            vpnctl_pb_reader_t pi;
            uint64_t pid = 0;
            int i;

            if (f.field != 1 || f.wire_type != 2) {
                continue;
            }
            vpnctl_pb_reader_init(&pi, f.data, f.data_len);
            if (vpnctl_pb_get_uint(&pi, 1, &pid) <= 0) {
                continue;
            }

            for (i = 0; i < n_peers; i++) {
                vpnctl_pb_reader_t rr;
                vpnctl_pb_field_t cf;

                if (peers[i].peer_id != (uint32_t)pid) {
                    continue;
                }
                /* 找第一个 conn 的 stats 与 tunnel */
                vpnctl_pb_reader_init(&rr, f.data, f.data_len);
                while (vpnctl_pb_next(&rr, &cf) > 0) {
                    if (cf.field != 2 || cf.wire_type != 2) {
                        continue;
                    }
                    {
                        vpnctl_pb_reader_t ci;
                        vpnctl_pb_field_t c2;

                        vpnctl_pb_reader_init(&ci, cf.data, cf.data_len);
                        while (vpnctl_pb_next(&ci, &c2) > 0) {
                            if (c2.field == 6 && c2.wire_type == 2) {
                                vpnctl_pb_reader_t st;
                                uint64_t v;

                                vpnctl_pb_reader_init(&st, c2.data, c2.data_len);
                                if (vpnctl_pb_get_uint(&st, 1, &v) > 0) {
                                    peers[i].rx = v;
                                }
                                {
                                    vpnctl_pb_reader_t st2;

                                    vpnctl_pb_reader_init(&st2, c2.data,
                                                          c2.data_len);
                                    if (vpnctl_pb_get_uint(&st2, 2, &v) > 0) {
                                        peers[i].tx = v;
                                    }
                                }
                                {
                                    vpnctl_pb_reader_t st3;

                                    vpnctl_pb_reader_init(&st3, c2.data,
                                                          c2.data_len);
                                    if (vpnctl_pb_get_uint(&st3, 5, &v) > 0) {
                                        peers[i].latency_us = (int64_t)v;
                                    }
                                }
                            } else if (c2.field == 5 && c2.wire_type == 2) {
                                vpnctl_pb_reader_t ti;
                                const uint8_t *d = NULL;
                                size_t l = 0;

                                vpnctl_pb_reader_init(&ti, c2.data, c2.data_len);
                                if (vpnctl_pb_get_bytes(&ti, 1, &d, &l) > 0 &&
                                    l > 0) {
                                    size_t n = l < sizeof(peers[i].tunnel) - 1
                                                   ? l
                                                   : sizeof(peers[i].tunnel) - 1;

                                    memcpy(peers[i].tunnel, d, n);
                                    peers[i].tunnel[n] = '\0';
                                }
                            }
                        }
                    }
                    break; /* 只看第一个 conn */
                }
                break;
            }
        }
    }
    free(resp);
}

static void fmt_bytes(uint64_t v, char *dst, size_t dst_len) {
    if (v < 1024) {
        snprintf(dst, dst_len, "%llu B", (unsigned long long)v);
    } else if (v < 1024 * 1024) {
        snprintf(dst, dst_len, "%.2f kB", (double)v / 1024.0);
    } else {
        snprintf(dst, dst_len, "%.2f MB", (double)v / (1024.0 * 1024.0));
    }
}

static int cmd_peer(void) {
    et_peer_t peers[ET_MAX_PEERS];
    int n = 0;
    int ret;
    int i;

    memset(peers, 0, sizeof(peers));
    ret = fetch_routes(peers, &n);
    if (ret != 0) {
        return ret;
    }
    fetch_peer_stats(peers, n);

    if (vpnctl_json_mode()) {
        json_t *arr = json_array();

        for (i = 0; i < n; i++) {
            json_t *o = json_object();

            json_object_set_new(o, "ipv4", json_string(peers[i].ipv4));
            json_object_set_new(o, "hostname", json_string(peers[i].hostname));
            json_object_set_new(o, "version", json_string(peers[i].version));
            json_object_set_new(o, "rx_bytes",
                                json_integer((json_int_t)peers[i].rx));
            json_object_set_new(o, "tx_bytes",
                                json_integer((json_int_t)peers[i].tx));
            json_array_append_new(arr, o);
        }
        vpnctl_print_json(arr);
        json_decref(arr);
        return 0;
    }

    {
        vpnctl_table_t t;
        static const char *titles[] = {"ipv4",  "hostname", "cost", "lat(ms)",
                                       "rx",    "tx",       "tunnel",
                                       "nat",   "version"};

        vpnctl_table_init(&t, titles, 9);
        if (n == 0) {
            /* vpnctl_table_finish 在无数据行时会补表头，这里显式调一次，
             * 否则空结果看起来像"命令没跑" */
            vpnctl_table_finish(&t);
            return 0;
        }
        for (i = 0; i < n; i++) {
            char rx[32], tx[32], lat[32];
            const char *row[9];

            fmt_bytes(peers[i].rx, rx, sizeof(rx));
            fmt_bytes(peers[i].tx, tx, sizeof(tx));
            if (peers[i].latency_us > 0) {
                snprintf(lat, sizeof(lat), "%.2f",
                         (double)peers[i].latency_us / 1000.0);
            } else {
                snprintf(lat, sizeof(lat), "-");
            }

            /* 本机那行：peer_id 为 0 或 ipv4 与自身一致，官方标 Local */
            row[0] = peers[i].ipv4;
            row[1] = peers[i].hostname;
            row[2] = i == 0 ? "Local" : "p2p";
            row[3] = lat;
            row[4] = rx;
            row[5] = tx;
            row[6] = peers[i].tunnel[0] ? peers[i].tunnel : "-";
            row[7] = peers[i].nat[0] ? peers[i].nat : "Unknown";
            row[8] = peers[i].version[0] ? peers[i].version : "-";
            vpnctl_table_row(&t, row);
        }
        vpnctl_table_finish(&t);
    }
    return 0;
}

/* ---------------- node info ---------------- */

static int cmd_node_info(void) {
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    int ret = et_inst_request(7, "PeerManageRpc", &resp, &resp_len);

    if (ret != 0) {
        return ret;
    }

    /* ShowNodeInfoResponse { NodeInfo node_info = 1 } */
    {
        vpnctl_pb_reader_t r, ni;
        int found = 0;

        vpnctl_pb_reader_init(&r, resp, resp_len);
        found = vpnctl_pb_get_msg(&r, 1, &ni);
        if (found > 0) {
            char ipv4[64] = "", hostname[128] = "", version[32] = "";
            char inst_id[64] = "", nat[128] = "";
            char public_ip[512] = "";
            uint64_t peer_id = 0;
            char peer_id_str[32] = "";
            vpnctl_pb_field_t f;

            vpnctl_pb_reader_init(&ni, ni.buf, ni.len);
            while (vpnctl_pb_next(&ni, &f) > 0) {
                if (f.wire_type == 0 && f.field == N_PEER_ID) {
                    peer_id = f.varint;
                } else if (f.wire_type == 2 && f.field == N_IPV4_ADDR) {
                    /* NodeInfo.ipv4_addr 是 string，不是 Ipv4Inet */
                    size_t n = f.data_len < sizeof(ipv4) - 1
                                   ? f.data_len
                                   : sizeof(ipv4) - 1;

                    memcpy(ipv4, f.data, n);
                    ipv4[n] = '\0';
                } else if (f.wire_type == 2 && f.field == N_HOSTNAME) {
                    size_t n = f.data_len < sizeof(hostname) - 1
                                   ? f.data_len
                                   : sizeof(hostname) - 1;

                    memcpy(hostname, f.data, n);
                    hostname[n] = '\0';
                } else if (f.wire_type == 2 && f.field == N_VERSION) {
                    size_t n = f.data_len < sizeof(version) - 1
                                   ? f.data_len
                                   : sizeof(version) - 1;

                    memcpy(version, f.data, n);
                    version[n] = '\0';
                } else if (f.wire_type == 2 && f.field == N_INST_ID) {
                    size_t n = f.data_len < sizeof(inst_id) - 1
                                   ? f.data_len
                                   : sizeof(inst_id) - 1;

                    memcpy(inst_id, f.data, n);
                    inst_id[n] = '\0';
                } else if (f.wire_type == 2 && f.field == N_STUN_INFO) {
                    vpnctl_pb_reader_t si;
                    vpnctl_pb_field_t sf;
                    uint64_t udp_nat = 0, tcp_nat = 0;

                    /*
                     * proto3 里 0 值不序列化：本机 UDP NAT 探测失败时字段 1
                     * 整个缺失，只有字段 2(tcp) 和 4(public_ip)。
                     * 所以必须按"缺省即 Unknown"处理，不能只看 udp。
                     */
                    vpnctl_pb_reader_init(&si, f.data, f.data_len);
                    while (vpnctl_pb_next(&si, &sf) > 0) {
                        if (sf.field == SI_UDP_NAT && sf.wire_type == 0) {
                            udp_nat = sf.varint;
                        } else if (sf.field == SI_TCP_NAT && sf.wire_type == 0) {
                            tcp_nat = sf.varint;
                        } else if (sf.field == SI_PUBLIC_IP &&
                                   sf.wire_type == 2) {
                            size_t used = strlen(public_ip);
                            size_t n = sf.data_len;

                            if (used + n + 2 < sizeof(public_ip)) {
                                if (used > 0) {
                                    public_ip[used++] = ',';
                                }
                                memcpy(public_ip + used, sf.data, n);
                                public_ip[used + n] = '\0';
                            }
                        }
                    }
                    snprintf(nat, sizeof(nat), "udp=%s tcp=%s",
                             nat_type_name(udp_nat), nat_type_name(tcp_nat));
                }
            }
            snprintf(peer_id_str, sizeof(peer_id_str), "%u",
                     (unsigned)peer_id);

            if (vpnctl_json_mode()) {
                json_t *o = json_object();

                json_object_set_new(o, "virtual_ip", json_string(ipv4));
                json_object_set_new(o, "hostname", json_string(hostname));
                json_object_set_new(o, "peer_id",
                                    json_integer((json_int_t)peer_id));
                json_object_set_new(o, "inst_id", json_string(inst_id));
                json_object_set_new(o, "version", json_string(version));
                json_object_set_new(o, "nat_type", json_string(nat));
                json_object_set_new(o, "public_ip", json_string(public_ip));
                vpnctl_print_json(o);
                json_decref(o);
            } else {
                vpnctl_table_t t;
                static const char *titles[] = {"key", "value"};
                const char *r1[] = {"Virtual IP", ipv4};
                const char *r2[] = {"Hostname", hostname};
                const char *r3[] = {"Peer ID", peer_id_str};
                const char *r4[] = {"Instance ID", inst_id};
                const char *r5[] = {"Version", version};
                const char *r6[] = {"NAT Type", nat};
                const char *r7[] = {"Public IP", public_ip};

                vpnctl_set_no_trunc(true);
                vpnctl_table_init(&t, titles, 2);
                vpnctl_table_row(&t, r1);
                vpnctl_table_row(&t, r2);
                vpnctl_table_row(&t, r3);
                vpnctl_table_row(&t, r4);
                vpnctl_table_row(&t, r5);
                vpnctl_table_row(&t, r6);
                vpnctl_table_row(&t, r7);
                vpnctl_table_finish(&t);
            }
        }
    }

    free(resp);
    return 0;
}

/* ---------------- route ---------------- */

static int cmd_route(void) {
    et_peer_t peers[ET_MAX_PEERS];
    int n = 0;
    int ret = fetch_routes(peers, &n);
    int i;

    if (ret != 0) {
        return ret;
    }

    if (vpnctl_json_mode()) {
        json_t *arr = json_array();

        for (i = 0; i < n; i++) {
            json_t *o = json_object();

            json_object_set_new(o, "ipv4", json_string(peers[i].ipv4));
            json_object_set_new(o, "hostname", json_string(peers[i].hostname));
            json_object_set_new(o, "version", json_string(peers[i].version));
            json_array_append_new(arr, o);
        }
        vpnctl_print_json(arr);
        json_decref(arr);
        return 0;
    }

    {
        vpnctl_table_t t;
        static const char *titles[] = {"ipv4", "hostname", "proxy_cidrs",
                                       "next_hop_hostname", "path_len",
                                       "version"};
        vpnctl_table_init(&t, titles, 6);
        if (n == 0) {
            vpnctl_table_finish(&t);
            return 0;
        }
        for (i = 0; i < n; i++) {
            const char *row[6];

            row[0] = peers[i].ipv4;
            row[1] = peers[i].hostname;
            row[2] = "-";
            row[3] = i == 0 ? "Local" : "-";
            row[4] = i == 0 ? "0" : "1";
            row[5] = peers[i].version[0] ? peers[i].version : "-";
            vpnctl_table_row(&t, row);
        }
        vpnctl_table_finish(&t);
    }
    return 0;
}

/* ---------------- stun ---------------- */

static int cmd_stun(void) {
    /* easytier 的 STUN 结果在 NodeInfo.stun_info 里 */
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    int ret = et_inst_request(7, "PeerManageRpc", &resp, &resp_len);

    if (ret != 0) {
        return ret;
    }

    {
        vpnctl_pb_reader_t r, ni;

        vpnctl_pb_reader_init(&r, resp, resp_len);
        if (vpnctl_pb_get_msg(&r, 1, &ni) > 0) {
            vpnctl_pb_field_t f;
            char udp_nat[32] = "Unknown", tcp_nat[32] = "Unknown";
            char public_ips[512] = "";

            vpnctl_pb_reader_init(&ni, ni.buf, ni.len);
            while (vpnctl_pb_next(&ni, &f) > 0) {
                if (f.field == N_STUN_INFO && f.wire_type == 2) {
                    vpnctl_pb_reader_t si;
                    vpnctl_pb_field_t sf;

                    vpnctl_pb_reader_init(&si, f.data, f.data_len);
                    while (vpnctl_pb_next(&si, &sf) > 0) {
                        if (sf.field == SI_UDP_NAT && sf.wire_type == 0) {
                            snprintf(udp_nat, sizeof(udp_nat), "%s",
                                     nat_type_name(sf.varint));
                        } else if (sf.field == SI_TCP_NAT &&
                                   sf.wire_type == 0) {
                            snprintf(tcp_nat, sizeof(tcp_nat), "%s",
                                     nat_type_name(sf.varint));
                        } else if (sf.field == SI_PUBLIC_IP &&
                                   sf.wire_type == 2) {
                            size_t used = strlen(public_ips);
                            size_t n = sf.data_len;

                            if (used + n + 2 < sizeof(public_ips)) {
                                if (used > 0) {
                                    public_ips[used++] = ',';
                                }
                                memcpy(public_ips + used, sf.data, n);
                                public_ips[used + n] = '\0';
                            }
                        }
                    }
                }
            }

            if (vpnctl_json_mode()) {
                json_t *o = json_object();

                json_object_set_new(o, "udp_nat_type", json_string(udp_nat));
                json_object_set_new(o, "tcp_nat_type", json_string(tcp_nat));
                json_object_set_new(o, "public_ip", json_string(public_ips));
                vpnctl_print_json(o);
                json_decref(o);
            } else {
                vpnctl_table_t t;
                static const char *titles[] = {"item", "value"};
                const char *r1[] = {"UDP NAT Type", udp_nat};
                const char *r2[] = {"TCP NAT Type", tcp_nat};
                const char *r3[] = {"Public IPv4", public_ips};

                vpnctl_set_no_trunc(true);
                vpnctl_table_init(&t, titles, 2);
                vpnctl_table_row(&t, r1);
                vpnctl_table_row(&t, r2);
                vpnctl_table_row(&t, r3);
                vpnctl_table_finish(&t);
            }
        }
    }

    free(resp);
    return 0;
}

/* ---------------- 入口 ---------------- */

static void usage(void) {
    printf("Usage: " PROG " <command> [args]\n\n"
           "Commands:\n"
           "  peer           show peers info\n"
           "  route          show route info\n"
           "  node info      inspect self easytier-core status\n"
           "  stun           show STUN / NAT info\n"
           "  version        print version\n\n"
           "Options:\n"
           "  -p, --rpc-portal <ip:port>  RPC endpoint (default %s:%d)\n"
           "                              pass the same value as easytier-core's --rpc-portal)\n"
           "\n"
           "Global flags:\n"
           "  --json         output JSON instead of a table\n"
           "  --no-trunc     do not truncate table columns\n"
           "\n"
           "Note: easytier-core must be started with --rpc-portal <ip:port>;\n"
           "      the rpc_portal key in TOML is ignored (verified on device).\n",
           ET_RPC_HOST, (int)ET_RPC_PORT);
}

int main(int argc, char **argv) {
    static const char *const cmds[] = {"peer", "route", "node",
                                       "stun", "version", "help"};
    int i, idx, rc;

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--rpc-portal") == 0)
            && i + 1 < argc) {
            const char *v = argv[++i];
            const char *colon = strrchr(v, ':');

            if (colon) {
                size_t hl = (size_t)(colon - v);
                if (hl >= sizeof(g_rpc_host)) {
                    hl = sizeof(g_rpc_host) - 1;
                }
                memcpy(g_rpc_host, v, hl);
                g_rpc_host[hl] = '\0';
                g_rpc_port = atoi(colon + 1);
            } else {
                g_rpc_port = atoi(v);
            }
            memmove(&argv[i - 1], &argv[i + 1],
                    sizeof(char *) * (size_t)(argc - i - 1));
            argc -= 2;
            i -= 2;
        } else if (strcmp(argv[i], "--json") == 0) {
            vpnctl_set_json_mode(true);
            memmove(&argv[i], &argv[i + 1],
                    sizeof(char *) * (size_t)(argc - i - 1));
            argc--;
            i--;
        } else if (strcmp(argv[i], "--no-trunc") == 0) {
            vpnctl_set_no_trunc(true);
            memmove(&argv[i], &argv[i + 1],
                    sizeof(char *) * (size_t)(argc - i - 1));
            argc--;
            i--;
        }
    }

    srand((unsigned)getpid());

    idx = vpnctl_match_cmd(argc, argv, cmds,
                           (int)(sizeof(cmds) / sizeof(cmds[0])));
    if (idx < 0) {
        usage();
        return 1;
    }

    switch (idx) {
    case 0:
        rc = cmd_peer();
        break;
    case 1:
        rc = cmd_route();
        break;
    case 2:
        rc = cmd_node_info();
        break;
    case 3:
        rc = cmd_stun();
        break;
    default:
        if (idx == 4) {
            printf(PROG " %s (vpnctl)\n", ET_VERSION);
        } else {
            usage();
        }
        rc = 0;
        break;
    }
    return rc == 0 ? 0 : 1;
}
