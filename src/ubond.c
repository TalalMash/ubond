/*
 * Copyright (c) 2018, Mark Burton <mark@helenandmark.org>
 * Copyright (c) 2015, Laurent COUSTET <ed@zehome.com>
 *
 * All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS AND CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ev.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>

#include "includes.h"
#include "setproctitle.h"
#include "socks.h"
#include "tool.h"
#include "ubond.h"
#ifdef ENABLE_CONTROL
#include "control.h"
#endif
#include "tuntap_generic.h"

/* Linux specific things */
#ifdef HAVE_LINUX
#include "systemd.h"
#include <sys/prctl.h>
#endif

/* GLOBALS */
struct tuntap_s tuntap;
char* _progname;
static char** saved_argv;
struct ev_loop* loop;
char* status_command = NULL;
char* process_title = NULL;
int logdebug = 0;

uint64_t bandwidthdata = 0;
double bandwidth = 0;
uint64_t out_resends = 0;

double srtt_min = 0;
float max_size_outoforder = 20;

ubond_pkt_list_t pool;
uint64_t pool_out = 0;
ubond_pkt_t* ubond_pkt_get()
{
    ubond_pkt_t* p;
    if (!UBOND_TAILQ_EMPTY(&pool)) {
        p = UBOND_TAILQ_FIRST(&pool);
        UBOND_TAILQ_REMOVE(&pool, p);
    } else {
        p = malloc(sizeof(struct ubond_pkt_t));
    }

    p->stream = NULL;
    p->sent_tun = NULL;

    pool_out++;
    return p;
};
void ubond_pkt_release(ubond_pkt_t* p)
{
    if (p->stream) {
        log_warnx("PKT", "Packet has stream on release?");
    }
    if (p->sent_tun) {
        log_warnx("PKT", "Packet has sent_tun on release?");
    }
    pool_out--;
    UBOND_TAILQ_INSERT_HEAD(&pool, p);
}
void ubond_pkt_insert(ubond_pkt_list_t* list, ubond_pkt_t* pkt)
{
    if (list->length >= list->max_size) {
        log_warnx("lists", "buffer overflow");
    }
    UBOND_TAILQ_INSERT_HEAD(list, pkt);
}
int ubond_pkt_list_is_full(ubond_pkt_list_t* list)
{
    return (list->length >= list->max_size);
}
void ubond_pkt_list_init(ubond_pkt_list_t* list, uint64_t size)
{
    UBOND_TAILQ_INIT(list);
    list->max_size = size;
}

#define LOSS_TOLERENCE 50.0
#define BANDWIDTHCALCTIME 0.1
#define INVERSEBWCALCTIME 10
static ev_timer bandwidth_calc_timer;

ubond_pkt_list_t send_buffer; /* send buffer */
ubond_pkt_list_t hpsend_buffer; /* send buffer */

void ubond_buffer_write(ubond_pkt_list_t* buffer, ubond_pkt_t* p)
{
    if (p) {
        // record the eventual wire length needed
        bandwidthdata += p->p.len + IP4_UDP_OVERHEAD + PKTHDRSIZ(p->p);
        UBOND_TAILQ_INSERT_HEAD(buffer, p);
    }
}

struct resend_data {
    char r, s;
    uint16_t seqn;
    uint16_t tun_id;
    uint16_t len;
};

struct ubond_status_s ubond_status = {
    .start_time = 0,
    .last_reload = 0,
    .fallback_mode = 0,
    .connected = 0,
    .initialized = 0
};
struct ubond_options_s ubond_options = {
    .change_process_title = 1,
    .process_name = "ubond",
    .control_unix_path = "",
    .control_bind_host = "",
    .control_bind_port = "",
    .ip4 = "",
    .ip6 = "",
    .ip4_gateway = "",
    .ip6_gateway = "",
    .ip4_routes = "",
    .ip6_routes = "",
    .mtu = 0,
    .config_path = "ubond.conf",
    .config_fd = -1,
    .debug = 0,
    .verbose = 0,
    .unpriv_user = "ubond",
    .password = "password",
    .static_tunnel = 0,
    .root_allowed = 0,
};
#ifdef HAVE_FILTERS
struct ubond_filters_s ubond_filters = {
    .count = 0
};
#endif

static char* optstr = "c:n:u:hvVD:p:";
static struct option long_options[] = {
    { "config", required_argument, 0, 'c' },
    { "debug", no_argument, 0, 2 },
    { "name", required_argument, 0, 'n' },
    { "natural-title", no_argument, 0, 1 },
    { "help", no_argument, 0, 'h' },
    { "user", required_argument, 0, 'u' },
    { "verbose", no_argument, 0, 'v' },
    { "quiet", no_argument, 0, 'q' },
    { "version", no_argument, 0, 'V' },
    { "yes-run-as-root", no_argument, 0, 3 },
    { "permitted", required_argument, 0, 'p' },
    { 0, 0, 0, 0 }
};

static int ubond_rtun_start(ubond_tunnel_t* t);
static void ubond_rtun_read(EV_P_ ev_io* w, int revents);
static void ubond_rtun_write(EV_P_ ev_io* w, int revents);
static void ubond_rtun_write_timeout(EV_P_ ev_timer* w, int revents);
static void ubond_rtun_check_timeout(EV_P_ ev_timer* w, int revents);
static void ubond_rtun_write_check(EV_P_ ev_check* w, int revents);
static void ubond_rtun_send_keepalive(ev_tstamp now, ubond_tunnel_t* t);
static void ubond_rtun_send_disconnect(ubond_tunnel_t* t);
static int ubond_rtun_send(ubond_tunnel_t* tun, ubond_pkt_t* pkt);
static void ubond_rtun_resend(struct resend_data* d);
static void ubond_rtun_request_resend(ubond_tunnel_t* loss_tun, uint16_t tun_seqn, uint16_t len);
static void ubond_rtun_send_auth(ubond_tunnel_t* t);
static void ubond_rtun_tuntap_up();
static void ubond_rtun_status_up(ubond_tunnel_t* t);
static void ubond_rtun_tick_connect(ubond_tunnel_t* t);
static void ubond_rtun_recalc_weight();
static void ubond_update_status();
static int ubond_rtun_bind(ubond_tunnel_t* t);
static void update_process_title();
static void ubond_tuntap_init();
static void ubond_rtun_choose(ubond_tunnel_t* rtun);
static void ubond_rtun_check_lossy(ubond_tunnel_t* tun);
static int
ubond_protocol_read(ubond_tunnel_t* tun,
    ubond_pkt_t* pkt);

static void
usage(char** argv)
{
    fprintf(stderr,
        "usage: %s [options]\n\n"
        "Options:\n"
        " -c, --config [path]   path to config file (ex. /etc/ubond.conf)\n"
        " --debug               don't use syslog, print to stdout\n"
        " --natural-title       do not change process title\n"
        " -n, --name            change process-title and include 'name'\n"
        " -h, --help            this help\n"
        " -u, --user [username] drop privileges to user 'username'\n"
        " --yes-run-as-root     ! please do not use !\n"
        " -v --verbose          increase verbosity\n"
        " -q --quiet            decrease verbosity\n"
        " -V, --version         output version information and exit\n"
        " -p, --permitted <tunnel>:<value>[bkm]      Preset tunnel initial permitted bandwidth (Bytes - Default,Kbytes or Mbytes)\n"
        "\n"
        "For more details see ubond(1) and ubond.conf(5).\n",
        argv[0]);
    exit(2);
}

void preset_permitted(int argc, char** argv)
{
    ubond_tunnel_t* t;
    char tunname[21];
    uint64_t val = 0;
    int c;
    char mag = 0;
    int filled, option_index;
    optind = 0;
    while (1) {
        c = getopt_long(argc, argv, optstr, long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
        case 'p':
            filled = sscanf(optarg, "%20[^:]:%lu%c", tunname, &val, &mag);
            if (filled < 2) {
                usage(argv);
            }
            if (filled == 3) {
                switch (mag) {
                default:
                    usage(argv);
                case 'm':
                    val *= 1000;
                case 'k':
                    val *= 1000;
                case 'b':
                    break;
                }
            }
            int found = 0;
            LIST_FOREACH(t, &rtuns, entries)
            {
                if (strcmp(t->name, tunname) == 0 && t->quota) {
                    t->permitted = val;
                    found++;
                }
            }
            if (!found)
                usage(argv);
        }
    }
}
static void
ubond_reset_perm(EV_P_ ev_signal* w, int revents)
{
    ubond_tunnel_t* t;
    LIST_FOREACH(t, &rtuns, entries)
    {
        if (t->quota) {
            log_info("quota", "%s quota reset to 0\n", t->name);
            t->permitted = 0;
        }
    }
}

int ubond_sock_set_nonblocking(int fd)
{
    int ret = 0;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) {
        log_warn(NULL, "fcntl");
        ret = -1;
    } else {
        fl |= O_NONBLOCK;
        if ((ret = fcntl(fd, F_SETFL, fl)) < 0)
            log_warn(NULL, "Unable to set socket %d non blocking",
                fd);
    }
    return ret;
}

inline static void ubond_rtun_tick(ubond_tunnel_t* tun)
{
    tun->last_activity = ev_now(EV_DEFAULT_UC);
}

/* Inject the packet to the tuntap device (real network) */
void ubond_rtun_inject_tuntap(ubond_pkt_t* pkt)
{
    ubond_tuntap_write(&tuntap, pkt);

    //UBOND_TAILQ_INSERT_HEAD(&tuntap.sbuf, pkt);
    ///* Send the packet back into the LAN */
    //if (!ev_is_active(&tuntap.io_write)) {
    //    ev_io_start(EV_A_ & tuntap.io_write);
    // }
}

