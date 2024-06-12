/*
 * Copyright (C) Ivan Pavlov
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

typedef struct ngx_stream_upstream_sct_neuro_peer_s   ngx_stream_upstream_sct_neuro_peer_t;

struct ngx_stream_upstream_sct_neuro_peer_s {
    struct sockaddr                 *sockaddr;
    socklen_t                        socklen;
    ngx_str_t                        name;
    ngx_str_t                        server;

    ngx_uint_t                       conns;
    ngx_uint_t                       max_conns;

    ngx_uint_t                       fails;
    time_t                           accessed;
    time_t                           checked;

    ngx_uint_t                       max_fails;
    time_t                           fail_timeout;
    ngx_msec_t                       slow_start;
    ngx_msec_t                       start_time;

    ngx_uint_t                       down;

    void                            *ssl_session;
    int                              ssl_session_len;

    ngx_uint_t                      cnt_requests;
    ngx_uint_t                      cnt_responses;
    double                          neuro_weight;

#if (NGX_STREAM_UPSTREAM_ZONE)
    ngx_atomic_t                     lock;
#endif

    ngx_stream_upstream_sct_neuro_peer_t   *next;

    NGX_COMPAT_BEGIN(25)
    NGX_COMPAT_END
};


typedef struct ngx_stream_upstream_sct_neuro_peers_s  ngx_stream_upstream_sct_neuro_peers_t;

struct ngx_stream_upstream_sct_neuro_peers_s {
    ngx_uint_t                       number;

#if (NGX_STREAM_UPSTREAM_ZONE)
    ngx_slab_pool_t                 *shpool;
    ngx_atomic_t                     rwlock;
    ngx_stream_upstream_sct_neuro_peers_t  *zone_next;
#endif

    ngx_uint_t                       tries;

    unsigned                         single:1;

    ngx_str_t                       *name;

    ngx_stream_upstream_sct_neuro_peers_t  *next;

    ngx_stream_upstream_sct_neuro_peer_t   *peer;
};

typedef struct {
    ngx_uint_t                       config;
    ngx_stream_upstream_sct_neuro_peers_t  *peers;
    ngx_stream_upstream_sct_neuro_peer_t   *current;
    uintptr_t                        data;
} ngx_stream_upstream_sct_neuro_peer_data_t;

typedef struct {
    ngx_str_t                                addr;
    uintptr_t                                peers;
    ngx_atomic_t                             lock;
    ngx_uint_t                               nreq;
    ngx_uint_t                               nres;
    ngx_uint_t                               fails;
} ngx_stream_upstream_sct_neuro_shm_block_t;

#define ngx_spinlock_unlock(lock)       (void) ngx_atomic_cmp_set(lock, ngx_pid, 0)

static ngx_int_t ngx_stream_upstream_init_sct_neuro_peer(
    ngx_stream_session_t *s, ngx_stream_upstream_srv_conf_t *us);
static ngx_int_t ngx_stream_upstream_get_sct_neuro_peer(
    ngx_peer_connection_t *pc, void *data);
static ngx_stream_upstream_sct_neuro_peer_t *ngx_stream_upstream_get_peer_from_neuro(
    ngx_stream_upstream_sct_neuro_peer_data_t *rrp);
static char *ngx_stream_upstream_sct_neuro(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

static char *ngx_stream_upstream_sct_neuro_set_shm_size(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);   
static char *ngx_stream_upstream_sct_neuro_set_gap_in_requests(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);   

ngx_uint_t                              nreq_since_last_weight_update_stream = 0;

static ngx_command_t  ngx_stream_upstream_sct_neuro_commands[] = {

    { ngx_string("sct_neuro"),
      NGX_STREAM_UPS_CONF|NGX_CONF_NOARGS,
      ngx_stream_upstream_sct_neuro,
      0,
      0,
      NULL },
    
    { ngx_string("upstream_sct_neuro_shm_size"),
      NGX_STREAM_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_stream_upstream_sct_neuro_set_shm_size,
      0,
      0,
      NULL },

    { ngx_string("upstream_sct_neuro_gap_in_requests"),
      NGX_STREAM_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_stream_upstream_sct_neuro_set_gap_in_requests,
      0,
      0,
      NULL },

      ngx_null_command
};

static ngx_uint_t       ngx_stream_upstream_sct_neuro_gap_in_requests; 

static ngx_uint_t       ngx_stream_upstream_sct_neuro_shm_size;
static ngx_shm_zone_t  *ngx_stream_upstream_sct_neuro_shm_zone;

static ngx_stream_module_t  ngx_stream_upstream_sct_neuro_module_ctx = {
    NULL,                                    /* preconfiguration */
    NULL,                                    /* postconfiguration */

    NULL,                                    /* create main configuration */
    NULL,                                    /* init main configuration */

    NULL,                                    /* create server configuration */
    NULL                                     /* merge server configuration */
};

