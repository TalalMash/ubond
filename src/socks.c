
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <ev.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "privsep.h"
#include "socks.h"
#include "ubond.h"

#define TCP_MAX_OUTSTANDING 1024

int aolderb(uint16_t a, uint16_t b)
{
    return ((int16_t)(b - a)) > 0;
}
int aoldereqb(uint16_t a, uint16_t b)
{
    return ((int16_t)(b - a)) >= 0;
}

static void on_read_cb(EV_P_ struct ev_io* ev, int revents);
static void on_write_cb(EV_P_ struct ev_io* ev, int revents);
static void resend_timer(EV_P_ ev_timer* w, int revents);
static void resend(stream_t* s);

extern struct ubond_options_s ubond_options;
extern ubond_pkt_list_t send_buffer; /* send buffer */
extern ubond_pkt_list_t hpsend_buffer; /* send buffer */
extern float max_size_outoforder;
extern float srtt_max;
extern struct ev_loop* loop;

ev_tstamp fullrtt()
{
    if (srtt_max) {
        return srtt_max / 250.0;
    } else {
        return 0.25;
    }
}

static int setnonblock(int fd);

#define MAXSTREAMS 10000
ev_io socks_read;

stream_list_t active;

stream_list_t s_pool;
uint64_t s_pool_out = 0;
uint32_t max_flow_id = 1; // flowid of 0 is illegal
stream_t* ubond_stream_get(int fd)
{
    stream_t* p;
    if (!UBOND_TAILQ_EMPTY(&s_pool)) {
        p = UBOND_TAILQ_FIRST(&s_pool);
        UBOND_TAILQ_REMOVE(&s_pool, p);
    } else {
        p = malloc(sizeof(struct stream_t));
        assert(p); // otherwise we are truely doomed.
        p->preset_flow_id = max_flow_id++; // NB, set once, and re-used when reallocated from the pool
        if (max_flow_id > MAXSTREAMS) {
            log_warnx("socks", "Using more TCP streams (%d) that configured (%d)", max_flow_id, MAXSTREAMS);
        }
    }
    s_pool_out++;

    p->fd = fd;
    p->data_seq = 0;
    p->io_read.data = (void*)p;
    p->io_write.data = (void*)p;
    ev_io_init(&p->io_read, on_read_cb, fd, EV_READ);
    ev_io_init(&p->io_write, on_write_cb, fd, EV_WRITE);
    p->flow_id = 0;
    p->sending = 0;
    p->seq_to_ack = 0;
    p->next_seq = 0;
    p->stall = 0;
    UBOND_TAILQ_INIT(&p->sent);
    UBOND_TAILQ_INIT(&p->received);
    UBOND_TAILQ_INIT(&p->draining);

    p->resend_timer.data = p;
    ev_timer_init(&p->resend_timer, &resend_timer, 0., 0.01);

    return p;
};
void ubond_stream_release(stream_t* p)
{
    s_pool_out--;
    UBOND_TAILQ_INSERT_HEAD(&s_pool, p);
}
void ubond_stream_list_init(stream_list_t* list, uint64_t size)
{
    UBOND_TAILQ_INIT(list);
    list->max_size = size;
}

static void ubond_stream_close(stream_t* s)
{
    log_warnx("sock", "Stream Closing (FD:%d)", s->fd);
    if (ev_is_active(&s->io_read)) {
        ev_io_stop(EV_A_ & s->io_read);
    }
    if (ev_is_active(&s->io_write)) {
        ev_io_stop(EV_A_ & s->io_write);
    }
    close(s->fd);

    while (!UBOND_TAILQ_EMPTY(&s->sent)) {
        ubond_v_pkt_t* l = UBOND_TAILQ_FIRST(&s->sent);
        UBOND_TAILQ_REMOVE(&s->sent, l);
        ubond_v_pkt_release(l);
    }
    while (!UBOND_TAILQ_EMPTY(&s->received)) {
        ubond_pkt_t* l = UBOND_TAILQ_FIRST(&s->received);
        UBOND_TAILQ_REMOVE(&s->received, l);
        ubond_pkt_release(l);
    }
    while (!UBOND_TAILQ_EMPTY(&s->draining)) {
        ubond_pkt_t* l = UBOND_TAILQ_FIRST(&s->draining);
        UBOND_TAILQ_REMOVE(&s->draining, l);
        ubond_pkt_release(l);
    }

    UBOND_TAILQ_REMOVE(&active, s);
    ubond_stream_release(s);

    if (UBOND_TAILQ_LENGTH(&active) < MAXSTREAMS) {
        if (!ev_is_active(&socks_read)) {
            ev_io_start(EV_A_ & socks_read);
        }
    }
}

