/*
Copyright (c) 2009 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/* Please, please, please.

   You are welcome to integrate this code in your favourite Bittorrent
   client.  Please remember, however, that it is meant to be usable by
   others, including myself.  This means no C++, no relicensing, and no
   gratuitious changes to the coding style.  And please send back any
   improvements to the author. */

/* For memmem. */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "dht.h"

#ifndef MSG_CONFIRM
#define MSG_CONFIRM 0
#endif

/* We set sin_family to 0 to mark unused slots. */
#if AF_INET == 0
#error You lose
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
/* nothing */
#elif defined(__GNUC__)
#define inline __inline
#if  (__GNUC__ >= 3)
#define restrict __restrict
#else
#define restrict /**/
#endif
#else
#define inline /**/
#define restrict /**/
#endif

#define MAX(x, y) ((x) >= (y) ? (x) : (y))
#define MIN(x, y) ((x) <= (y) ? (x) : (y))

struct node {
    unsigned char id[20];
    struct sockaddr_in sin;
    time_t time;                /* time of last message received */
    time_t reply_time;          /* time of last correct reply received */
    time_t pinged_time;         /* time of last request */
    int pinged;                 /* how many requests we sent since last reply */
    struct node *next;
};

struct bucket {
    unsigned char first[20];
    int count;                  /* number of nodes */
    int time;                   /* time of last reply in this bucket */
    struct node *nodes;
    struct sockaddr_in cached;  /* the address of a likely candidate */
    struct bucket *next;
};

struct search_node {
    unsigned char id[20];
    struct sockaddr_in sin;
    time_t request_time;        /* the time of the last unanswered request */
    time_t reply_time;          /* the time of the last reply */
    int pinged;
    unsigned char token[40];
    int token_len;
    int replied;                /* whether we have received a reply */
    int acked;                  /* whether they acked our announcement */
};

struct search {
    unsigned short tid;
    time_t step_time;           /* the time of the last search_step */
    unsigned char id[20];
    unsigned short port;        /* 0 for pure searches */
    int done;
    struct search_node nodes[8];
    int numnodes;
};

struct peer {
    time_t time;
    unsigned char ip[4];
    unsigned short port;
};

/* The maximum number of peers we store for a given hash. */
#ifndef DHT_MAX_PEERS
#define DHT_MAX_PEERS 2048
#endif

struct storage {
    unsigned char id[20];
    int numpeers;
    int maxpeers;
    struct peer *peers;
    struct storage *next;
};

static int send_ping(int s, struct sockaddr *sa, int salen,
                     const unsigned char *tid, int tid_len);
static int send_pong(int s, struct sockaddr *sa, int salen,
                     const unsigned char *tid, int tid_len);
static int send_find_node(int s, struct sockaddr *sa, int salen,
                          const unsigned char *tid, int tid_len,
                          const unsigned char *target, int confirm);
static int send_found_nodes(int s, struct sockaddr *sa, int salen,
                            const unsigned char *tid, int tid_len,
                            const unsigned char *nodes, int nodes_len,
                            const unsigned char *token, int token_len);
static int send_bucket_nodes(int s, struct sockaddr *sa, int salen,
                             const unsigned char *tid, int tid_len,
                             struct bucket *b,
                             const unsigned char *token, int token_len);
static int send_get_peers(int s, struct sockaddr *sa, int salen,
                          unsigned char *tid, int tid_len,
                          unsigned char *infohash, unsigned short port,
                          int confirm);
static int send_announce_peer(int s, struct sockaddr *sa, int salen,
                              unsigned char *tid, int tid_len,
                              unsigned char *infohas, unsigned short port,
                              unsigned char *token, int token_len, int confirm);
int send_peers_found(int s, struct sockaddr *sa, int salen,
                     unsigned char *tid, int tid_len,
                     struct peer *peers1, int numpeers1,
                     struct peer *peers2, int numpeers2,
                     unsigned char *token, int token_len);
int send_peer_announced(int s, struct sockaddr *sa, int salen,
                        unsigned char *tid, int tid_len);

#define REPLY 0
#define PING 1
#define FIND_NODE 2
#define GET_PEERS 3
#define ANNOUNCE_PEER 4
static int parse_message(const unsigned char *buf, int buflen,
                         unsigned char *tid_return, int *tid_len,
                         unsigned char *id_return,
                         unsigned char *info_hash_return,
                         unsigned char *target_return,
                         unsigned short *port_return,
                         unsigned char *token_return, int *token_len,
                         unsigned char *nodes_return, int *nodes_len,
                         const unsigned char *values_return, int *values_len);

static const unsigned char zeroes[20] = {0};
static const unsigned char ones[20] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF
};
static time_t search_time;
static time_t confirm_nodes_time;
static time_t rotate_secrets_time;

static unsigned char myid[20];
static unsigned char secret[8];
static unsigned char oldsecret[8];

static struct bucket *buckets = NULL;
static struct storage *storage;

/* The maximum number of concurrent searches. */
#ifndef DHT_MAX_SEARCHES
#define DHT_MAX_SEARCHES 20
#endif

static struct search searches[DHT_MAX_SEARCHES];
static int numsearches;
static unsigned short search_id;

/* The maximum number of nodes that we snub.  There is probably little
   reason to increase this value. */
#ifndef DHT_MAX_BLACKLISTED
#define DHT_MAX_BLACKLISTED 10
#endif
static struct sockaddr_in blacklist[DHT_MAX_BLACKLISTED];
int next_blacklisted;

static struct timeval now;
static time_t mybucket_grow_time;
static time_t expire_stuff_time;

FILE *dht_debug = NULL;

#ifdef __GNUC__
    __attribute__ ((format (printf, 1, 2)))
#endif
static void
debugf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if(dht_debug)
        vfprintf(dht_debug, format, args);
    va_end(args);
    fflush(dht_debug);
}

static void
debug_printable(const unsigned char *buf, int buflen)
{
    int i;
    if(dht_debug) {
        for(i = 0; i < buflen; i++)
            putc(buf[i] >= 32 && buf[i] <= 126 ? buf[i] : '.', dht_debug);
    }
}

static void
print_hex(FILE *f, const unsigned char *buf, int buflen)
{
    int i;
    for(i = 0; i < buflen; i++)
        fprintf(f, "%02x", buf[i]);
}

/* Forget about the ``XOR-metric''.  An id is just a path from the
   root of the tree, so bits are numbered from the start. */

static inline int
id_cmp(const unsigned char *restrict id1, const unsigned char *restrict id2)
{
    /* Memcmp is guaranteed to perform an unsigned comparison. */
    return memcmp(id1, id2, 20);
}

/* Find the lowest 1 bit in an id. */
static int
lowbit(const unsigned char *id)
{
    int i, j;
    for(i = 19; i >= 0; i--)
        if(id[i] != 0)
            break;

    if(i < 0)
        return -1;

    for(j = 7; j >= 0; j--)
        if((id[i] & (0x80 >> j)) != 0)
            break;

    return 8 * i + j;
}

/* Find how many bits two ids have in common. */
static int
common_bits(const unsigned char *id1, const unsigned char *id2)
{
    int i, j;
    unsigned char xor;
    for(i = 0; i < 20; i++) {
        if(id1[i] != id2[i])
            break;
    }

    if(i == 20)
        return 160;

    xor = id1[i] ^ id2[i];

    j = 0;
    while((xor & 0x80) == 0) {
        xor <<= 1;
        j++;
    }

    return 8 * i + j;
}