static ngx_int_t
ngx_stream_upstream_sct_neuro_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_slab_pool_t *shpool;
    ngx_stream_upstream_sct_neuro_shm_block_t *blocks;
    ngx_uint_t i, j, num_blocks;
    ngx_stream_upstream_srv_conf_t *uscf = shm_zone->data;
    
    if (data) {
        shm_zone->data = data;
        return NGX_OK;
    }

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    num_blocks = 0;
    ngx_stream_upstream_server_t *server = uscf->servers->elts;
    for (i = 0; i < uscf->servers->nelts; i++) {
        num_blocks += server[i].naddrs;
    }

    blocks = ngx_slab_alloc(shpool, sizeof(ngx_stream_upstream_sct_neuro_shm_block_t) * num_blocks);
    if (blocks == NULL) {
        return NGX_ERROR;
    }

    ngx_uint_t block_index = 0;
    for (i = 0; i < uscf->servers->nelts; i++) {
        for (j = 0; j < server[i].naddrs; j++) {
            ngx_memzero(&blocks[block_index], sizeof(ngx_stream_upstream_sct_neuro_shm_block_t));
            blocks[block_index].addr = server[i].addrs[j].name;
            block_index++;
        }
    }

    shm_zone->data = blocks;

    return NGX_OK;
}

static char *
ngx_stream_upstream_sct_neuro_set_shm_size(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ssize_t new_shm_size;
    ngx_str_t *value;

    value = cf->args->elts;

    new_shm_size = ngx_parse_size(&value[1]);                                           
    if (new_shm_size == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Invalid memory area size `%V'", &value[1]);
        return NGX_CONF_ERROR;
    }

    new_shm_size = ngx_align(new_shm_size, ngx_pagesize);                               

    if (new_shm_size < 8 * (ssize_t) ngx_pagesize) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0, "The upstream_sct_neuro_shm_size value must be at least %udKiB", (8 * ngx_pagesize) >> 10);
        new_shm_size = 8 * ngx_pagesize;
    }

    if (ngx_stream_upstream_sct_neuro_shm_size &&
        ngx_stream_upstream_sct_neuro_shm_size != (ngx_uint_t) new_shm_size) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0, "Cannot change memory area size without restart, ignoring change");
    } else {
        ngx_stream_upstream_sct_neuro_shm_size = new_shm_size;                                 
    }

    ngx_conf_log_error(NGX_LOG_DEBUG, cf, 0, "Using %udKiB of shared memory for upstream_sct_neuro", new_shm_size >> 10);

    return NGX_CONF_OK;
}

static char *
ngx_stream_upstream_sct_neuro_set_gap_in_requests(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ssize_t new_gap_in_requests;
    ngx_str_t *value;

    value = cf->args->elts;
    
    new_gap_in_requests = ngx_atoi(value[1].data, value[1].len);
    if (new_gap_in_requests == NGX_ERROR || new_gap_in_requests < 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Invalid gap in requests `%V'", &value[1]);
        return NGX_CONF_ERROR;
    }

    ngx_stream_upstream_sct_neuro_gap_in_requests = new_gap_in_requests;

    return NGX_CONF_OK;
}

