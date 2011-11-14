/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/ioctl.h> 
#include <sys/select.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <netdb.h>
#include <libserval/serval.h>
#include <libservalctrl/hostctrl.h>
#include <libservalctrl/init.h>
#include <netinet/serval.h>
#include <serval/platform.h>
#include <serval/list.h>
#include <common/timer.h>
#include <common/debug.h>
#include <pthread.h>

#if defined(OS_LINUX)
#include "rtnl.h"
#endif
#if defined(OS_BSD)
#include "ifa.h"
#endif

static int should_exit = 0;
static int p[2] = { -1, -1 };
static struct timer_queue tq;
static struct service_id default_service;

struct servd_context {
        int router; /* Whether this service daemon is a stub
                       end-host or a router */
        struct in_addr router_ip;
        int router_ip_set;
        struct sockaddr_sv raddr, caddr;
        struct hostctrl *lhc, *rhc;
        struct list_head reglist;
        unsigned int num_regs;
        pthread_mutex_t lock; /* Protects the registration list */
};

struct registration {
        struct list_head lh;
        struct service_id srvid;
        struct in_addr ipaddr;
        int ip_set;
};

static struct registration *registration_add(struct servd_context *ctx,
                                             const struct service_id *srvid, 
                                             const struct in_addr *ipaddr)
{
        struct registration *r;

        r = malloc(sizeof(struct registration));

        if (!r)
                return NULL;

        memset(r, 0, sizeof(struct registration));
        INIT_LIST_HEAD(&r->lh);
        memcpy(&r->srvid, srvid, sizeof(struct service_id));
        if (ipaddr) {
                memcpy(&r->ipaddr, ipaddr, sizeof(struct in_addr));
                r->ip_set = 1;
        }

        pthread_mutex_lock(&ctx->lock);
        list_add_tail(&r->lh, &ctx->reglist);
        ctx->num_regs++;
        pthread_mutex_unlock(&ctx->lock);

        return r;        
}

static int registration_del(struct servd_context *ctx,
                            const struct service_id *srvid)
{
        struct registration *r;
        int ret = 0;

        pthread_mutex_lock(&ctx->lock);
        
        list_for_each_entry(r, &ctx->reglist, lh) {
                if (memcmp(&r->srvid, srvid, 
                           sizeof(struct service_id)) == 0) {
                        list_del(&r->lh);
                        free(r);
                        ret = 1;
                        break;
                }
        }

        pthread_mutex_unlock(&ctx->lock);

        return ret;
}

static int registration_redo(struct servd_context *ctx)
{
        struct registration *r;
        int ret = 0;

        pthread_mutex_lock(&ctx->lock);
        
        list_for_each_entry(r, &ctx->reglist, lh) {
                printf("Reregistering service %s\n",
                        service_id_to_str(&r->srvid));

                ret = hostctrl_service_register(ctx->rhc, &r->srvid, 0);
                
                if (ret <= 0) {
                        fprintf(stderr, "Could not reregister service %s\n",
                                service_id_to_str(&r->srvid));
                }
        }

        pthread_mutex_unlock(&ctx->lock);

        return ret;
}

static void registration_clear(struct servd_context *ctx)
{
        while (!list_empty(&ctx->reglist)) {
                struct registration *reg = 
                        list_first_entry(&ctx->reglist, 
                                        struct registration, lh);
                list_del(&reg->lh);
                free(reg);
        }
}

static int name_to_inet_addr(const char *name, struct in_addr *ip)
{
        struct addrinfo *ai;
        struct addrinfo hints = { .ai_family = AF_INET,
                                  .ai_socktype = 0,
                                  .ai_protocol = 0, };
        int ret;

        ret = getaddrinfo(name, "0", &hints, &ai);
        
        if (ret != 0) {
                fprintf(stderr, "getaddrinfo error=%d\n", ret);
                return -1;
        }

        while (ai) {
                if (ai->ai_family == AF_INET) {
                        struct sockaddr_in *in = 
                                (struct sockaddr_in *)ai->ai_addr;
                        memcpy(ip, &in->sin_addr, sizeof(*ip));
                        ret = 1;
                        break;
                }
                ai = ai->ai_next;
        } 
        
        freeaddrinfo(ai);

        return ret;
}

