/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
#include <libstack/ctrlmsg.h>
#include <libstack/callback.h>
#include <event.h>
#include <string.h>
#include "debug.h"

extern struct libstack_callbacks *callbacks;
extern int eventloop_init(void);
extern void eventloop_fini(void);

int libstack_configure_interface(const char *ifname, 
                                 const struct as_addr *asaddr,
                                 const struct host_addr *haddr,
                                 unsigned short flags)
{
	struct ctrlmsg_iface_conf cm;

        memset(&cm, 0, sizeof(cm));
	cm.cmh.type = CTRLMSG_TYPE_IFACE_CONF;
	cm.cmh.len = sizeof(cm);
	strncpy(cm.ifname, ifname, IFNAMSIZ - 1);
        if (asaddr)
                memcpy(&cm.asaddr, asaddr, sizeof(*asaddr));
        if (haddr)
                memcpy(&cm.haddr, haddr, sizeof(*haddr));
	cm.flags = flags;

	return event_sendmsg(&cm, cm.cmh.len);
}

int libstack_set_service(struct service_id *srvid, const char *ifname)
{
        struct ctrlmsg_service cm;

        memset(&cm, 0, sizeof(cm));
        cm.cmh.type = CTRLMSG_TYPE_SET_SERVICE;
        cm.cmh.len = sizeof(cm);
        memcpy(&cm.srvid, srvid, sizeof(*srvid));
	strncpy(cm.ifname, ifname, IFNAMSIZ - 1);
        
        return event_sendmsg(&cm, cm.cmh.len);
}

int libstack_register_callbacks(struct libstack_callbacks *calls)
{
	if (callbacks) {
                LOG_ERR("Failed: callbacks already set\n");
                return -1;
        }
        LOG_DBG("registered callbacks\n");
	callbacks = calls;
	
	return 0;
}

void libstack_unregister_callbacks(struct libstack_callbacks *calls)
{
	if (callbacks == calls)
		callbacks = NULL;
}

int libstack_init(void)
{
	return eventloop_init();
}

void libstack_fini(void) 
{
	eventloop_fini();
}