ngx_module_t  ngx_stream_upstream_sct_neuro_module = {
    NGX_MODULE_V1,
    &ngx_stream_upstream_sct_neuro_module_ctx,  /* module context */
    ngx_stream_upstream_sct_neuro_commands,     /* module directives */
    NGX_STREAM_MODULE,                          /* module type */
    NULL,                                       /* init master */
    NULL,                                       /* init module */
    NULL,                                       /* init process */
    NULL,                                       /* init thread */
    NULL,                                       /* exit thread */
    NULL,                                       /* exit process */
    NULL,                                       /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t
ngx_stream_upstream_init_sct_neuro(ngx_conf_t *cf,
    ngx_stream_upstream_srv_conf_t *us)
{
    ngx_str_t shm_name = ngx_string("sct_neuro_stream");
    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, cf->log, 0,
                   "init sct neuro");

    // if (ngx_stream_upstream_init_round_robin(cf, us) != NGX_OK) {
    //     return NGX_ERROR;
    // }

/*Раскрываем функцию выше*/
    ngx_url_t                        u;
    ngx_uint_t                       i, j, n, w, t;
    ngx_stream_upstream_server_t    *server;
    ngx_stream_upstream_sct_neuro_peer_t   *peer, **peerp;
    ngx_stream_upstream_sct_neuro_peers_t  *peers;

    us->peer.init = ngx_stream_upstream_init_sct_neuro_peer;

    if (us->servers) {
        server = us->servers->elts;

        n = 0;
        w = 0;
        t = 0;

        for (i = 0; i < us->servers->nelts; i++) {
            if (server[i].backup) {
                continue;
            }

            n += server[i].naddrs;
            w += server[i].naddrs * server[i].weight;

            if (!server[i].down) {
                t += server[i].naddrs;
            }
        }

        if (n == 0) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                          "no servers in upstream \"%V\" in %s:%ui",
                          &us->host, us->file_name, us->line);
            return NGX_ERROR;
        }

        peers = ngx_pcalloc(cf->pool, sizeof(ngx_stream_upstream_sct_neuro_peers_t));
        if (peers == NULL) {
            return NGX_ERROR;
        }

        peer = ngx_pcalloc(cf->pool, sizeof(ngx_stream_upstream_sct_neuro_peer_t) * n);
        if (peer == NULL) {
            return NGX_ERROR;
        }

        peers->single = (n == 1);
        peers->number = n;
        peers->tries = t;
        peers->name = &us->host;

        n = 0;
        peerp = &peers->peer;

        for (i = 0; i < us->servers->nelts; i++) {
            if (server[i].backup) {
                continue;
            }

            for (j = 0; j < server[i].naddrs; j++) {
                peer[n].sockaddr = server[i].addrs[j].sockaddr;
                peer[n].socklen = server[i].addrs[j].socklen;
                peer[n].name = server[i].addrs[j].name;
                peer[n].max_conns = server[i].max_conns;
                peer[n].max_fails = server[i].max_fails;
                peer[n].fail_timeout = server[i].fail_timeout;
                peer[n].down = server[i].down;
                peer[n].server = server[i].name;

                peer[n].cnt_requests = 0;
                peer[n].cnt_responses = 0;
                peer[n].neuro_weight = 0.0;

                *peerp = &peer[n];
                peerp = &peer[n].next;
                n++;
            }
        }

        us->peer.data = peers;

    } else {

        /* an upstream implicitly defined by proxy_pass, etc. */

        if (us->port == 0) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                        "no port in upstream \"%V\" in %s:%ui",
                        &us->host, us->file_name, us->line);
            return NGX_ERROR;
        }

        ngx_memzero(&u, sizeof(ngx_url_t));

        u.host = us->host;
        u.port = us->port;

        if (ngx_inet_resolve_host(cf->pool, &u) != NGX_OK) {
            if (u.err) {
                ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                            "%s in upstream \"%V\" in %s:%ui",
                            u.err, &us->host, us->file_name, us->line);
            }

            return NGX_ERROR;
        }

        n = u.naddrs;

        peers = ngx_pcalloc(cf->pool, sizeof(ngx_stream_upstream_sct_neuro_peers_t));
        if (peers == NULL) {
            return NGX_ERROR;
        }

        peer = ngx_pcalloc(cf->pool, sizeof(ngx_stream_upstream_sct_neuro_peer_t) * n);
        if (peer == NULL) {
            return NGX_ERROR;
        }

        peers->single = (n == 1);
        peers->number = n;
        peers->tries = n;
        peers->name = &us->host;

        peerp = &peers->peer;

        for (i = 0; i < u.naddrs; i++) {
            peer[i].sockaddr = u.addrs[i].sockaddr;
            peer[i].socklen = u.addrs[i].socklen;
            peer[i].name = u.addrs[i].name;
            peer[i].max_conns = 0;
            peer[i].max_fails = 1;
            peer[i].fail_timeout = 10;
            *peerp = &peer[i];
            peerp = &peer[i].next;
        }

        us->peer.data = peers;

        /* implicitly defined upstream has no backup servers */
    }