static void signal_handler(int sig)
{
        ssize_t ret;
        char q = 'q';
	should_exit = 1;

        ret = write(p[1], &q, 1);

        if (ret < 0) {
                LOG_ERR("Could not signal quit!\n");
        }
}

int servd_interface_up(const char *ifname, void *context)
{
        struct servd_context *ctx = context;

        /* Delay operations for a short time. The reason is that the
           stack doesn't seem to be immediately ready for using the
           newly assigned address. */
        sleep(1);

	LOG_DBG("Interface %s changed address. Migrating flows\n", ifname);
        
        hostctrl_interface_migrate(ctx->lhc, ifname, ifname);
        
        /* Make sure our service router entry is present. It might
           have been purged when the interface went down
           previously. Get any new information default route (default
           broadcast) and update it in the callback. */
        if (ctx->router_ip_set && !ctx->router) {
                sleep(1);
                hostctrl_service_get(ctx->lhc, &default_service, 
                                     0, NULL);
        }

        LOG_DBG("Resending registrations\n");

        return registration_redo(ctx);
}

static int register_service_remotely(struct hostctrl *hc,
                                     const struct service_id *srvid,
                                     unsigned short flags,
                                     unsigned short prefix,
                                     const struct in_addr *local_ip)
{
        struct servd_context *ctx = hc->context;
	int ret;

	LOG_DBG("service=%s\n", service_id_to_str(srvid));

        ret = hostctrl_service_register(ctx->rhc, srvid, prefix);
    
        registration_add(ctx, srvid, local_ip);

        return ret;
}

static int unregister_service_remotely(struct hostctrl *hc,
                                       const struct service_id *srvid,
                                       unsigned short flags,
                                       unsigned short prefix,
                                       const struct in_addr *local_ip)
{
        struct servd_context *ctx = hc->context;
        int ret;

	LOG_DBG("serviceID=%s\n", service_id_to_str(srvid));

        ret = hostctrl_service_unregister(ctx->rhc, srvid, prefix);
    
        registration_del(ctx, srvid);

        return ret;
}

static int handle_incoming_registration(struct hostctrl *hc,
                                        const struct service_id *srvid,
                                        unsigned short flags,
                                        unsigned short prefix,
                                        const struct in_addr *remote_ip)
{
        struct servd_context *ctx = hc->context;

#if defined(ENABLE_DEBUG)
        {
                char buf[18];
                LOG_DBG("Remote service %s @ %s registered\n", 
                        service_id_to_str(srvid), 
                        inet_ntop(AF_INET, remote_ip, buf, 18));
        }
#endif
        /* Addd this service the local service table. */
        return hostctrl_service_add(ctx->lhc, srvid, prefix, 
                                    0, 0, remote_ip);
}

static int handle_incoming_unregistration(struct hostctrl *hc,
                                          const struct service_id *srvid,
                                          unsigned short flags,
                                          unsigned short prefix,
                                          const struct in_addr *remote_ip)
{
        struct servd_context *ctx = hc->context;

#if defined(ENABLE_DEBUG)
        {
                char buf[18];
                LOG_DBG("Remote service %s @ %s unregistered\n", 
                        service_id_to_str(srvid), 
                        inet_ntop(AF_INET, remote_ip, buf, 18));
        }
#endif
        /* Register this service the local service table. */
        return hostctrl_service_remove(ctx->lhc, srvid, prefix, 
                                       remote_ip);
}

/*
  Callback that returns the result of a previous service 'get'.

  We use it to set the default service resolve rule to a fixed service
  router IP.
 */
