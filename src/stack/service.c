/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- 
 *
 * Serval's service table.
 *
 * Authors: Erik Nordström <enordstr@cs.princeton.edu>
 *          David Shue <dshue@cs.princeton.edu>
 * 
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation; either version 2 of
 *	the License, or (at your option) any later version.
 */
#include <serval/platform.h>
#include <serval/netdevice.h>
#include <serval/atomic.h>
#include <serval/debug.h>
#include <serval/list.h>
#include <serval/lock.h>
#include <serval/dst.h>
#include <netinet/serval.h>
#if defined(OS_USER)
#include <stdlib.h>
#include <errno.h>
#endif
#if defined(OS_LINUX_KERNEL)
#include <serval_ipv4.h>
#endif
#include "service.h"
#include "radixtree.h"

#define get_service(n) radix_node_private(n, struct service_entry)

struct service_table {
        struct radix_tree tree;
        atomic_t bytes_resolved;
        atomic_t packets_resolved;
        atomic_t bytes_dropped;
        atomic_t packets_dropped;
        rwlock_t lock;
};

static struct service_table srvtable;

static const char *rule_str[] = {
        [SERVICE_RULE_UNDEFINED] = "UDF",
        [SERVICE_RULE_FORWARD] = "FWD",
        [SERVICE_RULE_DEMUX] = "DMX",
        [SERVICE_RULE_DELAY] = "DLY",
        [SERVICE_RULE_DROP] = "DRP"
};

static const char *rule_to_str(service_rule_type_t type)
{
        return rule_str[type];
}

static const char *protocol_to_str(int protocol)
{
        static char buf[20];
        
        switch (protocol) {
        case IPPROTO_TCP:
                sprintf(buf, "TCP");
                break;
        case IPPROTO_UDP:
                sprintf(buf, "UDP");
                break;
        default:
                sprintf(buf, "%d", protocol);
                break;
        }
        
        return buf;
}

static struct target *target_create(service_rule_type_t type,
                                    const void *dst, int dstlen,
                                    const union target_out out, 
                                    uint32_t weight,
                                    gfp_t alloc) 
{
        struct target *t;

        if (dstlen == 0 && out.raw == NULL)
                return NULL;

        t = (struct target *)kmalloc(sizeof(*t) + dstlen, alloc);

        if (!t)
                return NULL;

        memset(t, 0, sizeof(*t) + dstlen);
        t->type = type;
        t->weight = weight;
        t->dstlen = dstlen;

        if (dstlen > 0) {
                t->out.oif = out.oif;
                memcpy(t->dst, dst, dstlen);
        } else {
                t->out.sk = out.sk;
                sock_hold(t->out.sk);
                t->dstlen = 0;
        }

        INIT_LIST_HEAD(&t->lh);

        return t;
}

static void target_free(struct target *t) 
{
        if (is_sock_target(t) && t->out.sk)
                sock_put(t->out.sk);
        kfree(t);
}

static struct target_set *target_set_create(uint16_t flags, 
                                            uint32_t priority, 
                                            gfp_t alloc) {
        struct target_set *set;
        
        set = (struct target_set *)kmalloc(sizeof(*set), alloc);

        if (!set)
                return NULL;

        memset(set, 0, sizeof(*set));
        set->flags = flags;
        set->priority = priority;

        INIT_LIST_HEAD(&set->lh);
        INIT_LIST_HEAD(&set->list);

        return set;
}

static void target_set_free(struct target_set *set) 
{
        struct target *t;
       
        while (!list_empty(&set->list)) {
                t = list_first_entry(&set->list, struct target, lh);
                list_del(&t->lh);
                target_free(t);
        }
        kfree(set);
}

static struct target *__service_entry_get_dev(struct service_entry *se, 
                                              unsigned int ifindex) 
{
        struct target *t;
        struct target_set* set = NULL;
        
        list_for_each_entry(set, &se->target_set, lh) {
                list_for_each_entry(t, &set->list, lh) {
                        if (!is_sock_target(t) && 
                            t->out.oif == ifindex)
                                return t;
                }
        }

        return NULL;
}

enum {
        MATCH_NO_PROTOCOL = -1,
        MATCH_ANY_PROTOCOL = 0,
};

static int is_target(struct target *t, 
                     service_rule_type_t type, 
                     const void *dst,
                     int dstlen)
{
        if (t->type != type)
                return 0;

        switch (type) {
        case SERVICE_RULE_DEMUX:
                if (!dst || t->out.raw == dst)
                        return 1;
                break;
        case SERVICE_RULE_FORWARD:
                if (!dst || memcmp(t->dst, dst, dstlen) == 0)
                        return 1;
                break;
        default:
                return 1;
        }
        return 0;
}

static struct target * __service_entry_get_target(struct service_entry *se,
                                                  service_rule_type_t type,
                                                  const void *dst,
                                                  int dstlen,
                                                  const union target_out out,
                                                  struct target_set **set_p,
                                                  int protocol) 
{
        struct target *t = NULL;
        struct target_set* set = NULL;

        list_for_each_entry(set, &se->target_set, lh) {
                list_for_each_entry(t, &set->list, lh) {
                        if (t->type != type )
                                continue;

                        if (is_target(t, type, dst, dstlen)) {
                                if (type == SERVICE_RULE_DEMUX) {
                                        if ((t->out.sk->sk_protocol ==
                                             protocol) || 
                                            (protocol == MATCH_ANY_PROTOCOL)) {
                                                if (set_p)
                                                        *set_p = set;
                                                return t;
                                        }
                                } else {
                                         if (set_p)
                                                *set_p = set;
                                        return t;
                                }
                        }
                }
        }
        
        return NULL;
}

