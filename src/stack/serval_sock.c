/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
#include <serval/platform.h>
#include <serval/skbuff.h>
#include <serval/list.h>
#include <serval/debug.h>
#include <serval/timer.h>
#include <serval/netdevice.h>
#include <netinet/serval.h>
#include <serval_sock.h>
#include <serval_srv.h>
#if defined(OS_LINUX_KERNEL)
#include <linux/ip.h>
#else
#include <netinet/ip.h>
#endif

atomic_t serval_nr_socks = ATOMIC_INIT(0);
static atomic_t serval_sock_id = ATOMIC_INIT(1);
static struct serval_table established_table;
static struct serval_table listen_table;

static const char *sock_state_str[] = {
        "UNDEFINED",
        "CLOSED",
        "REQUEST",
        "RESPOND",
        "CONNECTED",
        "CLOSING",
        "TIMEWAIT",
        "MIGRATE",
        "RECONNECT",
        "RRESPOND",
        "LISTEN",
        "CLOSEWAIT",
        /* TCP only */
        "FINWAIT1",
        "FINWAIT2",
        "LASTACK",
        "SIMCLOSE"  
};

int __init serval_table_init(struct serval_table *table, const char *name)
{
	unsigned int i;

	table->hash = MALLOC(SERVAL_HTABLE_SIZE_MIN *
			      2 * sizeof(struct serval_hslot), GFP_KERNEL);
	if (!table->hash) {
		/* panic(name); */
		return -1;
	}

	table->mask = SERVAL_HTABLE_SIZE_MIN - 1;

	for (i = 0; i <= table->mask; i++) {
		INIT_HLIST_HEAD(&table->hash[i].head);
		table->hash[i].count = 0;
		spin_lock_init(&table->hash[i].lock);
	}

	return 0;
}

void __exit serval_table_fini(struct serval_table *table)
{
        unsigned int i;

        for (i = 0; i <= table->mask; i++) {
                spin_lock_bh(&table->hash[i].lock);
                        
                while (!hlist_empty(&table->hash[i].head)) {
                        struct sock *sk;

                        sk = hlist_entry(table->hash[i].head.first, 
                                          struct sock, sk_node);
                        
                        hlist_del(&sk->sk_node);
                        table->hash[i].count--;
                        sock_put(sk);
                }
                spin_unlock_bh(&table->hash[i].lock);           
	}

        FREE(table->hash);
}

static struct sock *serval_sock_lookup(struct serval_table *table,
                                         struct net *net, void *key, 
                                         size_t keylen)
{
        struct serval_hslot *slot;
        struct hlist_node *walk;
        struct sock *sk = NULL;

        if (!key)
                return NULL;

        slot = serval_hashslot(table, net, key, keylen);

        if (!slot)
                return NULL;

        spin_lock_bh(&slot->lock);
        
        hlist_for_each_entry(sk, walk, &slot->head, sk_node) {
                struct serval_sock *ssk = serval_sk(sk);
                if (memcmp(key, ssk->hash_key, keylen) == 0) {
                        sock_hold(sk);
                        goto out;
                }
        }
        sk = NULL;
 out:
        spin_unlock_bh(&slot->lock);
        
        return sk;
}

struct sock *serval_sock_lookup_sockid(struct sock_id *sockid)
{
        return serval_sock_lookup(&established_table, &init_net, 
                                    sockid, sizeof(*sockid));
}

struct sock *serval_sock_lookup_serviceid(struct service_id *srvid)
{
        return serval_sock_lookup(&listen_table, &init_net, 
                                    srvid, sizeof(*srvid));
}

static inline unsigned int serval_ehash(struct sock *sk)
{
        return serval_hashfn(sock_net(sk), 
                               &serval_sk(sk)->local_sockid,
                               sizeof(struct sock_id),
                               established_table.mask);
}

static inline unsigned int serval_lhash(struct sock *sk)
{
        return serval_hashfn(sock_net(sk), 
                               &serval_sk(sk)->local_srvid, 
                               sizeof(struct service_id),
                               listen_table.mask);
}

static void __serval_table_hash(struct serval_table *table, struct sock *sk)
{
        struct serval_sock *ssk = serval_sk(sk);
        struct serval_hslot *slot;

        sk->sk_hash = serval_hashfn(sock_net(sk), 
                                      ssk->hash_key,
                                      ssk->hash_key_len,
                                      table->mask);

        slot = &table->hash[sk->sk_hash];

        spin_lock(&slot->lock);
        slot->count++;
        hlist_add_head(&sk->sk_node, &slot->head);
#if defined(OS_LINUX_KERNEL)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,25)
	sock_prot_inuse_add(sock_net(sk), sk->sk_prot, 1);
#else
        sock_prot_inc_use(sk->sk_prot);
#endif
#endif
        spin_unlock(&slot->lock);     
}

static void __serval_sock_hash(struct sock *sk)
{
        if (!hlist_unhashed(&sk->sk_node)) {
                LOG_ERR("socket %p already hashed\n", sk);
        }
        
        if (sk->sk_state == SERVAL_LISTEN) {
                LOG_DBG("hashing socket %p based on service id %s\n",
                        sk, service_id_to_str(&serval_sk(sk)->local_srvid));
                serval_sk(sk)->hash_key = &serval_sk(sk)->local_srvid;
                serval_sk(sk)->hash_key_len = sizeof(serval_sk(sk)->local_srvid);
                __serval_table_hash(&listen_table, sk);

        } else { 
                LOG_DBG("hashing socket %p based on socket id %s\n",
                        sk, socket_id_to_str(&serval_sk(sk)->local_sockid));
                serval_sk(sk)->hash_key = &serval_sk(sk)->local_sockid;
                serval_sk(sk)->hash_key_len = sizeof(serval_sk(sk)->local_sockid);
                __serval_table_hash(&established_table, sk);
        }
}