int paused = 0;
void activate_streams()
{
    if (paused == 0 || ubond_pkt_list_is_full_watermark(&send_buffer))
        return;
    else {
        log_debug("tcp","Activate Streams\n");
        stream_t* l;
        UBOND_TAILQ_FOREACH(l, &active)
        {
            if (l->sent.length < TCP_MAX_OUTSTANDING) {
                ev_io_start(EV_A_ & l->io_read);
            }
        }
        paused = 0;
    }
}

void pause_streams()
{
    log_debug("tcp","Pause Streams\n");
    stream_t* l;
    UBOND_TAILQ_FOREACH(l, &active)
    {
        ev_io_stop(EV_A_ & l->io_read);
    }
    paused = 1;
}

stream_t* find(ubond_pkt_t* pkt)
{
    stream_t* l;
    if (!pkt->p.flow_id)
        return NULL;

    UBOND_TAILQ_FOREACH(l, &active)
    {
        if (l->flow_id == pkt->p.flow_id)
            return l;
    }
    return NULL;
}

void send_pkt_tun(stream_t* s, ubond_pkt_t* pkt, uint16_t type)
{
    pkt->stream = s;

    if (type == UBOND_PKT_TCP_ACK) {
        pkt->p.data_seq = 0;
    } else {
        pkt->p.data_seq = s->data_seq++;
    }

    pkt->p.flow_id = s->flow_id;
    pkt->p.type = type;

    pkt->p.ack_seq = s->seq_to_ack; // stamp the last delivered pack ack
    s->sending++;
    pkt->sending = 1;
    log_debug("tcp", "Sending package %d to tunnel (ack %d type %d len %d)", pkt->p.data_seq, pkt->p.ack_seq, pkt->p.type, pkt->p.len);
    if (type != UBOND_PKT_TCP_ACK) {
        ubond_v_pkt_t* v = ubond_v_pkt_get(pkt);
        UBOND_TAILQ_INSERT_TAIL(&s->sent, v);
        if (s->sent.length >= TCP_MAX_OUTSTANDING) {
            ev_io_stop(EV_A_ & s->io_read);
        }
    }
    ubond_buffer_write(&send_buffer, pkt);
    if (ubond_pkt_list_is_full(&send_buffer)) {
        log_warnx("tcp", "Send buffer is full !");
    }

    //s->resend_timer.repeat = fullrtt();
    //log_debug("tcp", "full rtt %f", fullrtt());
    resend(s);
    if (!ev_is_active(&s->resend_timer)) {
        ev_timer_start(EV_A_ & s->resend_timer);
    }
}

void stamp(stream_t* s)
{
    if (s->stall) {
        log_debug("tcp", "Stalling ACK's %d", s->draining.length);
        return;
    }
    ubond_pkt_t* l;
    UBOND_TAILQ_FOREACH_REVERSE(l, &send_buffer)
    {
        if (l->p.type == UBOND_PKT_TCP_DATA || l->p.type == UBOND_PKT_TCP_ACK) {
            l->p.ack_seq = s->seq_to_ack;
            return;
        }
    }
    ubond_pkt_t* p = ubond_pkt_get();
    p->p.len = 0;
    send_pkt_tun(s, p, UBOND_PKT_TCP_ACK);
}

//we have 3 ways to resend stuff:
//1/ If we have sent more than max_size_outoforder and we dont get an ack - then send the FIRST pack agin
//2/ they see a hole in the tunnel - they ask for packs again
//3/ they see a missing pack - (TCP_MAX_OUTSTANDING behind recieved pack), they will ask for it again
//  do this via an ACK - sending an extra ACK should triggure a resend