/* 
   The returned net_device will have an increased reference count, so
   a put is necessary following a successful call to this
   function.
*/
struct net_device *service_entry_get_dev(struct service_entry *se, 
                                         unsigned int ifindex) 
{
        struct target *t = NULL;
        struct net_device *dev = NULL;

        read_lock_bh(&se->lock);

        t = __service_entry_get_dev(se, ifindex);

        if (t) {
                dev = __dev_get_by_index(&init_net, ifindex);

                if (dev)
                        dev_hold(dev);
        }
        read_unlock_bh(&se->lock);

        return dev;
}

static void __target_set_add_target(struct target_set *set, 
                                    struct target *t) 
{
        list_add_tail(&t->lh, &set->list);
        set->normalizer += t->weight;
        set->count++;
}

static void __service_entry_insert_target_set(struct service_entry *se, 
                                              struct target_set *set) 
{

        struct target_set *pos = NULL;
        list_for_each_entry(pos, &se->target_set, lh) {
                if (pos->priority < set->priority) {
                        list_add_tail(&set->lh, &pos->lh);
                        return;
                }
        }
        list_add_tail(&set->lh, &se->target_set);
}

static struct target_set *
__service_entry_get_target_set(struct service_entry *se, 
                               uint32_t priority) 
{
        struct target_set *pos = NULL;

        list_for_each_entry(pos, &se->target_set, lh) {
                if (pos->priority == priority)
                        return pos;
        }

        return NULL;
}

static void target_set_remove_target(struct target_set *set, struct target* t) 
{
        set->normalizer -= t->weight;
        list_del(&t->lh);
        set->count--;
}

static int __service_entry_modify_target(struct service_entry *se,
                                         service_rule_type_t type,
                                         uint16_t flags, uint32_t priority,
                                         uint32_t weight, 
                                         const void *dst, 
                                         int dstlen, 
                                         const void *new_dst, 
                                         int new_dstlen, 
                                         const union target_out out, 
                                         gfp_t alloc) 
{
        struct target_set *set = NULL;
        struct target *t;

        if (type == SERVICE_RULE_DEMUX || dstlen == 0) {
                LOG_ERR("Cannot modify socket entry\n");
                return -1;
        }
        
        t = __service_entry_get_target(se, type, dst, dstlen, 
                                       out, &set,
                                       MATCH_NO_PROTOCOL);
        
        if (!t) {
                LOG_DBG("Could not find matching target entry\n");
                return 0;
        }
        
#if defined(OS_LINUX_KERNEL)
        /* Make sure it makes sense to add this target address on this
           interface */
        {
                struct rtable *rt;
                __be32 dst_ip = *((__be32 *)new_dst);
                
                /* FIXME: This routing does not work as expected. It
                   returns a valid entry even if the dst_ip does not
                   really match the subnet/destination of the returned
                   routing entry. We're probably giving some weird
                   input... */
                rt = serval_ip_route_output(&init_net, 
                                            dst_ip,
                                            0, 0, t->out.oif);
                if (!rt)
                        return 0;
        }
#endif

        if (new_dstlen == t->dstlen && new_dst)
                memcpy(t->dst, new_dst, new_dstlen);

        if (set->priority != priority) {
                struct target_set *nset;

                /* We are changing the priority of a target, which
                   means we need to move it to the set corresponding
                   to that priority */
                nset = __service_entry_get_target_set(se, priority);
                
                if (!nset) {
                        nset = target_set_create(flags, priority, alloc);

                        if (!nset)
                                return -ENOMEM;

                        __service_entry_insert_target_set(se, nset);
                }

                target_set_remove_target(set, t);

                if (set->count == 0) {
                        list_del(&set->lh);
                        target_set_free(set);
                }

                t->weight = weight;
                __target_set_add_target(nset, t);
                nset->flags = flags;
        } else {
                /*adjust the normalizer*/
                set->normalizer -= t->weight;
                t->weight = weight;
                set->normalizer += t->weight;
                set->flags = flags;
        }

        return 1;
}

int service_entry_modify_target(struct service_entry *se, 
                                service_rule_type_t type,
                                uint16_t flags, uint32_t priority,
                                uint32_t weight, 
                                const void *dst, 
                                int dstlen, 
                                const void *new_dst, 
                                int new_dstlen, 
                                const union target_out out,
                                gfp_t alloc) 
{
        int ret = 0;
        
        write_lock_bh(&se->lock);
        ret = __service_entry_modify_target(se, type, flags, priority, 
                                            weight, dst, dstlen, 
                                            new_dst, new_dstlen,
                                            out, GFP_ATOMIC);
        write_unlock_bh(&se->lock);

        return ret;
}