inline int count_1s(uint64_t b)
{
    b -= ((b >> 1) & 0x5555555555555555ULL);
    b = ((b >> 2) & 0x3333333333333333ULL) + (b & 0x3333333333333333ULL);
    b = ((b >> 4) + b) & 0x0F0F0F0F0F0F0F0FULL;
    b *= 0x0101010101010101ULL;
    return (int)(b >> 56);
}

/* Count the loss on the last 64 packets */
static void
ubond_loss_update(ubond_tunnel_t* tun, uint16_t seq)
{
    if (seq >= tun->seq_last + 64) {
        /* consider a connection reset. */
        tun->seq_vect = (uint64_t)-1;
        tun->seq_last = seq;
        tun->loss = 0;
        return;
    }
    if (seq > tun->seq_last) {
        tun->seq_vect <<= seq - tun->seq_last;
        tun->seq_vect |= 1ull;
    } else {
        tun->seq_vect |= 1ull << (tun->seq_last - seq);
    }
    // according to RFC 3208 you can target the last two packets being out of order
    int64_t v = tun->seq_vect | 0x8000000000000000ULL; // signed int.
    tun->loss = 64 - count_1s(v >> 2);
    tun->seq_last = seq;

    if (tun->seq_vect & 0x8) // if this isn't set, we suspect a loss.
    {
        ubond_rtun_request_resend(tun, tun->seq_last - 3, 1);
    }
}

/* read from the rtunnel => write directly to the tap send buffer */
static void
ubond_rtun_read(EV_P_ ev_io* w, int revents)
{
    ubond_tunnel_t* tun = w->data;
    ssize_t len;
    struct sockaddr_storage clientaddr;
    socklen_t addrlen = sizeof(clientaddr);
    do {
        ubond_pkt_t* pkt = ubond_pkt_get();
        len = recvfrom(tun->fd, &(pkt->p),
            sizeof(pkt->p),
            MSG_DONTWAIT, (struct sockaddr*)&clientaddr, &addrlen);
        if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                log_warn("net", "%s read error", tun->name);
                ubond_rtun_status_down(tun);
            }
            ubond_pkt_release(pkt);
            break;
        }
        if (len == 0) {
            log_info("protocol", "%s peer closed the connection", tun->name);
            ubond_pkt_release(pkt);
            break;
        }
        betoh_proto(&(pkt->p));

        pkt->len = len; // stamp the wire length

        /* validate the received packet */
        if (ubond_protocol_read(tun, pkt) < 0) {
            ubond_pkt_release(pkt);
            //            break;
            //            return;
        }

        tun->recvbytes += len;
        tun->recvpackets += 1;
        tun->bm_data += pkt->p.len;
        if (tun->quota) {
            if (tun->permitted > (len + PKTHDRSIZ(pkt->p) + IP4_UDP_OVERHEAD)) {
                tun->permitted -= (len + PKTHDRSIZ(pkt->p) + IP4_UDP_OVERHEAD);
            } else {
                tun->permitted = 0;
            }
        }

        if (!tun->addrinfo)
            fatalx("tun->addrinfo is NULL!");

        if ((tun->addrinfo->ai_addrlen != addrlen) || (memcmp(tun->addrinfo->ai_addr, &clientaddr, addrlen) != 0)) {
            if (tun->status >= UBOND_AUTHOK) {
                log_warnx("protocol", "%s rejected non authenticated connection",
                    tun->name);
                ubond_rtun_status_down(tun);
                ubond_pkt_release(pkt);
                return;
            }
            char clienthost[NI_MAXHOST];
            char clientport[NI_MAXSERV];
            int ret;
            if ((ret = getnameinfo((struct sockaddr*)&clientaddr, addrlen,
                     clienthost, sizeof(clienthost),
                     clientport, sizeof(clientport),
                     NI_NUMERICHOST | NI_NUMERICSERV))
                < 0) {
                log_warn("protocol", "%s error in getnameinfo: %d",
                    tun->name, ret);
            } else {
                log_info("protocol", "%s new connection -> %s:%s",
                    tun->name, clienthost, clientport);
                memcpy(tun->addrinfo->ai_addr, &clientaddr, addrlen);
            }
        }
        log_debug("net", "< %s recv %d bytes (size=%d, type=%d, seq=%d)",
            tun->name, (int)len, pkt->p.len, pkt->p.type, pkt->p.tun_seq);

        if (tun->status >= UBOND_AUTHOK) {
            switch (pkt->p.type) {
            case UBOND_PKT_DATA:
            case UBOND_PKT_DATA_RESEND:
                ubond_rtun_tick(tun);
                ubond_reorder_insert(tun, pkt);
                break;
            case UBOND_PKT_KEEPALIVE:
                log_debug("protocol", "%s keepalive received", tun->name);
                ubond_rtun_tick(tun);
                uint64_t bw = 0;
                sscanf(pkt->p.data, "%lu", &bw);
                if (bw > 0) {
                    tun->bandwidth_out = bw;
                }
                ubond_pkt_release(pkt);
                break;
            case UBOND_PKT_DISCONNECT:
                log_info("protocol", "%s disconnect received", tun->name);
                ubond_rtun_status_down(tun);
                ubond_pkt_release(pkt);
                break;
            case UBOND_PKT_RESEND:
                ubond_rtun_tick(tun);
                ubond_rtun_resend((struct resend_data*)pkt->p.data);
                ubond_pkt_release(pkt);
                break;
            case UBOND_PKT_TCP_OPEN:
                ubond_rtun_tick(tun);
                ubond_socks_init(pkt);
                ubond_pkt_release(pkt);
                break;
            case UBOND_PKT_TCP_CLOSE:
            case UBOND_PKT_TCP_DATA:
                ubond_rtun_tick(tun);
                ubond_stream_write(pkt);
                break;
            case UBOND_PKT_AUTH:
                ubond_rtun_send_auth(tun);
            case UBOND_PKT_AUTH_OK:
                break;
            default:
                log_warnx("protocol", "Unknown packet type %d", pkt->p.type);
                ubond_pkt_release(pkt);
            }

        } else {
            if (pkt->p.type == UBOND_PKT_AUTH || pkt->p.type == UBOND_PKT_AUTH_OK) {
                // recieve any quota info, if there is any
                ubond_rtun_tick(tun);

                ubond_pkt_challenge* challenge = (ubond_pkt_challenge*)(pkt->p.data);
                challenge->version = be16toh(challenge->version);
                challenge->permitted = be64toh(challenge->permitted);

                if (challenge->version != UBOND_PROTOCOL_VERSION) {
                    fatalx("Protocol version must match");
                }
                if (strcmp(challenge->password, ubond_options.password) != 0) {
                    log_warnx("password", "Invalid password");
                } else {
                    int64_t perm = challenge->permitted;
                    if (perm > tun->permitted)
                        tun->permitted = perm;

                    ubond_rtun_send_auth(tun);
                }
            } else {
                log_debug("protocol", "%s ignoring non authenticated packet",
                    tun->name);
                ubond_pkt_release(pkt);
            }
        }
    } while (1);
}

static int
ubond_protocol_read(ubond_tunnel_t* tun, ubond_pkt_t* pkt)
{
    ubond_proto_t* proto = &pkt->p;
    int ret;
    uint64_t now64 = ubond_timestamp64(ev_now(EV_DEFAULT_UC));

    tun->pkts_cnt++;

    /* Overkill */
    /* pkt->data contains ubond_proto_t struct */
    if (pkt->len > sizeof(*pkt) || pkt->len < (PKTHDRSIZ(pkt->p))) {
        log_warnx("protocol", "%s received invalid packet of %d bytes",
            tun->name, pkt->len);
        goto fail;
    }

    if (/*rlen == 0 ||*/ proto->len > sizeof(proto->data)) {
        log_warnx("protocol", "%s invalid packet size: %d", tun->name, proto->len);
        goto fail;
    }

    ubond_loss_update(tun, proto->tun_seq);
    // use the TUN seq number to
    // calculate loss

    tun->sent_loss = proto->sent_loss;
    if (tun->sent_loss >= (LOSS_TOLERENCE / 4.0)) {
        ubond_rtun_recalc_weight();
    }

    if (proto->timestamp != (uint16_t)-1) {
        tun->saved_timestamp = proto->timestamp;
        tun->saved_timestamp_received_at = now64;
    }
    if (proto->timestamp_reply != (uint16_t)-1) {
        uint16_t now16 = ubond_timestamp16(now64);
        uint16_t R = ubond_timestamp16_diff(now16, proto->timestamp_reply);
        if (R < 5000) { /* ignore large values, or
                                * reordered packets */
            tun->srtt_d += R;
            tun->srtt_c++;
        }
        //        log_debug("rtt", "%ums srtt %ums loss ratio: %d",
        //            (unsigned int)R, (unsigned int)R, ubond_loss_ratio(tun));
    }
    return 0;
fail:
    return -1;
}

int is_tcp(ubond_pkt_t* pkt)
{
    // should packet inspect, and only re-order TCP packets !
    // 17 - UDP
    // 6 - TCP
    if ((pkt->p.type == UBOND_PKT_DATA || pkt->p.type == UBOND_PKT_DATA_RESEND) && pkt->p.data[9] == 6) {
        return 1;
    } else {
        return 0;
    }
}

