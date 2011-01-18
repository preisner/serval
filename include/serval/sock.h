/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
#ifndef _SOCK_H_
#define _SOCK_H_

#include <serval/platform.h>

#if defined(OS_LINUX_KERNEL)
#include <net/sock.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
static inline wait_queue_head_t *sk_sleep(struct sock *sk)
{
        return sk->sk_sleep;
}
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,32)
static inline struct net *sock_net(struct sock *sk)
{
        return sk->sk_net;
}
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,34)
#define __sk_add_backlog sk_add_backlog
#endif

#endif
#if defined(OS_USER)
#include <serval/platform.h>
#include <serval/atomic.h>
#include <serval/lock.h>
#include <serval/list.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "skbuff.h"
#include "net.h"
#include "wait.h"
#include "timer.h"

struct sk_buff;
struct proto;

#define SHUTDOWN_MASK	3
#define RCV_SHUTDOWN	1
#define SEND_SHUTDOWN	2

typedef struct {
        pthread_mutex_t slock;
        int owned;
} socket_lock_t;

struct sock_common {
        struct hlist_node	skc_node;
	atomic_t		skc_refcnt;
        int     	        skc_tx_queue_mapping;
        union  {
                unsigned int skc_hash;
                uint16_t skc_u16hashes[2];
        };
        unsigned short          skc_family;
        unsigned char	        skc_state;
        unsigned char	        skc_reuse;
        int     	        skc_bound_dev_if;
        struct proto            *skc_prot;
        struct net              *skc_net;
};

struct sock {
        struct sock_common      __sk_common;
#define sk_node __sk_common.skc_node
#define sk_refcnt __sk_common.skc_refcnt
#define sk_tx_queue_mapping __sk_common.skc_tx_queue_mapping
#define sk_copy_start __sk_common.skc_hash
#define sk_hash __sk_common.skc_hash
#define sk_family __sk_common.skc_family
#define sk_state __sk_common.skc_state
#define sk_reuse __sk_common.skc_reuse
#define sk_bound_dev_if __sk_common.skc_bound_dev_if
#define sk_prot __sk_common.skc_prot
#define sk_net __sk_common.skc_net
        gfp_t                   sk_allocation;
        unsigned int		sk_shutdown  : 2,
				sk_no_check  : 2,
				sk_userlocks : 4,
				sk_protocol  : 8,
				sk_type      : 16;
        struct socket_wq  	*sk_wq;
	int			sk_rcvbuf;
        socket_lock_t           sk_lock;
        struct {
                struct sk_buff *head;
                struct sk_buff *tail;
                int len;
        } sk_backlog;
	atomic_t		sk_rmem_alloc;
	atomic_t		sk_wmem_alloc;
	atomic_t		sk_omem_alloc;
	atomic_t		sk_drops;
	unsigned short		sk_ack_backlog;
	unsigned short		sk_max_ack_backlog;
	int			sk_sndbuf;
	struct sk_buff_head	sk_receive_queue;
	struct sk_buff_head	sk_write_queue;
        int			sk_wmem_queued;
	int			sk_write_pending;
	unsigned long 		sk_flags;
	unsigned long	        sk_lingertime;
        struct sk_buff_head	sk_error_queue;
	rwlock_t		sk_callback_lock;
        int                     sk_err,
                                sk_err_soft;
        uint32_t                sk_priority;
	long			sk_rcvtimeo;
	long			sk_sndtimeo;
	struct timer_list	sk_timer;
	struct socket		*sk_socket;
	struct sk_buff		*sk_send_head;

        void (*sk_destruct)(struct sock *sk);
	void (*sk_state_change)(struct sock *sk);
	void (*sk_data_ready)(struct sock *sk, int bytes);
	void (*sk_write_space)(struct sock *sk);
	void (*sk_error_report)(struct sock *sk);
  	int (*sk_backlog_rcv)(struct sock *sk,
                              struct sk_buff *skb);  
};

struct kiocb;
#define __user 

struct proto {
        struct module           *owner;
	void			(*close)(struct sock *sk, 
					long timeout);
	int			(*connect)(struct sock *sk,
				        struct sockaddr *uaddr, 
					int addr_len);
	int			(*disconnect)(struct sock *sk, int flags);

	struct sock *		(*accept) (struct sock *sk, int flags, int *err);

	int			(*ioctl)(struct sock *sk, int cmd,
					 unsigned long arg);
	int			(*init)(struct sock *sk);
	void			(*destroy)(struct sock *sk);
	void			(*shutdown)(struct sock *sk, int how);
	int			(*setsockopt)(struct sock *sk, int level, 
					int optname, char __user *optval,
					unsigned int optlen);
	int			(*getsockopt)(struct sock *sk, int level, 
					int optname, char __user *optval, 
					int __user *option);  	 
	int			(*sendmsg)(struct kiocb *iocb, struct sock *sk,
					   struct msghdr *msg, size_t len);
	int			(*recvmsg)(struct kiocb *iocb, struct sock *sk,
					   struct msghdr *msg,
					size_t len, int noblock, int flags, 
					int *addr_len);
	int			(*bind)(struct sock *sk, 
					struct sockaddr *uaddr, int addr_len);