static void __service_entry_inc_target_stats(struct service_entry *se, 
                                             service_rule_type_t type,
                                             const void *dst, int dstlen, 
                                             int packets, int bytes) 
{
        struct target_set* set = NULL;
        struct target *t = __service_entry_get_target(se, type, dst, dstlen, 
                                                      make_sock_target(NULL), 
                                                      &set, 
                                                      MATCH_NO_PROTOCOL);

        if (!t)
                return;

        if (packets > 0) {
                atomic_add(packets, &t->packets_resolved);
                atomic_add(bytes, &t->bytes_resolved);

                atomic_add(packets, &se->packets_resolved);
                atomic_add(bytes, &se->bytes_resolved);

                atomic_add(packets, &srvtable.packets_resolved);
                atomic_add(bytes, &srvtable.bytes_resolved);
        } else {
                atomic_add(-packets, &t->packets_dropped);
                atomic_add(-bytes, &t->bytes_dropped);

                atomic_add(-packets, &se->packets_dropped);
                atomic_add(-bytes, &se->bytes_dropped);

                atomic_add(-packets, &srvtable.packets_dropped);
                atomic_add(-bytes, &srvtable.bytes_dropped);
        }

}
void service_entry_inc_target_stats(struct service_entry *se,
                                    service_rule_type_t type,
                                    const void* dst, int dstlen, 
                                    int packets, int bytes) 
{
        /* using a read lock since we are atomically updating stats and
           not modifying the set/target itself */
        read_lock_bh(&se->lock);
        __service_entry_inc_target_stats(se, type, dst, dstlen, packets, bytes);
        read_unlock_bh(&se->lock);
}

int __service_entry_remove_target_by_dev(struct service_entry *se, 
                                         unsigned int ifindex) 
{
        struct target *t;
        struct target *dtemp = NULL;
        struct target_set* set = NULL;
        struct target_set* setemp = NULL;
        int count = 0;

        list_for_each_entry_safe(set, setemp, &se->target_set, lh) {
                list_for_each_entry_safe(t, dtemp, &set->list, lh) {
                        if (t->type == SERVICE_RULE_FORWARD && 
                            t->out.oif == ifindex) {
                                target_set_remove_target(set, t);
                                target_free(t);
                                
                                if (set->count == 0) {
                                        list_del(&set->lh);
                                        target_set_free(set);
                                }
                                se->count--;
                                count++;
                        }
                }
        }

        return count;
}

int service_entry_remove_target_by_dev(struct service_entry *se, 
                                       unsigned int ifindex) {
        int ret;
        
        service_entry_hold(se);

        write_lock_bh(&se->lock);
        
        ret = __service_entry_remove_target_by_dev(se, ifindex);
        
        if (ret > 0) {
                if (list_empty(&se->target_set)) {
                        write_lock(&srvtable.lock);
                        radix_node_remove(se->node, GFP_ATOMIC);
                        write_unlock(&srvtable.lock);
                }
        }

        write_unlock_bh(&se->lock);

        service_entry_put(se);

        return ret;
}

int __service_entry_remove_target(struct service_entry *se, 
                                  service_rule_type_t type,
                                  const void *dst, int dstlen,
                                  struct target_stats *stats) 
{
        struct target *t;
        struct target_set* set = NULL;
        
        list_for_each_entry(set, &se->target_set, lh) {
                list_for_each_entry(t, &set->list, lh) {
                        if (t->type == type && 
                            ((t->type == SERVICE_RULE_DEMUX && dstlen == 0) || 
                            (t->type == SERVICE_RULE_FORWARD && 
                             memcmp(t->dst, dst, dstlen) == 0))) {
                                target_set_remove_target(set, t);
                                
                                if (stats) {
                                        stats->packets_resolved = atomic_read(&t->packets_resolved);
                                        stats->bytes_resolved = atomic_read(&t->bytes_resolved);
                                        stats->packets_dropped = atomic_read(&t->packets_dropped);
                                        stats->bytes_dropped = atomic_read(&t->bytes_dropped);
                                }
                                
                                target_free(t);
                                
                                if (set->count == 0) {
                                        list_del(&set->lh);
                                        target_set_free(set);
                                }
                                se->count--;
                                return 1;
                        }
                }
        }
        return 0;
}

int service_entry_remove_target(struct service_entry *se,
                                service_rule_type_t type,
                                const void *dst, int dstlen,
                                struct target_stats *stats) 
{
        int ret;
        
        write_lock_bh(&se->lock);

        ret = __service_entry_remove_target(se, type, dst, dstlen, stats);
        
        if (list_empty(&se->target_set)) {
                write_lock_bh(&srvtable.lock);
                radix_node_remove(se->node, GFP_ATOMIC);
                write_unlock_bh(&srvtable.lock);
                write_unlock_bh(&se->lock);
                service_entry_put(se);
        } else {
                write_unlock_bh(&se->lock);
        }

        return ret;
}

static struct service_entry *service_entry_create(struct service_table *tbl, 
                                                  gfp_t alloc) 
{
        struct service_entry *se;

        se = (struct service_entry *)kmalloc(sizeof(*se), alloc);

        if (!se)
                return NULL;

        memset(se, 0, sizeof(*se));
        se->tbl = tbl;
        INIT_LIST_HEAD(&se->target_set);
        rwlock_init(&se->lock);
        atomic_set(&se->refcnt, 1);

        return se;
}

void __service_entry_free(struct service_entry *se) 
{
        struct target_set *set;
        
        while (!list_empty(&se->target_set)) {
                set = list_first_entry(&se->target_set, 
                                       struct target_set, lh);
                list_del(&set->lh);
                target_set_free(set);
        }

        rwlock_destroy(&se->lock);
        kfree(se);
}

void service_entry_hold(struct service_entry *se) 
{
        atomic_inc(&se->refcnt);
        /* LOG_DBG("%p refcount=%u\n",
                se, atomic_read(&se->refcnt));
        */
}

void service_entry_put(struct service_entry *se) 
{
        /* LOG_DBG("%p refcount=%u\n",
                se, atomic_read(&se->refcnt));
        */
        if (atomic_dec_and_test(&se->refcnt))
                __service_entry_free(se);
}

static void service_entry_free(struct service_entry *se) 
{
        service_entry_put(se);
}