/* We keep buckets in a sorted linked list.  A bucket b ranges from
   b->first inclusive up to b->next->first exclusive. */
static int
in_bucket(const unsigned char *id, struct bucket *b)
{
    return id_cmp(b->first, id) <= 0 &&
        (b->next == NULL || id_cmp(id, b->next->first) < 0);
}

static struct bucket *
find_bucket(unsigned const char *id)
{
    struct bucket *b = buckets;

    while(1) {
        if(b->next == NULL)
            return b;
        if(id_cmp(id, b->next->first) < 0)
            return b;
        b = b->next;
    }
}

static struct bucket *
previous_bucket(struct bucket *b)
{
    struct bucket *p = buckets;

    if(b == p)
        return NULL;

    while(1) {
        if(p->next == NULL)
            return NULL;
        if(p->next == b)
            return p;
        p = p->next;
    }
}

/* Every bucket contains an unordered list of nodes. */
static struct node *
find_node(const unsigned char *id)
{
    struct bucket *b = find_bucket(id);
    struct node *n;

    if(b == NULL)
        return NULL;
    n = b->nodes;
    while(n) {
        if(id_cmp(n->id, id) == 0)
            return n;
        n = n->next;
    }
    return NULL;
}

/* Return a random node in a bucket. */
static struct node *
random_node(struct bucket *b)
{
    struct node *n;
    int nn;

    if(b->count == 0)
        return NULL;

    nn = random() % b->count;
    n = b->nodes;
    while(nn > 0 && n) {
        n = n->next;
        nn--;
    }
    return n;
}

/* Return the middle id of a bucket. */
static int
bucket_middle(struct bucket *b, unsigned char *id_return)
{
    int bit1 = lowbit(b->first);
    int bit2 = b->next ? lowbit(b->next->first) : -1;
    int bit = (bit1 >= bit2 ? bit1 : bit2) + 1;

    if(bit >= 160)
        return -1;

    memcpy(id_return, b->first, 20);
    id_return[bit / 8] |= (0x80 >> (bit % 8));
    return 1;
}

/* Return a random id within a bucket. */
static int
bucket_random(struct bucket *b, unsigned char *id_return)
{
    int bit1 = lowbit(b->first);
    int bit2 = b->next ? lowbit(b->next->first) : -1;
    int bit = (bit1 >= bit2 ? bit1 : bit2) + 1;
    int i;

    if(bit >= 160) {
        memcpy(id_return, b->first, 20);
        return 1;
    }

    memcpy(id_return, b->first, bit / 8);
    id_return[bit / 8] = b->first[bit / 8] & (0xFF00 >> (bit % 8));
    id_return[bit / 8] |= random() & 0xFF >> (bit % 8);
    for(i = bit / 8 + 1; i < 20; i++)
        id_return[i] = random() & 0xFF;
    return 1;
}    

/* Insert a new node into a bucket. */
static struct node *
insert_node(struct node *node)
{
    struct bucket *b = find_bucket(node->id);

    node->next = b->nodes;
    b->nodes = node;
    b->count++;
    return node;
}

/* This is our definition of a known-good node. */
static int
node_good(struct node *node)
{
    return
        node->pinged <= 2 &&
        node->reply_time >= now.tv_sec - 7200 &&
        node->time >= now.tv_sec - 900;
}

/* Every bucket caches the address of a likely node.  Ping it. */
static int
send_cached_ping(int s, struct bucket *b)
{
    int rc;
    /* We set family to 0 when there's no cached node. */
    if(b->cached.sin_family == AF_INET) {
        debugf("Sending ping to cached node.\n");
        rc = send_ping(s, (struct sockaddr*)&b->cached,
                       sizeof(struct sockaddr_in),
                       (unsigned char*)"pn", 2);
        b->cached.sin_family = 0;
        return rc;
    }
    return 0;
}

/* Split a bucket into two equal parts. */
static struct bucket *
split_bucket(int s, struct bucket *b)
{
    struct bucket *new;
    struct node *nodes;
    int rc;
    unsigned char new_id[20];

    rc = bucket_middle(b, new_id);
    if(rc < 0)
        return NULL;

    new = calloc(1, sizeof(struct bucket));
    if(new == NULL)
        return NULL;

    send_cached_ping(s, b);

    memcpy(new->first, new_id, 20);
    new->time = b->time;

    nodes = b->nodes;
    b->nodes = NULL;
    b->count = 0;
    new->next = b->next;
    b->next = new;
    while(nodes) {
        struct node *n;
        n = nodes;
        nodes = nodes->next;
        insert_node(n);
    }
    return b;
}

/* Called whenever we send a request to a node. */
static void
pinged(int s, struct node *n, struct bucket *b)
{
    n->pinged++;
    n->pinged_time = now.tv_sec;
    if(n->pinged >= 3)
        send_cached_ping(s, b ? b : find_bucket(n->id));
}

/* We just learnt about a node, not necessarily a new one.  Confirm is 1 if
   the node sent a message, 2 if it sent us a reply. */
static struct node *
new_node(int s, const unsigned char *id, struct sockaddr_in *sin,
         int confirm)
{
    struct bucket *b = find_bucket(id);
    struct node *n;
    int mybucket = in_bucket(myid, b);

    if(id_cmp(id, myid) == 0)
        return NULL;

    if(confirm)
        b->time = now.tv_sec;

    n = b->nodes;
    while(n) {
        if(id_cmp(n->id, id) == 0) {
            if(confirm || n->time < now.tv_sec - 15 * 60) {
                /* Known node.  Update stuff. */
                n->sin = *sin;
                if(confirm)
                    n->time = now.tv_sec;
                if(confirm >= 2) {
                    n->reply_time = now.tv_sec;
                    n->pinged = 0;
                    n->pinged_time = 0;
                }
            }
            return n;
        }
        n = n->next;
    }

    /* New node.  First, try to get rid of a known-bad node. */
    n = b->nodes;
    while(n) {
        if(n->pinged >= 3) {
            memcpy(n->id, id, 20);
            n->sin = *sin;
            n->time = confirm ? now.tv_sec : 0;
            n->reply_time = confirm >= 2 ? now.tv_sec : 0;
            n->pinged_time = 0;
            n->pinged = 0;
            if(mybucket)
                mybucket_grow_time = now.tv_sec;
            return n;
        }
        n = n->next;
    }

    if(b->count >= 8) {
        /* Bucket full.  Ping a dubious node */
        int dubious = 0;
        n = b->nodes;
        while(n) {
            /* Pick the first dubious node that we haven't pinged in the
               last 15 seconds.  This gives nodes the time to reply, but
               tends to concentrate on the same nodes. */
            if(!node_good(n) && n->pinged_time < now.tv_sec - 15) {
                debugf("Sending ping to dubious node.\n");
                send_ping(s,
                          (struct sockaddr*)&n->sin, sizeof(struct sockaddr_in),
                          (unsigned char*)"pn", 2);
                n->pinged++;
                n->pinged_time = now.tv_sec;
                dubious = 1;
                break;
            }
            n = n->next;
        }
        
        if(!dubious && mybucket) {
            debugf("Splitting.\n");
            b = split_bucket(s, b);
            mybucket_grow_time = now.tv_sec;
            return new_node(s, id, sin, confirm);
        }

        /* No space for this node.  Cache it away for later. */
        if(confirm || b->cached.sin_family == 0)
            b->cached = *sin;

        return NULL;
    }

    /* Create a new node. */
    n = calloc(1, sizeof(struct node));
    if(n == NULL)
        return NULL;
    memcpy(n->id, id, 20);
    n->sin = *sin;
    n->time = confirm ? now.tv_sec : 0;
    n->reply_time = confirm >= 2 ? now.tv_sec : 0;
    n->next = b->nodes;
    b->nodes = n;
    b->count++;
    if(mybucket)
        mybucket_grow_time = now.tv_sec;
    return n;
}