static int
ubond_rtun_send(ubond_tunnel_t* tun, ubond_pkt_t* pkt)
{
    ssize_t ret;
    size_t wlen;
    ubond_proto_t* proto = &(pkt->p);

    wlen = PKTHDRSIZ(pkt->p) + pkt->p.len;

    // old_pkts is a ring buffer of the last N packets.
    // The packets may be still held by the stream.
    if (tun->old_pkts[tun->seq % RESENDBUFSIZE]) {
        if (!tun->old_pkts[tun->seq % RESENDBUFSIZE]->stream) {
            ubond_pkt_release(tun->old_pkts[tun->seq % RESENDBUFSIZE]);
        }
    }
    tun->old_pkts[tun->seq % RESENDBUFSIZE] = pkt;
    pkt->sent_tun = tun;

    // we should still use this to measure packet loss even if they are UDP packets
    // tun seq incrememts even if we resend
    proto->tun_seq = tun->seq;

    tun->seq++; // ALL packets are stored in the resend old_pkts buffer, even if
    // they fail to send.

    //proto->flow_id = tun->flow_id; this is now handled by socks
    proto->sent_loss = tun->loss;

    pkt->len = wlen;

    // significant time can have elapsed, so maybe better use the current time
    // rather than... uint64_t now64 = ubond_timestamp64(ev_now(EV_DEFAULT_UC));
    uint64_t now64 = ubond_timestamp64(ev_time());
    /* we have a recent received timestamp */
    if (tun->saved_timestamp != -1) {
        if (now64 - tun->saved_timestamp_received_at < 1000) {
            /* send "corrected" timestamp advanced by how long we held it */
            /* Cast to uint16_t there intentional */
            proto->timestamp_reply = ubond_timestamp16(tun->saved_timestamp + (now64 - tun->saved_timestamp_received_at));
            tun->saved_timestamp = -1;
            tun->saved_timestamp_received_at = 0;
        } else {
            proto->timestamp_reply = -1;
            tun->saved_timestamp = -1;
            tun->saved_timestamp_received_at = 0;
            log_debug("rtt", "(%s) No timestamp added, time too long! (%lu > 1000)", tun->name, tun->saved_timestamp + (now64 - tun->saved_timestamp_received_at));
        }
    } else {
        proto->timestamp_reply = -1;
        //      log_debug("rtt","(%s) No timestamp available!",tun->name);
    }

    proto->timestamp = ubond_timestamp16(now64);

    htobe_proto(proto);
    ret = sendto(tun->fd, proto, wlen, MSG_DONTWAIT,
        tun->addrinfo->ai_addr, tun->addrinfo->ai_addrlen);
    betoh_proto(proto);

    if (ret < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            if (pkt->p.type != UBOND_PKT_AUTH) {
                log_warnx("net", "%s write error", tun->name);
                ubond_rtun_status_down(tun);
            } // dont report AUTH packet loss, as we know that !
        } else {
            // we should never attempt a send on a blockable tunnel, so we should
            // nevr get here...
            log_warnx("net", "%s lost write!", tun->name);
            ubond_rtun_status_down(tun);
        }
    } else {
        // we are here when we succeed to send the packet

        tun->sentpackets++;
        tun->sentbytes += ret;
        if (tun->quota) {
            if (tun->permitted > (ret + PKTHDRSIZ(pkt->p) + IP4_UDP_OVERHEAD)) {
                tun->permitted -= (ret + PKTHDRSIZ(pkt->p) + IP4_UDP_OVERHEAD);
            } else {
                tun->permitted = 0;
            }
        }

        // inform the stream
        if (pkt->stream)
            tcp_sent(pkt->stream, pkt);

        if (wlen != ret) {
            log_warnx("net", "%s write error %d/%u",
                tun->name, (int)ret, (unsigned int)wlen);
        } else {
            log_debug("net", "> %s sent %d bytes (size=%d, type=%d, seq=%d)", tun->name, (int)ret, (pkt->p.len), pkt->p.type, (pkt->p.tun_seq));
        }
    }

    return ret;
}

static void
ubond_rtun_do_send(ubond_tunnel_t* tun, int timed)
{
    ev_tstamp now = ev_now(EV_DEFAULT_UC);
    ev_tstamp diff = now - tun->last_adjust;

    int len = 0;
    // if there is hp stuff for us - SEND IT !
    double b = tun->bytes_per_sec * diff;

    if (tun->busy_writing)
        return;

    if (timed || (double)(tun->bytes_since_adjust) < b) {
#ifdef USEIDLELOOP
        if (ev_is_active(&tun->check_ev)) {
            ev_check_stop(EV_A_ & tun->check_ev);
            ev_idle_stop(EV_A_ & tun->idle_ev);
        }
#endif
        if (!UBOND_TAILQ_EMPTY(&tun->hpsbuf)) {
            ubond_pkt_t* pkt = UBOND_TAILQ_POP_LAST(&tun->hpsbuf);
            len = ubond_rtun_send(tun, pkt);
        } else {
            ubond_rtun_choose(tun); //EV_P_ ev_timer *w, int revents);
            if (!UBOND_TAILQ_EMPTY(&tun->sbuf)) {
                ubond_pkt_t* pkt = UBOND_TAILQ_POP_LAST(&tun->sbuf);
                len = ubond_rtun_send(tun, pkt);
            } else { // nothing sent, so disable the write events
                if (ev_is_active(&tun->io_write)) {
                    ev_io_stop(EV_A_ & tun->io_write);
                }
            }
        }
        if (len > 0) {
            // len + the UDP  overhead ??
            tun->bytes_since_adjust += len + IP4_UDP_OVERHEAD;
            tun->busy_writing++; // semaphore that we're busy
            if (!ev_is_active(&tun->io_write)) {
                ev_io_start(EV_A_ & tun->io_write);
            }
            tun->send_timer.repeat = (double)(len + IP4_UDP_OVERHEAD) / tun->bytes_per_sec;
        }
    } else {
// we're too soon, use a checker to wait for the right time
//double tte=(tun->bytes_since_adjust - b)/(tun->bandwidth_max * 128);
//printf("wait %s %lld %f target %f bytes/s max %lld kbits time diff %f (%f to early)\n", tun->name, tun->bytes_since_adjust, b ,tun->bytes_per_sec , tun->bandwidth_max, diff, tte);
#ifdef USEIDLELOOP
        if (!ev_is_active(&tun->check_ev)) {
            ev_check_start(EV_A_ & tun->check_ev);
            ev_idle_start(EV_A_ & tun->idle_ev);
        }
#endif
    }
}
static void
ubond_rtun_write(EV_P_ ev_io* w, int revents)
{
    ubond_tunnel_t* tun = w->data;
    if (tun->busy_writing) {
        tun->busy_writing--;
    }
    ubond_rtun_do_send(tun, 0);
}

static void
ubond_rtun_write_timeout(EV_P_ ev_timer* w, int revents)
{
    ubond_tunnel_t* tun = w->data;
    if (!tun->busy_writing)
        ubond_rtun_do_send(tun, 1);
}

static void
ubond_rtun_write_check(EV_P_ ev_check* w, int revents)
{
    ubond_tunnel_t* tun = w->data;
    if (!tun->busy_writing)
        ubond_rtun_do_send(tun, 0);
}

ubond_tunnel_t*
ubond_rtun_new(const char* name,
    const char* bindaddr, const char* bindport, const char* binddev, uint32_t bindfib,
    const char* destaddr, const char* destport,
    int server_mode, uint32_t timeout,
    int fallback_only, uint32_t bandwidth_max,
    uint32_t quota)
{
    ubond_tunnel_t* new;

    /* Some basic checks */
    if (server_mode) {
        if (bindport == NULL) {
            log_warnx(NULL,
                "cannot initialize socket without bindport");
            return NULL;
        }
    } else {
        if (destaddr == NULL || destport == NULL) {
            log_warnx(NULL,
                "cannot initialize socket without destaddr or destport");
            return NULL;
        }
    }

    new = (ubond_tunnel_t*)calloc(1, sizeof(ubond_tunnel_t));
    if (!new)
        fatal(NULL, "calloc failed");
    /* other values are enforced by calloc to 0/NULL */
    new->name = strdup(name);
    new->fd = -1;
    new->server_mode = server_mode;
    new->weight = 1;
    new->status = UBOND_DISCONNECTED;
    new->addrinfo = NULL;
    new->sentpackets = 0;
    new->sentbytes = 0;
    new->recvbytes = 0;
    new->permitted = 0;
    new->quota = quota;
    new->seq = 0;
    new->saved_timestamp = -1;
    new->saved_timestamp_received_at = 0;
    new->srtt = 40;
    new->srtt_av = 40;
    new->srtt_d = 0;
    new->srtt_c = 0;
    new->srtt_min = 0;
    new->srtt_reductions = 0;
    new->seq_last = 0;
    new->seq_vect = (uint64_t)-1;
    new->loss = 0;
    new->flow_id = 0;
    if (bandwidth_max == 0) {
        log_warnx("config",
            "Enabling automatic bandwidth adjustment");
        bandwidth_max = 10000; // faster lines will go up faster from 10000, slower
        // ones will drop from here.... it's a compromise
    }
    new->bandwidth_max = bandwidth_max;
    new->bandwidth_measured = 0;
    new->bm_data = 0;
    new->fallback_only = fallback_only;
    if (bindaddr)
        strlcpy(new->bindaddr, bindaddr, sizeof(new->bindaddr));
    if (bindport)
        strlcpy(new->bindport, bindport, sizeof(new->bindport));
    new->bindfib = bindfib;
    if (binddev) {
        strlcpy(new->binddev, binddev, sizeof(new->binddev));
    }
    if (destaddr)
        strlcpy(new->destaddr, destaddr, sizeof(new->destaddr));
    if (destport)
        strlcpy(new->destport, destport, sizeof(new->destport));
    ubond_pkt_list_init(&new->sbuf, PKTBUFSIZE);
    ubond_pkt_list_init(&new->hpsbuf, PKTBUFSIZE);
    ubond_rtun_tick(new);
    new->timeout = timeout;
    LIST_INSERT_HEAD(&rtuns, new, entries);
    new->io_read.data = new;
    new->io_write.data = new;
    new->io_timeout.data = new;
    ev_init(&new->io_read, ubond_rtun_read);
    ev_init(&new->io_write, ubond_rtun_write);
    ev_timer_init(&new->io_timeout, ubond_rtun_check_timeout,
        0., UBOND_IO_TIMEOUT_DEFAULT);
    ev_timer_start(EV_A_ & new->io_timeout);
    new->check_ev.data = new;
    ev_check_init(&new->check_ev, ubond_rtun_write_check);
#ifdef USEIDLELOOP
    new->idle_ev.data = new;
    ev_idle_init(&new->idle_ev, ubond_rtun_write_check);
#endif
    new->send_timer.data = new;
    ev_timer_init(&new->send_timer, &ubond_rtun_write_timeout, 0., 0.01);
    ev_timer_start(EV_A_ & new->send_timer);

    new->last_adjust = ev_now(EV_DEFAULT_UC);
    new->bytes_since_adjust = 0;
    new->bytes_per_sec = 0;
    new->busy_writing = 0;
    new->lossless = 0;

    memset(&new->old_pkts, 0, sizeof(new->old_pkts));
    update_process_title();
    return new;
}