int service_iter_init(struct service_iter *iter, 
                      struct service_entry *se,
                      iter_mode_t mode) 
{
        /* lock the service entry, take the top priority entry and
         * determine the extent of iteration */
        struct target_set *set;

        memset(iter, 0, sizeof(*iter));
        
        iter->mode = mode;
        iter->entry = se;
        read_lock_bh(&se->lock);

        if (se->count == 0 || list_empty(&se->target_set))
                return -1;

        set = list_first_entry(&se->target_set, struct target_set, lh);
        
        if (!set)
                return -1;
        
        if (mode == SERVICE_ITER_ANYCAST &&
            set->flags & SVSF_MULTICAST)
                return -1;
 
        /* round robin or sample */
        if (mode == SERVICE_ITER_ALL ||
            mode == SERVICE_ITER_DEMUX ||
            mode == SERVICE_ITER_FORWARD) {
                iter->pos = set->list.next;
                iter->set = set;
        } else {
#define SAMPLE_SHIFT 32
                struct target *t = NULL;
                uint64_t sample, sumweight = 0;
#if defined(OS_LINUX_KERNEL)
                uint32_t rand;
                unsigned long rem;
                get_random_bytes(&rand, sizeof(rand));
                sample = rand;
                sample = sample << SAMPLE_SHIFT;
                rem = 0xffffffff;
                rem = do_div(sample, rem);
#else
                sample = random();
                sample = (sample << SAMPLE_SHIFT) / RAND_MAX;
#endif
                sample = sample * set->normalizer;

                /*
                  LOG_DBG("sample=%llu normalizer=%u\n", 
                  sample, set->normalizer);
                */
                list_for_each_entry(t, &set->list, lh) {
                        uint64_t weight = t->weight;
                        
                        sumweight += (weight << SAMPLE_SHIFT);

                        if (sample <= sumweight) {
                                iter->pos = &t->lh;
                                iter->set = NULL;
                                return 0;
                        }
                }
                
                if (t) {
                        iter->pos = &t->lh;
                        iter->set = NULL;
                }
        }
        return 0;
}

void service_iter_destroy(struct service_iter *iter) 
{
        iter->pos = NULL;
        iter->set = NULL;
        read_unlock_bh(&iter->entry->lock);
}

struct target *service_iter_next(struct service_iter *iter)
{
        struct target *t;

        iter->last_pos = iter->pos;

        if (iter->pos == NULL)
                return NULL;

        while (1) {
                t = list_entry(iter->pos, struct target, lh);
                
                if (iter->set) {
                        if (iter->pos == &iter->set->list) {
                                /* We've reached the head again. */
                                t = NULL;
                                break;
                        } else {
                                iter->pos = t->lh.next;
                                
                                if (iter->mode == SERVICE_ITER_ALL)
                                        break;
                                else if (iter->mode == SERVICE_ITER_DEMUX &&
                                         t->type == SERVICE_RULE_DEMUX)
                                        break;
                                else if (iter->mode == SERVICE_ITER_FORWARD &&
                                         t->type == SERVICE_RULE_FORWARD)
                                        break;
                        }
                } else {
                        iter->pos = NULL;
                        break;
                }
        }

        return t;
}

void service_iter_inc_stats(struct service_iter *iter, 
                            int packets, int bytes) 
{
        struct target *dst = NULL;

        if (iter == NULL)
                return;

        if (packets > 0) {
                if (iter->last_pos == NULL)
                        return;

                dst = list_entry(iter->last_pos, struct target, lh);

                atomic_add(packets, &dst->packets_resolved);
                atomic_add(bytes, &dst->bytes_resolved);

                atomic_add(packets, &iter->entry->packets_resolved);
                atomic_add(bytes, &iter->entry->bytes_resolved);

                atomic_add(packets, &srvtable.packets_resolved);
                atomic_add(bytes, &srvtable.bytes_resolved);

        } else {
                if (iter->last_pos != NULL) {
                        dst = list_entry(iter->last_pos, struct target, lh);
                        atomic_add(-packets, &dst->packets_dropped);
                        atomic_add(-bytes, &dst->bytes_dropped);
                }

                atomic_add(-packets, &iter->entry->packets_dropped);
                atomic_add(-bytes, &iter->entry->bytes_dropped);

                atomic_add(-packets, &srvtable.packets_dropped);
                atomic_add(-bytes, &srvtable.bytes_dropped);
        }
}

int service_iter_get_priority(struct service_iter* iter) 
{
        if (iter == NULL)
                return 0;

        if (iter->last_pos != NULL && iter->set)
                return iter->set->priority;

        return 0;
}

int service_iter_get_flags(struct service_iter *iter)
{
        if (iter == NULL)
                return 0;

        if (iter->last_pos != NULL && iter->set)
                return iter->set->flags;

        return 0;
}

struct print_buf {
        char *buf;
        size_t buflen;
        size_t totlen;
};

static inline void update_print_buf(struct print_buf *b,
                                    int len)
{
        if (len > 0 && b->buflen >= len) {
                b->buflen -= len;
                b->totlen += len;
        }
}

