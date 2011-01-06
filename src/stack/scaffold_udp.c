/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
#include <scaffold/platform.h>
#include <scaffold/debug.h>
#include <scaffold/netdevice.h>
#include <scaffold/skbuff.h>
#include <netinet/scaffold.h>
#include <scaffold_udp_sock.h>
#include <scaffold_sock.h>
#include <scaffold_ipv4.h>
#include <scaffold_srv.h>
#include <input.h>

#if defined(OS_LINUX_KERNEL)
#include <linux/ip.h>
#include <net/udp.h>
#endif

#if defined(OS_USER)
#include <netinet/ip.h>
#if defined(OS_BSD)
#include <scaffold/platform_tcpip.h>
#else
#include <netinet/udp.h>
#endif
#endif /* OS_USER */

#define EXTRA_HDR (20)
/* payload + LL + IP + extra */
#define UDP_MAX_HDR (MAX_HEADER + 20 + EXTRA_HDR + \
                     sizeof(struct scaffold_hdr) + \
                     sizeof(struct scaffold_service_ext)) 

static int scaffold_udp_connection_request(struct sock *sk, 
                                           struct sk_buff *skb);

struct sock *scaffold_udp_connection_response(struct sock *sk, struct sk_buff *skb,
                                              struct scaffold_request_sock *req,
                                              struct dst_entry *dst);

static struct scaffold_sock_af_ops scaffold_udp_af_ops = {
        .conn_request = scaffold_udp_connection_request,
        .conn_response = scaffold_udp_connection_response,
};

/* from fastudpsrc */
static void udp_checksum(uint16_t total_len,
                         struct udphdr *uh, void *data) 
{
        uint32_t src = *(uint32_t *)data;
        unsigned short len = total_len - 14 - sizeof(struct iphdr);
        unsigned csum = 0; 
        uh->check = 0;
        /* FIXME: Do not assume IP header lacks options */
        csum = ~in_cksum((unsigned char *)uh, len) & 0xFFFF;
        csum += src & 0xFFFF;
        csum += (src >> 16) & 0xFFFF;
        csum += htons(SCAFFOLD_PROTO_UDP) + htons(len);
        csum = (csum & 0xFFFF) + (csum >> 16);
        uh->check = ~csum & 0xFFFF;
}

static int scaffold_udp_transmit_skb(struct sock *sk, 
                                     struct sk_buff *skb,
                                     enum scaffold_packet_type type)
{
        int err;
        unsigned short tot_len;
        struct udphdr *uh;

        /* Push back to make space for transport header */
        uh = (struct udphdr *)skb_push(skb, sizeof(struct udphdr));
	skb_reset_transport_header(skb);
	skb_set_owner_w(skb, sk);
        SCAFFOLD_SKB_CB(skb)->pkttype = type;
        
        tot_len = skb->len + 20 + 14;
        
        /* Build UDP header */
        uh->source = scaffold_sk(sk)->local_srvid.s_sid16;
        memcpy(&uh->dest, &SCAFFOLD_SKB_CB(skb)->srvid, sizeof(uh->dest));
        uh->len = htons(skb->len);
        udp_checksum(tot_len, uh, &scaffold_sk(sk)->src_flowid);
        
        skb->protocol = IPPROTO_UDP;

        LOG_DBG("udp pkt [s=%u d=%u len=%u]\n",
                ntohs(uh->source),
                ntohs(uh->dest),
                ntohs(uh->len));

        err = scaffold_srv_xmit_skb(sk, skb);
        
        if (err < 0) {
                LOG_ERR("xmit failed\n");
        }

        return err;
}

static int scaffold_udp_init_sock(struct sock *sk)
{
        struct scaffold_sock *ssk = scaffold_sk(sk);

        ssk->af_ops = &scaffold_udp_af_ops;

        LOG_DBG("\n");
        
        return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 25)
static int scaffold_udp_destroy_sock(struct sock *sk)
#else
static void scaffold_udp_destroy_sock(struct sock *sk)
#endif
{
        //struct scaffold_udp_sock *usk = scaffold_udp_sk(sk);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 25)
        return 0;
#endif
}

static void scaffold_udp_close(struct sock *sk, long timeout)
{
        //struct scaffold_udp_sock *usk = scaffold_udp_sk(sk);
        LOG_DBG("\n");
}