static int local_service_get_result(struct hostctrl *hc,
                                    const struct service_id *srvid,
                                    unsigned short flags,
                                    unsigned short prefix,
                                    unsigned int priority,
                                    unsigned int weight,
                                    struct in_addr *ip)
{
        struct servd_context *ctx = hc->context;
        int ret = 0;

#if defined(ENABLE_DEBUG)
        char buf[18], buf2[18];
        LOG_DBG("GET: %s valid=%s is_router=%s router_ip_set=%s"
                " prio=%u weight=%u"
                " requested_ip=%s new_ip=%s\n",
                service_id_to_str(srvid),
                flags & SVSF_INVALID ? "false" : "true",
                ctx->router ? "true" : "false",
                ctx->router_ip_set ? "true" : "false",
                priority, weight,
                inet_ntop(AF_INET, ip, buf, 18),
                inet_ntop(AF_INET, &ctx->router_ip, buf2, 18));
#endif
     
        if (flags & SVSF_INVALID) {
                LOG_DBG("No default service route set\n");
                /* There was no existing route, the 'get' returned
                   nothing. Just add our default route */
                ret = hostctrl_service_add(ctx->lhc, &default_service,
                                           0, 1, 0, &ctx->router_ip);
        } else if (!ctx->router && ctx->router_ip_set && 
                   memcmp(&default_service, srvid, 
                          sizeof(default_service)) == 0 && 
                   memcmp(&ctx->router_ip, ip, sizeof(*ip)) != 0) {
                LOG_DBG("Replacing default route\n");
                /* The 'get' for the default service returned
                   something. Update the existing entry */
                ret = hostctrl_service_modify(ctx->lhc, srvid, 
                                              prefix, priority,
                                              weight, ip, &ctx->router_ip);
        }
        
        return ret;
}
                                   
static struct hostctrl_callback lcb = {
        .service_registration = register_service_remotely,
        .service_unregistration = unregister_service_remotely,
        .service_get = local_service_get_result,
};

static struct hostctrl_callback rcb = {
        .service_registration = handle_incoming_registration,
        .service_unregistration = handle_incoming_unregistration,
};

static int daemonize(void)
{
        int i, sid;
	FILE *f;

        /* check if already a daemon */
	if (getppid() == 1) 
                return -1; 
	
	i = fork();

	if (i < 0) {
		fprintf(stderr, "Fork error...\n");
                return -1;
	}
	if (i > 0) {
		//printf("Parent done... pid=%u\n", getpid());
                exit(EXIT_SUCCESS);
	}
	/* new child (daemon) continues here */
	
	/* Change the file mode mask */
	umask(0);
		
	/* Create a new SID for the child process */
	sid = setsid();
	
	if (sid < 0)
		return -1;
	
	/* 
	 Change the current working directory. This prevents the current
	 directory from being locked; hence not being able to remove it. 
	 */
	if (chdir("/") < 0) {
		return -1;
	}
	
	/* Redirect standard files to /dev/null */
	f = freopen("/dev/null", "r", stdin);

        if (!f) {
                LOG_ERR("stdin redirection failed\n");
        }

	f = freopen("/dev/null", "w", stdout);

        if (!f) {
                LOG_ERR("stdout redirection failed\n");
        }

	f = freopen("/dev/null", "w", stderr);

        if (!f) {
                LOG_ERR("stderr redirection failed\n");
        }

        return 0;
}