	int			(*backlog_rcv) (struct sock *sk, 
						struct sk_buff *skb);

	/* Keeping track of sk's, looking them up, and port selection methods. */
	void			(*hash)(struct sock *sk);
	void			(*unhash)(struct sock *sk);
	int			(*get_port)(struct sock *sk, unsigned short snum);

	unsigned int		obj_size;
	char			name[32];

	struct list_head	node;
};

static inline int wq_has_sleeper(struct socket_wq *wq)
{
        return wq && waitqueue_active(&wq->wait);
}

static inline int sock_no_getsockopt(struct socket *s, int a, 
                                     int b, char __user *c, int __user *d)
{
        return -1;
}

static inline int sock_no_setsockopt(struct socket *s, int a, int b, 
                                     char __user *c, unsigned int d)
{
        return -1;
}

extern int proto_register(struct proto *prot, int);
extern void proto_unregister(struct proto *prot);

enum sock_flags {
	SOCK_DEAD,
	SOCK_DONE,
	SOCK_URGINLINE,
	SOCK_KEEPOPEN,
	SOCK_LINGER,
	SOCK_DESTROY,
	SOCK_BROADCAST,
	SOCK_TIMESTAMP,
	SOCK_ZAPPED,
	SOCK_USE_WRITE_QUEUE, /* whether to call sk->sk_write_space in sock_wfree */
        SOCK_FASYNC,
};

#define sock_net(s) ((s)->sk_net)


static inline void sk_node_init(struct hlist_node *node)
{
        node->pprev = NULL;
}

static inline void sk_tx_queue_set(struct sock *sk, int tx_queue)
{
	sk->sk_tx_queue_mapping = tx_queue;
}

static inline void sk_tx_queue_clear(struct sock *sk)
{
	sk->sk_tx_queue_mapping = -1;
}

static inline int sk_tx_queue_get(const struct sock *sk)
{
	return sk ? sk->sk_tx_queue_mapping : -1;
}

static inline void sk_set_socket(struct sock *sk, struct socket *sock)
{
	sk_tx_queue_clear(sk);
	sk->sk_socket = sock;
}

#define sk_wait_event(__sk, __timeo, __condition)                       \
        ({ int __rc;                                                    \
                release_sock(__sk);                                     \
                __rc = __condition;                                     \
                if (!__rc) {                                            \
                        *(__timeo) = schedule_timeout(*(__timeo));      \
                }                                                       \
                lock_sock(__sk);                                        \
                __rc = __condition;                                     \
                __rc;                                                   \
        })

int sk_wait_data(struct sock *sk, long *timeo);

static inline int sock_writeable(const struct sock *sk) 
{
	return atomic_read(&sk->sk_wmem_alloc) < (sk->sk_sndbuf >> 1);
}

static inline long sock_rcvtimeo(const struct sock *sk, int noblock)
{
	return noblock ? 0 : sk->sk_rcvtimeo;
}

static inline long sock_sndtimeo(const struct sock *sk, int noblock)
{
	return noblock ? 0 : sk->sk_sndtimeo;
}


void sk_reset_timer(struct sock *sk, struct timer_list* timer,
                    unsigned long expires);
void sk_stop_timer(struct sock *sk, struct timer_list* timer);
int sock_queue_rcv_skb(struct sock *sk, struct sk_buff *skb);
int sock_queue_err_skb(struct sock *sk, struct sk_buff *skb);

static inline void sk_eat_skb(struct sock *sk, struct sk_buff *skb, int copied_early)
{
	__skb_unlink(skb, &sk->sk_receive_queue);
	__free_skb(skb);
}

static inline void sock_set_flag(struct sock *sk, enum sock_flags flag)
{
        sk->sk_flags |= (0x1 << flag);
}

static inline void sock_reset_flag(struct sock *sk, enum sock_flags flag)
{
        sk->sk_flags &= ((0x1 << flag) ^ -1UL);
}

static inline int sock_flag(struct sock *sk, enum sock_flags flag)
{
	return sk->sk_flags & (0x1 << flag);
}

static inline int sock_error(struct sock *sk)
{
        return 0;
}

static inline int sock_intr_errno(long timeo)
{
	return timeo == MAX_SCHEDULE_TIMEOUT ? -ERESTARTSYS : -EINTR;
}

void sock_wfree(struct sk_buff *skb);
void sock_rfree(struct sk_buff *skb);

static inline void skb_set_owner_w(struct sk_buff *skb, struct sock *sk)
{
	skb_orphan(skb);
	skb->sk = sk;
	skb->destructor = sock_wfree;
	/*
	 * We used to take a refcount on sk, but following operation
	 * is enough to guarantee sk_free() wont free this sock until
	 * all in-flight packets are completed
	 */
	atomic_add(skb->truesize, &sk->sk_wmem_alloc);
}

