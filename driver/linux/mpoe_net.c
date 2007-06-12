#include <linux/kernel.h>
#include <linux/module.h>

#include "mpoe_common.h"
#include "mpoe_hal.h"

/*************
 * Finding, attaching, detaching interfaces
 */

/* returns an interface hold matching ifname */
static struct net_device *
mpoe_net_find_iface_by_name(const char * ifname)
{
	struct net_device * ifp;

        read_lock(&dev_base_lock);
        mpoe_for_each_netdev(ifp) {
		dev_hold(ifp);
		if (!strcmp(ifp->name, ifname)) {
		        read_unlock(&dev_base_lock);
			return ifp;
		}
		dev_put(ifp);
	}
        read_unlock(&dev_base_lock);

	printk(KERN_ERR "MPoE: Failed to find interface '%s'\n", ifname);
	return NULL;
}

static struct mpoe_iface ** mpoe_ifaces;
static unsigned int mpoe_iface_nr = 0;
static DECLARE_MUTEX_LOCKED(mpoe_iface_mutex);

/* called with interface hold */
static int
mpoe_net_attach_iface(struct net_device * ifp)
{
	struct mpoe_iface * iface;
	int ret;
	int i;

	if (mpoe_iface_nr == mpoe_iface_max) {
		printk(KERN_ERR "MPoE: Too many interfaces already attached\n");
		ret = -EBUSY;
		goto out_with_ifp_hold;
	}

	/* TODO: do not attach twice ? */

	for(i=0; i<mpoe_iface_max; i++)
		if (mpoe_ifaces[i] == NULL)
			break;

	iface = kzalloc(sizeof(struct mpoe_iface), GFP_KERNEL);
	if (!iface) {
		printk(KERN_ERR "MPoE: Failed to allocate interface as board %d\n", i);
		ret = -ENOMEM;
		goto out_with_ifp_hold;
	}

	printk(KERN_INFO "MPoE: Attaching interface '%s' as #%i\n", ifp->name, i);

	iface->eth_ifp = ifp;
	iface->endpoint_nr = 0;
	iface->endpoints = kzalloc(mpoe_endpoint_max * sizeof(struct mpoe_endpoint *), GFP_KERNEL);
	if (!iface->endpoints) {
		printk(KERN_ERR "MPoE: Failed to allocate interface endpoint pointers\n");
		ret = -ENOMEM;
		goto out_with_iface;
	}

	spin_lock_init(&iface->endpoint_lock);
	iface->index = i;
	mpoe_iface_nr++;
	mpoe_ifaces[i] = iface;

	return 0;

 out_with_iface:
	kfree(iface);
 out_with_ifp_hold:
	dev_put(ifp);
	return ret;
}

/* called with interface hold */
static int
mpoe_net_detach_iface(struct mpoe_iface * iface)
{
	if (iface->endpoint_nr) {
		printk(KERN_INFO "MPoE: cannot detach interface #%d '%s', still %d endpoints open\n",
		       iface->index, iface->eth_ifp->name, iface->endpoint_nr);
		return -EBUSY;
	}

	printk(KERN_INFO "MPoE: detaching interface #%d '%s'\n", iface->index, iface->eth_ifp->name);

	BUG_ON(mpoe_ifaces[iface->index] == NULL);
	mpoe_ifaces[iface->index] = NULL;
	mpoe_iface_nr--;
	kfree(iface->endpoints);
	dev_put(iface->eth_ifp);
	kfree(iface);

	return 0;
}

/* list attached interfaces */
int
mpoe_net_ifaces_show(char *buf)
{
	int total = 0;
	int i;

	down(&mpoe_iface_mutex);
	for (i=0; i<mpoe_iface_max; i++) {
		struct mpoe_iface * iface = mpoe_ifaces[i];
		if (iface) {
			char * ifname = iface->eth_ifp->name;
			int length = strlen(ifname);
			/* TODO: check total+length+2 <= PAGE_SIZE ? */
			strcpy(buf, ifname);
			buf += length;
			strcpy(buf, "\n");
			buf += 1;
			total += length+1;
		}
	}
	up(&mpoe_iface_mutex);

	return total + 1;
}