/*Закрываем функцию выше*/

    us->peer.init = ngx_stream_upstream_init_sct_neuro_peer;

    ngx_shm_zone_t *shm_zone = ngx_shared_memory_add(cf, &shm_name, 
        ngx_stream_upstream_sct_neuro_shm_size, &ngx_stream_upstream_sct_neuro_module);
    if (shm_zone == NULL) {
        return NGX_ERROR;
    }

    shm_zone->data = us;
    shm_zone->init = ngx_stream_upstream_sct_neuro_init_shm_zone;
    ngx_stream_upstream_sct_neuro_shm_zone = shm_zone;

    return NGX_OK;
}

static ngx_int_t
ngx_stream_upstream_init_sct_neuro_peer(ngx_stream_session_t *s,
    ngx_stream_upstream_srv_conf_t *us)
{
    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                   "init sct neuro peer");

    // if (ngx_stream_upstream_init_round_robin_peer(s, us) != NGX_OK) {
    //     return NGX_ERROR;
    // }

/*Раскрываем функцию выше*/

    ngx_stream_upstream_sct_neuro_peer_data_t  *rrp;

    rrp = s->upstream->peer.data;

    if (rrp == NULL) {
        rrp = ngx_palloc(s->connection->pool,
                         sizeof(ngx_stream_upstream_sct_neuro_peer_data_t));
        if (rrp == NULL) {
            return NGX_ERROR;
        }

        s->upstream->peer.data = rrp;
    }

    rrp->peers = us->peer.data;
    rrp->current = NULL;
    rrp->config = 0;

    // n = rrp->peers->number;

    // if (rrp->peers->next && rrp->peers->next->number > n) {
    //     n = rrp->peers->next->number;
    // }

    // if (n <= 8 * sizeof(uintptr_t)) {
    //     rrp->tried = &rrp->data;
    //     rrp->data = 0;

    // } else {
    //     n = (n + (8 * sizeof(uintptr_t) - 1)) / (8 * sizeof(uintptr_t));

    //     rrp->tried = ngx_pcalloc(s->connection->pool, n * sizeof(uintptr_t));
    //     if (rrp->tried == NULL) {
    //         return NGX_ERROR;
    //     }
    // }

    s->upstream->peer.get = ngx_stream_upstream_get_sct_neuro_peer;
    // s->upstream->peer.free = ngx_stream_upstream_free_round_robin_peer;
    // s->upstream->peer.notify = ngx_stream_upstream_notify_round_robin_peer;
    // s->upstream->peer.tries = ngx_stream_upstream_tries(rrp->peers);
#if (NGX_STREAM_SSL)
    s->upstream->peer.set_session =
                             ngx_stream_upstream_set_round_robin_peer_session;
    s->upstream->peer.save_session =
                             ngx_stream_upstream_save_round_robin_peer_session;
#endif

/*Закрываем функцию выше*/

    // s->upstream->peer.get = ngx_stream_upstream_get_sct_neuro_peer;

    return NGX_OK;
}

static ngx_int_t
ngx_stream_upstream_get_sct_neuro_peer(ngx_peer_connection_t *pc, void *data)
{
    // ngx_stream_upstream_sct_neuro_peer_data_t *rrp = data;

    // return ngx_stream_upstream_get_round_robin_peer(pc, rrp);

/*Раскрываем функцию выше*/

    ngx_stream_upstream_sct_neuro_peer_data_t *rrp = data;

    ngx_int_t                        rc;
    // ngx_uint_t                       i;
    ngx_stream_upstream_sct_neuro_peer_t   *peer;
    ngx_stream_upstream_sct_neuro_peers_t  *peers;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, pc->log, 0,
                   "get rr peer, try: %ui", pc->tries);

    pc->connection = NULL;

    peers = rrp->peers;
    ngx_stream_upstream_rr_peers_wlock(peers);

    if (peers->single) {
        peer = peers->peer;

        if (peer->down) {
            goto failed;
        }

        if (peer->max_conns && peer->conns >= peer->max_conns) {
            goto failed;
        }

        rrp->current = peer;

    } else {

        /* there are several peers */

        peer = ngx_stream_upstream_get_peer_from_neuro(rrp);

        if (peer == NULL) {
            goto failed;
        }

        // ngx_log_debug2(NGX_LOG_DEBUG_STREAM, pc->log, 0,
        //                "get rr peer, current: %p %i",
        //                peer, peer->current_weight);
    }

    pc->sockaddr = peer->sockaddr;
    pc->socklen = peer->socklen;
    pc->name = &peer->name;

    peer->conns++;

    ngx_stream_upstream_rr_peers_unlock(peers);

    return NGX_OK;