static void resend(stream_t* s)
{
    if (ubond_pkt_list_is_full(&hpsend_buffer)) {
        log_warnx("tcp", "HPSend buffer is full for resend!");
        return;
    }

    int to_send = s->sent.length - (max_size_outoforder * 2);
    int i = 0;
    uint64_t now64 = ubond_timestamp64(ev_now(EV_A));
    ubond_v_pkt_t* l;
    uint64_t fullrtt64 = fullrtt() * 1000;

    if (s->sent.length > (max_size_outoforder * 2)) {
        ubond_v_pkt_t* l = UBOND_TAILQ_FIRST(&s->sent);
//        log_debug("tcp", "sending %d last %ul full %ul now %ul = %ld", l->pkt->sending, l->pkt->last_sent, fullrtt64, now64, (int64_t)(l->pkt->last_sent + fullrtt64 - now64));
        if (!l->pkt->sending && (now64 - l->pkt->last_sent > fullrtt64)) {
            l->pkt->last_sent = now64;
            log_debug("tcp", "Resend as we have no ack %d package in sent list", l->pkt->p.data_seq);
            // should check we're not already sending it (slowly?)
            l->pkt->sending = 1;
            s->sending++;
            // were reusing the same packet, so claim it
            l->pkt->usecnt++;
            ubond_buffer_write(&hpsend_buffer, l->pkt);
        }
    }
    if (s->received.length > max_size_outoforder) {
        stamp(s);
    }

#if 0
    UBOND_TAILQ_FOREACH(l, &s->sent)
    {
        if (ubond_pkt_list_is_full(&hpsend_buffer)) {
            log_warnx("tcp", "HPSend buffer is full for resend!");
            break;
        }
        if (i++ > to_send)
            break;
        if (!l->pkt->sending && (int64_t)(l->pkt->last_sent + fullrtt64 - now64) > 0) {

            l->pkt->last_sent = now64;
            log_debug("tcp", "Resend as we have no ack %d package in sent list", l->pkt->p.data_seq);
            // should check we're not already sending it (slowly?)
            l->pkt->sending=1;
            s->sending++;
            // were reusing the same packet, so claim it
            l->pkt->usecnt++;
            ubond_buffer_write(&hpsend_buffer, l->pkt);
        }
    }
#endif
}

static void resend_timer(EV_P_ ev_timer* w, int revents)
{
    stream_t* s = w->data;

    if (s->sent.length == 0) {
        ev_timer_stop(EV_A_ & s->resend_timer);
    }
    s->resend_timer.repeat = fullrtt();
    resend(s);
}