/* +name add an interface, -name removes one */
int
mpoe_net_ifaces_store(const char *buf, size_t size)
{
	char copy[IFNAMSIZ];
	char * ptr;

	/* remove the ending \n if required, so copy first since buf is const */
	strncpy(copy, buf+1, IFNAMSIZ);
	copy[IFNAMSIZ-1] = '\0';
	ptr = strchr(copy, '\n');
	if (ptr)
		*ptr = '\0';

	if (buf[0] == '-') {
		int i, found = 0;
		/* in case none matches, we return -EINVAL. if one matches, it sets ret accordingly */
		int ret = -EINVAL;

		down(&mpoe_iface_mutex);
		for(i=0; i<mpoe_iface_max; i++) {
			struct mpoe_iface * iface = mpoe_ifaces[i];
			if (iface != NULL && !strcmp(iface->eth_ifp->name, copy)) {
				ret = mpoe_net_detach_iface(iface);
				if (!ret)
					found = 1;
				break;
			}
		}
		up(&mpoe_iface_mutex);

		if (!found) {
			printk(KERN_ERR "MPoE: Cannot find any attached interface '%s' to detach\n", copy);
			return -EINVAL;
		}
		return size;

	} else if (buf[0] == '+') {
		struct net_device * ifp;
		int ret;

		ifp = mpoe_net_find_iface_by_name(copy);
		if (!ifp)
			return -EINVAL;

		down(&mpoe_iface_mutex);
		ret = mpoe_net_attach_iface(ifp);
		up(&mpoe_iface_mutex);
		if (ret < 0)
			return ret;

		return size;

	} else {
		printk(KERN_ERR "MPoE: Unrecognized command passed in the ifaces file, need either +name or -name\n");
		return -EINVAL;
	}
}

struct mpoe_iface *
mpoe_net_iface_from_ifp(struct net_device *ifp)
{
	int i;

	for (i=0; i<mpoe_iface_max; i++) {
		struct mpoe_iface * iface = mpoe_ifaces[i];
		if (iface && iface->eth_ifp == ifp)
			return iface;
	}

	return NULL;
}

int
mpoe_net_get_iface_count(void)
{
	int i, count = 0;

	for (i=0; i<mpoe_iface_max; i++)
		if (mpoe_ifaces[i] != NULL)
			count++;

	return count;
}

int
mpoe_net_get_iface_id(uint8_t board_index, struct mpoe_mac_addr * board_addr, char * board_name)
{
	struct net_device * ifp;

	if (board_index >= mpoe_iface_max
	    || mpoe_ifaces[board_index] == NULL)
		return -EINVAL;

	ifp = mpoe_ifaces[board_index]->eth_ifp;

	mpoe_mac_addr_of_netdevice(ifp, board_addr);
	strncpy(board_name, ifp->name, MPOE_IF_NAMESIZE);

	return 0;
}

/**********
 * Attaching endpoints to boards
 */

int
mpoe_net_attach_endpoint(struct mpoe_endpoint * endpoint, uint8_t board_index, uint8_t endpoint_index)
{
	struct mpoe_iface * iface;

	down(&mpoe_iface_mutex);
	if (board_index >= mpoe_iface_max || mpoe_ifaces[board_index] == NULL) {
		printk(KERN_ERR "MPoE: Cannot open endpoint on unexisting board %d\n", board_index);
		up(&mpoe_iface_mutex);
		return -EINVAL;
	}

	iface = mpoe_ifaces[board_index];

	if (endpoint_index >= mpoe_endpoint_max || iface->endpoints[endpoint_index] != NULL) {
		printk(KERN_ERR "MPoE: Cannot open busy endpoint %d\n", endpoint_index);
		up(&mpoe_iface_mutex);
		return -EBUSY;
	}

	endpoint->iface = iface;
	endpoint->board_index = board_index;
	endpoint->endpoint_index = endpoint_index;

	spin_lock(&iface->endpoint_lock);
	iface->endpoint_nr++;
	iface->endpoints[endpoint_index] = endpoint ;
	spin_unlock(&iface->endpoint_lock);
	up(&mpoe_iface_mutex);

	return 0;
}

void
mpoe_net_detach_endpoint(struct mpoe_endpoint * endpoint)
{
	struct mpoe_iface * iface = endpoint->iface;
	int index = endpoint->endpoint_index;

	spin_lock(&iface->endpoint_lock);
	iface->endpoint_nr--;
	BUG_ON(iface->endpoints[index] == NULL);
	iface->endpoints[index] = NULL;
	spin_unlock(&iface->endpoint_lock);
}