void ubond_rtun_drop(ubond_tunnel_t* t)
{
    ubond_tunnel_t* tmp;
    ubond_rtun_send_disconnect(t);
    ubond_rtun_status_down(t);
    ev_timer_stop(EV_A_ & t->io_timeout);
    ev_io_stop(EV_A_ & t->io_read);

    LIST_FOREACH(tmp, &rtuns, entries)
    {
        if (mystr_eq(tmp->name, t->name)) {
            LIST_REMOVE(tmp, entries);
            if (tmp->name)
                free(tmp->name);
            if (tmp->addrinfo)
                freeaddrinfo(tmp->addrinfo);
            while (!UBOND_TAILQ_EMPTY(&tmp->sbuf)) {
                ubond_pkt_release(UBOND_TAILQ_POP_LAST(&tmp->sbuf));
            }
            while (!UBOND_TAILQ_EMPTY(&tmp->hpsbuf)) {
                ubond_pkt_release(UBOND_TAILQ_POP_LAST(&tmp->hpsbuf));
            }
            /* Safety */
            tmp->name = NULL;
            break;
        }
    }
    update_process_title();
}

/* Based on tunnel bandwidth, with priority compute a "weight" value
 * to balance correctly the round robin rtun_choose.
 */
static void
ubond_rtun_recalc_weight()
{
    ubond_tunnel_t* t;
    double bwneeded = bandwidth * 2;
    if (bwneeded < 1000)
        bwneeded = 1000;
    double bwavailable = 0;

    // reset all tunnels
    int tuns = 0;
    double total = 0;
    LIST_FOREACH(t, &rtuns, entries)
    {
        if ((t->quota == 0 || t->permitted > (t->bandwidth_max * 128 * BANDWIDTHCALCTIME)) && (t->status == UBOND_AUTHOK) && ubond_status.fallback_mode == t->fallback_only) {
            t->weight = bwneeded / 50;
            total += t->bandwidth_max;
        } else {
            t->weight = 0;
        }
        tuns++;
    }
    if (bwneeded < total / 4) {
        bwneeded = total / 4;
    }
    if (send_buffer.length > tuns * 2)
        bwneeded = total;

    LIST_FOREACH(t, &rtuns, entries)
    {
        if (t->status == UBOND_AUTHOK && ubond_status.fallback_mode == t->fallback_only) {
            if ((t->quota == 0) || (t->permitted > (t->bandwidth_max * 128 * BANDWIDTHCALCTIME))) {

                double part = 1;
                double lt = LOSS_TOLERENCE / 2.0;
                if (t->sent_loss >= lt) {
                    part = 1.0 - (((double)t->sent_loss - lt) / lt);
                    if (part <= 0.2) {
                        part = 0.2;
                        t->srtt_reductions++;
                    }
                }
                // 0 is too little - 3 is too much!
                // NB, this doesn't really 'slow' traffic on the poor link, that will
                // slow anyway - this sets up so that other links will get more!
                if (t->srtt > t->srtt_min * 2) {
                    part *= (t->srtt_min * 2) / t->srtt;
                    if (part <= 0.2)
                        part = 0.2;
                }
                double bw = bwneeded - bwavailable;
                if (bw > 0) {
                    if (t->quota != 0 && (double)(t->bandwidth_max) * part > bw) {
                        t->weight = (bw * part); // let the quota link soak it up
                        bwavailable += bw * part;
                    } else {
                        if ((double)(t->bandwidth_max) * part < bw) {
                            t->weight = (double)(t->bandwidth_max) * part;
                            bwavailable += (double)(t->bandwidth_max) * part;
                            bwneeded += (double)(t->bandwidth_max) * (1.0 - part); // compensate for losses!
                        } else {
                            // just take what we need
                            t->weight = (bw * part);
                            bwavailable += (bw * part);
                            bwneeded += (bw * (1 - part)); // compensate for losses!
                        }
                    }
                }
            }
        }
    }

    LIST_FOREACH(t, &rtuns, entries)
    {

        if (t->weight > 0) {
            double b = t->weight * 128.0;
            t->bytes_per_sec = b;
        } else {
            t->bytes_per_sec = DEFAULT_MTU * 2; //even for non-active tunnels, give
            //them enough bandwidth to do 'timeout pings' etc...
            t->send_timer.repeat = UBOND_IO_TIMEOUT_DEFAULT / 2;
        }
    }
}

static int
ubond_rtun_bind(ubond_tunnel_t* t)
{
    struct addrinfo hints, *res;
    struct ifreq ifr;
    char bindifstr[UBOND_IFNAMSIZ + 5];
    int n, fd;

    memset(&hints, 0, sizeof(hints));
    /* AI_PASSIVE flag: the resulting address is used to bind
       to a socket for accepting incoming connections.
       So, when the hostname==NULL, getaddrinfo function will
       return one entry per allowed protocol family containing
       the unspecified address for that family. */
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_UNSPEC;
    fd = t->fd;
    hints.ai_socktype = SOCK_DGRAM;

    if (*t->bindaddr) {
        n = priv_getaddrinfo(t->bindaddr, t->bindport, &res, &hints);
        if (n < 0) {
            log_warnx(NULL, "%s getaddrinfo error: %s", t->name, gai_strerror(n));
            return -1;
        }
    }

    /* Try open socket with each address getaddrinfo returned,
       until getting a valid listening socket. */
    memset(bindifstr, 0, sizeof(bindifstr));
    if (*t->binddev) {
        snprintf(bindifstr, sizeof(bindifstr) - 1, " on %s", t->binddev);
    }
    log_info(NULL, "%s bind to %s%s",
        t->name, t->bindaddr ? t->bindaddr : "any",
        bindifstr);

    if (*t->binddev) {
        memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, sizeof(ifr.ifr_name) - 1, t->binddev);
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, (void*)&ifr, sizeof(ifr)) < 0) {
            log_warn(NULL, "failed to bind on interface %s", t->binddev);
        }
    }
    if (*t->bindaddr) {
        n = bind(fd, res->ai_addr, res->ai_addrlen);
        freeaddrinfo(res);
        if (n < 0) {
            log_warn(NULL, "%s bind error", t->name);
            return -1;
        }
    }

    return 0;
}

static int
ubond_rtun_start(ubond_tunnel_t* t)
{
    int ret, fd = -1;
    char *addr, *port;
    struct addrinfo hints, *res;
#if defined(HAVE_FREEBSD) || defined(HAVE_OPENBSD)
    int fib = t->bindfib;
#endif
    fd = t->fd;
    if (t->server_mode) {
        addr = t->bindaddr;
        port = t->bindport;
        t->id = atoi(t->bindport);
    } else {
        addr = t->destaddr;
        port = t->destport;
        t->id = atoi(t->destport);
    }

    /* Initialize hints */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    ret = priv_getaddrinfo(addr, port, &t->addrinfo, &hints);
    if (ret < 0 || !t->addrinfo) {
        log_warnx("dns", "%s getaddrinfo(%s,%s) failed: %s",
            t->name, addr, port, gai_strerror(ret));
        return -1;
    }

    res = t->addrinfo;
    while (res) {
        /* creation de la socket(2) */
        if ((fd = socket(t->addrinfo->ai_family,
                 t->addrinfo->ai_socktype,
                 t->addrinfo->ai_protocol))
            < 0) {
            log_warn(NULL, "%s socket creation error",
                t->name);
        } else {
/* Setting fib/routing-table is supported on FreeBSD and OpenBSD only */
#if defined(HAVE_FREEBSD)
            if (fib > 0 && setsockopt(fd, SOL_SOCKET, SO_SETFIB, &fib, sizeof(fib)) < 0)
#elif defined(HAVE_OPENBSD)
            if (fib > 0 && setsockopt(fd, SOL_SOCKET, SO_RTABLE, &fib, sizeof(fib)) < 0) {
                log_warn(NULL, "Cannot set FIB %d for kernel socket", fib);
                goto error;
            }
#endif
                t->fd = fd;
            break;
        }
        res = res->ai_next;
    }

    if (fd < 0) {
        log_warnx("dns", "%s connection failed. Check DNS?",
            t->name);
        goto error;
    }

    /* setup non blocking sockets */
    socklen_t val = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(socklen_t)) < 0) {
        log_warn(NULL, "%s setsockopt SO_REUSEADDR failed", t->name);
        goto error;
    }
    if (*t->bindaddr || *t->binddev) {
        if (ubond_rtun_bind(t) < 0) {
            goto error;
        }
    }

    /* set non blocking after connect... May lockup the entiere process */
    ubond_sock_set_nonblocking(fd);
    ubond_rtun_tick(t);
    ev_io_set(&t->io_read, fd, EV_READ);
    ev_io_set(&t->io_write, fd, EV_WRITE);
    ev_io_start(EV_A_ & t->io_read);
    t->io_timeout.repeat = UBOND_IO_TIMEOUT_DEFAULT / 2;
    return 0;