failed:

    if (peers->next) {

        ngx_log_debug0(NGX_LOG_DEBUG_STREAM, pc->log, 0, "backup servers");

        rrp->peers = peers->next;

        // n = (rrp->peers->number + (8 * sizeof(uintptr_t) - 1))
        //         / (8 * sizeof(uintptr_t));

        // for (i = 0; i < n; i++) {
        //     rrp->tried[i] = 0;
        // }

        ngx_stream_upstream_rr_peers_unlock(peers);

        rc = ngx_stream_upstream_get_sct_neuro_peer(pc, rrp);

        if (rc != NGX_BUSY) {
            return rc;
        }

        ngx_stream_upstream_rr_peers_wlock(peers);
    }

    ngx_stream_upstream_rr_peers_unlock(peers);

    pc->name = peers->name;

    return NGX_BUSY;

/*Закрываем функцию выше*/
}

/*Новая функция так как она используется при раскрытии сверху*/

static ngx_stream_upstream_sct_neuro_peer_t *
ngx_stream_upstream_get_peer_from_neuro(ngx_stream_upstream_sct_neuro_peer_data_t *rrp)
{
    int                                     sock;
    time_t                                  now;
    struct sockaddr_in                      server;
    ngx_uint_t                              i, num_blocks;
    ngx_stream_upstream_sct_neuro_peer_t    *peer, *best;
    struct hostent                          *host;
    
    now = ngx_time();
    best = NULL;

    ngx_stream_upstream_sct_neuro_shm_block_t *blocks;
    ngx_stream_upstream_sct_neuro_shm_block_t *block = NULL;

    blocks = (ngx_stream_upstream_sct_neuro_shm_block_t *) ngx_stream_upstream_sct_neuro_shm_zone->data;
    num_blocks = ngx_stream_upstream_sct_neuro_shm_size / sizeof(ngx_stream_upstream_sct_neuro_shm_block_t);

    for (peer = rrp->peers->peer, i = 0;
         peer;
         peer = peer->next, i++)
    {
        // n = i / (8 * sizeof(uintptr_t));
        // m = (uintptr_t) 1 << i % (8 * sizeof(uintptr_t));

        // if (rrp->tried[n] & m) {
        //     continue;
        // }

        for (i = 0; i < num_blocks; i++) {
            if (ngx_strcmp(blocks[i].addr.data, peer->name.data) == 0) {
                block = &blocks[i];
                break;
            }
        }
        if (!block) {
            continue;
        }

        ngx_spinlock(&block->lock, ngx_pid, 1024);

        if (peer->down) {
            ngx_spinlock_unlock(&block->lock);
            continue;
        }

        if (peer->max_fails
            && peer->fails >= peer->max_fails
            && now - peer->checked <= peer->fail_timeout)
        {
            ngx_spinlock_unlock(&block->lock);
            continue;
        }

        if (peer->max_conns && peer->conns >= peer->max_conns) {
            ngx_spinlock_unlock(&block->lock);
            continue;
        }

        // peer->current_weight += peer->effective_weight;
        // total += peer->effective_weight;

        // if (peer->effective_weight < peer->weight) {
        //     peer->effective_weight++;
        // }

        peer->cnt_requests = block->nreq;
        peer->cnt_responses = block->nres;

        if (best == NULL) {
            best = peer;
        }
        ngx_spinlock_unlock(&block->lock);
    }

    if (nreq_since_last_weight_update_stream >= ngx_stream_upstream_sct_neuro_gap_in_requests) {
        nreq_since_last_weight_update_stream = 0;
    } else {
        nreq_since_last_weight_update_stream++;
    }
    if (nreq_since_last_weight_update_stream == 0) {
        int cnt_req_and_res[rrp->peers->number * 2];

        ngx_spinlock(&block->lock, ngx_pid, 1024);
        for (peer = rrp->peers->peer, i = 0;
            peer;
            peer = peer->next, i += 2)
        {
            cnt_req_and_res[i] = peer->cnt_requests;
            cnt_req_and_res[i + 1] = peer->cnt_responses;
        }

        host = gethostbyname("recalculator");
        sock = socket(AF_INET, SOCK_STREAM, 0);
        memset(&server, 0, sizeof(server));
        server.sin_family = AF_INET;
        server.sin_port = htons(7998);
        memcpy(&server.sin_addr.s_addr, host->h_addr_list[0], host->h_length);
        connect(sock, (struct sockaddr *)&server, sizeof(server));
        uint32_t len = htonl(sizeof(cnt_req_and_res));
        send(sock, &len, sizeof(len), 0);
        send(sock, cnt_req_and_res, sizeof(cnt_req_and_res), 0);
        int response[rrp->peers->number];
        recv(sock, response, sizeof(response), 0);
        ngx_spinlock_unlock(&block->lock);
        
        for (peer = rrp->peers->peer, i = 0;
            peer;
            peer = peer->next, i++)
        {
            peer->neuro_weight = response[i];
        }    
    }  

    // choose best peer
    for (peer = rrp->peers->peer, i = 0;
         peer;
         peer = peer->next, i++)
    {
        if (peer->neuro_weight > best->neuro_weight) {
            best = peer;
        }
    }

    for (i = 0; i < num_blocks; i++) {
        if (ngx_strcmp(blocks[i].addr.data, best->name.data) == 0) {
            block = &blocks[i];
            ngx_spinlock(&block->lock, ngx_pid, 1024);
            block->nreq++;
            ngx_spinlock_unlock(&block->lock);
            break;
        }
    }

    if (best == NULL) {
        return NULL;
    }

    rrp->current = best;

    // n = p / (8 * sizeof(uintptr_t));
    // m = (uintptr_t) 1 << p % (8 * sizeof(uintptr_t));

    // rrp->tried[n] |= m;

    // best->current_weight -= total;

    if (now - best->checked > best->fail_timeout) {
        best->checked = now;
    }

    return best;
}