// could be client or server
void ubond_stream_write(ubond_pkt_t* pkt)
{
    log_debug("tcp", "Recieved packet %d (type %d, length %d) from tunnel", pkt->p.data_seq, pkt->p.type, pkt->p.len);

    stream_t* s = find(pkt);
    if (!s) {
        ubond_pkt_release(pkt);
        return;
    }
    pkt->stream = s;

    if (s->sending == 0)
        ev_feed_fd_event(EV_A_ s->fd, EV_READ); // triggure a read event, just in case we got blocked.

    /* first check off the things from the 'sent' queue */
    int acks = 0;
    while (!UBOND_TAILQ_EMPTY(&s->sent)) {
        ubond_v_pkt_t* l = UBOND_TAILQ_FIRST(&s->sent);
        if (l && aoldereqb(l->pkt->p.data_seq, pkt->p.ack_seq)) {
            UBOND_TAILQ_REMOVE(&s->sent, l);
            l->pkt->stream = NULL;
            acks++;
//            log_debug("tcp", "Found ACK'd package %d (ack to %d) in sent list", l->pkt->p.data_seq, pkt->p.ack_seq);
            ubond_v_pkt_release(l);
            if (l->pkt->p.type == UBOND_PKT_TCP_CLOSE) {
                ubond_stream_close(s);
                break;
            }
            if (l->pkt->p.data_seq == pkt->p.ack_seq) {
                break;
            }

        } else {
            if (!acks) {
                log_debug("tcp", "Unable to find ACK %d package in sent list", pkt->p.ack_seq);
                resend(s);
            }
            break;
        }
    }
    if (s->sent.length < TCP_MAX_OUTSTANDING) {
        if (!paused) {
            ev_io_start(EV_A_ & s->io_read);
        }
    }
    if (pkt->p.type != UBOND_PKT_TCP_ACK) {
        /* now insert in the received queue */
        ubond_pkt_t* l;

        if (aolderb(pkt->p.data_seq, s->next_seq)) {
//            log_debug("tcp", "Un-necissary resend %d", pkt->p.data_seq);
            ubond_pkt_release(pkt);
            stamp(s);
            return;
        }
        l = UBOND_TAILQ_FIRST(&s->received);
        if (l && pkt->p.data_seq == l->p.data_seq) {
//            log_debug("tcp", "Un-necissary resend %d", pkt->p.data_seq);
            ubond_pkt_release(pkt);
            stamp(s);
            return;
        }
        if (l && !aolderb(pkt->p.data_seq, l->p.data_seq)) {
            UBOND_TAILQ_FOREACH_REVERSE(l, &s->received)
            {
                if (pkt->p.data_seq == l->p.data_seq) { // replicated packet!
//                    log_debug("tcp", "Un-necissary resend %d", pkt->p.data_seq);
                    ubond_pkt_release(pkt);
                    stamp(s);
                    return;
                }
                if (aolderb(l->p.data_seq, pkt->p.data_seq)) {
                    l = TAILQ_NEXT(l, entry);
                    break;
                }
            }
        }
        if (l) {
            UBOND_TAILQ_INSERT_BEFORE(&s->received, l, pkt);
        } else {
            UBOND_TAILQ_INSERT_TAIL(&s->received, pkt);
        }
        log_debug("tcp", "Insert %d (length now %d)", pkt->p.data_seq, s->received.length);
    } else {
        ubond_pkt_release(pkt);
    }
    /* drain */
    int drained = 0;
    while (!UBOND_TAILQ_EMPTY(&s->received) && (UBOND_TAILQ_FIRST(&s->received)->p.data_seq == s->next_seq)) {

        ubond_pkt_t* l = UBOND_TAILQ_FIRST(&s->received);
        UBOND_TAILQ_REMOVE(&s->received, l);

        s->seq_to_ack = l->p.data_seq;
        s->next_seq = s->seq_to_ack + 1;

        if (l->p.type == UBOND_PKT_TCP_CLOSE) {
            ubond_pkt_release(l);
            ubond_stream_close(s);
            drained++;
            break;
        }

        if (l->p.len > 0) {
            l->sent = 0;
            UBOND_TAILQ_INSERT_TAIL(&s->draining, l);
//            log_debug("tcp", "drain packet %d", l->p.data_seq);
            drained++;
        } else {
            ubond_pkt_release(l);
        }
    }
    if (drained) {
        if (!ev_is_active(&s->io_write)) {
            ev_io_start(EV_A_ & s->io_write);
        }
        ev_feed_fd_event(EV_A_ s->fd, EV_WRITE);
    }
    if (s->draining.length > 1000) {
        log_debug("tcp", "Stalling due to full drain buffer");
        s->stall = 1;
    }
    if (drained || s->received.length > max_size_outoforder) {
        stamp(s);
    }
}

// called once the packet is sent
void tcp_sent(stream_t* s, ubond_pkt_t* pkt)
{
    if (s->sending > 0)
        s->sending--;
    pkt->sending = 0;
}

//send recieved packets back on the socket
// could be server or client side
static void on_write_cb(EV_P_ struct ev_io* ev, int revents)
{
    stream_t* s = (stream_t*)ev->data;

    /* drain */
    log_debug("tcp", "write cb");
    // while?
    if (!UBOND_TAILQ_EMPTY(&s->draining)) {
        ubond_pkt_t* l = UBOND_TAILQ_FIRST(&s->draining);
        ssize_t ret = send(s->fd, &(l->p.data[l->sent]), l->p.len - l->sent, MSG_DONTWAIT|MSG_DONTROUTE);
        if (ret > 0)
            l->sent += ret;
        if (ret < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                log_warn("tcp", "write error: %zd/%d bytes sent (closing stream) ", ret, l->p.len);
                ubond_pkt_t* p = ubond_pkt_get();
                p->p.len = 0;
                send_pkt_tun(s, p, UBOND_PKT_TCP_CLOSE);
                //ubond_stream_close(s); // is this safe? it will drain all pkts including this one
            }
            return;
        }
        if (l->sent >= l->p.len) {
//            log_debug("tcp", "drained %d", l->p.data_seq);
            UBOND_TAILQ_REMOVE(&s->draining, l);
            ubond_pkt_release(l);

            if (s->draining.length < 1000) {
                s->stall = 0;
            }
        }
    }
    if (UBOND_TAILQ_EMPTY(&s->draining)) {
        log_debug("tcp", "Stopping io_write");
        if (ev_is_active(&s->io_write)) {
            ev_io_stop(EV_A_ & s->io_write);
        }
    }
}