static int __service_entry_print(struct radix_node *n, void *arg) 
{
        struct service_entry *se = get_service(n);
        struct target_set *set;
        struct target *t;
        struct print_buf *pb = (struct print_buf *)arg;
        char dststr[18]; /* Currently sufficient for IPv4 */
        char node_buf[sizeof(struct service_id)];
        size_t old_totlen = pb->totlen;
        int len = 0;
        
        if (!radix_node_is_active(n))
                return 0;
        
        read_lock_bh(&se->lock);
        
        radix_node_print(n, node_buf, sizeof(node_buf));
        
        list_for_each_entry(set, &se->target_set, lh) {
                list_for_each_entry(t, &set->list, lh) {
                        len = snprintf(pb->buf + pb->totlen, pb->buflen, 
                                       "%-64s %-4s %-5u %-6u %-6u %-8u %-7u ", 
                                       node_buf,
                                       rule_to_str(t->type),
                                       set->flags, 
                                       set->priority, 
                                       t->weight,
                                       atomic_read(&t->packets_resolved),
                                       atomic_read(&t->packets_dropped));
                        
                        update_print_buf(pb, len);
                        
                        if (t->type == SERVICE_RULE_DEMUX && t->out.sk) {
                                len = snprintf(pb->buf + pb->totlen, 
                                               pb->buflen, 
                                               "%-5s %s\n", 
                                               t->out.sk ? 
                                               "sock" : "NULL",
                                               protocol_to_str(t->out.sk->sk_protocol));
                                
                                update_print_buf(pb, len);
                        } else if (t->type == SERVICE_RULE_FORWARD) {
                                struct net_device *dev;

                                dev = __dev_get_by_index(&init_net, t->out.oif);
                                len = snprintf(pb->buf + pb->totlen, 
                                               pb->buflen, 
                                               "%-5s %s\n",
                                               dev ? dev->name : "any",
                                               inet_ntop(AF_INET,
                                                         t->dst, 
                                                         dststr, 18));
                                update_print_buf(pb, len);
                        } else {
                                len = snprintf(pb->buf + pb->totlen, 
                                               pb->buflen, 
                                               "-\n");
                                update_print_buf(pb, len);
                        }
                }
        }
        
        read_unlock_bh(&se->lock);

        return (int)(pb->totlen - old_totlen);
}

int service_entry_print(struct service_entry *se, char *buf, size_t buflen) 
{
        struct print_buf pb = { buf, buflen, 0 };
        
        __service_entry_print(se->node, &pb);

        return (int)pb.totlen;
}

void service_table_read_lock(void)
{
        read_lock_bh(&srvtable.lock);
}

void service_table_read_unlock(void)
{
        read_unlock_bh(&srvtable.lock);
}

void service_table_iterator_init(service_table_iterator_t *iter)
{
        read_lock_bh(&srvtable.lock);
        radix_tree_iterator_init(&srvtable.tree, iter);
}

void service_table_iterator_destroy(service_table_iterator_t *iter)
{
        read_unlock_bh(&srvtable.lock);
}

struct service_entry *
service_table_iterator_next(service_table_iterator_t *iter)
{
        struct radix_node *n = radix_tree_iterator_next(iter);

        if (!n)
                return NULL;

        return get_service(n);
}

int service_table_print_header(char *buf, size_t buflen)
{
        return snprintf(buf, buflen, 
                        "%-64s %-4s %-5s %-6s %-6s %-8s %-7s %s\n", 
                        "prefix", "type", "flags", "prio", "weight", 
                        "resolved", "dropped", "target(s)");
}

int service_table_print(char *buf, size_t buflen)
{
        int tot_len = 0, len = 0;
        struct radix_tree_iterator iter;
        struct radix_node *n;

        radix_tree_iterator_init(&srvtable.tree, &iter);
        
        len = service_table_print_header(buf, buflen);

        tot_len += len;
        
        if (len > buflen)
                buflen = 0;
        else
                buflen -= len;

        read_lock_bh(&srvtable.lock);
        
        while (1) {
                n = radix_tree_iterator_next(&iter);

                if (!n)
                        break;

                len = service_entry_print(get_service(n), buf + tot_len, 
                                          buflen - tot_len);

                tot_len += len;
                
                if (len > buflen)
                        buflen = 0;
                else
                        buflen -= len;
        }
                
        read_unlock_bh(&srvtable.lock);
       
        radix_tree_iterator_destroy(&iter);

        return tot_len;
}

static int service_entry_local_match(struct radix_node *n)
{
        struct service_entry *se = get_service(n);
        struct target *t;

        t = __service_entry_get_target(se, SERVICE_RULE_DEMUX, 
                                       NULL, 0, 
                                       make_sock_target(NULL), NULL, 
                                       MATCH_ANY_PROTOCOL);
        
        if (t && t->out.sk) 
                return 1;

        return 0;
}

static int service_entry_global_match(struct radix_node *n)
{
        struct service_entry *se = get_service(n);        
        struct target *t;

        t = __service_entry_get_target(se, SERVICE_RULE_FORWARD, 
                                       NULL, 0, 
                                       make_sock_target(NULL), NULL, 
                                       MATCH_NO_PROTOCOL);
        
        if (t)
                return 1;

        return 0;
}

static int service_entry_any_match(struct radix_node *n)
{
        /* Any entry will match */        
        return 1;
}

static struct service_entry *__service_table_find(struct service_table *tbl,
                                                  struct service_id *srvid, 
                                                  rule_match_t match) 
{
        struct radix_node *n;
        int (*func)(struct radix_node *) = NULL;
        struct service_entry *se = NULL;

        if (!srvid)
                return NULL;
        
        LOG_DBG("service find %s\n",
                service_id_to_str(srvid));

        switch (match) {
        case RULE_MATCH_LOCAL:
                func = service_entry_local_match;
                break;
        case RULE_MATCH_GLOBAL:
        case RULE_MATCH_EXACT:
                func = service_entry_global_match;
                break;
        case RULE_MATCH_ANY:
                func = service_entry_any_match;
                break;
        }

        n = radix_tree_find(&tbl->tree, srvid->s_sid, func);
        
        if (n) {
                if (match == RULE_MATCH_EXACT) {
                        struct service_id id;                        
                        radix_node_get_key(n, &id, sizeof(id));
                        
                        if (service_id_cmp(&id, srvid) != 0)
                                return NULL;
                } 
                se = get_service(n);
                service_entry_hold(se);
        }

        return se;
}