static int scaffold_udp_connect(struct sock *sk, struct sockaddr *uaddr, 
                                int addr_len)
{
        struct sk_buff *skb;
        struct service_id *srvid = &((struct sockaddr_sf *)uaddr)->sf_srvid;
        int err;

        LOG_DBG("addr_len=%d\n", addr_len);

	if (addr_len < sizeof(struct sockaddr_sf))
		return -EINVAL;
        
        skb = ALLOC_SKB(UDP_MAX_HDR, GFP_KERNEL);

        if (!skb)
                return -ENOMEM;
        
        skb_reserve(skb, UDP_MAX_HDR);
        
        memcpy(&SCAFFOLD_SKB_CB(skb)->srvid, srvid, sizeof(*srvid));
         
        err = scaffold_udp_transmit_skb(sk, skb, SCAFFOLD_PKT_SYN);
        
        if (err < 0) {
                LOG_ERR("udp xmit failed\n");
                FREE_SKB(skb);
        }

        return err;
}

static int scaffold_udp_disconnect(struct sock *sk, int flags)
{
        LOG_DBG("\n");
        return 0;
}

static void scaffold_udp_shutdown(struct sock *sk, int how)
{
        LOG_DBG("\n");
}

int scaffold_udp_connection_request(struct sock *sk, struct sk_buff *skb)
{
        //struct scaffold_sock *ssk = scaffold_sk(sk);
        /* struct iphdr *iph = ip_hdr(skb);
           struct udphdr *udph = udp_hdr(skb); */

        int err = 0;

        LOG_DBG("SYN received\n");

        return err;
}

struct sock *scaffold_udp_connection_response(struct sock *sk, struct sk_buff *skb,
                                              struct scaffold_request_sock *req,
                                              struct dst_entry *dst)
{
        return NULL;
}

static int __scaffold_udp_queue_rcv_skb(struct sock *sk, struct sk_buff *skb)
{
        FREE_SKB(skb);

        return 0;
}

/* 
   Receive from network
*/
int scaffold_udp_rcv(struct sk_buff *skb)
{
        struct udphdr *udph = udp_hdr(skb);
        /*
	struct sock *sk;
        struct iphdr *iph = ip_hdr(skb);
        struct service_id *srvid = (struct service_id *)&udph->dest;
        */
        int err = 0;
        
        LOG_DBG("udp packet len=%u\n", ntohs(udph->len));
        
        /*
        sk = scaffold_sock_lookup_sockid(sockid);

        if (!sk) {
                LOG_ERR("No matching scaffold sock\n");
                FREE_SKB(skb);
        } else {
                FREE_SKB(skb);
        }
        */

        FREE_SKB(skb);

        return err;
}

static int scaffold_udp_sendmsg(struct kiocb *iocb, struct sock *sk, 
                                struct msghdr *msg, size_t len)
{
        int err;
        struct sk_buff *skb;
        int ulen = len;
        struct service_id *srvid = NULL;
        //struct scaffold_sock *ssk = scaffold_sk(sk);

	if (len > 0xFFFF)
		return -EMSGSIZE;

	if (msg->msg_flags & MSG_OOB) 
		return -EOPNOTSUPP;

        LOG_DBG("sending message\n");

	if (msg->msg_name && scaffold_sock_flag(scaffold_sk(sk), SSK_FLAG_BOUND)) {
		struct sockaddr_sf *addr = 
                        (struct sockaddr_sf *)msg->msg_name;

		if ((unsigned)msg->msg_namelen < sizeof(*addr))
			return -EINVAL;
		if (addr->sf_family != AF_SCAFFOLD) {
			if (addr->sf_family != AF_UNSPEC)
				return -EAFNOSUPPORT;
		}
                
                srvid = &addr->sf_srvid;
        } else if (sk->sk_state != SCAFFOLD_CONNECTED) {
                return -EDESTADDRREQ;
        }

        ulen += sizeof(struct udphdr);

        skb = ALLOC_SKB(UDP_MAX_HDR + ulen, GFP_KERNEL);

        if (!skb)
                return -ENOMEM;
        
        skb_reserve(skb, UDP_MAX_HDR);
        
        if (srvid) {
                memcpy(&SCAFFOLD_SKB_CB(skb)->srvid, srvid, sizeof(*srvid));
        }
        /* 
           TODO: 
           
           This is an extra copy operation for the user space version
           that we could try to get rid of, i.e., reading the data
           from the file descriptor directly into the socket buffer
        */
        
        err = memcpy_fromiovec(skb_put(skb, len), msg->msg_iov, len);
     