/* Called periodically to purge known-bad nodes.  Note that we're very
   conservative here: broken nodes in the table don't do much harm, we'll
   recover as soon as we find better ones. */
static int
expire_buckets(int s)
{
    struct bucket *b = buckets;

    while(b) {
        struct node *n, *p;
        int changed = 0;

        while(b->nodes && b->nodes->pinged >= 4) {
            n = b->nodes;
            b->nodes = n->next;
            b->count--;
            changed = 1;
            free(n);
        }

        p = b->nodes;
        while(p) {
            while(p->next && p->next->pinged >= 4) {
                n = p->next;
                p->next = n->next;
                b->count--;
                changed = 1;
                free(n);
            }
            p = p->next;
        }

        if(changed)
            send_cached_ping(s, b);

        b = b->next;
    }
    expire_stuff_time = now.tv_sec + 120 + random() % 240;
    return 1;
}

/* While a search is in progress, we don't necessarily keep the nodes being
   walked in the main bucket table.  A search in progress is identified by
   a unique transaction id, a short (and hence small enough to fit in the
   transaction id of the protocol packets). */

struct search *
find_search(unsigned short tid)
{
    int i;
    for(i = 0; i < numsearches; i++) {
        if(searches[i].tid == tid)
            return &searches[i];
    }
    return NULL;
}

/* A search contains a list of nodes, sorted by decreasing distance to the
   target.  We just got a new candidate, insert it at the right spot or
   discard it. */

int
insert_search_node(unsigned char *id, struct sockaddr_in *sin,
                   struct search *sr, int replied,
                   unsigned char *token, int token_len)
{
    int bits = common_bits(id, sr->id);
    struct search_node *n;
    int i, j;

    for(i = 0; i < sr->numnodes; i++) {
        if(id_cmp(id, sr->nodes[i].id) == 0) {
            n = &sr->nodes[i];
            goto found;
        }
        if(common_bits(sr->id, sr->nodes[i].id) < bits)
            break;
    }

    if(i == 8)
        return 0;

    if(sr->numnodes < 8)
        sr->numnodes++;

    for(j = sr->numnodes - 1; j > i; j--) {
        sr->nodes[j] = sr->nodes[j - 1];
    }

    n = &sr->nodes[i];

    memset(n, 0, sizeof(struct search_node));
    memcpy(n->id, id, 20);

found:
    n->sin = *sin;

    if(replied) {
        n->replied = 1;
        n->reply_time = now.tv_sec;
        n->request_time = 0;
        n->pinged = 0;
    }
    if(token) {
        if(token_len >= 40) {
            debugf("Eek!  Overlong token.\n");
        } else {
            memcpy(n->token, token, token_len);
            n->token_len = token_len;
        }
    }

    return 1;
}

static void
flush_search_node(struct search_node *n, struct search *sr)
{
    int i = n - sr->nodes, j;
    for(j = i; j < sr->numnodes - 1; j++)
        sr->nodes[j] = sr->nodes[j + 1];
    sr->numnodes--;
}

/* This must always return 0 or 1, never -1, not even on failure (see below). */
static int
search_send_get_peers(int s, struct search *sr, struct search_node *n)
{
    struct node *node;
    unsigned char tid[4];

    if(n == NULL) {
        int i;
        for(i = 0; i < sr->numnodes; i++) {
            if(sr->nodes[i].pinged < 3 && !sr->nodes[i].replied &&
               sr->nodes[i].request_time < now.tv_sec - 15)
                n = &sr->nodes[i];
        }
    }

    if(!n || n->pinged >= 3 || n->replied ||
       n->request_time >= now.tv_sec - 15)
        return 0;

    debugf("Sending get_peers.\n");
    tid[0] = 'g'; tid[1] = 'p'; memcpy(tid + 2, &sr->tid, 2);
    send_get_peers(s, (struct sockaddr*)&n->sin,
                   sizeof(struct sockaddr_in),
                   tid, 4, sr->id, sr->tid,
                   n->reply_time >= now.tv_sec - 15);
    n->pinged++;
    n->request_time = now.tv_sec;
    /* If the node happens to be in our main routing table, mark it
       as pinged. */
    node = find_node(n->id);
    if(node) pinged(s, node, NULL);
    return 1;
}

/* When a search is in progress, we periodically call search_step to send
   further requests. */
void
search_step(int s, struct search *sr, dht_callback *callback, void *closure)
{
    int i, j = 0;
    int all_done = 1;

    for(i = 0; i < sr->numnodes; i++) {
        struct search_node *n = &sr->nodes[i];
        if(n->pinged < 3 && !n->replied) {
            all_done = 0;
            break;
        }
    }

    if(all_done) {
        if(sr->port == 0) {
            goto done;
        } else {
            int all_acked = 1;
            for(i = 0; i < sr->numnodes; i++) {
                struct search_node *n = &sr->nodes[i];
                struct node *node;
                unsigned char tid[4];
                if(n->pinged < 3 && !n->acked) {
                    all_acked = 0;
                    debugf("Sending announce_peer.\n");
                    tid[0] = 'a'; tid[1] = 'p'; memcpy(tid + 2, &sr->tid, 2);
                    send_announce_peer(s,
                                       (struct sockaddr*)&n->sin,
                                       sizeof(struct sockaddr_in),
                                       tid, 4, sr->id, sr->port,
                                       n->token, n->token_len,
                                       n->reply_time >= now.tv_sec - 15);
                    n->pinged++;
                    n->request_time = now.tv_sec;
                    node = find_node(n->id);
                    if(node) pinged(s, node, NULL);
                }
            }
            if(all_acked)
                goto done;
        }
        sr->step_time = now.tv_sec;
        return;
    }

    if(sr->step_time + 15 >= now.tv_sec)
        return;

    for(i = 0; i < sr->numnodes; i++) {
        j += search_send_get_peers(s, sr, &sr->nodes[i]);
        if(j >= 3)
            break;
    }
    sr->step_time = now.tv_sec;
    return;

 done:
    sr->done = 1;
    if(callback)
        (*callback)(closure, DHT_EVENT_SEARCH_DONE, sr->id, NULL, 0);
    sr->step_time = now.tv_sec;
}

static struct search *
find_free_search_slot(void)
{
    int i;
    struct search *sr = NULL;

    if(numsearches < DHT_MAX_SEARCHES)
        return &searches[numsearches++];

    for(i = 0; i < numsearches; i++) {
        if(searches[i].done &&
           (sr == NULL || searches[i].step_time < sr->step_time))
            sr = &searches[i];
    }
    return sr;
}

/* Insert the contents of a bucket into a search structure. */
static void
insert_search_bucket(struct bucket *b, struct search *sr)
{
    struct node *n;
    n = b->nodes;
    while(n) {
        insert_search_node(n->id, &n->sin, sr, 0, NULL, 0);
        n = n->next;
    }
}

/* Start a search.  If port is non-zero, perform an announce when the
   search is complete. */