static struct service_entry *service_table_find(struct service_table *tbl,
                                                struct service_id *srvid, 
                                                rule_match_t match)
{
        struct service_entry *se = NULL;

        read_lock_bh(&tbl->lock);

        se = __service_table_find(tbl, srvid, match);

        read_unlock_bh(&tbl->lock);

        return se;        
}

static struct sock* service_table_find_sock(struct service_table *tbl, 
                                            struct service_id *srvid,
                                            int protocol) 
{
        struct service_entry *se = NULL;
        struct sock *sk = NULL;
        
        if (!srvid)
                return NULL;
        
        read_lock_bh(&tbl->lock);

        se = __service_table_find(tbl, srvid, RULE_MATCH_LOCAL);
        
        if (se) {
                struct target *t;
                t = __service_entry_get_target(se, SERVICE_RULE_DEMUX, NULL, 0, 
                                               make_sock_target(NULL), 
                                               NULL, protocol);
                
                if (t) {
                        sk = t->out.sk;
                        sock_hold(sk);
                }
                service_entry_put(se);
        }
        
        read_unlock_bh(&tbl->lock);

        return sk;
}

static void service_table_get_stats(struct service_table *tbl, 
                                    struct table_stats *tstats) 
{
        
        /* TODO - not sure if the read lock here should be bh, since
         * this function will generally be called from a user-process
         * initiated netlink/ioctl/proc call
         */
        read_lock_bh(&tbl->lock);
        tstats->bytes_resolved = atomic_read(&tbl->bytes_resolved);
        tstats->packets_resolved = atomic_read(&tbl->packets_resolved);
        tstats->bytes_dropped = atomic_read(&tbl->bytes_dropped);
        tstats->packets_dropped = atomic_read(&tbl->packets_dropped);
        read_unlock_bh(&tbl->lock);
}

int service_get_id(const struct service_entry *se, struct service_id *srvid)
{
        if (!se || !se->node || !srvid)
                return -1;

        memset(srvid, '\0', sizeof(*srvid));
        return radix_node_get_key(se->node, srvid, sizeof(*srvid)); 
}

void service_get_stats(struct table_stats* tstats) 
{
        return service_table_get_stats(&srvtable, tstats);
}

struct service_entry *service_find_type(struct service_id *srvid,
                                        rule_match_t match) 
{
        return service_table_find(&srvtable, srvid, match);
}

struct sock *service_find_sock(struct service_id *srvid, int protocol) 
{
        return service_table_find_sock(&srvtable, srvid, protocol);
}

static int service_table_modify(struct service_table *tbl,
                                struct service_id *srvid,
                                service_rule_type_t type,
                                uint16_t flags, 
                                uint32_t priority, 
                                uint32_t weight, 
                                const void *dst,
                                int dstlen, 
                                const void *new_dst,
                                int new_dstlen, 
                                const union target_out out) 
{
        struct radix_node *n;
        struct service_entry *se;
        int ret = 0;

        if (!srvid)
                return -EINVAL;
        
        read_lock_bh(&tbl->lock);
        
        n = radix_tree_find(&tbl->tree, srvid->s_sid, NULL);
        
        if (n) {
                se = get_service(n);
                service_entry_hold(se);
                read_unlock_bh(&tbl->lock);

                if (dst || dstlen == 0) {
                        ret = service_entry_modify_target(se, type, 
                                                          flags, priority, 
                                                          weight, dst, dstlen,
                                                          new_dst, new_dstlen,
                                                          out, GFP_ATOMIC);
                }
                service_entry_put(se);
        } else {
                read_unlock_bh(&tbl->lock);
                ret = -EINVAL;
        }

        return ret;
}

int service_modify(struct service_id *srvid, 
                   service_rule_type_t type,
                   uint16_t flags,
                   uint32_t priority, 
                   uint32_t weight, 
                   const void *dst, 
                   int dstlen,    
                   const void *new_dst, 
                   int new_dstlen, 
                   const union target_out out) 
{
        return service_table_modify(&srvtable, srvid, type, flags,
                                    priority, weight == 0 ? 1 : weight, 
                                    dst, dstlen, new_dst, new_dstlen, 
                                    out);
}