static inline void skb_set_owner_r(struct sk_buff *skb, struct sock *sk)
{
	skb_orphan(skb);
	skb->sk = sk;
	skb->destructor = sock_rfree;
	atomic_add(skb->truesize, &sk->sk_rmem_alloc);
	/* sk_mem_charge(sk, skb->truesize); */
}

static inline unsigned long sock_wspace(struct sock *sk)
{
        int amt = 0;

        if (!(sk->sk_shutdown & SEND_SHUTDOWN)) {
                amt = sk->sk_sndbuf - atomic_read(&sk->sk_wmem_alloc);
                if (amt < 0) 
                        amt = 0;
        }
        return amt;
}

static inline void sk_wake_async(struct sock *sk, int how, int band)
{
        /* Check if async notification is required on this socket. */
        if (sock_flag(sk, SOCK_FASYNC))
                sock_wake_async(sk->sk_socket, how, band);
}

#define SOCK_MIN_SNDBUF 2048
#define SOCK_MIN_RCVBUF 256

void sock_init_data(struct socket *sock, struct sock *sk);
struct sock *sk_clone(const struct sock *sk, const gfp_t priority);
struct sock *sk_alloc(struct net *net, int family, gfp_t priority,
		      struct proto *prot);
void sk_free(struct sock *sk);

static inline void sock_hold(struct sock *sk)
{
        atomic_inc(&sk->sk_refcnt);
}

/* 
   Ungrab socket in the context, which assumes that socket refcnt
   cannot hit zero, f.e. it is true in context of any socketcall.
 */
static inline void __sock_put(struct sock *sk)
{
	atomic_dec(&sk->sk_refcnt);
}

static inline void sock_put(struct sock *sk)
{
        if (atomic_dec_and_test(&sk->sk_refcnt))
                sk_free(sk);
}

void lock_sock(struct sock *sk);

/* BH context may only use the following locking interface. */
#define bh_lock_sock(__sk) spin_lock(&((__sk)->sk_lock.slock))
/* #define bh_lock_sock_nested(__sk)                \
        spin_lock_nested(&((__sk)->sk_lock.slock), \
        SINGLE_DEPTH_NESTING) */
#define bh_lock_sock_nested(__sk) bh_lock_sock(__sk)

#define bh_unlock_sock(__sk) spin_unlock(&((__sk)->sk_lock.slock))

void release_sock(struct sock *sk);

static inline wait_queue_head_t *sk_sleep(struct sock *sk)
{
        return sk->sk_wq ? &sk->sk_wq->wait : NULL;
}

static inline void sock_orphan(struct sock *sk)
{
	write_lock(&sk->sk_callback_lock);
	sock_set_flag(sk, SOCK_DEAD);
	sk_set_socket(sk, NULL);
	sk->sk_wq  = NULL;
	write_unlock(&sk->sk_callback_lock);
}

static inline void sock_graft(struct sock *sk, struct socket *parent)
{
	write_lock(&sk->sk_callback_lock);
        sk->sk_wq = parent->wq;
	parent->sk = sk;
	sk_set_socket(sk, parent);
	write_unlock(&sk->sk_callback_lock);
}

void sk_common_release(struct sock *sk);

/* Used by processes to "lock" a socket state, so that
 * interrupts and bottom half handlers won't change it
 * from under us. It essentially blocks any incoming
 * packets, so that we won't get any new data or any
 * packets that change the state of the socket.
 *
 * While locked, BH processing will add new packets to
 * the backlog queue.  This queue is processed by the
 * owner of the socket lock right before it is released.
 *
 * Since ~2.3.5 it is also exclusive sleep lock serializing
 * accesses from user process context.
 */
#define sock_owned_by_user(sk)((sk)->sk_lock.owned)

static inline void __sk_add_backlog(struct sock *sk, struct sk_buff *skb)
{
        /* dont let skb dst not refcounted, we are going to leave rcu lock */
        //skb_dst_force(skb);

        if (!sk->sk_backlog.tail)
                sk->sk_backlog.head = skb;
        else
                sk->sk_backlog.tail->next = skb;

        sk->sk_backlog.tail = skb;
        skb->next = NULL;
}

static inline int sk_rcvqueues_full(const struct sock *sk, 
                                    const struct sk_buff *skb)
{
        unsigned int qsize = sk->sk_backlog.len + atomic_read(&sk->sk_rmem_alloc);

        return qsize + skb->truesize > (unsigned int)sk->sk_rcvbuf;
}

/* The per-socket spinlock must be held here. */
static inline int sk_add_backlog(struct sock *sk, struct sk_buff *skb)
{
        if (sk_rcvqueues_full(sk, skb))
                return -ENOBUFS;

        __sk_add_backlog(sk, skb);
        sk->sk_backlog.len += skb->truesize;
        return 0;
}

static inline int sk_backlog_rcv(struct sock *sk, struct sk_buff *skb)
{
        return sk->sk_backlog_rcv(sk, skb);
}

#endif /* OS_USER */

#endif /* _SOCK_H_ */