int 
dht_search(int s, const unsigned char *id, int port,
           dht_callback *callback, void *closure)
{
    struct search *sr;
    struct bucket *b;
    int i;

    for(i = 0; i < numsearches; i++) {
        if(id_cmp(searches[i].id, id) == 0)
            break;
    }

    if(i < numsearches) {
        /* We're reusing data from an old search.  Reusing the same tid
           means that we can merge replies for both searches. */
        int j;
        sr = searches + i;
        sr->done = 0;
    again:
        for(j = 0; j < sr->numnodes; j++) {
            struct search_node *n;
            n = &sr->nodes[j];
            /* Discard any doubtful nodes. */
            if(n->pinged >= 3 || n->reply_time < now.tv_sec - 7200) {
                flush_search_node(n, sr);
                goto again;
            }
            n->pinged = 0;
            n->token_len = 0;
            n->replied = 0;
            n->acked = 0;
        }
    } else {
        sr = find_free_search_slot();
        if(sr == NULL) {
            errno = ENOSPC;
            return -1;
        }
        memset(sr, 0, sizeof(struct search));
        sr->tid = search_id++;
        memcpy(sr->id, id, 20);
        sr->numnodes = 0;
    }

    sr->port = port;

    b = find_bucket(id);
    insert_search_bucket(b, sr);

    if(sr->numnodes < 8 && b->next)
        insert_search_bucket(b->next, sr);
    if(sr->numnodes < 8) {
        struct bucket *p = previous_bucket(b);
        if(p)
            insert_search_bucket(p, sr);
    }
    if(sr->numnodes < 8)
        insert_search_bucket(find_bucket(myid), sr);

    search_step(s, sr, callback, closure);
    search_time = now.tv_sec;
    return 1;
}

/* A struct storage stores all the stored peer addresses for a given info
   hash. */

static struct storage *
find_storage(const unsigned char *id)
{
    struct storage *st = storage;

    while(st) {
        if(id_cmp(id, st->id) == 0)
            break;
        st = st->next;
    }
    return st;
}

static int
storage_store(const unsigned char *id, const unsigned char *ip,
              unsigned short port)
{
    int i;
    struct storage *st = storage;

    st = find_storage(id);

    if(st == NULL) {
        st = calloc(1, sizeof(struct storage));
        if(st == NULL) return -1;
        memcpy(st->id, id, 20);
        st->next = storage;
        storage = st;
    }

    for(i = 0; i < st->numpeers; i++) {
        if(st->peers[i].port == port && memcmp(st->peers[i].ip, ip, 4) == 0)
            break;
    }
    if(i < st->numpeers) {
        /* Already there, only need to refresh */
        st->peers[i].time = now.tv_sec;
        return 0;
    } else {
        struct peer *p;
        if(i >= st->maxpeers) {
            /* Need to expand the array. */
            struct peer *new_peers;
            if(st->maxpeers > DHT_MAX_PEERS / 2)
                return 0;
            int n = st->maxpeers == 0 ? 2 : 2 * st->maxpeers;
            new_peers = realloc(st->peers, n * sizeof(struct peer));
            if(new_peers == NULL)
                return -1;
            st->peers = new_peers;
            st->maxpeers = n;
        }
        p = &st->peers[st->numpeers++];
        p->time = now.tv_sec;
        memcpy(p->ip, ip, 4);
        p->port = port;
        return 1;
    }
}

static int
expire_storage()
{
    struct storage *st = storage, *previous = NULL;
    while(st) {
        int i = 0;
        while(i < st->numpeers) {
            if(st->peers[i].time < now.tv_sec - 32 * 60) {
                if(i != st->numpeers - 1)
                    st->peers[i] = st->peers[st->numpeers - 1];
                st->numpeers--;
            } else {
                i++;
            }
        }

        if(st->numpeers == 0) {
            free(st->peers);
            if(previous)
                previous->next = st->next;
            else
                storage = st->next;
            free(st);
            if(previous)
                st = previous->next;
            else
                st = storage;
        } else {
            previous = st;
            st = st->next;
        }
    }
    return 1;
}

/* We've just found out that a node is buggy. */
static void
broken_node(int s, const unsigned char *id, struct sockaddr_in *sin)
{
    int i, j;

    debugf("Blacklisting broken node.\n");

    if(id) {
        /* Make the node easy to discard. */
        struct node *n;
        n = find_node(id);
        if(n) {
            n->pinged = 3;
            pinged(s, n, NULL);
        }
        /* Discard it from any searches in progress. */
        for(i = 0; i < numsearches; i++) {
            for(j = 0; j < searches[i].numnodes; j++)
                if(id_cmp(searches[i].nodes[j].id, id) == 0)
                    flush_search_node(&searches[i].nodes[j],
                                      &searches[i]);
        }
    }
    /* And make sure we don't hear from it again. */
    blacklist[next_blacklisted] = *sin;
    next_blacklisted = (next_blacklisted + 1) % DHT_MAX_BLACKLISTED;
}

static int
rotate_secrets()
{
    int fd;
    unsigned seed;

    fd = open("/dev/urandom", O_RDONLY);
    if(fd < 0)
        return -1;

    memcpy(oldsecret, secret, sizeof(secret));
    read(fd, secret, sizeof(secret));

    read(fd, &seed, sizeof(seed));
    srandom(seed);

    close(fd);
    rotate_secrets_time = now.tv_sec + 900 + random() % 1800;
    return 1;
}

#ifndef TOKEN_SIZE
#define TOKEN_SIZE 8
#endif

static void
make_token(const unsigned char *ipv4, unsigned short port, int old,
           unsigned char *token_return)
{
    dht_hash(token_return, TOKEN_SIZE,
             old ? oldsecret : secret, sizeof(secret),
             ipv4, 4,
             (unsigned char*)&port, 2);
}
static int
token_match(unsigned char *token, int token_len,
            const unsigned char *ipv4, unsigned short port)
{
    unsigned char t[TOKEN_SIZE];
    if(token_len != TOKEN_SIZE)
        return 0;
    make_token(ipv4, port, 0, t);
    if(memcmp(t, token, TOKEN_SIZE) == 0)
        return 1;
    make_token(ipv4, port, 1, t);
    if(memcmp(t, token, TOKEN_SIZE) == 0)
        return 1;
    return 0;
}

int
dht_nodes(int *good_return, int *dubious_return, int *cached_return,
          int *incoming_return)
{
    int good = 0, dubious = 0, cached = 0, incoming = 0;
    struct bucket *b = buckets;
    while(b) {
        struct node *n = b->nodes;
        while(n) {
            if(node_good(n)) {
                good++;
                if(n->time > n->reply_time)
                    incoming++;
            } else {
                dubious++;
            }
            n = n->next;
        }
        if(b->cached.sin_family == AF_INET)
            cached++;
        b = b->next;
    }
    if(good_return)
        *good_return = good;
    if(dubious_return)
        *dubious_return = dubious;
    if(cached_return)
        *cached_return = cached;
    if(incoming_return)
        *incoming_return = cached;
    return good + dubious;
}
                