static int service_table_add(struct service_table *tbl,
                             struct service_id *srvid,
                             service_rule_type_t type,
                             uint16_t flags, 
                             uint32_t priority, 
                             uint32_t weight, 
                             const void *dst,
                             int dstlen, 
                             const union target_out out, 
                             gfp_t alloc) {
        struct service_entry *se;
        struct radix_node *n = NULL;
        struct target_set *set = NULL;
        struct target *t = NULL;
        int ret = 0;
        
        if (!srvid)
                return -EINVAL;

        //        printf("In service_table_add, srvid: %s\n", srvid->s_sid);

        /* Sanity checks */
        switch (type) {
        case SERVICE_RULE_UNDEFINED:
                return -EINVAL;
        case SERVICE_RULE_FORWARD:
                if (dstlen == 0 || dst == NULL)
                        return -EINVAL;
                break;
        case SERVICE_RULE_DEMUX:
                if (dstlen > 0)
                        return -EINVAL;
                break;
        case SERVICE_RULE_DELAY:
        case SERVICE_RULE_DROP:
                break;
        }

        se = service_entry_create(tbl, alloc);

        if (!se)
                return -ENOMEM;

        write_lock_bh(&tbl->lock);

        ret = radix_tree_add(&tbl->tree, srvid->s_sid,
                             se, &n, GFP_ATOMIC);
        
        if (ret == -1) {
                /* Insertion failed, assume memory allocation error */
                service_entry_free(se);
                write_unlock_bh(&tbl->lock);
                return -ENOMEM;
        } else if (ret == 0) {
                /* struct service_iter iter;
                   struct target *target; */

                /* Found existing service entry - free the newly
                 * created entry and then check if the target exists
                 * in the found one. */
                service_entry_free(se);
                se = get_service(n);

                /* Set node to NULL to indicate that we are using an
                 * existing entry */
                n = NULL;
                                
                /* Hold entry so that we can safely unlock the
                 * table */
                service_entry_hold(se);

                /* Unlock table and lock service entry instead */
                write_unlock_bh(&tbl->lock);

                read_lock_bh(&se->lock);

                t = __service_entry_get_target(se, type, dst, dstlen,
                                               out, &set,
                                               dstlen == 0 ? 
                                               out.sk->sk_protocol :
                                               MATCH_NO_PROTOCOL);

                if (t) {
                        ret = -EEXIST;
                        /* Found existing target, we are trying to
                         * insert a duplicate. */
                        if (is_sock_target(t)) {
                                /* A socket target should return
                                 * EADDRINUSE since this is typically
                                 * a result of a bind() */
                                ret = -EADDRINUSE;
                        }
                        read_unlock_bh(&se->lock);
                        service_entry_put(se);
                        return ret;
                }
                read_unlock_bh(&se->lock);
        } else {
                /* Hold this entry since it is now in the table */
                service_entry_hold(se);
                se->node = n;
                /* We should add target to new service entry */
                write_unlock_bh(&tbl->lock);
        }

        t = target_create(type, dst, dstlen, out, weight, alloc);
        
        if (!t)
                goto error;

        write_lock_bh(&se->lock);

        set = __service_entry_get_target_set(se, priority);

        if (!set) {
                /* No existing set, we must create one */
                set = target_set_create(flags, priority, GFP_ATOMIC);
                
                if (!set) {
                        target_free(t);
                        goto error_unlock;
                }
                /* Insert the new set */
                __service_entry_insert_target_set(se, set);
        } 

        __target_set_add_target(set, t);

        se->count++;

        write_unlock_bh(&se->lock);
        
        service_entry_put(se);

        return 1;

error_unlock:
        write_unlock_bh(&se->lock);
error:
        if (n) {
                /* If n is non-NULL, we created the entry
                 * above. Since we failed to create the target,
                 * and there is no existing target, we must
                 * remove the node from the tree and free
                 * it. */
                write_lock_bh(&tbl->lock);
                radix_node_remove(n, GFP_ATOMIC);
                write_unlock_bh(&tbl->lock);
                /* Must put twice to free entry */
                service_entry_put(se);
        }

        service_entry_put(se);

        return ret;
}

void service_inc_stats(int packets, int bytes) 
{
        /*only for drops*/
        if (packets < 0) {
                atomic_add(-packets, &srvtable.packets_dropped);
                atomic_add(-bytes, &srvtable.bytes_dropped);
        }
}

int service_add(struct service_id *srvid, 
                service_rule_type_t type,
                uint16_t flags, 
                uint32_t priority,
                uint32_t weight, 
                const void *dst, 
                int dstlen, 
                const union target_out out, 
                gfp_t alloc) 
{
        struct service_id srvid_temp;
        //        struct service_id srvid1, srvid2, srvid3;
        char *p;
        int i = 0;

        //strcpy(srvid1.s_sid, "com");
        //strcpy(srvid2.s_sid, "google.com");
        //strcpy(srvid3.s_sid, "www.google.com");
       
        memset(&srvid_temp, 0, sizeof(srvid_temp));
        p = srvid->s_sid;
        
        while (i++ < strlen(srvid->s_sid)) {
                if (*p == '.') {
                         strncpy(srvid_temp.s_sid, srvid->s_sid, i - 1);
                         printf("srvid: %s\n", srvid_temp.s_sid);
                         service_table_add(&srvtable, &srvid_temp, 
                                           type, flags, priority, 
                                           weight == 0 ? 1 : weight, dst, dstlen,
                                           out, alloc);
                        }
                ++p;
        }
       
        /*
        service_table_add(&srvtable, &srvid1, 
                                 type, flags, priority, 
                                 weight == 0 ? 1 : weight, dst, dstlen,
                                 out, alloc);
        service_table_add(&srvtable, &srvid2, 
                                 type, flags, priority, 
                                 weight == 0 ? 1 : weight, dst, dstlen,
                                 out, alloc);
        service_table_add(&srvtable, &srvid3, 
                                 type, flags, priority, 
                                 weight == 0 ? 1 : weight, dst, dstlen,
                                 out, alloc);
        */

        printf("srvid: %s\n", srvid->s_sid);
        return service_table_add(&srvtable, srvid, 
                                 type, flags, priority, 
                                 weight == 0 ? 1 : weight, dst, dstlen,
                                 out, alloc);
}