/*Конец новой функции*/

static char *
ngx_stream_upstream_sct_neuro(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_stream_upstream_srv_conf_t  *uscf;

    uscf = ngx_stream_conf_get_module_srv_conf(cf, ngx_stream_upstream_module);

    if (uscf->peer.init_upstream) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                           "load balancing method redefined");
    }

    uscf->peer.init_upstream = ngx_stream_upstream_init_sct_neuro;

    uscf->flags = NGX_STREAM_UPSTREAM_CREATE
                  |NGX_STREAM_UPSTREAM_MAX_CONNS
                  |NGX_STREAM_UPSTREAM_MAX_FAILS
                  |NGX_STREAM_UPSTREAM_FAIL_TIMEOUT
                  |NGX_STREAM_UPSTREAM_DOWN;

    return NGX_CONF_OK;
}


typedef struct {
    ngx_chain_t  *from_upstream;
    ngx_chain_t  *from_downstream;
} ngx_stream_sct_neuro_filter_ctx_t;

static ngx_int_t ngx_stream_sct_neuro_filter(ngx_stream_session_t *s,
    ngx_chain_t *in, ngx_uint_t from_upstream);
static ngx_int_t ngx_stream_sct_neuro_filter_init(ngx_conf_t *cf);

static ngx_stream_module_t  ngx_stream_sct_neuro_filter_module_ctx = {
    NULL,                                     /* preconfiguration */
    ngx_stream_sct_neuro_filter_init,         /* postconfiguration */

    NULL,                                     /* create main configuration */
    NULL,                                     /* init main configuration */

    NULL,                                     /* create server configuration */
    NULL                                      /* merge server configuration */
};