        if (err < 0) {
                LOG_ERR("could not copy user data to skb\n");
                FREE_SKB(skb);
                goto out;
        }

        lock_sock(sk);
                
        err = scaffold_udp_transmit_skb(sk, skb, SCAFFOLD_PKT_DATA);
        
        if (err < 0) {
                LOG_ERR("udp xmit failed\n");
                FREE_SKB(skb);
        }

        release_sock(sk);
out:
        return err;
}

static int scaffold_udp_recvmsg(struct kiocb *iocb, struct sock *sk, 
                                struct msghdr *msg,
                                size_t len, int nonblock, int flags, 
                                int *addr_len)
{
	struct scaffold_sock *ss = scaffold_sk(sk);
        struct sockaddr_sf *sfaddr = (struct sockaddr_sf *)msg->msg_name;
        int retval = -ENOMEM;
	long timeo;
        
        lock_sock(sk);
        
        if (sk->sk_state == SCAFFOLD_CLOSED) {
                /* SCAFFOLD_CLOSED is a valid state here because recvmsg
                 * should return 0 and not an error */
		retval = -ENOTCONN;
		goto out;
	}

        if ((unsigned)msg->msg_namelen < sizeof(struct sockaddr_sf)) {
                retval = -EINVAL;
                LOG_DBG("address length is incorrect\n");
                goto out;
        }

	timeo = sock_rcvtimeo(sk, nonblock);

	do {
		struct sk_buff *skb = skb_peek(&sk->sk_receive_queue);

		if (skb)
			goto found_ok_skb;
	
		if (sk->sk_state >= SCAFFOLD_CLOSED) {
                        /*
			if (!sock_flag(sk, SOCK_DONE)) {
				retval = -ENOTCONN;
				break;
			}
                        */
                        retval = 0;
			break;
		}
                
		if (!timeo) {
			retval = -EAGAIN;
			break;
		}

		if (signal_pending(current)) {
			retval = sock_intr_errno(timeo);
			break;
		}
                //LOG_DBG("waiting for data\n");

		sk_wait_data(sk, &timeo);
		continue;
	found_ok_skb:
		if (len >= skb->len) {
			retval = skb->len;
                        len = skb->len;
                } else if (len < skb->len) {
			msg->msg_flags |= MSG_TRUNC;
                        retval = len;
                }
                
                /* Copy service id */
                if (sfaddr) {
                        size_t addrlen = msg->msg_namelen;
                        unsigned short from = udp_hdr(skb)->source;

                        sfaddr->sf_family = AF_SCAFFOLD;
                        *addr_len = sizeof(struct sockaddr_sf);
                        memcpy(&sfaddr->sf_srvid, &from, sizeof(struct service_id));

                        /* Copy also our local service id to the
                         * address buffer if size admits */
                        if (addrlen >= sizeof(struct sockaddr_sf) * 2) {
                                sfaddr = (struct sockaddr_sf *)((char *)msg->msg_name + sizeof(struct sockaddr_sf));
                                sfaddr->sf_family = AF_SCAFFOLD;

                                memcpy(&sfaddr->sf_srvid, &ss->local_srvid, 
                                       sizeof(struct service_id));
                        }
                }
                
                //LOG_DBG("dequeing skb with length %u len=%zu retval=%d\n", skb->len, len, retval);

		if (skb_copy_datagram_iovec(skb, 0, msg->msg_iov, len)) {
			/* Exception. Bailout! */
			retval = -EFAULT;
                        LOG_DBG("could not copy data, len=%zu\n", len);
			break;
		}
		if (!(flags & MSG_PEEK))
			sk_eat_skb(sk, skb, 0);
		break;
	} while (1);
out:
        release_sock(sk);
        
        return retval;
}

struct proto scaffold_udp_proto = {
	.name			= "SCAFFOLD_UDP",
	.owner			= THIS_MODULE,
        .init                   = scaffold_udp_init_sock,
        .destroy                = scaffold_udp_destroy_sock,        
	.close  		= scaffold_udp_close,   
        .connect                = scaffold_udp_connect,
	.disconnect 		= scaffold_udp_disconnect,
	.shutdown		= scaffold_udp_shutdown,
        .sendmsg                = scaffold_udp_sendmsg,
        .recvmsg                = scaffold_udp_recvmsg,
	.backlog_rcv		= __scaffold_udp_queue_rcv_skb,
        .hash                   = scaffold_sock_hash,
        .unhash                 = scaffold_sock_unhash,
	.obj_size		= sizeof(struct scaffold_udp_sock),
};