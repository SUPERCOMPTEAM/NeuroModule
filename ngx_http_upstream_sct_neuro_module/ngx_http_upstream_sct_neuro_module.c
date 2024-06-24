/*
 * Copyright (C) Ivan Pavlov
 * Copyright (C) Fedor Merkulov
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

typedef struct ngx_http_upstream_sct_neuro_peer_s   ngx_http_upstream_sct_neuro_peer_t;

struct ngx_http_upstream_sct_neuro_peer_s {
    struct sockaddr                *sockaddr;
    socklen_t                       socklen;
    ngx_str_t                       name;
    ngx_str_t                       server;

    ngx_uint_t                      conns;
    ngx_uint_t                      max_conns;

    ngx_uint_t                      fails;
    time_t                          accessed;
    time_t                          checked;

    ngx_uint_t                      max_fails;
    time_t                          fail_timeout;
    ngx_msec_t                      slow_start;
    ngx_msec_t                      start_time;

    ngx_uint_t                      down;

    ngx_uint_t                      cnt_requests;   // кол-во запросов на этот адрес
    ngx_uint_t                      cnt_responses;  // кол-во ответов с этого адреса
    double                          neuro_weight;   // текущие веса от нейронки


#if (NGX_HTTP_SSL || NGX_COMPAT)
    void                           *ssl_session;
    int                             ssl_session_len;
#endif

#if (NGX_HTTP_UPSTREAM_ZONE)
    ngx_atomic_t                    lock;
#endif

    ngx_http_upstream_sct_neuro_peer_t    *next;

    NGX_COMPAT_BEGIN(32)
    NGX_COMPAT_END
};


typedef struct ngx_http_upstream_sct_neuro_peers_s  ngx_http_upstream_sct_neuro_peers_t;

struct ngx_http_upstream_sct_neuro_peers_s {
    ngx_uint_t                      number;

#if (NGX_HTTP_UPSTREAM_ZONE)
    ngx_slab_pool_t                *shpool;
    ngx_atomic_t                    rwlock;
    ngx_http_upstream_sct_neuro_peers_t   *zone_next;
#endif

    ngx_uint_t                             tries;

    unsigned                               single:1;

    ngx_str_t                             *name;

    ngx_http_upstream_sct_neuro_peers_t   *next;

    ngx_http_upstream_sct_neuro_peer_t    *peer;
};

typedef struct {
    ngx_uint_t                              config;
    ngx_http_upstream_sct_neuro_peers_t    *peers;
    ngx_http_upstream_sct_neuro_peer_t     *current;
    // uintptr_t                              *tried;
    uintptr_t                               data;
    // ngx_uint_t                              nreq_since_last_weight_update;
} ngx_http_upstream_sct_neuro_peer_data_t;

typedef struct {
    ngx_str_t                                addr;
    uintptr_t                                peers;
    ngx_atomic_t                             lock;
    ngx_uint_t                               nreq;
    ngx_uint_t                               nres;
    ngx_uint_t                               fails;
} ngx_http_upstream_sct_neuro_shm_block_t;

#define ngx_spinlock_unlock(lock)       (void) ngx_atomic_cmp_set(lock, ngx_pid, 0)
#define ngx_http_upstream_tries(p) ((p)->tries)

static ngx_int_t ngx_http_sct_neuro_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_sct_neuro_filter_init(ngx_conf_t *cf);

static ngx_int_t ngx_http_upstream_init_sct_neuro_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us);
void
ngx_http_upstream_free_sct_neuro_peer(ngx_peer_connection_t *pc, void *data, ngx_uint_t state);
static ngx_http_upstream_sct_neuro_peer_t *ngx_http_upstream_get_peer_from_neuro(ngx_http_upstream_sct_neuro_peer_data_t *rrp);               // выбра пира из списка
// static ngx_http_upstream_sct_neuro_peer_t *
// ngx_http_upstream_get_peer_from_neuro_from_neuro(ngx_http_upstream_sct_neuro_peer_data_t *scp);
static ngx_int_t ngx_http_upstream_get_sct_neuro_peer(
    ngx_peer_connection_t *pc, void *data);
// static void ngx_http_upstream_free_sct_neuro_peer(ngx_peer_connection_t *pc, void *data,
//     ngx_uint_t state);
static char *ngx_http_upstream_sct_neuro(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_upstream_sct_neuro_set_shm_size(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_http_upstream_sct_neuro_set_gap_in_requests(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);


ngx_uint_t                              nreq_since_last_weight_update = 0; //TODO: перенести ее в shm


static ngx_command_t  ngx_http_upstream_sct_neuro_commands[] = {
    { ngx_string("sct_neuro"),              /* directive for using this module */
      NGX_HTTP_UPS_CONF|NGX_CONF_NOARGS,
      ngx_http_upstream_sct_neuro,
      0,
      0,
      NULL },

    { ngx_string("upstream_sct_neuro_shm_size"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_upstream_sct_neuro_set_shm_size,
      0,
      0,
      NULL },

    { ngx_string("upstream_sct_neuro_gap_in_requests"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_upstream_sct_neuro_set_gap_in_requests,
      0,
      0,
      NULL },

      ngx_null_command
};

static ngx_http_module_t  ngx_http_upstream_sct_neuro_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_upstream_sct_neuro_module = {
    NGX_MODULE_V1,
    &ngx_http_upstream_sct_neuro_module_ctx,            /* module context */
    ngx_http_upstream_sct_neuro_commands,               /* module directives */
    NGX_HTTP_MODULE,                                    /* module type */
    NULL,                                               /* init master */
    NULL,                                               /* init module */
    NULL,                                               /* init process */
    NULL,                                               /* init thread */
    NULL,                                               /* exit thread */
    NULL,                                               /* exit process */
    NULL,                                               /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_http_module_t  ngx_http_sct_neuro_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_sct_neuro_filter_init,        /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};

ngx_module_t  ngx_http_sct_neuro_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_sct_neuro_filter_module_ctx, /* module context */
    NULL,                                  /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_uint_t ngx_http_upstream_sct_neuro_gap_in_requests; 

static ngx_uint_t ngx_http_upstream_sct_neuro_shm_size;             
static ngx_shm_zone_t *ngx_http_upstream_sct_neuro_shm_zone;

static ngx_int_t
ngx_http_upstream_sct_neuro_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_slab_pool_t *shpool;
    ngx_http_upstream_sct_neuro_shm_block_t *blocks;
    ngx_uint_t i, j, num_blocks;
    ngx_http_upstream_srv_conf_t *uscf = shm_zone->data;
    
    if (data) {
        shm_zone->data = data;
        return NGX_OK;
    }

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    num_blocks = 0;
    ngx_http_upstream_server_t *server = uscf->servers->elts;
    for (i = 0; i < uscf->servers->nelts; i++) {
        num_blocks += server[i].naddrs;
    }

    blocks = ngx_slab_alloc(shpool, sizeof(ngx_http_upstream_sct_neuro_shm_block_t) * num_blocks);
    if (blocks == NULL) {
        return NGX_ERROR;
    }

    ngx_uint_t block_index = 0;
    for (i = 0; i < uscf->servers->nelts; i++) {
        for (j = 0; j < server[i].naddrs; j++) {
            ngx_memzero(&blocks[block_index], sizeof(ngx_http_upstream_sct_neuro_shm_block_t));
            blocks[block_index].addr = server[i].addrs[j].name;
            block_index++;
        }
    }

    shm_zone->data = blocks;

    return NGX_OK;
}