ngx_module_t  ngx_stream_sct_neuro_filter_module = {
    NGX_MODULE_V1,
    &ngx_stream_sct_neuro_filter_module_ctx,  /* module context */
    NULL,                                     /* module directives */
    NGX_STREAM_MODULE,                        /* module type */
    NULL,                                     /* init master */
    NULL,                                     /* init module */
    NULL,                                     /* init process */
    NULL,                                     /* init thread */
    NULL,                                     /* exit thread */
    NULL,                                     /* exit process */
    NULL,                                     /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t
ngx_stream_sct_neuro_filter(ngx_stream_session_t *s, ngx_chain_t *in,
    ngx_uint_t from_upstream)
{
    off_t                           size;
    ngx_uint_t                      last, flush, sync;
    ngx_chain_t                    *cl, *ln, **ll, **out, *chain;
    ngx_connection_t               *c;
    ngx_stream_sct_neuro_filter_ctx_t  *ctx;
    // ngx_int_t                      rc;
    // ngx_chain_t                   *cl;
    // ngx_connection_t              *c;
    // ngx_stream_sct_neuro_filter_ctx_t *ctx;
    ngx_stream_upstream_sct_neuro_shm_block_t *blocks;
    ngx_stream_upstream_sct_neuro_shm_block_t *block = NULL;
    ngx_uint_t i, num_blocks;
    ngx_atomic_t *lock;

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_sct_neuro_filter_module);

    if (ctx == NULL) {
        ctx = ngx_pcalloc(s->connection->pool,
                          sizeof(ngx_stream_sct_neuro_filter_ctx_t));
        if (ctx == NULL) {
            return NGX_ERROR;
        }

        ngx_stream_set_ctx(s, ctx, ngx_stream_sct_neuro_filter_module);
    }

    if (from_upstream) {
        c = s->connection;
        out = &ctx->from_upstream;

    } else {
        c = s->upstream->peer.connection;
        out = &ctx->from_downstream;
    }

    if (c->error) {
        return NGX_ERROR;
    }

    // ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
    //                    "upstream addr: %s",
    //                    s->upstream->upstream->host.data);

    size = 0;
    flush = 0;
    sync = 0;
    last = 0;
    ll = out;

    /* find the size, the flush point and the last link of the saved chain */

    for (cl = *out; cl; cl = cl->next) {
        ll = &cl->next;

        ngx_log_debug7(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "sct_neuro old buf t:%d f:%d %p, pos %p, size: %z "
                       "file: %O, size: %O",
                       cl->buf->temporary, cl->buf->in_file,
                       cl->buf->start, cl->buf->pos,
                       cl->buf->last - cl->buf->pos,
                       cl->buf->file_pos,
                       cl->buf->file_last - cl->buf->file_pos);

        if (ngx_buf_size(cl->buf) == 0 && !ngx_buf_special(cl->buf)) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "zero size buf in writer "
                          "t:%d r:%d f:%d %p %p-%p %p %O-%O",
                          cl->buf->temporary,
                          cl->buf->recycled,
                          cl->buf->in_file,
                          cl->buf->start,
                          cl->buf->pos,
                          cl->buf->last,
                          cl->buf->file,
                          cl->buf->file_pos,
                          cl->buf->file_last);

            ngx_debug_point();
            return NGX_ERROR;
        }

        if (ngx_buf_size(cl->buf) < 0) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "negative size buf in writer "
                          "t:%d r:%d f:%d %p %p-%p %p %O-%O",
                          cl->buf->temporary,
                          cl->buf->recycled,
                          cl->buf->in_file,
                          cl->buf->start,
                          cl->buf->pos,
                          cl->buf->last,
                          cl->buf->file,
                          cl->buf->file_pos,
                          cl->buf->file_last);

            ngx_debug_point();
            return NGX_ERROR;
        }

        size += ngx_buf_size(cl->buf);

        if (cl->buf->flush || cl->buf->recycled) {
            flush = 1;
        }

        if (cl->buf->sync) {
            sync = 1;
        }

        if (cl->buf->last_buf) {
            last = 1;
        }
    }

    /* add the new chain to the existent one */

    for (ln = in; ln; ln = ln->next) {
        cl = ngx_alloc_chain_link(c->pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        cl->buf = ln->buf;
        *ll = cl;
        ll = &cl->next;

        ngx_log_debug7(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "write new buf t:%d f:%d %p, pos %p, size: %z "
                       "file: %O, size: %O",
                       cl->buf->temporary, cl->buf->in_file,
                       cl->buf->start, cl->buf->pos,
                       cl->buf->last - cl->buf->pos,
                       cl->buf->file_pos,
                       cl->buf->file_last - cl->buf->file_pos);

        if (ngx_buf_size(cl->buf) == 0 && !ngx_buf_special(cl->buf)) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "zero size buf in writer "
                          "t:%d r:%d f:%d %p %p-%p %p %O-%O",
                          cl->buf->temporary,
                          cl->buf->recycled,
                          cl->buf->in_file,
                          cl->buf->start,
                          cl->buf->pos,
                          cl->buf->last,
                          cl->buf->file,
                          cl->buf->file_pos,
                          cl->buf->file_last);

            ngx_debug_point();
            return NGX_ERROR;
        }

        if (ngx_buf_size(cl->buf) < 0) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "negative size buf in writer "
                          "t:%d r:%d f:%d %p %p-%p %p %O-%O",
                          cl->buf->temporary,
                          cl->buf->recycled,
                          cl->buf->in_file,
                          cl->buf->start,
                          cl->buf->pos,
                          cl->buf->last,
                          cl->buf->file,
                          cl->buf->file_pos,
                          cl->buf->file_last);

            ngx_debug_point();
            return NGX_ERROR;
        }

        size += ngx_buf_size(cl->buf);

        if (cl->buf->flush || cl->buf->recycled) {
            flush = 1;
        }

        if (cl->buf->sync) {
            sync = 1;
        }

        if (cl->buf->last_buf) {
            last = 1;
        }
    }

    if (last) {
        blocks = (ngx_stream_upstream_sct_neuro_shm_block_t *) ngx_stream_upstream_sct_neuro_shm_zone->data;
        num_blocks = ngx_stream_upstream_sct_neuro_shm_size / sizeof(ngx_stream_upstream_sct_neuro_shm_block_t);

        for (i = 0; i < num_blocks; i++) {
            if (ngx_strcmp(blocks[i].addr.data, s->upstream->peer.name->data) == 0) {
                block = &blocks[i];
                break;
            }
        }

        if (block) {
            lock = &block->lock;
            ngx_spinlock(lock, ngx_pid, 1024);
            
            block->nres++;

            ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                        "\n\n\nnreq: %d, nres: %d",
                        block->nreq, block->nres);

            ngx_spinlock_unlock(lock);
        }

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                        "upstream addr: %s",
                        s->upstream->peer.name->data);
    }

    *ll = NULL;

    ngx_log_debug3(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream sct_neuro filter: l:%ui f:%ui s:%O", last, flush, size);

    if (size == 0
        && !(c->buffered & NGX_LOWLEVEL_BUFFERED)
        && !(last && c->need_last_buf)
        && !(flush && c->need_flush_buf))
    {
        if (last || flush || sync) {
            for (cl = *out; cl; /* void */) {
                ln = cl;
                cl = cl->next;
                ngx_free_chain(c->pool, ln);
            }

            *out = NULL;
            c->buffered &= ~NGX_STREAM_WRITE_BUFFERED;

            return NGX_OK;
        }

        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "the stream output chain is empty");

        ngx_debug_point();

        return NGX_ERROR;
    }

    chain = c->send_chain(c, *out, 0);

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream sct_neuro filter %p", chain);

    if (chain == NGX_CHAIN_ERROR) {
        c->error = 1;
        return NGX_ERROR;
    }

    for (cl = *out; cl && cl != chain; /* void */) {
        ln = cl;
        cl = cl->next;
        ngx_free_chain(c->pool, ln);
    }

    *out = chain;

    if (chain) {
        if (c->shared) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "shared connection is busy");
            return NGX_ERROR;
        }

        c->buffered |= NGX_STREAM_WRITE_BUFFERED;
        return NGX_AGAIN;
    }

    c->buffered &= ~NGX_STREAM_WRITE_BUFFERED;

    if (c->buffered & NGX_LOWLEVEL_BUFFERED) {
        return NGX_AGAIN;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_stream_sct_neuro_filter_init(ngx_conf_t *cf)
{
    ngx_stream_top_filter = ngx_stream_sct_neuro_filter;
    return NGX_OK;
}