error:
    if (t->fd > 0) {
        close(t->fd);
        t->fd = -1;
    }
    if (t->io_timeout.repeat < UBOND_IO_TIMEOUT_MAXIMUM)
        t->io_timeout.repeat *= UBOND_IO_TIMEOUT_INCREMENT;
    return -1;
}

static void
ubond_script_get_env(int* env_len, char*** env)
{
    char** envp;
    int arglen;
    *env_len = 8;
    *env = (char**)calloc(*env_len + 1, sizeof(char*));
    if (!*env)
        fatal(NULL, "out of memory");
    envp = *env;
    arglen = sizeof(ubond_options.ip4) + 4;
    envp[0] = calloc(1, arglen + 1);
    if (snprintf(envp[0], arglen, "IP4=%s", ubond_options.ip4) < 0)
        log_warn(NULL, "snprintf IP4= failed");

    arglen = sizeof(ubond_options.ip6) + 4;
    envp[1] = calloc(1, arglen + 1);
    if (snprintf(envp[1], arglen, "IP6=%s", ubond_options.ip6) < 0)
        log_warn(NULL, "snprintf IP6= failed");

    arglen = sizeof(ubond_options.ip4_gateway) + 12;
    envp[2] = calloc(1, arglen + 1);
    if (snprintf(envp[2], arglen, "IP4_GATEWAY=%s", ubond_options.ip4_gateway) < 0)
        log_warn(NULL, "snprintf IP4_GATEWAY= failed");

    arglen = sizeof(ubond_options.ip6_gateway) + 12;
    envp[3] = calloc(1, arglen + 1);
    if (snprintf(envp[3], arglen, "IP6_GATEWAY=%s", ubond_options.ip6_gateway) < 0)
        log_warn(NULL, "snprintf IP6_GATEWAY= failed");

    arglen = sizeof(ubond_options.ip4_routes) + 11;
    envp[4] = calloc(1, arglen + 1);
    if (snprintf(envp[4], arglen, "IP4_ROUTES=%s", ubond_options.ip4_routes) < 0)
        log_warn(NULL, "snprintf IP4_ROUTES= failed");

    arglen = sizeof(ubond_options.ip6_routes) + 11;
    envp[5] = calloc(1, arglen + 1);
    if (snprintf(envp[5], arglen, "IP6_ROUTES=%s", ubond_options.ip6_routes) < 0)
        log_warn(NULL, "snprintf IP6_ROUTES= failed");

    arglen = sizeof(tuntap.devname) + 7;
    envp[6] = calloc(1, arglen + 1);
    if (snprintf(envp[6], arglen, "DEVICE=%s", tuntap.devname) < 0)
        log_warn(NULL, "snprintf DEVICE= failed");

    envp[7] = calloc(1, 16);
    if (snprintf(envp[7], 15, "MTU=%d", ubond_options.mtu) < 0)
        log_warn(NULL, "snprintf MTU= failed");
    envp[8] = NULL;
}

static void
ubond_free_script_env(char** env)
{
    char** envp = env;
    while (*envp) {
        free(*envp);
        envp++;
    }
    free(env);
}

static void
ubond_rtun_tuntap_up()
{
    if ((ubond_status.connected > 0 || ubond_options.static_tunnel) && ubond_status.initialized == 0) {
        char* cmdargs[4] = { tuntap.devname, "tuntap_up", NULL, NULL };
        char** env;
        int env_len;
        ubond_script_get_env(&env_len, &env);
        priv_run_script(2, cmdargs, env_len, env);
        ubond_status.initialized = 1;
        ubond_free_script_env(env);
    }
}

static void
ubond_rtun_status_up(ubond_tunnel_t* t)
{
    enum chap_status old_status = t->status;
    ev_tstamp now = ev_now(EV_DEFAULT_UC);
    t->status = UBOND_AUTHOK;
    t->last_activity = now;
    t->saved_timestamp = -1;
    t->saved_timestamp_received_at = 0;
    t->srtt = 40;
    t->srtt_d = 0;
    t->srtt_c = 0;
    t->loss = 0;
    t->bm_data = 0;
    ubond_update_status();
    update_process_title();
    ubond_rtun_recalc_weight();
    if (old_status < UBOND_AUTHOK) {
        char* cmdargs[4] = { tuntap.devname, "rtun_up", t->name, NULL };
        char** env;
        int env_len;
        ubond_script_get_env(&env_len, &env);
        priv_run_script(3, cmdargs, env_len, env);
        ubond_free_script_env(env);
        ubond_rtun_tuntap_up();
    }

    while (!UBOND_TAILQ_EMPTY(&t->sbuf)) {
        ubond_pkt_release(UBOND_TAILQ_POP_LAST(&t->sbuf));
    }
    while (!UBOND_TAILQ_EMPTY(&t->hpsbuf)) {
        ubond_pkt_release(UBOND_TAILQ_POP_LAST(&t->hpsbuf));
    }
}

void ubond_rtun_status_down(ubond_tunnel_t* t)
{
    char* cmdargs[4] = { tuntap.devname, "rtun_down", t->name, NULL };
    char** env;
    int env_len;
    enum chap_status old_status = t->status;
    t->status = UBOND_DISCONNECTED;
    t->disconnects++;
    t->srtt = 0;
    t->srtt_d = 0;
    t->srtt_c = 0;
    t->loss = 64;
    t->saved_timestamp = -1;
    t->saved_timestamp_received_at = 0;

    ubond_tunnel_t* tun;
    LIST_FOREACH(tun, &rtuns, entries)
    {
        if (tun->status >= UBOND_AUTHOK)
            break;
    }
    if (!tun)

        ubond_rtun_recalc_weight();

    // hpsbuf has tun specific stuff in it, drop it.
    while (!UBOND_TAILQ_EMPTY(&t->hpsbuf)) {
        ubond_pkt_release(UBOND_TAILQ_POP_LAST(&t->hpsbuf));
    }
    // everythign in our send buffer, we'll drop - they will bound to ask for
    // more, and better they ask for the right things
    while (!UBOND_TAILQ_EMPTY(&t->sbuf)) {
        ubond_pkt_release(UBOND_TAILQ_POP_LAST(&t->sbuf));
    }
    // for the normal buffer, lets request resends of all possible packets from
    // the last one we recieved
    ubond_rtun_request_resend(t, t->seq_last, RESENDBUFSIZE);

    ubond_update_status();
    update_process_title();
    ubond_rtun_recalc_weight();
    if (old_status >= UBOND_AUTHOK) {
        ubond_script_get_env(&env_len, &env);
        priv_run_script(3, cmdargs, env_len, env);
        /* Re-initialize weight round robin */
        if (ubond_status.connected == 0 && ubond_status.initialized == 1 && ubond_options.static_tunnel == 0) {
            cmdargs[0] = tuntap.devname;
            cmdargs[1] = "tuntap_down";
            cmdargs[2] = NULL;
            priv_run_script(2, cmdargs, env_len, env);
            ubond_status.initialized = 0;
        }
        ubond_free_script_env(env);
        /* MAYBE flushing the re-order buffer here would be good, as we might
    have a lot of packets in flight which will never arrive, so recovery MAY be
    quicker with a flush...*/
    }
}

static void
ubond_update_status()
{
    ubond_tunnel_t* t;
    int fb = ubond_options.fallback_available;
    int connected = 0;
    LIST_FOREACH(t, &rtuns, entries)
    {
        if (t->status == UBOND_AUTHOK) {
            if (!t->fallback_only)
                fb = 0;
            connected++;
        }
    }
    if (ubond_status.fallback_mode != fb || ubond_status.connected != connected) {
        ubond_status.fallback_mode = fb;
        ubond_status.connected = connected;
        if (ubond_status.fallback_mode || !ubond_status.connected) {
            if (ubond_options.fallback_available) {
                log_info(NULL, "all tunnels are down or lossy, switching to fallback mode");
            } else {
                log_info(NULL, "all tunnels are down or lossy but fallback is not available");
            }
        } else {
            log_info(NULL, "%d tunnels up (normal mode)", connected);
        }
    }
}

static void
ubond_rtun_challenge_send(ubond_tunnel_t* t)
{
    ubond_pkt_t* pkt;

    if (ubond_pkt_list_is_full(&t->hpsbuf))
        log_warnx("net", "%s high priority buffer: overflow", t->name);

    pkt = ubond_pkt_get();
    UBOND_TAILQ_INSERT_HEAD(&t->hpsbuf, pkt);

    ubond_pkt_challenge challenge = {
        UBOND_CHALLENGE_AUTH,
        htobe16(UBOND_PROTOCOL_VERSION),
        htobe64((t->quota) ? t->permitted : 0),
    };
    strcpy(challenge.password, ubond_options.password);

    *(ubond_pkt_challenge*)(pkt->p.data) = challenge;
    pkt->p.len = sizeof(ubond_pkt_challenge);

    pkt->p.type = UBOND_PKT_AUTH;

    t->status = UBOND_AUTHSENT;
    ubond_rtun_do_send(t, 0);
    log_debug("protocol", "%s ubond_rtun_challenge_send", t->name);
}