void
dht_dump_tables(FILE *f)
{
    int i, j;
    struct bucket *b = buckets;
    struct storage *st = storage;

    fprintf(f, "My id ");
    print_hex(f, myid, 20);
    fprintf(f, "\n");
    while(b) {
        struct node *n = b->nodes;
        fprintf(f, "Bucket ");
        print_hex(f, b->first, 20);
        fprintf(f, " count %d age %d%s%s:\n",
               b->count, (int)(now.tv_sec - b->time),
               in_bucket(myid, b) ? " (mine)" : "",
               b->cached.sin_family ? " (cached)" : "");
        while(n) {
            char buf[512];
            fprintf(f, "    Node ");
            print_hex(f, n->id, 20);
            inet_ntop(AF_INET, &n->sin.sin_addr, buf, 512);
            fprintf(f, " %s:%d ", buf, ntohs(n->sin.sin_port));
            if(n->time != n->reply_time)
                fprintf(f, "age %ld, %ld",
                       (long)(now.tv_sec - n->time),
                       (long)(now.tv_sec - n->reply_time));
            else
                fprintf(f, "age %ld", (long)(now.tv_sec - n->time));
            if(n->pinged)
                fprintf(f, " (%d)", n->pinged);
            if(node_good(n))
                fprintf(f, " (good)");
            fprintf(f, "\n");
            n = n->next;
        }
        b = b->next;
    }
    for(i = 0; i < numsearches; i++) {
        struct search *sr = &searches[i];
        fprintf(f, "\nSearch %d id ", i);
        print_hex(f, sr->id, 20);
        fprintf(f, " age %d%s\n", (int)(now.tv_sec - sr->step_time),
               sr->done ? " (done)" : "");
        for(j = 0; j < sr->numnodes; j++) {
            struct search_node *n = &sr->nodes[j];
            fprintf(f, "Node %d id ", j);
            print_hex(f, n->id, 20);
            fprintf(f, " bits %d age ", common_bits(sr->id, n->id));
            if(n->request_time)
                fprintf(f, "%d, ", (int)(now.tv_sec - n->request_time));
            fprintf(f, "%d", (int)(now.tv_sec - n->reply_time));
            if(n->pinged)
                fprintf(f, " (%d)", n->pinged);
            fprintf(f, "%s%s.\n",
                   find_node(n->id) ? " (known)" : "",
                   n->replied ? " (replied)" : "");
        }
    }

    
    while(st) {
        fprintf(f, "\nStorage ");
        print_hex(f, st->id, 20);
        fprintf(f, " %d/%d nodes:", st->numpeers, st->maxpeers);
        for(i = 0; i < st->numpeers; i++) {
            char buf[20];
            inet_ntop(AF_INET, st->peers[i].ip, buf, 20);
            fprintf(f, " %s:%u (%ld)",
                    buf, st->peers[i].port,
                    (long)(now.tv_sec - st->peers[i].time));
        }
        st = st->next;
    }
    
    fprintf(f, "\n\n");
    fflush(f);
}

int
dht_init(int s, const unsigned char *id)
{
    int rc;

    if(buckets) {
        errno = EBUSY;
        return -1;
    }

    buckets = calloc(sizeof(struct bucket), 1);
    if(buckets == NULL)
        return -1;

    numsearches = 0;

    storage = NULL;

    rc = fcntl(s, F_GETFL, 0);
    if(rc < 0)
        return -1;

    rc = fcntl(s, F_SETFL, (rc | O_NONBLOCK));
    if(rc < 0)
        return -1;

    memcpy(myid, id, 20);

    gettimeofday(&now, NULL);

    mybucket_grow_time = now.tv_sec;
    confirm_nodes_time = now.tv_sec + random() % 3;

    search_id = random() & 0xFFFF;
    search_time = 0;

    next_blacklisted = 0;

    memset(secret, 0, sizeof(secret));
    rotate_secrets();
    expire_buckets(s);

    return 1;
}

int
dht_uninit(int s, int dofree)
{
    if(!dofree)
        return 1;

    while(buckets) {
        struct bucket *b = buckets;
        buckets = b->next;
        while(b->nodes) {
            struct node *n = b->nodes;
            b->nodes = n->next;
            free(n);
        }
        free(b);
    }

    while(storage) {
        struct storage *st = storage;
        storage = storage->next;
        free(st->peers);
        free(st);
    }
    
    return 1;
}