//recieve a packet and set it up to be sent
// could be server or client side
ubond_pkt_t* sock_spair = NULL;
static void on_read_cb(EV_P_ struct ev_io* ev, int revents)
{
    stream_t* s = (stream_t*)ev->data;
    ssize_t rv;
    do {
        if (ubond_pkt_list_is_full(&send_buffer))
            break;
        if (!sock_spair)
            sock_spair = ubond_pkt_get();
        ubond_pkt_t* pkt = sock_spair;
        rv = recv(ev->fd, &pkt->p.data, ubond_options.mtu, MSG_DONTWAIT);
        if (rv < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                log_warn("sock", "stream closing ");
                pkt->p.len = 0;
                send_pkt_tun(s, pkt, UBOND_PKT_TCP_CLOSE);
                sock_spair = NULL;
                break;
            }
        }
        if (rv > 0) {
            pkt->p.len = rv;
            send_pkt_tun(s, pkt, UBOND_PKT_TCP_DATA);
            sock_spair = NULL;
        } else {
            break;
        }
    } while (0);

    if (ubond_pkt_list_is_full_watermark(&send_buffer)) {
        pause_streams();
    }
}

static void on_accept_cb(EV_P_ struct ev_io* ev, int revents)
{
    struct sockaddr cliaddr;
    socklen_t clilen = sizeof(cliaddr);

    if (ubond_pkt_list_is_full(&hpsend_buffer)) {
        log_warnx("sock", "Unable to proccess accept into HP send buffer");
        return;
    }

    /* libev is level-triggering so don't have to loop accept */
    int fd = accept(ev->fd, (struct sockaddr*)&cliaddr, &clilen);
    getsockname(fd, (struct sockaddr*)&cliaddr, &clilen);
    if (fd < 0)
        return;
    setnonblock(fd);

    log_info("socks", "New stream addr %u port %s (FD:%d)", ntohs(((struct sockaddr_in*)&cliaddr)->sin_port), inet_ntoa(((struct sockaddr_in*)&cliaddr)->sin_addr), fd);

    stream_t* s;
    s = ubond_stream_get(fd);
    s->flow_id = s->preset_flow_id; // WE set the flowid
    UBOND_TAILQ_INSERT_TAIL(&active, s);
    if (!paused)
        ev_io_start(EV_A_ & s->io_read);

    ubond_pkt_t* pkt = ubond_pkt_get();
    struct sockaddr* d = (struct sockaddr*)(pkt->p.data);
    *d = cliaddr;

    pkt->p.len = sizeof(struct sockaddr);
    pkt->p.flow_id = s->flow_id;
    pkt->p.data_seq = 0;
    pkt->p.type = UBOND_PKT_TCP_OPEN;
    ubond_buffer_write(&hpsend_buffer, pkt);
}

void socks_init()
{
    UBOND_TAILQ_INIT(&s_pool);
    ubond_stream_list_init(&active, MAXSTREAMS);

    short bindport = ubond_options.tcp_socket;
    if (!bindport) {
        log_warnx("socks", "No TCP tunnel : (config tcp_socket set to 0)");
        return;
    }

    int serverfd = priv_set_socket_transparent(bindport);

    ev_io_init(&socks_read, on_accept_cb, serverfd, EV_READ);
    ev_io_start(EV_A_ & socks_read);
    log_info("socks", "TCP Socket tunnel up on port %d", bindport);
    return;
}

static int setnonblock(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        return -1;
    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0)
        return -1;

    return 0;
}

/*
 *  server side
*/

void ubond_socks_init(ubond_pkt_t* pkt)
{
    struct sockaddr* rp = (struct sockaddr*)(pkt->p.data);

    log_debug("tcp", "New socket request");

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        log_warn("sock", "Unable to open socket ");
        return;
    }
    int r = connect(fd, rp, sizeof(struct sockaddr));
    if (r < 0) {
        log_warn("sock", "Unable to connect socket fd:%d  ip:%s port:%d",
            fd, inet_ntoa(((struct sockaddr_in*)rp)->sin_addr), ntohs(((struct sockaddr_in*)rp)->sin_port));
        close(fd);
        return;
    }

    stream_t* s;
    s = ubond_stream_get(fd);
    s->flow_id = pkt->p.flow_id; // THEY set the flowid
    UBOND_TAILQ_INSERT_TAIL(&active, s);
    if (!paused)
        ev_io_start(EV_A_ & s->io_read);
}