static void
ubond_rtun_send_auth(ubond_tunnel_t* t)
{
    ubond_pkt_t* pkt;
    if (t->server_mode) {
        /* server side */
        if (t->status == UBOND_DISCONNECTED || t->status >= UBOND_AUTHOK) {
            ubond_rtun_tick(t);
            ubond_rtun_status_up(t); // mark this as up, before trying to send
            // somethign on it !

            if (ubond_pkt_list_is_full(&t->hpsbuf)) {
                log_warnx("net", "%s high priority buffer: overflow", t->name);
            }
            pkt = ubond_pkt_get();
            UBOND_TAILQ_INSERT_HEAD(&t->hpsbuf, pkt);

            ubond_pkt_challenge challenge = {
                UBOND_CHALLENGE_OK,
                htobe16(UBOND_PROTOCOL_VERSION),
                htobe64((t->quota) ? t->permitted : 0),
            };
            strcpy(challenge.password, ubond_options.password);

            *(ubond_pkt_challenge*)(pkt->p.data) = challenge;
            pkt->p.len = sizeof(ubond_pkt_challenge);

            pkt->p.type = UBOND_PKT_AUTH_OK;
            if (t->status < UBOND_AUTHOK)
                t->status = UBOND_AUTHSENT;
            ubond_rtun_do_send(t, 0);
            log_debug("protocol", "%s sending 'OK'", t->name);
            log_info("protocol", "%s authenticated", t->name);
        }
    } else {
        /* client side */
        if (t->status == UBOND_AUTHSENT) {
            log_info("protocol", "%s authenticated", t->name);
            ubond_rtun_tick(t);
            ubond_rtun_status_up(t);
        }
    }
}

static void
ubond_rtun_request_resend(ubond_tunnel_t* loss_tun, uint16_t tun_seqn, uint16_t len)
{
    ubond_pkt_t* pkt;
    pkt = ubond_pkt_get();

    struct resend_data* d = (struct resend_data*)(pkt->p.data);
    d->r = 'R';
    d->s = 'S';

    d->seqn = htobe16(tun_seqn);
    d->tun_id = htobe16(loss_tun->id);
    d->len = htobe16(len);
    pkt->p.len = sizeof(struct resend_data);

    pkt->p.type = UBOND_PKT_RESEND;
    out_resends += len;
    ubond_buffer_write(&hpsend_buffer, pkt);

    log_debug("resend", "Request resend %lu (lost from tunnel %s)", /* t->name,*/ tun_seqn, loss_tun->name);
}

static ubond_tunnel_t* ubond_find_tun(uint16_t id)
{
    ubond_tunnel_t* t;
    LIST_FOREACH(t, &rtuns, entries)
    {
        if (t->id == id)
            return t;
    }
    return NULL;
}

static void
ubond_rtun_resend(struct resend_data* d)
{
    uint16_t tun_id = be16toh(d->tun_id);
    uint16_t len = be16toh(d->len);
    uint16_t seqn_base = be16toh(d->seqn);
    ubond_tunnel_t* loss_tun = ubond_find_tun(tun_id);
    if (!loss_tun)
        return;
    if (len > RESENDBUFSIZE / 4) {
        if (loss_tun->status >= UBOND_AUTHOK) {
            log_info("rtt", "%s resend request reached threashold: %d/%d", loss_tun->name, len, RESENDBUFSIZE / 4);
            loss_tun->status = UBOND_LOSSY;
            loss_tun->sent_loss = 100.0; //tun->loss_tollerence
        }
    }

    for (int i = 0; i < len; i++) {
        uint16_t seqn = seqn_base + i;
        ubond_pkt_t* old_pkt = loss_tun->old_pkts[seqn % RESENDBUFSIZE];
        if (old_pkt && old_pkt->p.tun_seq == seqn) {
            if (old_pkt->p.type != UBOND_PKT_DATA || is_tcp(old_pkt) /*|| old_pkt->p.data[9]==17*/) { // only send tcp, e.g. refuse UDP packets!
                ubond_buffer_write(&hpsend_buffer, old_pkt);
                loss_tun->old_pkts[seqn % RESENDBUFSIZE] = NULL; // remove this from the old list
                if (old_pkt->p.type == UBOND_PKT_DATA)
                    old_pkt->p.type = UBOND_PKT_DATA_RESEND;
                log_debug("resend", "resend packet (tun seq: %lu tun seq %lu) previously sent on %s", /*t->name,*/ seqn, old_pkt->p.tun_seq, loss_tun->name);
            } else {
                log_debug("resend", "Wont resent packet (tun seq: %lu tun seq %lu) of type %d", seqn, old_pkt->p.tun_seq, (unsigned char)old_pkt->p.data[6]);
            }
        } else {
            if (old_pkt) {
                log_debug("resend+", "unable to resend seq %lu (Not Found - replaced by %lu)", seqn, old_pkt->p.tun_seq);
            } else {
                log_debug("resend+", "unable to resend seq %lu (Not Found - empty slot)", seqn);
            }
        }
    }
}

static void
ubond_rtun_tick_connect(ubond_tunnel_t* t)
{
    ev_tstamp now = ev_now(EV_DEFAULT_UC);
    if (t->server_mode) {
        if (t->fd < 0) {
            if (ubond_rtun_start(t) == 0) {
                t->conn_attempts = 0;
            } else {
                return;
            }
        }
    } else {
        if (t->status < UBOND_AUTHOK) {
            t->conn_attempts++;
            t->last_connection_attempt = now;
            if (t->fd < 0) {
                if (ubond_rtun_start(t) == 0) {
                    t->conn_attempts = 0;
                } else {
                    return;
                }
            }
        }
        ubond_rtun_challenge_send(t);
    }
}
ev_tstamp last = 0;
void ubond_calc_bandwidth(EV_P_ ev_timer* w, int revents)
{
    ev_tstamp now = ev_now(EV_DEFAULT_UC);
    ev_tstamp diff = BANDWIDTHCALCTIME;
    if (last && (now - last) > BANDWIDTHCALCTIME / 2 && (now - last) < BANDWIDTHCALCTIME * 2) {
        diff = now - last;
    }
    last = now;
    bandwidth = ((bandwidth * 9.0) + (((double)bandwidthdata / 128.0) / diff)) / 10.0;
    bandwidthdata = 0;

    int max_srtt = 0;
    int min_srtt = 0;

    ubond_tunnel_t* t;
    int tuns = 0;
    LIST_FOREACH(t, &rtuns, entries)
    {
        if (t->status >= UBOND_AUTHOK) {
            tuns++;
            // permitted is in BYTES per second.
            if (t->quota) {
                t->permitted += (double)t->quota * diff * 128.0; // listed in kbps (1024/8)
            }

            if (t->srtt_c > 2) {
                t->srtt = (t->srtt_d / t->srtt_c);

                if (t->srtt_min == 0 || t->srtt < t->srtt_min) {
                    t->srtt_min = t->srtt;
                }
                if (srtt_min == 0 || t->srtt < srtt_min) {
                    srtt_min = t->srtt;
                }
                t->srtt_d = 0;
                t->srtt_c = 0;
            } else {
                t->srtt = t->srtt_min;
            }
            t->srtt_av = ((t->srtt_av * 9.0) + t->srtt) / 10.0;

            if (!min_srtt || t->srtt_av < min_srtt)
                min_srtt = t->srtt_av;
            if (!max_srtt || t->srtt_av > max_srtt)
                max_srtt = t->srtt_av;

            // calc measured bandwidth for INCOMMING
            t->bandwidth_measured = (t->bm_data / 128) * INVERSEBWCALCTIME; // kbits/sec
            t->bm_data = 0;

            double bandwidth_sent = ((t->bytes_since_adjust / 128.0) / diff);

            double reductions = ((double)t->srtt_reductions / (double)t->pkts_cnt) * 100.0;
            if (t->pkts_cnt < 10)
                reductions = 0;

            t->pkts_cnt = 0;

            /*
      would have to delay it by srtt?
      double bandwidth_sent = ((t->bytes_since_adjust/128.0) / diff);
      if (bandwidth_sent > t->bandwidth_max/2) {
        if (bandwidth_sent * 3 > t->bandwidth_out * 4) {
      printf("%s %d %d %f\n", t->name, t->bandwidth_out, t->bandwidth_max, bandwidth_sent)    ;
          double f = (double)((bandwidth_sent * 3 ) - (t->bandwidth_out *4)) / (double)(bandwidth_sent * 3);
          f/=10.0;
          t->bandwidth_max *= 1.0 - f;
        } 
        if (t->bandwidth_out > t->bandwidth_max) {
          t->bandwidth_max = t->bandwidth_out * 1.25;
        }
      }
*/
            // hunt a high watermark with slow drift
            if (bandwidth_sent > t->bandwidth_max / 2) {
                double new_bwm = t->bandwidth_max;

                if (t->sent_loss < (LOSS_TOLERENCE / 4.0) && (t->srtt < 3 * t->srtt_min)) {

                    if (t->sent_loss == 0 && ((double)(t->bandwidth_out) > ((double)(t->bandwidth_max) * 0.80))) {
                        if (t->lossless) {
                            // FASTGROTH MODE
                            new_bwm *= 1.01;
                        } else {
                            t->lossless++;
                        }
                    } else {
                        if (t->sent_loss != 0 && t->lossless) {
                            // correct old fastgrowth
                            new_bwm *= 0.99;
                        }
                        t->lossless = 0;
                    }
                    // normal growth
                    if (t->bandwidth_out > t->bandwidth_max) {
                        new_bwm = ((new_bwm * 9) + t->bandwidth_out) / 10;
                    }
                } else {
                    if (t->lossless) {
                        // correct old fastgrowth
                        new_bwm *= 0.99;
                    }
                    if (t->srtt > 3 * t->srtt_min) {
                        new_bwm *= 0.99;
                    }
                    t->lossless = 0;
                    if (t->bandwidth_out < bandwidth_sent) {
                        new_bwm *= 0.995;
                    }
                    if (new_bwm < 100)
                        new_bwm = 100;
                }
                t->bandwidth_max = new_bwm;
            } else {
                if (reductions > 50) { // more than 50% reductions, we should reduce bandwidth
                    t->bandwidth_max *= 0.99;
                }
                if (t->bandwidth_max < 100)
                    t->bandwidth_max = 100;
                t->lossless = 0;
            }
        }
        t->bytes_since_adjust = 0;
        t->last_adjust = now;
    }

    if (min_srtt && max_srtt) {
        max_size_outoforder = max_srtt / min_srtt;
    }

    ubond_rtun_recalc_weight();
}