void serval_sock_hash(struct sock *sk)
{
        if (sk->sk_state != SERVAL_CLOSED) {
		local_bh_disable();
		__serval_sock_hash(sk);
		local_bh_enable();
	}
}

void serval_sock_unhash(struct sock *sk)
{
        struct net *net = sock_net(sk);
        spinlock_t *lock;

        LOG_DBG("unhashing socket %p\n", sk);

        /* grab correct lock */
        if (sk->sk_state == SERVAL_LISTEN) {
                lock = &serval_hashslot(&listen_table, net, 
                                          &serval_sk(sk)->local_srvid, 
                                          sizeof(struct service_id))->lock;
        } else {
                lock = &serval_hashslot(&established_table,
                                          net, &serval_sk(sk)->local_sockid, 
                                          sizeof(struct sock_id))->lock;
        }

	spin_lock_bh(lock);

        if (!hlist_unhashed(&sk->sk_node)) {
                hlist_del_init(&sk->sk_node);
#if defined(OS_LINUX_KERNEL)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,25)
                sock_prot_inuse_add(sock_net(sk), sk->sk_prot, -1);
#else
                sock_prot_dec_use(sk->sk_prot);
#endif
#endif
        }
	spin_unlock_bh(lock);
}

int __init serval_sock_tables_init(void)
{
        int ret;

        ret = serval_table_init(&listen_table, "LISTEN");

        if (ret < 0)
                goto fail_table;
        
        ret = serval_table_init(&established_table, "ESTABLISHED");

fail_table:
        return ret;
}

void __exit serval_sock_tables_fini(void)
{
        serval_table_fini(&listen_table);
        serval_table_fini(&established_table);
        if (sock_state_str[0]) {} /* Avoid compiler warning when
                                   * compiling with debug off */
}

int __serval_assign_sockid(struct sock *sk)
{
        struct serval_sock *ssk = serval_sk(sk);
       
        /* 
           TODO: 
           - Check for ID wraparound and conflicts 
           - Make sure code does not assume sockid is a short
        */
        return serval_sock_get_sockid(&ssk->local_sockid);
}

int serval_sock_get_sockid(struct sock_id *sid)
{
        sid->s_id = htons(atomic_inc_return(&serval_sock_id));

        return 0;
}

struct sock *serval_sk_alloc(struct net *net, struct socket *sock, 
                               gfp_t priority, int protocol, 
                               struct proto *prot)
{
        struct sock *sk;

        sk = sk_alloc(net, PF_SERVAL, priority, prot);

	if (!sk)
		return NULL;

	sock_init_data(sock, sk);
        sk->sk_family = PF_SERVAL;
	sk->sk_protocol	= protocol;
	sk->sk_destruct	= serval_sock_destruct;
        sk->sk_backlog_rcv = sk->sk_prot->backlog_rcv;
        
        /* Only assign socket id here in case we have a user
         * socket. If socket is NULL, then it means this socket is a
         * child socket from a LISTENing socket, and it will be
         * assigned the socket id from the request sock */
        if (sock && __serval_assign_sockid(sk) < 0) {
                LOG_DBG("could not assign sock id\n");
                sock_put(sk);
                return NULL;
        }

        atomic_inc(&serval_nr_socks);
                
        LOG_DBG("SERVAL socket %p created, %d are alive.\n", 
               sk, atomic_read(&serval_nr_socks));

        return sk;
}


void serval_sock_init(struct sock *sk)
{
        struct serval_sock *ssk = serval_sk(sk);
        sk->sk_state = 0;
        INIT_LIST_HEAD(&ssk->accept_queue);
        INIT_LIST_HEAD(&ssk->syn_queue);
        setup_timer(&ssk->retransmit_timer, 
                    serval_srv_rexmit_timeout,
                    (unsigned long)sk);
}

void serval_sock_destruct(struct sock *sk)
{
        struct serval_sock *ssk = serval_sk(sk);

        __skb_queue_purge(&sk->sk_receive_queue);
        __skb_queue_purge(&sk->sk_error_queue);

        if (ssk->dev) {
                dev_put(ssk->dev);
        }

	if (sk->sk_type == SOCK_STREAM && 
            sk->sk_state != SERVAL_CLOSED) {
		LOG_ERR("Bad state %d %p\n",
                        sk->sk_state, sk);
		return;
	}

	if (!sock_flag(sk, SOCK_DEAD)) {
		LOG_DBG("Attempt to release alive serval socket: %p\n", sk);
		return;
	}

	if (atomic_read(&sk->sk_rmem_alloc)) {
                LOG_WARN("sk_rmem_alloc is not zero\n");
        }

	if (atomic_read(&sk->sk_wmem_alloc)) {
                LOG_WARN("sk_wmem_alloc is not zero\n");
        }

	atomic_dec(&serval_nr_socks);

	LOG_DBG("SERVAL socket %p destroyed, %d are still alive.\n", 
               sk, atomic_read(&serval_nr_socks));
}

int serval_sock_set_state(struct sock *sk, int new_state)
{
        /* TODO: state transition checks */
        
        if (new_state < SERVAL_SOCK_STATE_MIN ||
            new_state > SERVAL_SOCK_STATE_MAX) {
                LOG_ERR("invalid state\n");
                return -1;
        }

        LOG_DBG("%s -> %s\n",
                sock_state_str[sk->sk_state],
                sock_state_str[new_state]);

        sk->sk_state = new_state;

        return new_state;
}