int main(int argc, char **argv)
{
	struct sigaction sigact;
#if defined(OS_LINUX)
        struct netlink_handle nlh;
#endif
        fd_set readfds;
        int daemon = 0;
	int ret = EXIT_SUCCESS;
        unsigned int router_id = 88888, client_id = 55555;
        struct servd_context ctx;

        memset(&default_service, 0, sizeof(default_service));
	memset(&sigact, 0, sizeof(struct sigaction));
        memset(&ctx, 0, sizeof(ctx));
        INIT_LIST_HEAD(&ctx.reglist);
        pthread_mutex_init(&ctx.lock, NULL);

	sigact.sa_handler = &signal_handler;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGHUP, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);

        argc--;
	argv++;
        
	while (argc) {
                if (strcmp(argv[0], "-d") == 0 ||
                    strcmp(argv[0], "--daemon") == 0) {
                        daemon = 1;
                } else if (strcmp(argv[0], "-r") == 0 ||
                           strcmp(argv[0], "--router") == 0) {
                        LOG_DBG("Host is service router\n");
                        ctx.router = 1;
                } else if (strcmp(argv[0], "-rid") == 0 ||
                           strcmp(argv[0], "--router-id") == 0) {
                        char *ptr;

                        if (argc < 1) {
                                fprintf(stderr, "No router ID given\n");
                                return -1;
                        }

                        router_id = strtoul(argv[1], &ptr, 10);
                        
                        if (!(*ptr == '\0' && argv[1] != '\0')) {
                                fprintf(stderr, "bad router id format '%s',"
                                        " should beinteger string\n",
                                        argv[1]);
                                return -1;
                        }

                        argc--;
                        argv++;
                } else if (strcmp(argv[0], "-rip") == 0 ||
                           strcmp(argv[0], "--router-ip") == 0) {
                        if (argc < 1) {
                                fprintf(stderr, "No router IP given\n");
                                return -1;
                        }
                        if (name_to_inet_addr(argv[1], &ctx.router_ip) == 1 ||
                            inet_pton(AF_INET, argv[1], &ctx.router_ip) == 1) {
                                LOG_DBG("Service router IP is %s\n", argv[1]);
                                ctx.router_ip_set = 1;
                        }
                } else if (strcmp(argv[0], "-cid") == 0 ||
                           strcmp(argv[0], "--client-id") == 0) {
                        char *ptr;

                        if (argc < 1) {
                                fprintf(stderr, "No client ID given\n");
                                return -1;
                        }

                        client_id = strtoul(argv[1], &ptr, 10);
                        
                        if (!(*ptr == '\0' && argv[1] != '\0')) {
                                fprintf(stderr, "bad client id format '%s',"
                                        " should be short integer string\n",
                                        argv[1]);
                                return -1;
                        }

                        argc--;
                        argv++;
                } 
		argc--;
		argv++;
	}	
      
        if (daemon) {
                LOG_DBG("going daemon...\n");
                ret = daemonize();
                
                if (ret < 0) {
                        LOG_ERR("Could not make daemon\n");
                        return ret;
                } 
        }

        ret = timer_queue_init(&tq);

        if (ret == -1) {
                LOG_ERR("timer_queue_init failure\n");
                return -1;
        }

	ret = pipe(p);

        if (ret == -1) {
		LOG_ERR("Could not open pipe\n");
		goto fail_pipe;
        }

	ret = libservalctrl_init();

	if (ret == -1) {
		LOG_ERR("Could not init libservalctrl\n");
		goto fail_libservalctrl;
	}

       
        ctx.lhc = hostctrl_local_create(&lcb, &ctx, 0);
        
        if (!ctx.lhc) {
                LOG_ERR("Could not create local host control\n");
                goto fail_hostctrl_local;
        }

        ctx.raddr.sv_family = AF_SERVAL;
        ctx.raddr.sv_srvid.srv_un.un_id32[0] = htonl(router_id);

        ctx.caddr.sv_family = AF_SERVAL;
        ctx.caddr.sv_srvid.srv_un.un_id32[0] = htonl(client_id);
	
                
        if (ctx.router) {
                ctx.rhc = hostctrl_remote_create_specific(&rcb, &ctx,
                                                          (struct sockaddr *)&ctx.raddr, 
                                                          sizeof(ctx.raddr),
                                                          (struct sockaddr *)&ctx.caddr, 
                                                          sizeof(ctx.caddr), HCF_ROUTER);                
        } else {
                ctx.rhc = hostctrl_remote_create_specific(&rcb, &ctx,
                                                          (struct sockaddr *)&ctx.caddr, 
                                                          sizeof(ctx.caddr),
                                                          (struct sockaddr *)&ctx.raddr, 
                                                          sizeof(ctx.raddr), 0);
        }
        
        if (!ctx.rhc) {
                LOG_ERR("Could not create remote host control\n");
                goto fail_hostctrl_remote;
        }

        hostctrl_start(ctx.rhc);	
        hostctrl_start(ctx.lhc);