static void
ubond_rtun_choose(ubond_tunnel_t* rtun)
{

    if (rtun->status != UBOND_AUTHOK)
        return;
    if (rtun->quota && rtun->permitted < DEFAULT_MTU * 2)
        return;
    if (ubond_status.fallback_mode != rtun->fallback_only)
        return;

    ubond_pkt_t* spkt = NULL;
    if (!UBOND_TAILQ_EMPTY(&hpsend_buffer) && (rtun->sent_loss <= (LOSS_TOLERENCE / 4.0))) {
        spkt = UBOND_TAILQ_POP_LAST(&hpsend_buffer);
    } else {
        if (!UBOND_TAILQ_EMPTY(&send_buffer)) {
            spkt = UBOND_TAILQ_POP_LAST(&send_buffer);
        }
    }
    if (!spkt)
        return;

    activate_streams();
    if (!ev_is_active(&tuntap.io_read)) {
        ev_io_start(EV_A_ & tuntap.io_read);
    }

    ubond_pkt_list_t* sbuf = &rtun->sbuf;

#ifdef HAVE_FILTERS
    u_char* data = (u_char*)(spkt->p.data);
    uint32_t len = spkt->p.len;

    ubond_tunnel_t* frtun = ubond_filters_choose((uint32_t)len, data);
    if (frtun) {
        /* High priority buffer, not reorderd when a filter applies */
        rtun = frtun;
        sbuf = &rtun->hpsbuf;
    }
#endif

    if (ubond_pkt_list_is_full(sbuf))
        log_warnx("tuntap", "%s buffer: overflow", rtun->name);

    /* Ask for a free buffer */
    UBOND_TAILQ_INSERT_HEAD(sbuf, spkt);

    return;
}

static void
ubond_rtun_send_keepalive(ev_tstamp now, ubond_tunnel_t* t)
{
    ubond_pkt_t* pkt;
    if (ubond_pkt_list_is_full(&t->hpsbuf))
        log_warnx("net", "%s high priority buffer: overflow", t->name);
    else {
        log_debug("protocol", "%s sending keepalive", t->name);
        pkt = ubond_pkt_get();
        UBOND_TAILQ_INSERT_HEAD(&t->hpsbuf, pkt);
        pkt->p.type = UBOND_PKT_KEEPALIVE;
        pkt->p.len = sprintf(pkt->p.data, "%lu", t->bandwidth_measured) + 1;
        ubond_rtun_do_send(t, 0);
    }
}

static void
ubond_rtun_send_disconnect(ubond_tunnel_t* t)
{
    ubond_pkt_t* pkt;
    if (ubond_pkt_list_is_full(&t->hpsbuf))
        log_warnx("net", "%s high priority buffer: overflow", t->name);
    else {
        log_debug("protocol", "%s sending disconnect", t->name);
        pkt = ubond_pkt_get();
        UBOND_TAILQ_INSERT_HEAD(&t->hpsbuf, pkt);
        pkt->p.type = UBOND_PKT_DISCONNECT;
        pkt->p.len = 1;
        ubond_rtun_do_send(t, 0);
    }
}

static void
ubond_rtun_check_lossy(ubond_tunnel_t* tun)
{
    double loss = tun->sent_loss;
    int status_changed = 0;
    ev_tstamp now = ev_now(EV_DEFAULT_UC);
    int keepalive_ok = ((tun->last_activity == 0) || (tun->last_activity + (UBOND_IO_TIMEOUT_DEFAULT * 2) + ((tun->srtt_av / 1000.0) * 2)) > now);

    if (!keepalive_ok && tun->status == UBOND_AUTHOK) {
        log_info("rtt", "%s keepalive reached threashold, last activity recieved %fs ago", tun->name, now - tun->last_activity);
        tun->status = UBOND_LOSSY;
        ubond_rtun_request_resend(tun, tun->seq_last, RESENDBUFSIZE);
        // We wont mark the tunnel down yet (hopefully it will come back again, and
        // coming back from a loss is quicker than pulling it down etc. However,
        // here, we fear the worst, and will ask for all packets again. Lets hope
        // there are not too many in flight.
        status_changed = 1;
    } else if (loss >= LOSS_TOLERENCE && tun->status == UBOND_AUTHOK) {
        log_info("rtt", "%s packet loss reached threashold: %f%%/%f%%",
            tun->name, loss, LOSS_TOLERENCE);
        //    tun->status = UBOND_LOSSY;
        //    status_changed = 1;
    } else if (keepalive_ok && loss < LOSS_TOLERENCE && tun->status == UBOND_LOSSY) {
        log_info("rtt", "%s packet loss acceptable again: %f%%/%f%%",
            tun->name, loss, LOSS_TOLERENCE);
        tun->status = UBOND_AUTHOK;
        status_changed = 1;
    }
    /* are all links in lossy mode ? switch to fallback ? */
    if (status_changed) {
        ubond_update_status();
        update_process_title();
        ubond_rtun_recalc_weight();
/*    ubond_tunnel_t *t;
    LIST_FOREACH(t, &rtuns, entries) {
      if (! t->fallback_only && t->status != UBOND_LOSSY) {
        ubond_status.fallback_mode = 0;
        ubond_rtun_wrr_reset(&rtuns, ubond_status.fallback_mode);
        return;
      }
    }
    if (ubond_options.fallback_available) {
      log_info(NULL, "all tunnels are down or lossy, switch fallback mode");
      ubond_status.fallback_mode = 1;
      ubond_rtun_wrr_reset(&rtuns, ubond_status.fallback_mode);
    } else {
      log_info(NULL, "all tunnels are down or lossy but fallback is not available");
    }
*/  }
}

static void
ubond_rtun_check_timeout(EV_P_ ev_timer* w, int revents)
{
    ubond_tunnel_t* t = w->data;
    ev_tstamp now = ev_now(EV_DEFAULT_UC);

    ubond_rtun_check_lossy(t);

    if (t->status == UBOND_LOSSY) {
        if ((t->last_activity != 0) && (t->last_activity + t->timeout + (UBOND_IO_TIMEOUT_DEFAULT * 2) + ((t->srtt_av / 1000.0) * 2)) < now) {
            log_info("protocol", "%s timeout", t->name);
            ubond_rtun_status_down(t);
        }
    }
    if (t->status < UBOND_AUTHOK) {
        ubond_rtun_tick_connect(t);
    } else {
        ubond_rtun_send_keepalive(now, t);
    }
}

static void
tuntap_io_event(EV_P_ ev_io* w, int revents)
{
    if (revents & EV_READ) {
        ubond_pkt_t* pkt;
        while (!ubond_pkt_list_is_full(&send_buffer) && (pkt = ubond_tuntap_read(&tuntap))) {
            pkt->stream = NULL;
            pkt->sent_tun = NULL;
            ubond_buffer_write(&send_buffer, pkt);
            ubond_tunnel_t* t;
            // ev_now_update(EV_DEFAULT_UC);
            LIST_FOREACH(t, &rtuns, entries)
            {
                if (!t->busy_writing) {
                    ubond_rtun_do_send(t, 0);
                    if (UBOND_TAILQ_EMPTY(&send_buffer))
                        break;
                }
            }
        }
        if (ubond_pkt_list_is_full(&send_buffer)) {
            if (ev_is_active(&tuntap.io_read)) {
                ev_io_stop(EV_A_ & tuntap.io_read);
            }
        }
    } else if (revents & EV_WRITE) {
        if (!UBOND_TAILQ_EMPTY(&tuntap.sbuf)) {
            ubond_pkt_t* pkt = UBOND_TAILQ_POP_LAST(&tuntap.sbuf);
            ubond_tuntap_write(&tuntap, pkt);
            /* Nothing else to read */
        }
        if (UBOND_TAILQ_EMPTY(&tuntap.sbuf)) {
            ev_io_stop(EV_A_ & tuntap.io_write);
        }
    }
}

static void
ubond_tuntap_init()
{
    ubond_proto_t proto;
    memset(&tuntap, 0, sizeof(tuntap));
    snprintf(tuntap.devname, UBOND_IFNAMSIZ - 1, "%s", "ubond0");
    tuntap.maxmtu = 1500 - PKTHDRSIZ(proto) - IP4_UDP_OVERHEAD;
    log_debug(NULL, "absolute maximum mtu: %d", tuntap.maxmtu);
    tuntap.type = UBOND_TUNTAPMODE_TUN;
    ubond_pkt_list_init(&tuntap.sbuf, PKTBUFSIZE);
    ev_init(&tuntap.io_read, tuntap_io_event);
    ev_init(&tuntap.io_write, tuntap_io_event);
}