static void service_table_del(struct service_table *tbl, 
                              struct service_id *srvid,
                              gfp_t alloc) 
{
        if (!srvid)
                return;

        write_lock_bh(&tbl->lock);
        radix_tree_remove(&tbl->tree, srvid->s_sid, GFP_ATOMIC);
        write_unlock_bh(&tbl->lock);
}

void service_del(struct service_id *srvid, gfp_t alloc) 
{
        return service_table_del(&srvtable, srvid, alloc);
}

static void service_table_del_target(struct service_table *tbl, 
                                     struct service_id *srvid,
                                     service_rule_type_t type,
                                     const void *dst, 
                                     int dstlen, 
                                     struct target_stats* stats,
                                     gfp_t alloc) 
{
        struct radix_node *n;
        struct service_entry *se;

        read_lock_bh(&tbl->lock);

        n = radix_tree_find(&tbl->tree, srvid->s_sid, NULL);

        if (!n) {
                read_unlock_bh(&tbl->lock);
                return;
        }

        se = get_service(n);
        service_entry_hold(se);
        read_unlock_bh(&tbl->lock);
        
        write_lock_bh(&se->lock);
        
        __service_entry_remove_target(se, type,
                                      dst, dstlen, stats);
        
        if (list_empty(&se->target_set)) {
                write_lock_bh(&tbl->lock);
                radix_node_remove(n, GFP_ATOMIC);
                write_unlock_bh(&tbl->lock);
                write_unlock_bh(&se->lock);
                service_entry_put(se);
        } else {
                write_unlock_bh(&se->lock);
        }
        
        service_entry_put(se);
}

void service_del_target(struct service_id *srvid, 
                        service_rule_type_t type,
                        const void *dst, int dstlen,
                        struct target_stats* stats,
                        gfp_t alloc) 
{
        return service_table_del_target(&srvtable, srvid, type,
                                        dst, dstlen, stats, alloc);
}

/*
  This function can only with service table lock held.
 */
static int del_dev_func(struct radix_node *n, void *arg) 
{
        struct service_entry *se = get_service(n);
        unsigned int ifindex = *(unsigned int *)arg;
        int ret = 0;
        
        if (!radix_node_is_active(n))
                return 0;

        write_lock_bh(&se->lock);
        
        ret = __service_entry_remove_target_by_dev(se, ifindex);
        
        if (ret > 0 && list_empty(&se->target_set)) {
                radix_node_remove(n, GFP_ATOMIC);
                write_unlock_bh(&se->lock);
                service_entry_put(se);
        } else {
                write_unlock_bh(&se->lock);
        }

        return ret;
}

static int service_table_del_dev_all(struct service_table *tbl, 
                                     unsigned int ifindex) 
{
        int ret = 0;

        write_lock_bh(&tbl->lock);
        
        ret = radix_tree_foreach(&tbl->tree, del_dev_func, 
                                 (void *)&ifindex);

        write_unlock_bh(&tbl->lock);

        return ret;
}

int service_del_dev_all(unsigned int ifindex) 
{
        return service_table_del_dev_all(&srvtable, ifindex);
}

/*
 * This function can only be called with service table lock held.  
 */
static int del_target_func(struct radix_node *n, void *arg) 
{
        struct service_entry *se = get_service(n);
        struct _d {
                service_rule_type_t type;
                const void *d_dst;
                int d_len;
        } *d = (struct _d *)arg;
        int ret = 0;

        if (!radix_node_is_active(n))
                return 0;

        write_lock_bh(&se->lock);

        ret = __service_entry_remove_target(se, d->type, 
                                            d->d_dst, d->d_len, NULL);

        if (ret > 0 && list_empty(&se->target_set)) {
                radix_node_remove(n, GFP_ATOMIC);
                write_unlock_bh(&se->lock);
                service_entry_put(se);
        } else {
                write_unlock_bh(&se->lock);
        }

        return ret;
}

static int service_table_del_target_all(struct service_table *tbl,
                                        service_rule_type_t type,
                                        const void *dst, int dstlen) 
{
        int ret = 0;
        struct {
                service_rule_type_t type;
                const void *d_dst;
                int d_len;
        } d = { type, dst, dstlen };
        
        write_lock_bh(&tbl->lock);

        ret = radix_tree_foreach(&tbl->tree, del_target_func, &d);
                
        write_unlock_bh(&tbl->lock);

        return ret;
}

int service_del_target_all(service_rule_type_t type, 
                           const void *dst, int dstlen) 
{
        return service_table_del_target_all(&srvtable, type, dst, dstlen);
}

static void service_entry_destroy(struct radix_node *n)
{
        struct service_entry *se = get_service(n);
        LOG_DBG("freeing service entry\n");
        service_entry_put(se);
}

void __service_table_destroy(struct service_table *tbl) 
{
        radix_tree_destroy(&tbl->tree, service_entry_destroy);
}

void service_table_destroy(struct service_table *tbl) 
{
        write_lock_bh(&tbl->lock);
        __service_table_destroy(tbl);
        write_unlock_bh(&tbl->lock);
}

void service_table_init(struct service_table *tbl) 
{
        radix_tree_initialize(&tbl->tree);
        atomic_set(&tbl->packets_resolved, 0);
        atomic_set(&tbl->bytes_resolved, 0);
        atomic_set(&tbl->packets_dropped, 0);
        atomic_set(&tbl->bytes_dropped, 0);
        rwlock_init(&tbl->lock);
}

int service_init(void) 
{
        service_table_init(&srvtable);

        return 0;
}

void service_fini(void) 
{
        service_table_destroy(&srvtable);
}