#if defined(OS_LINUX)
	ret = rtnl_init(&nlh, &ctx);

	if (ret < 0) {
		LOG_ERR("Could not open netlink socket\n");
                goto fail_netlink;
	}

	ret = rtnl_getaddr(&nlh);

        if (ret < 0) {
                LOG_ERR("Could not netlink request: %s\n",
                        strerror(errno));
                rtnl_close(&nlh);
                goto fail_netlink;
        }
#endif

#if defined(OS_BSD)
        ret = ifaddrs_init();

        if (ret < 0) {
                LOG_ERR("Could not discover interfaces\n");
                goto fail_ifaddrs;
        }
#endif
        /* If we are a client and have a fixed IP for the service
           router, then replace an existing "default" service rule by
           querying for the current one and modifying it in the
           resulting callback. */
        if (ctx.router_ip_set && !ctx.router) {
                hostctrl_service_get(ctx.lhc, &default_service, 0, NULL);
        }

#define MAX(x,y) (x > y ? x : y)

        while (!should_exit) {
                int maxfd = 0;
                struct timeval timeout = { 0, 0 }, *t = NULL;

                FD_ZERO(&readfds);

                FD_SET(timer_queue_get_signal(&tq), &readfds);
                maxfd = MAX(timer_queue_get_signal(&tq), maxfd);

#if defined(OS_LINUX)
                FD_SET(nlh.fd, &readfds);
		maxfd = MAX(nlh.fd, maxfd);
#endif
                FD_SET(p[0], &readfds);               
                maxfd = MAX(p[0], maxfd);

                if (timer_next_timeout_timeval(&tq, &timeout))
                        t = &timeout;

                ret = select(maxfd + 1, &readfds, NULL, NULL, t);

                if (ret == 0) {
                        ret = timer_handle_timeout(&tq);
                } else if (ret == -1) {
			if (errno == EINTR) {
				should_exit = 1;
			} else {
				LOG_ERR("select: %s\n", 
					strerror(errno));
                                should_exit = 1;
			}
                } else {
                        if (FD_ISSET(timer_queue_get_signal(&tq), &readfds)) {
                                /* Just reschedule timeout */
                        }
#if defined(OS_LINUX)
                        if (FD_ISSET(nlh.fd, &readfds)) {
                                LOG_DBG("netlink readable\n");
                                rtnl_read(&nlh);
                        }
#endif
                        if (FD_ISSET(p[0], &readfds)) {
                                should_exit = 1;
                        }
                }        
        }

	LOG_DBG("servd exits\n");

        if (ctx.router_ip_set && !ctx.router) {
                hostctrl_service_remove(ctx.lhc, &ctx.raddr.sv_srvid, 0, 
                                        &ctx.router_ip);
        }

#if defined(OS_BSD)
        ifaddrs_fini();
fail_ifaddrs:
#endif
#if defined(OS_LINUX)
	rtnl_close(&nlh);
fail_netlink:
#endif
        hostctrl_free(ctx.rhc);
fail_hostctrl_remote:
        hostctrl_free(ctx.lhc);
fail_hostctrl_local:
        libservalctrl_fini();
fail_libservalctrl:
	close(p[0]);
	close(p[1]);
fail_pipe:
        timer_queue_fini(&tq);

        registration_clear(&ctx);
        pthread_mutex_destroy(&ctx.lock);
	LOG_DBG("done\n");

        return ret;
}