static char *
ngx_http_upstream_sct_neuro_set_shm_size(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
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

    if (ngx_http_upstream_sct_neuro_shm_size &&
        ngx_http_upstream_sct_neuro_shm_size != (ngx_uint_t) new_shm_size) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0, "Cannot change memory area size without restart, ignoring change");
    } else {
        ngx_http_upstream_sct_neuro_shm_size = new_shm_size;                                 
    }

    ngx_conf_log_error(NGX_LOG_DEBUG, cf, 0, "Using %udKiB of shared memory for upstream_sct_neuro", new_shm_size >> 10);

    return NGX_CONF_OK;
}

static char *
ngx_http_upstream_sct_neuro_set_gap_in_requests(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ssize_t new_gap_in_requests;
    ngx_str_t *value;

    value = cf->args->elts;

    new_gap_in_requests = ngx_atoi(value[1].data, value[1].len);
    if (new_gap_in_requests == NGX_ERROR || new_gap_in_requests < 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Invalid gap in requests `%V'", &value[1]);
        return NGX_CONF_ERROR;
    }

    ngx_http_upstream_sct_neuro_gap_in_requests = new_gap_in_requests;

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_upstream_init_sct_neuro(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us)
{

    ngx_str_t                      shm_name = ngx_string("sct_neuro");
    ngx_uint_t                     i, j, n, w, t;
    ngx_http_upstream_server_t    *server;
    ngx_http_upstream_sct_neuro_peer_t   *peer, **peerp;
    ngx_http_upstream_sct_neuro_peers_t  *peers;

    if (us->servers) {
        
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0,
                       "init sct neuro");

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, cf->log, 0,
                       "info: %s", cf->args->elts);

        server = us->servers->elts;

        n = 0;
        w = 0;
        t = 0;

        for (i = 0; i < us->servers->nelts; i++) {
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

        peers = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_sct_neuro_peers_t));
        if (peers == NULL) {
            return NGX_ERROR;
        }

        peer = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_sct_neuro_peer_t) * n);
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
    }

    
    ngx_shm_zone_t *shm_zone = ngx_shared_memory_add(cf, &shm_name, ngx_http_upstream_sct_neuro_shm_size, &ngx_http_upstream_sct_neuro_module);
    if (shm_zone == NULL) {
        return NGX_ERROR;
    }

    shm_zone->data = us;
    shm_zone->init = ngx_http_upstream_sct_neuro_init_shm_zone;
    ngx_http_upstream_sct_neuro_shm_zone = shm_zone;

    us->peer.init = ngx_http_upstream_init_sct_neuro_peer;

    return NGX_OK;
}

static ngx_int_t
ngx_http_upstream_init_sct_neuro_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us)
{
    // ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
    //                "init sct neuro peer");


    // if (ngx_http_upstream_init_round_robin_peer(r, us) != NGX_OK) {
    //     return NGX_ERROR;
    // }

    // ngx_uint_t                         n;
    ngx_http_upstream_sct_neuro_peer_data_t  *rrp;                                     

    rrp = r->upstream->peer.data;

    if (rrp == NULL) {
        rrp = ngx_palloc(r->pool, sizeof(ngx_http_upstream_sct_neuro_peer_data_t));
        if (rrp == NULL) {
            return NGX_ERROR;
        }

        r->upstream->peer.data = rrp;
    }

    rrp->peers = us->peer.data;
    rrp->current = NULL;
    rrp->config = 0;

    // rrp->nreq_since_last_weight_update = 0;

    // n = rrp->peers->number;

    // if (rrp->peers->next && rrp->peers->next->number > n) {
    //     n = rrp->peers->next->number;
    // }

    /*
        tried отслеживает, какие серверы были уже испробованы. 
        Если количество серверов меньше или равно количеству битов в uintptr_t, 
        достаточно одного такого значения для отслеживания. 
        Если серверов больше, требуется массив для их отслеживания.                                                                            
    */

    // if (n <= 8 * sizeof(uintptr_t)) {
    //     rrp->tried = &rrp->data;
    //     rrp->data = 0;
    // } else {
    //     n = (n + (8 * sizeof(uintptr_t) - 1)) / (8 * sizeof(uintptr_t));

    //     rrp->tried = ngx_pcalloc(r->pool, n * sizeof(uintptr_t));
    //     if (rrp->tried == NULL) {
    //         return NGX_ERROR;
    //     }
    // }

    r->upstream->peer.get = ngx_http_upstream_get_sct_neuro_peer;           // Устанавливаем методы для обработки
    r->upstream->peer.free = ngx_http_upstream_free_sct_neuro_peer;
    r->upstream->peer.tries = ngx_http_upstream_tries(rrp->peers);
#if (NGX_HTTP_SSL)
    r->upstream->peer.set_session =
                               ngx_http_upstream_set_round_robin_peer_session;
    r->upstream->peer.save_session =
                               ngx_http_upstream_save_round_robin_peer_session;
#endif
    r->upstream->peer.get = ngx_http_upstream_get_sct_neuro_peer;

    return NGX_OK;
}

void
ngx_http_upstream_free_sct_neuro_peer(ngx_peer_connection_t *pc, void *data,  // очистка памяти узла
    ngx_uint_t state)                                                           // нигде не вызывается
{
    ngx_http_upstream_sct_neuro_peer_data_t  *rrp = data;

    time_t                       now;
    ngx_http_upstream_sct_neuro_peer_t  *peer;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "free rr peer %ui %ui", pc->tries, state);

    /* TODO: NGX_PEER_KEEPALIVE */

    peer = rrp->current;

    ngx_http_upstream_rr_peers_rlock(rrp->peers);
    ngx_http_upstream_rr_peer_lock(rrp->peers, peer);

    if (rrp->peers->single) {

        peer->conns--;

        ngx_http_upstream_rr_peer_unlock(rrp->peers, peer);
        ngx_http_upstream_rr_peers_unlock(rrp->peers);

        pc->tries = 0;
        return;
    }

    if (state & NGX_PEER_FAILED) {
        now = ngx_time();

        peer->fails++;
        peer->accessed = now;
        peer->checked = now;

        if (peer->max_fails) {
            if (peer->fails >= peer->max_fails) {
                ngx_log_error(NGX_LOG_WARN, pc->log, 0,
                              "upstream server temporarily disabled");
            }
        }

    } else {

        /* mark peer live if check passed */

        if (peer->accessed < peer->checked) {
            peer->fails = 0;
        }
    }

    peer->conns--;

    ngx_http_upstream_rr_peer_unlock(rrp->peers, peer);
    ngx_http_upstream_rr_peers_unlock(rrp->peers);

    if (pc->tries) {
        pc->tries--;
    }
}


static ngx_int_t
ngx_http_upstream_get_sct_neuro_peer(ngx_peer_connection_t *pc, void *data)
{
    // ngx_uint_t                    i;
    // ngx_http_upstream_sct_neuro_peer_t  *peer;
    ngx_http_upstream_sct_neuro_peer_data_t  *scp = data;
    // ngx_http_upstream_sct_neuro_peer_data_t  *scp = data;
    // for (peer = rrp->peers->peer, i = 0;
    //     peer;
    //     peer = peer->next, i++)
    // {
    //     ngx_log_debug8(NGX_LOG_DEBUG_HTTP, pc->log, 0,
    //                "peer no: %ui, socklen: %ui, name: %s, server: %s, "
    //                "current_weight: %d, effective_weight: %d, weight: %d, "
    //                "conns: %ui", i, peer->socklen, peer->name.data,
    //                peer->server.data, peer->current_weight, 
    //                peer->effective_weight, peer->weight, peer->conns);
    //     ngx_log_debug8(NGX_LOG_DEBUG_HTTP, pc->log, 0,
    //                "max_conns: %ui, fails: %ui, accessed: %ld, checked: %ld"
    //                ", max_fails: %ui, fail_timeout: %ld, slow_start: %d, "
    //                "start_time: %d", peer->max_conns, peer->fails,
    //                peer->accessed, peer->checked, peer->max_fails, 
    //                peer->fail_timeout, peer->slow_start, peer->start_time);
    // }
    // ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
    //                "get sct neuro peer, try: %ui", pc->tries);
    


    //return ngx_http_upstream_get_round_robin_peer(pc, scp);

    ngx_http_upstream_sct_neuro_peer_data_t  *rrp = scp;

    ngx_http_upstream_sct_neuro_peer_t   *peer;
    ngx_http_upstream_sct_neuro_peers_t  *peers;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "get rr peer, try: %ui", pc->tries);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "\n\n\nno of peers: %ui\n\n\n", rrp->peers->number);

    pc->cached = 0;
    pc->connection = NULL;

    peers = rrp->peers;
    ngx_http_upstream_rr_peers_wlock(peers);

    if (peers->single) {
        peer = peers->peer;

        rrp->current = peer;

    } else {
        /* there are several peers */
        peer = ngx_http_upstream_get_peer_from_neuro(rrp);
        // ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0,
        //            "\n\n\n nreq: %ui, gap: %ui", rrp->nreq_since_last_weight_update, ngx_http_upstream_sct_neuro_gap_in_requests);
    }

    pc->sockaddr = peer->sockaddr;
    pc->socklen = peer->socklen;
    pc->name = &peer->name;

    peer->conns++;

    ngx_http_upstream_rr_peers_unlock(peers);

    return NGX_OK;
}

static ngx_http_upstream_sct_neuro_peer_t *
ngx_http_upstream_get_peer_from_neuro(ngx_http_upstream_sct_neuro_peer_data_t *rrp)
{
    int                                 sock, flag;
    time_t                              now;
    struct sockaddr_in                  server;
    ngx_uint_t                          i, num_blocks;
    ngx_http_upstream_sct_neuro_peer_t  *peer, *best;
    struct hostent                      *host;
    now = ngx_time();
    best = NULL;

    ngx_http_upstream_sct_neuro_shm_block_t *blocks;
    ngx_http_upstream_sct_neuro_shm_block_t *block = NULL;

    blocks = (ngx_http_upstream_sct_neuro_shm_block_t *) ngx_http_upstream_sct_neuro_shm_zone->data;
    num_blocks = ngx_http_upstream_sct_neuro_shm_size / sizeof(ngx_http_upstream_sct_neuro_shm_block_t);

    for (peer = rrp->peers->peer, i = 0;
         peer;
         peer = peer->next, i++)
    {
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
        peer->cnt_requests = block->nreq;
        peer->cnt_responses = block->nres;

        if (best == NULL) {
            best = peer;
        }

        ngx_spinlock_unlock(&block->lock);
    }

    // set neuro weights
    if (nreq_since_last_weight_update >= ngx_http_upstream_sct_neuro_gap_in_requests) {
        nreq_since_last_weight_update = 0;
    } else {
        nreq_since_last_weight_update++;
    }
    if (nreq_since_last_weight_update == 0) {
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
    flag = 0;
    for (peer = rrp->peers->peer, i = 0;
         peer;
         peer = peer->next, i++)
    {
        if (peer->cnt_requests == peer->cnt_responces) {
            if (peer->cnt_requests < best->cnt_requests) {
                best = peer;
                flag = 1;
            }
        }
    }
    if (!flag) {
        for (peer = rrp->peers->peer, i = 0;
            peer;
            peer = peer->next, i++)
        {
            if (peer->neuro_weight > best->neuro_weight) {
                best = peer;
            }
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

    if (now - best->checked > best->fail_timeout) {
        best->checked = now;
    }

    return best;
}

static char *
ngx_http_upstream_sct_neuro(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstream_srv_conf_t  *uscf;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    if (uscf->peer.init_upstream) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                           "load balancing method redefined");
    }

    uscf->peer.init_upstream = ngx_http_upstream_init_sct_neuro;

    uscf->flags = NGX_HTTP_UPSTREAM_CREATE
                  |NGX_HTTP_UPSTREAM_MAX_CONNS
                  |NGX_HTTP_UPSTREAM_MAX_FAILS
                  |NGX_HTTP_UPSTREAM_FAIL_TIMEOUT
                  |NGX_HTTP_UPSTREAM_DOWN;

    return NGX_CONF_OK;
}


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;

static ngx_int_t ngx_http_sct_neuro_header_filter(ngx_http_request_t *r) {
    ngx_table_elt_t  *h;
    struct sockaddr_in  *sin;
    ngx_http_upstream_sct_neuro_shm_block_t *blocks;
    ngx_http_upstream_sct_neuro_shm_block_t *block = NULL;
    // ngx_slab_pool_t *shpool;
    ngx_uint_t i, num_blocks;
    ngx_atomic_t *lock;

    if (r->headers_out.status != NGX_HTTP_OK) {
        return ngx_http_next_header_filter(r);
    }

    if (r->upstream && r->upstream->peer.name) {
        // Получаем доступ к разделяемой памяти
        // shpool = (ngx_slab_pool_t *) ngx_http_upstream_sct_neuro_shm_zone->shm.addr;
        blocks = (ngx_http_upstream_sct_neuro_shm_block_t *) ngx_http_upstream_sct_neuro_shm_zone->data;

        // Определяем количество блоков
        num_blocks = ngx_http_upstream_sct_neuro_shm_size / sizeof(ngx_http_upstream_sct_neuro_shm_block_t);

        // Ищем блок, соответствующий текущему upstream серверу
        for (i = 0; i < num_blocks; i++) {
            if (ngx_strcmp(blocks[i].addr.data, r->upstream->peer.name->data) == 0) {
                block = &blocks[i];
                break;
            }
        }

        if (block) {
            lock = &block->lock;
            ngx_spinlock(lock, ngx_pid, 1024);
            
            block->nres++;

            h = ngx_list_push(&r->headers_out.headers);
            if (h == NULL) {
                ngx_spinlock_unlock(lock);
                return NGX_ERROR;
            }
            h->hash = 1;
            ngx_str_set(&h->key, "X-Upstream-Addr");
            h->value.data = ngx_pnalloc(r->pool, block->addr.len + 1);
            if (h->value.data == NULL) {
                ngx_spinlock_unlock(lock);
                return NGX_ERROR;
            }
            ngx_memcpy(h->value.data, block->addr.data, block->addr.len);
            h->value.data[block->addr.len] = '\0';
            h->value.len = block->addr.len;

            // Добавляем заголовок X-Upstream-Port
            h = ngx_list_push(&r->headers_out.headers);
            if (h == NULL) {
                ngx_spinlock_unlock(lock);
                return NGX_ERROR;
            }
            h->hash = 1;
            ngx_str_set(&h->key, "X-Upstream-Port");
            h->value.data = ngx_pnalloc(r->pool, NGX_INT_T_LEN);
            if (h->value.data == NULL) {
                ngx_spinlock_unlock(lock);
                return NGX_ERROR;
            }
            sin = (struct sockaddr_in *) r->upstream->peer.sockaddr;
            h->value.len = ngx_sprintf(h->value.data, "%d", ntohs(sin->sin_port)) - h->value.data;

            // Добавляем заголовок X-Upstream-nreq
            h = ngx_list_push(&r->headers_out.headers);
            if (h == NULL) {
                ngx_spinlock_unlock(lock);
                return NGX_ERROR;
            }
            h->hash = 1;
            ngx_str_set(&h->key, "X-Upstream-nreq");
            h->value.data = ngx_pnalloc(r->pool, NGX_INT_T_LEN);
            if (h->value.data == NULL) {
                ngx_spinlock_unlock(lock);
                return NGX_ERROR;
            }
            h->value.len = ngx_sprintf(h->value.data, "%ui", block->nreq) - h->value.data;

            // Добавляем заголовок X-Upstream-nres
            h = ngx_list_push(&r->headers_out.headers);
            if (h == NULL) {
                ngx_spinlock_unlock(lock);
                return NGX_ERROR;
            }
            h->hash = 1;
            ngx_str_set(&h->key, "X-Upstream-nres");
            h->value.data = ngx_pnalloc(r->pool, NGX_INT_T_LEN);
            if (h->value.data == NULL) {
                ngx_spinlock_unlock(lock);
                return NGX_ERROR;
            }
            h->value.len = ngx_sprintf(h->value.data, "%ui", block->nres) - h->value.data;
            ngx_spinlock_unlock(lock);
        }
    }

    return ngx_http_next_header_filter(r);
}

static ngx_int_t
ngx_http_sct_neuro_filter_init(ngx_conf_t *cf)
{

    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_sct_neuro_header_filter;

    return NGX_OK;
}