int
dht_periodic(int s, int available, time_t *tosleep,
             dht_callback *callback, void *closure)
{
    gettimeofday(&now, NULL);

    if(available) {
        int rc, i, message;
        unsigned char tid[16], id[20], info_hash[20], target[20];
        unsigned char buf[1024], nodes[256], token[128];
        int tid_len = 16, token_len = 128;
        int nodes_len = 256;
        unsigned short port;
        unsigned char values[2048];
        int values_len = 2048;
        struct sockaddr_in source;
        socklen_t source_len = sizeof(struct sockaddr_in);

        rc = recvfrom(s, buf, 1024, 0,
                      (struct sockaddr*)&source, &source_len);
        if(rc < 0) {
            if(errno == EAGAIN)
                goto dontread;
            else
                return rc;
        }

        if(source_len != sizeof(struct sockaddr_in)) {
            /* Hmm... somebody gave us an IPv6 socket. */
            errno = EINVAL;
            return -1;
        }

        for(i = 0; i < DHT_MAX_BLACKLISTED; i++) {
            if(blacklist[i].sin_family == AF_INET &&
               blacklist[i].sin_port == source.sin_port &&
               memcmp(&blacklist[i].sin_addr, &source.sin_addr, 4) == 0) {
                debugf("Received packet from blacklisted node.\n");
                goto dontread;
            }
        }

        message = parse_message(buf, rc, tid, &tid_len, id, info_hash,
                                target, &port, token, &token_len,
                                nodes, &nodes_len, values, &values_len);
        if(id_cmp(id, zeroes) == 0) {
            debugf("Message with no id: ");
            debug_printable(buf, rc);
            debugf("\n");
            goto dontread;
        }

        if(id_cmp(id, myid) == 0) {
            debugf("Received message from self.\n");
            goto dontread;
        }

        switch(message) {
        case REPLY:
            if(tid_len == 2 && memcmp(tid, "pn", 2) == 0) {
                debugf("Pong!\n");
                new_node(s, id, &source, 2);
            } else if((tid_len == 2 && memcmp(tid, "fn", 2) == 0) ||
                      (tid_len == 4 && memcmp(tid, "gp", 2) == 0)) {
                int gp = 0;
                struct search *sr = NULL;
                if(tid_len == 4 && memcmp(tid, "gp", 2) == 0) {
                    unsigned short ttid;
                    gp = 1;
                    memcpy(&ttid, tid + 2, 2);
                    sr = find_search(ttid);
                }
                debugf("Nodes found (%d)%s!\n", nodes_len / 26,
                       gp ? " for get_peers" : "");
                if(nodes_len % 26 != 0) {
                    debugf("Unexpected length for node info!\n");
                    broken_node(s, id, &source);
                } else if(gp && sr == NULL) {
                    debugf("Unknown search!\n");
                    new_node(s, id, &source, 1);
                } else {
                    int i;
                    new_node(s, id, &source, 2);
                    for(i = 0; i < nodes_len / 26; i++) {
                        unsigned char *ni = nodes + i * 26;
                        struct sockaddr_in sin;
                        if(id_cmp(ni, myid) == 0)
                            continue;
                        memset(&sin, 0, sizeof(sin));
                        sin.sin_family = AF_INET;
                        memcpy(&sin.sin_addr, ni + 20, 4);
                        memcpy(&sin.sin_port, ni + 24, 2);
                        new_node(s, ni, &sin, 0);
                        if(sr) {
                            insert_search_node(ni, &sin, sr, 0, NULL, 0);
                        }
                    }
                    if(sr)
                        /* Since we received a reply, the number of
                           requests in flight has decreased.  Let's push
                           another request. */
                        search_send_get_peers(s, sr, NULL);
                }
                if(sr) {
                    insert_search_node(id, &source, sr,
                                       1, token, token_len);
                    if(values_len > 0) {
                        debugf("Got values (%d)!\n", values_len / 6);
                        if(callback) {
                            (*callback)(closure, DHT_EVENT_VALUES,
                                        sr->id, (void*)values, values_len);
                        }
                    }
                }
            } else if(tid_len == 4 && memcmp(tid, "ap", 2) == 0) {
                struct search *sr;
                unsigned short ttid;
                debugf("Got reply to announce_peer.\n");
                memcpy(&ttid, tid + 2, 2);
                sr = find_search(ttid);
                if(!sr) {
                    debugf("Unknown search!");
                    new_node(s, id, &source, 1);
                } else {
                    int i;
                    new_node(s, id, &source, 2);
                    for(i = 0; i < sr->numnodes; i++)
                        if(id_cmp(sr->nodes[i].id, id) == 0) {
                            sr->nodes[i].request_time = 0;
                            sr->nodes[i].reply_time = now.tv_sec;
                            sr->nodes[i].acked = 1;
                        }
                    /* See comment for gp above. */
                    search_send_get_peers(s, sr, NULL);
                }
            } else if(tid_len == 2 &&
                      (memcmp(tid, "gp", 2) == 0 ||
                       memcmp(tid, "ap", 2) == 0)) {
                debugf("Broken node truncates search ids.\n");
                /* This is really annoying, as it means that we will
                   time-out all our searches that go through this node.
                   Kill it. */
                broken_node(s, id, &source);
            } else {
                debugf("Unexpected reply: ");
                debug_printable(buf, rc);
                debugf("\n");
                /* Potentially annoying too. */
            }
            break;
        case PING:
            debugf("Ping (%d)!\n", tid_len);
            new_node(s, id, &source, 1);
            debugf("Sending pong!\n");
            send_pong(s, (struct sockaddr*)&source, sizeof(source),
                      tid, tid_len);
            break;
        case FIND_NODE:
            debugf("Find node!\n");
            new_node(s, id, &source, 1);
            {
                struct bucket *b = find_bucket(target);
                if(b) {
                    debugf("Sending nodes from bucket.\n");
                    send_bucket_nodes(s,
                                      (struct sockaddr*)&source,
                                      sizeof(source),
                                      tid, tid_len, b, NULL, 0);
                }
            }
            break;
        case GET_PEERS:
            debugf("Get_peers!\n");
            new_node(s, id, &source, 1);
            if(id_cmp(info_hash, zeroes) == 0) {
                debugf("Eek!  Got get_peers with no info_hash.\n");
                break;
            } else {
                struct storage *st = find_storage(info_hash);
                if(st && st->numpeers > 0) {
                    int i0, n0, n1;
                    unsigned char token[TOKEN_SIZE];
                    make_token((unsigned char*)&source.sin_addr, port,
                               0, token);
                    i0 = random() % st->numpeers;
                    /* We treat peers as a circular list, and choose 50
                       peers starting at i0. */
                    n0 = MIN(st->numpeers - i0, 50);
                    n1 = n0 >= 50 ? 0 : MIN(50, i0);

                    send_peers_found(s, (struct sockaddr*)&source,
                                     sizeof(source), tid, tid_len,
                                     st->peers + i0, n0,
                                     st->peers, n1,
                                     token, TOKEN_SIZE);

                } else {
                    struct bucket *b = find_bucket(info_hash);
                    if(b) {
                        debugf("Sending nodes for get_peers.\n");
                        send_bucket_nodes(s, (struct sockaddr*)&source,
                                          sizeof(source),
                                          tid, tid_len, b,
                                          secret, sizeof(secret));
                    }
                }
            }
            break;
        case ANNOUNCE_PEER:
            debugf("Announce peer!\n");
            new_node(s, id, &source, 1);
            if(id_cmp(info_hash, zeroes) == 0) {
                debugf("Announce_peer with no info_hash.\n");
                break;
            }
            if(token_match(token, token_len,
                           (unsigned char*)&source.sin_addr, port)) {
                debugf("Incorrect token for announce_peer.\n");
                break;
            }
            if(port == 0) {
                debugf("Announce_peer with 0 port.\n");
                break;
            }
            storage_store(info_hash,
                          (unsigned char*)&source.sin_addr, port);
            send_peer_announced(s, (struct sockaddr*)&source,
                                sizeof(source), tid, tid_len);
        }
    }

 dontread:
    if(now.tv_sec >= rotate_secrets_time)
        rotate_secrets();

    if(now.tv_sec >= expire_stuff_time) {
        expire_buckets(s);
        expire_storage();
    }

    if(search_time > 0 && now.tv_sec >= search_time) {
        int i;
        for(i = 0; i < numsearches; i++) {
            struct search *sr = &searches[i];
            if(!sr->done && sr->step_time + 5 <= now.tv_sec) {
                search_step(s, sr, callback, closure);
            }
        }
                   
        search_time = 0;
        
        for(i = 0; i < numsearches; i++) {
            struct search *sr = &searches[i];
            if(!sr->done) {
                time_t tm = sr->step_time + 15 + random() % 10;
                if(search_time == 0 || search_time > tm)
                    search_time = tm;
            }
        }
    }

    if(now.tv_sec >= confirm_nodes_time) {
        struct bucket *b;
        int soon = 0;
        b = buckets;
        while(!soon && b) {
            struct bucket *q;
            if(b->time < now.tv_sec - 900) {
                /* This bucket hasn't seen any activity for a long
                   time.  Pick a random id in this bucket's range, and
                   send a request to a random node. */
                unsigned char id[20];
                struct node *n;
                int rc;
                
                rc = bucket_random(b, id);
                if(rc < 0)
                    memcpy(id, b->first, 20);
                
                q = b;
                /* If the bucket is empty, we try to fill it from
                   a neighbour.  We also sometimes do it gratuitiously
                   to recover from buckets full of broken nodes. */
                if(q->next && (q->count == 0 || random() % 7 == 0))
                    q = b->next;
                if(q->count == 0 || random() % 7 == 0) {
                    struct bucket *r;
                    r = previous_bucket(b);
                    if(r && r->count > 0)
                        q = r;
                }

                if(q) {
                    n = random_node(q);
                    if(n) {
                        debugf("Sending find_node "
                               "for bucket maintenance.\n");
                        send_find_node(s, (struct sockaddr*)&n->sin,
                                       sizeof(struct sockaddr_in),
                                       (unsigned char*)"fn", 2, id,
                                       n->reply_time >= now.tv_sec - 15);
                        pinged(s, n, q);
                        /* In order to avoid sending queries back-to-back,
                           give up for now and reschedule us soon. */
                        soon = 1;
                    }
                }
            }
            b = b->next;
        }

        if(!soon && mybucket_grow_time >= now.tv_sec - 150) {
            /* We've seen updates to our own bucket recently.  Try to
               improve our neighbourship. */
            unsigned char id[20];
            struct bucket *b, *q;
            struct node *n;
            
            memcpy(id, myid, 20);
            id[19] = random() % 0xFF;
            b = find_bucket(myid);
            q = b;
            if(q->next && (q->count == 0 || random() % 7 == 0))
                q = b->next;
            if(q->count == 0 || random() % 7 == 0) {
                struct bucket *r;
                r = previous_bucket(b);
                if(r && r->count > 0)
                    q = r;
            }

            if(q) {
                n = random_node(q);
                if(n) {
                    debugf("Sending find_node "
                           "for neighborhood maintenance.\n");
                    send_find_node(s, (struct sockaddr*)&n->sin,
                                   sizeof(struct sockaddr_in),
                                   (unsigned char*)"fn", 2, id,
                                   n->reply_time >= now.tv_sec - 15);
                    pinged(s, n, q);
                }
            }
            soon = 1;
        }

        if(soon)
            confirm_nodes_time = now.tv_sec + 5 + random() % 10;
        else
            confirm_nodes_time = now.tv_sec + 60 + random() % 120;
    }

    if(confirm_nodes_time > now.tv_sec)
        *tosleep = confirm_nodes_time - now.tv_sec;
    else
        *tosleep = 0;

    if(search_time > 0) {
        if(search_time <= now.tv_sec)
            *tosleep = 0;
        else if(*tosleep > search_time - now.tv_sec)
            *tosleep = search_time - now.tv_sec;
    }
    
    return find_bucket(myid)->count > 2;
}