static void
update_process_title()
{
    if (!process_title)
        return;
    char title[1024];
    char* s;
    ubond_tunnel_t* t;
    char status[32];
    int len;
    memset(title, 0, sizeof(title));
    if (*process_title)
        strlcat(title, process_title, sizeof(title));
    LIST_FOREACH(t, &rtuns, entries)
    {
        switch (t->status) {
        case UBOND_AUTHOK:
            s = "@";
            break;
        case UBOND_LOSSY:
            s = "~";
            break;
        default:
            s = "!";
            break;
        }
        len = snprintf(status, sizeof(status) - 1, " %s%s", s, t->name);
        if (len) {
            status[len] = 0;
            strlcat(title, status, sizeof(title));
        }
    }
    setproctitle("%s", title);
}

static void
ubond_config_reload(EV_P_ ev_signal* w, int revents)
{
    log_info("config", "reload (SIGHUP)");
    priv_reload_resolver();
    /* configuration file path does not matter after
     * the first intialization.
     */
    int config_fd = priv_open_config("");
    if (config_fd > 0) {
        if (ubond_config(config_fd, 0) != 0) {
            log_warn("config", "reload failed");
        } else {
            if (time(&ubond_status.last_reload) == -1)
                log_warn("config", "last_reload time set failed");
            ubond_rtun_recalc_weight();
        }
    } else {
        log_warn("config", "open failed");
    }
}

static void
ubond_quit(EV_P_ ev_signal* w, int revents)
{
    ubond_tunnel_t* t;
    log_info(NULL, "killed by signal SIGTERM, SIGQUIT or SIGINT");
    LIST_FOREACH(t, &rtuns, entries)
    {
        ev_timer_stop(EV_A_ & t->io_timeout);
        ev_io_stop(EV_A_ & t->io_read);
        if (t->status >= UBOND_AUTHOK) {
            ubond_rtun_send_disconnect(t);
        }
    }
    ev_break(EV_A_ EVBREAK_ALL);
}

int main(int argc, char** argv)
{
    int i, c, option_index, config_fd;
    struct stat st;
    ev_signal signal_hup, signal_usr1;
    ev_signal signal_sigquit, signal_sigint, signal_sigterm;
    extern char* __progname;
#ifdef ENABLE_CONTROL
    struct ubond_control control;
#endif
    /* uptime statistics */
    if (time(&ubond_status.start_time) == -1)
        log_warn(NULL, "start_time time() failed");
    if (time(&ubond_status.last_reload) == -1)
        log_warn(NULL, "last_reload time() failed");

    //    log_init(1, 2, "ubond");

    _progname = strdup(__progname);
    saved_argv = calloc(argc + 1, sizeof(*saved_argv));
    for (i = 0; i < argc; i++) {
        saved_argv[i] = strdup(argv[i]);
    }
    saved_argv[i] = NULL;
    compat_init_setproctitle(argc, argv);
    argv = saved_argv;

    /* Parse the command line quickly for config file name.
     * This is needed for priv_init to know where the config
     * file is.
     *
     * priv_init will not allow to change the config file path.
     */
    while (1) {
        c = getopt_long(argc, saved_argv, optstr,
            long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
        case 1: /* --natural-title */
            ubond_options.change_process_title = 0;
            break;
        case 2: /* --debug */
            ubond_options.debug = 1;
            break;
        case 3: /* --yes-run-as-root */
            ubond_options.root_allowed = 1;
            break;
        case 'c': /* --config */
            strlcpy(ubond_options.config_path, optarg,
                sizeof(ubond_options.config_path));
            break;
        case 'D': /* debug= */
            ubond_options.debug = 1;
            log_accept(optarg);
            break;
        case 'n': /* --name */
            strlcpy(ubond_options.process_name, optarg,
                sizeof(ubond_options.process_name));
            break;
        case 'u': /* --user */
            strlcpy(ubond_options.unpriv_user, optarg,
                sizeof(ubond_options.unpriv_user));
            break;
        case 'v': /* --verbose */
            ubond_options.verbose++;
            break;
        case 'V': /* --version */
            printf("ubond version %s. Protocol version %hu\n", VERSION, UBOND_PROTOCOL_VERSION);
            _exit(0);
            break;
        case 'q': /* --quiet */
            ubond_options.verbose--;
            break;
        case 'p': /* will be checked later, move on */
            break;
        case 'h': /* --help */
        default:
            usage(argv);
        }
    }

    /* Config file check */
    if (access(ubond_options.config_path, R_OK) != 0) {
        log_warnx("config", "unable to read config file %s",
            ubond_options.config_path);
    }
    if (stat(ubond_options.config_path, &st) < 0) {
        fatal("config", "unable to open file");
    } else if (st.st_mode & (S_IRWXG | S_IRWXO)) {
        fatal("config", "file is group/other accessible");
    }

    /* Some common checks */
    if (getuid() == 0) {
        void* pw = getpwnam(ubond_options.unpriv_user);
        if (!ubond_options.root_allowed && !pw)
            fatal(NULL, "you are not allowed to run this program as root. "
                        "please specify a valid user with --user option");
        if (!pw)
            fatal(NULL, "invalid unprivilged username");
    }

#ifdef HAVE_LINUX
    if (access("/dev/net/tun", R_OK | W_OK) != 0) {
        fatal(NULL, "unable to open /dev/net/tun");
    }
#endif

    if (ubond_options.change_process_title) {
        if (*ubond_options.process_name) {
            __progname = strdup(ubond_options.process_name);
            process_title = ubond_options.process_name;
            setproctitle("%s [priv]", ubond_options.process_name);
        } else {
            __progname = "ubond";
            process_title = "";
            setproctitle("[priv]");
        }
    }

    log_init(ubond_options.debug, ubond_options.verbose, __progname);

#ifdef HAVE_LINUX
    ubond_systemd_notify();
#endif

    UBOND_TAILQ_INIT(&pool);

    priv_init(argv, ubond_options.unpriv_user);
    if (ubond_options.change_process_title)
        update_process_title();

    LIST_INIT(&rtuns);

/* Kill me if my root process dies ! */
#ifdef HAVE_LINUX
    prctl(PR_SET_PDEATHSIG, SIGCHLD);
#endif

    /* Config file opening / parsing */
    config_fd = priv_open_config(ubond_options.config_path);
    if (config_fd < 0)
        fatalx("cannot open config file");
    if (!(loop = ev_default_loop(EVFLAG_AUTO)))
        fatal(NULL, "cannot initialize libev. check LIBEV_FLAGS?");

    /* init the reorder buffer after ev is enabled, but before we have all the
       tunnels */
    ubond_reorder_init();

    /* tun/tap initialization */
    ubond_tuntap_init();

    ev_timer_init(&bandwidth_calc_timer, &ubond_calc_bandwidth, 0., BANDWIDTHCALCTIME);
    ev_timer_start(EV_A_ & bandwidth_calc_timer);

    if (ubond_config(config_fd, 1) != 0)
        fatalx("cannot open config file");

    {
        //ubond_tunnel_t *t;
        //int i=0;
        //LIST_FOREACH(t, &rtuns, entries) {i++;}
        //ubond_pkt_list_init(&send_buffer, i*2);
        ubond_pkt_list_init(&send_buffer, 102400);
        ubond_pkt_list_init(&hpsend_buffer, PKTBUFSIZE);
    }

    if (ubond_tuntap_alloc(&tuntap) <= 0)
        fatalx("cannot create tunnel device");
    else
        log_info(NULL, "created interface `%s'", tuntap.devname);
    ubond_sock_set_nonblocking(tuntap.fd);

    preset_permitted(argc, saved_argv);

    ev_io_set(&tuntap.io_read, tuntap.fd, EV_READ);
    ev_io_set(&tuntap.io_write, tuntap.fd, EV_WRITE);
    ev_io_start(loop, &tuntap.io_read);

    /* tcp socket init */
    socks_init();

    priv_set_running_state();

#ifdef ENABLE_CONTROL
    /* Initialize ubond remote control system */
    strlcpy(control.fifo_path, ubond_options.control_unix_path,
        sizeof(control.fifo_path));
    control.mode = UBOND_CONTROL_READWRITE;
    control.fifo_mode = 0600;
    control.bindaddr = strdup(ubond_options.control_bind_host);
    control.bindport = strdup(ubond_options.control_bind_port);
    ubond_control_init(&control);
#endif

    /* re-compute rtun weight based on bandwidth allocation */
    ubond_rtun_recalc_weight();

    if (ubond_options.static_tunnel) {
        ubond_rtun_tuntap_up();
    }
    /* Last check before running */
    if (getppid() == 1)
        fatalx("Privileged process died");

    ev_signal_init(&signal_hup, ubond_config_reload, SIGHUP);
    ev_signal_init(&signal_usr1, ubond_reset_perm, SIGUSR1);
    ev_signal_init(&signal_sigint, ubond_quit, SIGINT);
    ev_signal_init(&signal_sigquit, ubond_quit, SIGQUIT);
    ev_signal_init(&signal_sigterm, ubond_quit, SIGTERM);
    ev_signal_start(loop, &signal_hup);
    ev_signal_start(loop, &signal_usr1);
    ev_signal_start(loop, &signal_sigint);
    ev_signal_start(loop, &signal_sigquit);
    ev_signal_start(loop, &signal_sigterm);

    ev_run(loop, 0);

    free(_progname);
    return 0;
}