static struct packet_type mpoe_pt = {
	.type = __constant_htons(ETH_P_MPOE),
	.func = mpoe_net_recv,
};

/*************
 * Netdevice notifier
 */

static int
mpoe_netdevice_notifier_cb(struct notifier_block *unused,
				      unsigned long event, void *ptr)
{
        struct net_device *ifp = (struct net_device *) ptr;

        if (event == NETDEV_UNREGISTER) {
		int i;
		for (i=0; i<mpoe_iface_max; i++) {
			struct mpoe_iface * iface = mpoe_ifaces[i];
			if (iface && iface->eth_ifp == ifp) {
				int ret;
				int j;
				printk(KERN_INFO "MPoE: interface '%s' being unregistered, forcing closing of endpoints...\n",
				       ifp->name);
				for(j=0; j<mpoe_endpoint_max; j++) {
					struct mpoe_endpoint * endpoint = iface->endpoints[j];
					if (endpoint)
						mpoe_close_endpoint(endpoint, NULL);
				}
				ret = mpoe_net_detach_iface(iface);
				BUG_ON(ret);
			}
		}
	}

        return NOTIFY_DONE;
}

/*************
 * Initialization and termination
 */

static struct notifier_block mpoe_netdevice_notifier = {
        .notifier_call = mpoe_netdevice_notifier_cb,
};

int
mpoe_net_init(const char * ifnames)
{
	int ret = 0;

	dev_add_pack(&mpoe_pt);

	ret = register_netdevice_notifier(&mpoe_netdevice_notifier);
	if (ret < 0) {
		printk(KERN_ERR "MPoE: failed to register netdevice notifier\n");
		goto abort_with_pack;
	}

	mpoe_ifaces = kzalloc(mpoe_iface_max * sizeof(struct mpoe_iface *), GFP_KERNEL);
	if (!mpoe_ifaces) {
		printk(KERN_ERR "MPoE: failed to allocate interface array\n");
		ret = -ENOMEM;
		goto abort_with_notifier;
	}

	if (ifnames) {
		/* attach ifaces whose name are in ifnames (limited to mpoe_iface_max) */
		char * copy = kstrdup(ifnames, GFP_KERNEL);
		char * ifname;

		while ((ifname = strsep(&copy, ",")) != NULL) {
			struct net_device * ifp;
			ifp = mpoe_net_find_iface_by_name(ifname);
			if (ifp)
				if (mpoe_net_attach_iface(ifp) < 0)
					break;
		}

		kfree(copy);

	} else {
		/* attach everything (limited to mpoe_iface_max) */
		struct net_device * ifp;

	        read_lock(&dev_base_lock);
		mpoe_for_each_netdev(ifp) {
			dev_hold(ifp);
			if (mpoe_net_attach_iface(ifp) < 0)
				break;
		}
	        read_unlock(&dev_base_lock);
	}
	up(&mpoe_iface_mutex); /* has been initialized locked */

	printk(KERN_INFO "MPoE: attached %d interfaces\n", mpoe_iface_nr);
	return 0;

 abort_with_notifier:
	unregister_netdevice_notifier(&mpoe_netdevice_notifier);
 abort_with_pack:
	dev_remove_pack(&mpoe_pt);
	return ret;
}

void
mpoe_net_exit(void)
{
	int i, nr = 0;

	down(&mpoe_iface_mutex); /* should not be needed, since all other users
				  * have a reference to the chardev and thus prevent
				  * the module from being unloaded */

	for (i=0; i<mpoe_iface_max; i++) {
		struct mpoe_iface * iface = mpoe_ifaces[i];
		if (iface != NULL) {
			BUG_ON(mpoe_net_detach_iface(iface) < 0);
			nr++;
		}
	}

	printk(KERN_INFO "MPoE: detached %d interfaces\n", nr);

	kfree(mpoe_ifaces);

	unregister_netdevice_notifier(&mpoe_netdevice_notifier);

	dev_remove_pack(&mpoe_pt);
}

/*
 * Local variables:
 *  tab-width: 8
 *  c-basic-offset: 8
 *  c-indent-level: 8
 * End:
 */