int
dht_get_nodes(struct sockaddr_in *sins, int num)
{
    int i;
    struct bucket *b;

    i = 0;
    b = buckets;

    while(b && i < num) {
        struct node *n= b->nodes;
        while(n && i < num) {
            if(node_good(n)) {
                sins[i] = n->sin;
                i++;
            }
            n = n->next;
        }
        b = b->next;
    }
    return i;
}

int
dht_insert_node(int s, const unsigned char *id, struct sockaddr_in *sin)
{
    struct node *n;
    n = new_node(s, id, sin, 0);
    return !!n;
}

int
dht_ping_node(int s, struct sockaddr_in *sin)
{
    debugf("Sending ping.\n");
    return send_ping(s, (struct sockaddr*)sin, sizeof(struct sockaddr_in),
                     (unsigned char*)"pn", 2);
}

/* We could use a proper bencoding printer and parser, but the format of
   DHT messages is fairly stylised, so this seemed simpler. */

#define CHECK(offset, delta, size)                      \
    if(delta < 0 || offset + delta > size) goto fail

#define INC(offset, delta, size)                        \
    CHECK(offset, delta, size);                         \
    offset += delta

#define COPY(buf, offset, src, delta, size)             \
    CHECK(offset, delta, size);                         \
    memcpy(buf + offset, src, delta);                   \
    i += delta;

int
send_ping(int s, struct sockaddr *sa, int salen,
          const unsigned char *tid, int tid_len)
{
    char buf[512];
    int i = 0, rc;
    rc = snprintf(buf + i, 512 - i, "d1:ad2:id20:"); INC(i, rc, 512);
    COPY(buf, i, myid, 20, 512);
    rc = snprintf(buf + i, 512 - i, "e1:q4:ping1:t%d:", tid_len);
    INC(i, rc, 512);
    COPY(buf, i, tid, tid_len, 512);
    rc = snprintf(buf + i, 512 - i, "1:y1:qe"); INC(i, rc, 512);
    return sendto(s, buf, i, 0, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

int
send_pong(int s, struct sockaddr *sa, int salen,
          const unsigned char *tid, int tid_len)
{
    char buf[512];
    int i = 0, rc;
    rc = snprintf(buf + i, 512 - i, "d1:rd2:id20:"); INC(i, rc, 512);
    COPY(buf, i, myid, 20, 512);
    rc = snprintf(buf + i, 512 - i, "e1:t%d:", tid_len); INC(i, rc, 512);
    COPY(buf, i, tid, tid_len, 512);
    rc = snprintf(buf + i, 512 - i, "1:y1:re"); INC(i, rc, 512);
    return sendto(s, buf, i, 0, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

int
send_find_node(int s, struct sockaddr *sa, int salen,
               const unsigned char *tid, int tid_len,
               const unsigned char *target, int confirm)
{
    char buf[512];
    int i = 0, rc;
    rc = snprintf(buf + i, 512 - i, "d1:ad2:id20:"); INC(i, rc, 512);
    COPY(buf, i, myid, 20, 512);
    rc = snprintf(buf + i, 512 - i, "6:target20:"); INC(i, rc, 512);
    COPY(buf, i, target, 20, 512);
    rc = snprintf(buf + i, 512 - i, "e1:q9:find_node1:t%d:", tid_len);
    INC(i, rc, 512);
    COPY(buf, i, tid, tid_len, 512);
    rc = snprintf(buf + i, 512 - i, "1:y1:qe"); INC(i, rc, 512);
    return sendto(s, buf, i, confirm ? MSG_CONFIRM : 0, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

int
send_found_nodes(int s, struct sockaddr *sa, int salen,
                 const unsigned char *tid, int tid_len,
                 const unsigned char *nodes, int nodes_len,
                 const unsigned char *token, int token_len)
{
    char buf[2048];
    int i = 0, rc;
    rc = snprintf(buf + i, 2048 - i, "d1:rd2:id20:"); INC(i, rc, 2048);
    COPY(buf, i, myid, 20, 2048);
    if(nodes) {
        rc = snprintf(buf + i, 2048 - i, "5:nodes%d:", nodes_len);
        INC(i, rc, 2048);
        COPY(buf, i, nodes, nodes_len, 2048);
    }
    if(token) {
        rc = snprintf(buf + i, 2048 - i, "5:token%d:", token_len);
        INC(i, rc, 2048);
        COPY(buf, i, token, token_len, 2048);
    }
    rc = snprintf(buf + i, 2048 - i, "e1:t%d:", tid_len); INC(i, rc, 2048);
    COPY(buf, i, tid, tid_len, 2048);
    rc = snprintf(buf + i, 2048 - i, "1:y1:re"); INC(i, rc, 2048);

    return sendto(s, buf, i, 0, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

static int
buffer_bucket(unsigned char *buf, int bufsize, struct bucket *b)
{
    int i = 0;
    struct node *n = b->nodes;
    while(n && i < bufsize - 26) {
        if(node_good(n)) {
            memcpy(buf + i, n->id, 20);
            memcpy(buf + i + 20, &n->sin.sin_addr, 4);
            memcpy(buf + i + 24, &n->sin.sin_port, 2);
            i += 26;
        }
        n = n->next;
    }
    return i;
}

int
send_bucket_nodes(int s, struct sockaddr *sa, int salen,
                  const unsigned char *tid, int tid_len,
                  struct bucket *b,
                  const unsigned char *token, int token_len)
{
    unsigned char nodes[8 * 26];
    int nodeslen = 0;

    nodeslen = buffer_bucket(nodes, 8 * 26, b);
    return send_found_nodes(s, sa, salen, tid, tid_len,
                            nodes, nodeslen,
                            token, token_len);
}

int
send_get_peers(int s, struct sockaddr *sa, int salen,
               unsigned char *tid, int tid_len,
               unsigned char *infohash, unsigned short port, int confirm)
{
    char buf[512];
    int i = 0, rc;

    rc = snprintf(buf + i, 512 - i, "d1:ad2:id20:"); INC(i, rc, 512);
    COPY(buf, i, myid, 20, 512);
    rc = snprintf(buf + i, 512 - i, "9:info_hash20:"); INC(i, rc, 512);
    COPY(buf, i, infohash, 20, 512);
    rc = snprintf(buf + i, 512 - i, "e1:q9:get_peers1:t%d:", tid_len);
    INC(i, rc, 512);
    COPY(buf, i, tid, tid_len, 512);
    rc = snprintf(buf + i, 512 - i, "1:y1:qe"); INC(i, rc, 512);
    return sendto(s, buf, i, confirm ? MSG_CONFIRM : 0, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

int
send_announce_peer(int s, struct sockaddr *sa, int salen,
                   unsigned char *tid, int tid_len,
                   unsigned char *infohash, unsigned short port,
                   unsigned char *token, int token_len, int confirm)
{
    char buf[512];
    int i = 0, rc;

    rc = snprintf(buf + i, 512 - i, "d1:ad2:id20:"); INC(i, rc, 512);
    COPY(buf, i, myid, 20, 512);
    rc = snprintf(buf + i, 512 - i, "9:info_hash20:"); INC(i, rc, 512);
    COPY(buf, i, infohash, 20, 512);
    rc = snprintf(buf + i, 512 - i, "4:porti%ue5:token%d:", (unsigned)port,
                  token_len);
    INC(i, rc, 512);
    COPY(buf, i, token, token_len, 512);
    rc = snprintf(buf + i, 512 - i, "e1:q13:announce_peer1:t%d:", tid_len);
    INC(i, rc, 512);
    COPY(buf, i, tid, tid_len, 512);
    rc = snprintf(buf + i, 512 - i, "1:y1:qe"); INC(i, rc, 512);

    return sendto(s, buf, i, confirm ? 0 : MSG_CONFIRM, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

int
send_peers_found(int s, struct sockaddr *sa, int salen,
                 unsigned char *tid, int tid_len,
                 struct peer *peers1, int numpeers1,
                 struct peer *peers2, int numpeers2,
                 unsigned char *token, int token_len)
{
    char buf[1400];
    int i = 0, rc, j;

    rc = snprintf(buf + i, 1400 - i, "d1:rd2:id20:"); INC(i, rc, 1400);
    COPY(buf, i, myid, 20, 1400);
    rc = snprintf(buf + i, 1400 - i, "5:token%d:", token_len); INC(i, rc, 1400);
    COPY(buf, i, token, token_len, 1400);
    rc = snprintf(buf + i, 1400 - i, "6:valuesl"); INC(i, rc, 1400);
    for(j = 0; j < numpeers1; j++) {
        unsigned short swapped = htons(peers1[j].port);
        rc = snprintf(buf + i, 1400 - i, "6:"); INC(i, rc, 1400);
        COPY(buf, i, peers1[j].ip, 4, 1400);
        COPY(buf, i, &swapped, 2, 1400);
    }
    for(j = 0; j < numpeers2; j++) {
        unsigned short swapped = htons(peers2[j].port);
        rc = snprintf(buf + i, 1400 - i, "6:"); INC(i, rc, 1400);
        COPY(buf, i, peers2[j].ip, 4, 1400);
        COPY(buf, i, &swapped, 2, 1400);
    }
    rc = snprintf(buf + i, 1400 - i, "ee1:t%d", tid_len);
    INC(i, rc, 1400);
    COPY(buf, i, tid, tid_len, 1400);
    rc = snprintf(buf + i, 2048 - i, "1:y1:re"); INC(i, rc, 2048);
    return sendto(s, buf, i, 0, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

int
send_peer_announced(int s, struct sockaddr *sa, int salen,
                    unsigned char *tid, int tid_len)
{
    char buf[512];
    int i = 0, rc;

    rc = snprintf(buf + i, 512 - i, "d1:ad2:id20:"); INC(i, rc, 512);
    COPY(buf, i, myid, 20, 512);
    rc = snprintf(buf + i, 512 - i, ":t%d:", tid_len);
    INC(i, rc, 512);
    COPY(buf, i, tid, tid_len, 512);
    rc = snprintf(buf + i, 2048 - i, "1:y1:re"); INC(i, rc, 2048);
    return sendto(s, buf, i, 0, sa, salen);

 fail:
    errno = ENOSPC;
    return -1;
}

static int
parse_message(const unsigned char *buf, int buflen,
              unsigned char *tid_return, int *tid_len,
              unsigned char *id_return, unsigned char *info_hash_return,
              unsigned char *target_return, unsigned short *port_return,
              unsigned char *token_return, int *token_len,
              unsigned char *nodes_return, int *nodes_len,
              const unsigned char *values_return, int *values_len)
{
    const unsigned char *p;

    p = memmem(buf, buflen, "1:t", 3);
    if(p) {
        long l;
        char *q;
        l = strtol((char*)p + 3, &q, 10);
            if(q && *q == ':' && l > 0 && l < *tid_len) {
                memcpy(tid_return, q + 1, l);
                *tid_len = l;
            } else
                *tid_len = 0;
    }
    if(id_return) {
        p = memmem(buf, buflen, "2:id20:", 7);
        if(p) {
            memcpy(id_return, p + 7, 20);
        } else {
            memset(id_return, 0, 20);
        }
    }
    if(info_hash_return) {
        p = memmem(buf, buflen, "9:info_hash20:", 14);
        if(p) {
            memcpy(info_hash_return, p + 14, 20);
        } else {
            memset(info_hash_return, 0, 20);
        }
    }
    if(port_return) {
        p = memmem(buf, buflen, "porti", 5);
        if(p) {
            long l;
            char *q;
            l = strtol((char*)p + 5, &q, 10);
            if(q && *q == 'e' && l > 0 && l < 0x10000)
                *port_return = l;
            else
                *port_return = 0;
        } else
            *port_return = 0;
    }
    if(target_return) {
        p = memmem(buf, buflen, "6:target20:", 11);
        if(p) {
            memcpy(target_return, p + 11, 20);
        } else {
            memset(target_return, 0, 20);
        }
    }
    if(token_return) {
        p = memmem(buf, buflen, "5:token", 7);
        if(p) {
            long l;
            char *q;
            l = strtol((char*)p + 7, &q, 10);
            if(q && *q == ':' && l > 0 && l < *token_len) {
                memcpy(token_return, q + 1, l);
                *token_len = l;
            } else
                *token_len = 0;
        } else
            *token_len = 0;
    }
        
    if(nodes_return) {
        p = memmem(buf, buflen, "5:nodes", 7);
        if(p) {
            long l;
            char *q;
            l = strtol((char*)p + 7, &q, 10);
            if(q && *q == ':' && l > 0 && l < *nodes_len) {
                memcpy(nodes_return, q + 1, l);
                *nodes_len = l;
            } else
                *nodes_len = 0;
        } else
            *nodes_len = 0;
    }

    if(values_return) {
        p = memmem(buf, buflen, "6:valuesl", 9);
        if(p) {
            int i = p - buf + 9;
            int j = 0;
            while(buf[i] == '6' && buf[i + 1] == ':' && i + 8 < buflen) {
                if(j + 6 > *values_len)
                    break;
                memcpy((char*)values_return + j, buf + i + 2, 6);
                i += 8;
                j += 6;
            }
            if(buf[i] != 'e')
                debugf("Eek... unexpected end for values.\n");
            *values_len = j;
        } else {
            *values_len = 0;
        }
    }
                
    if(memmem(buf, buflen, "1:y1:r", 6))
        return REPLY;
    if(!memmem(buf, buflen, "1:y1:q", 6))
        return -1;
    if(memmem(buf, buflen, "1:q4:ping", 9))
        return PING;
    if(memmem(buf, buflen, "1:q9:find_node", 14))
       return FIND_NODE;
    if(memmem(buf, buflen, "1:q9:get_peers", 14))
        return GET_PEERS;
    if(memmem(buf, buflen, "1:q13:announce_peer", 19))
       return ANNOUNCE_PEER;
    return -1;
}
