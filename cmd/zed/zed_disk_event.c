/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License Version 1.0 (CDDL-1.0).
 * You can obtain a copy of the license from the top-level file
 * "OPENSOLARIS.LICENSE" or at <http://opensource.org/licenses/CDDL-1.0>.
 * You may not use this file except in compliance with the license.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2016, 2017, Intel Corporation.
 */

#ifdef HAVE_LIBUDEV

#include <errno.h>
#include <fcntl.h>
#include <libnvpair.h>
#include <libudev.h>
#include <libzfs.h>
#include <libzutil.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>

#include <sys/sysevent/eventdefs.h>
#include <sys/sysevent/dev.h>
#include <sys/fm/protocol.h>

#include "zed_log.h"
#include "zed_disk_event.h"
#include "agents/zfs_agents.h"

/*
 * Portions of ZED need to see disk events for disks belonging to ZFS pools.
 * A libudev monitor is established to monitor block device actions and pass
 * them on to internal ZED logic modules.  Initially, zfs_mod.c is the only
 * consumer and is the Linux equivalent for the illumos syseventd ZFS SLM
 * module responsible for handling disk events for ZFS.
 */

pthread_t g_mon_tid;
pthread_t dwd_mon_tid;
struct udev *g_udev;
struct udev_monitor *g_mon;


#define	DEV_BYID_PATH	"/dev/disk/by-id/"

/* 64MB is minimum usable disk for ZFS */
#define	MINIMUM_SECTORS		131072


/*
 * Post disk event to SLM module
 *
 * occurs in the context of monitor thread
 */
static void
zed_udev_event(const char *class, const char *subclass, nvlist_t *nvl)
{
	char *strval;
	uint64_t numval;

	zed_log_msg(LOG_INFO, "zed_disk_event:");
	zed_log_msg(LOG_INFO, "\tclass: %s", class);
	zed_log_msg(LOG_INFO, "\tsubclass: %s", subclass);
	if (nvlist_lookup_string(nvl, DEV_NAME, &strval) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %s", DEV_NAME, strval);
	if (nvlist_lookup_string(nvl, DEV_PATH, &strval) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %s", DEV_PATH, strval);
	if (nvlist_lookup_string(nvl, DEV_IDENTIFIER, &strval) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %s", DEV_IDENTIFIER, strval);
	if (nvlist_lookup_string(nvl, DEV_PHYS_PATH, &strval) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %s", DEV_PHYS_PATH, strval);
	if (nvlist_lookup_uint64(nvl, DEV_SIZE, &numval) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %llu", DEV_SIZE, numval);
	if (nvlist_lookup_uint64(nvl, ZFS_EV_POOL_GUID, &numval) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %llu", ZFS_EV_POOL_GUID, numval);
	if (nvlist_lookup_uint64(nvl, ZFS_EV_VDEV_GUID, &numval) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %llu", ZFS_EV_VDEV_GUID, numval);

	(void) zfs_agent_post_event(class, subclass, nvl);
}

/*
 * dev_event_nvlist: place event schema into an nv pair list
 *
 * NAME			VALUE (example)
 * --------------	--------------------------------------------------------
 * DEV_NAME		/dev/sdl
 * DEV_PATH		/devices/pci0000:00/0000:00:03.0/0000:04:00.0/host0/...
 * DEV_IDENTIFIER	ata-Hitachi_HTS725050A9A362_100601PCG420VLJ37DMC
 * DEV_PHYS_PATH	pci-0000:04:00.0-sas-0x4433221101000000-lun-0
 * DEV_IS_PART		---
 * DEV_SIZE		500107862016
 * ZFS_EV_POOL_GUID	17523635698032189180
 * ZFS_EV_VDEV_GUID	14663607734290803088
 */
static nvlist_t *
dev_event_nvlist(struct udev_device *dev)
{
	nvlist_t *nvl;
	char strval[128];
	const char *value, *path;
	uint64_t guid;

	if (nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0) != 0)
		return (NULL);

	if (zfs_device_get_devid(dev, strval, sizeof (strval)) == 0)
		(void) nvlist_add_string(nvl, DEV_IDENTIFIER, strval);
	if (zfs_device_get_physical(dev, strval, sizeof (strval)) == 0)
		(void) nvlist_add_string(nvl, DEV_PHYS_PATH, strval);
	if ((path = udev_device_get_devnode(dev)) != NULL)
		(void) nvlist_add_string(nvl, DEV_NAME, path);
	if ((value = udev_device_get_devpath(dev)) != NULL)
		(void) nvlist_add_string(nvl, DEV_PATH, value);
	value = udev_device_get_devtype(dev);
	if ((value != NULL && strcmp("partition", value) == 0) ||
	    (udev_device_get_property_value(dev, "ID_PART_ENTRY_NUMBER")
	    != NULL)) {
		(void) nvlist_add_boolean(nvl, DEV_IS_PART);
	}
	if ((value = udev_device_get_sysattr_value(dev, "size")) != NULL) {
		uint64_t numval = DEV_BSIZE;

		numval *= strtoull(value, NULL, 10);
		(void) nvlist_add_uint64(nvl, DEV_SIZE, numval);
	}

	/*
	 * Grab the pool and vdev guids from blkid cache
	 */
	value = udev_device_get_property_value(dev, "ID_FS_UUID");
	if (value != NULL && (guid = strtoull(value, NULL, 10)) != 0)
		(void) nvlist_add_uint64(nvl, ZFS_EV_POOL_GUID, guid);

	value = udev_device_get_property_value(dev, "ID_FS_UUID_SUB");
	if (value != NULL && (guid = strtoull(value, NULL, 10)) != 0)
		(void) nvlist_add_uint64(nvl, ZFS_EV_VDEV_GUID, guid);

	/*
	 * Either a vdev guid or a devid must be present for matching
	 */
	if (!nvlist_exists(nvl, DEV_IDENTIFIER) &&
	    !nvlist_exists(nvl, ZFS_EV_VDEV_GUID)) {
		nvlist_free(nvl);
		return (NULL);
	}

	return (nvl);
}

#define	DWDFAULT "/var/run/dwd.fifo-faulted"	/* DWD telemetry pipe */

struct pollfd dwd_pfd[1];

static char
skipover(char match, boolean_t *hit_eof)
{
	int cnt;
	char c;

	while ((cnt = read(dwd_pfd[0].fd, &c, 1)) == 1 && c == match)
		;
	*hit_eof = (cnt == 0);
	return (c);
}

static char
skipto(char match, boolean_t *hit_eof)
{
	int cnt;
	char c;

	while ((cnt = read(dwd_pfd[0].fd, &c, 1)) == 1 && c != match)
		;
	*hit_eof = (cnt == 0);
	return (c);
}

/*
 *  Listen for Cray dwd device events
 */
static void *
zed_dwd_monitor(void *arg)
{

	char eventbuf[256];
	char *ptr;
	boolean_t hit_eof;
	char c;
	uint64_t vdev_guid, pool_guid;
	nvlist_t *nvl;
	const char *class, *subclass;

	dwd_pfd[0].events = POLLIN;
	dwd_pfd[0].revents = 0;
	zed_log_msg(LOG_INFO, "Waiting for new dwd disk events...");
	while (1) {
		/* allow a cancellation while blocked (poll) */
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		/* blocks at poll until an event occurs */
		if (poll(dwd_pfd, 1, -1) <= 0) {
			zed_log_msg(LOG_WARNING, "zed_dwd_monitor: poll "
			    "error %d", errno);
			/*
			 * re-init fd
			 */
			(void) close(dwd_pfd[0].fd);
			if ((dwd_pfd[0].fd = open(DWDFAULT, O_RDWR|O_NONBLOCK))
			    < 0) {
				zed_log_msg(LOG_WARNING,
			    "Failed to reopen DWD fault notification pipe %s"
				" (%d)", DWDFAULT, errno);
			}
			continue;
		}

		/* allow all steps to complete before a cancellation */
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		/*
		 * Here if a notification is in the pipe.
		 * Empty the dwd notification pipe so we will block till zed
		 * gets another notification from dwd.  Post the event we got
		 * notification of.  We expect:
		 * .<faulted device guid>,<pool guid>. in the
		 * pipe for each event.
		 */
		ptr = eventbuf;
		/* skip to a non-dot character */
		c = skipover('.', &hit_eof);
		if (hit_eof) /* no more input */
			continue;
		/* gather characters till comma seen */
		*ptr++ = c;
		while (read(dwd_pfd[0].fd, &c, 1) == 1 && c != ',') {
			*ptr++ = c;
			if (c == '.') { /* unexpected end of record */
				zed_log_msg(LOG_WARNING,
				    "Malformed record on DWD fault pipe %s",
				    DWDFAULT);
				goto loop;
			}
			if ((ptr - eventbuf) >= 256) {
				zed_log_msg(LOG_WARNING,
				    "Overflow on DWD fault pipe %s", DWDFAULT);
				/* skip till next dot seen */
				(void) skipto('.', &hit_eof);
				if (hit_eof) /* no more input */
					goto loop;
			}
		}
		*ptr = '\0';
		/* eventbuf now has guid of faulted device */
		vdev_guid = strtoull(eventbuf, NULL, 10);
		/*
		 * We have the vdev guid now get the pool guid it is in and
		 * post a dwdfault event.
		 */
		ptr = eventbuf;
		/* gather characters till dot seen */
		while (read(dwd_pfd[0].fd, &c, 1) == 1 && c != '.') {
			*ptr++ = c;
			if ((ptr - eventbuf) >= 256) {
				zed_log_msg(LOG_WARNING,
				    "Overflow on DWD fault pipe %s", DWDFAULT);
				/* skip till next dot seen */
				(void) skipto('.', &hit_eof);
				if (hit_eof) /* no more input */
					goto loop;
			}
		}
		*ptr = '\0';
		/* eventbuf now has guid of pool with faulted device */
		pool_guid = strtoull(eventbuf, NULL, 10);
		/*
		 * Create a dwdfault event and post it
		 */
		zed_log_msg(LOG_INFO,
		    "Posting DWD fault event for pool %llu, vdev %llu",
		    pool_guid, vdev_guid);
		if (nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0) != 0)
			return (NULL);
		(void) nvlist_add_uint64(nvl, ZFS_EV_POOL_GUID, pool_guid);
		(void) nvlist_add_uint64(nvl, ZFS_EV_VDEV_GUID, vdev_guid);
		class = "dwdfault.fs.zfs.device";
		subclass = ESC_DISK;
		(void) nvlist_add_string(nvl, FM_CLASS, class);
		(void) zfs_agent_post_event(class, subclass, nvl);
		c = skipto('.', &hit_eof);
loop:
		continue;
	}
}

/*
 * Set up monitor thread for notifications from Cray Disk Watcher Daemon
 */
int
cray_dwd_watcher_init(void)
{
	/*
	 * Set up the named pipe that DWD will write to.  Note that
	 * DWD will also create the pipe so this may fail with EEXIST.
	 */
	if (mkfifo(DWDFAULT, 0666) < 0) {
		if (errno != EEXIST) {
			zed_log_msg(LOG_WARNING,
			    "Failed to create DWD fault notification pipe %s",
			    DWDFAULT);
			return (-1);
		}
	}

	if ((dwd_pfd[0].fd = open(DWDFAULT, O_RDWR|O_NONBLOCK)) < 0) {
		zed_log_msg(LOG_WARNING,
		    "Failed to open DWD fault notification pipe %s (%d)",
		    DWDFAULT, errno);
		return (-1);
	}
	/* spawn a thread to monitor the pipe */
	if (pthread_create(&dwd_mon_tid, NULL, zed_dwd_monitor, NULL) != 0) {
		zed_log_msg(LOG_WARNING,
		    "pthread_create of dwd monitor failed");
		return (-1);
	}

	zed_log_msg(LOG_INFO, "cray_dwd_watcher_init");
	return (0);
}

/*
 *  Listen for block device uevents
 */
static void *
zed_udev_monitor(void *arg)
{
	struct udev_monitor *mon = arg;
	char *tmp, *tmp2;

	zed_log_msg(LOG_INFO, "Waiting for new udev disk events...");

	while (1) {
		struct udev_device *dev;
		const char *action, *type, *part, *sectors;
		const char *bus, *uuid;
		const char *class, *subclass;
		nvlist_t *nvl;
		boolean_t is_zfs = B_FALSE;

		/* allow a cancellation while blocked (recvmsg) */
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		/* blocks at recvmsg until an event occurs */
		if ((dev = udev_monitor_receive_device(mon)) == NULL) {
			zed_log_msg(LOG_WARNING, "zed_udev_monitor: receive "
			    "device error %d", errno);
			continue;
		}

		/* allow all steps to complete before a cancellation */
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		/*
		 * Strongly typed device is the preferred filter
		 */
		type = udev_device_get_property_value(dev, "ID_FS_TYPE");
		if (type != NULL && type[0] != '\0') {
			if (strcmp(type, "zfs_member") == 0) {
				is_zfs = B_TRUE;
			} else {
				/* not ours, so skip */
				zed_log_msg(LOG_INFO, "zed_udev_monitor: skip "
				    "%s (in use by %s)",
				    udev_device_get_devnode(dev), type);
				udev_device_unref(dev);
				continue;
			}
		}

		/*
		 * if this is a disk and it is partitioned, then the
		 * zfs label will reside in a DEVTYPE=partition and
		 * we can skip passing this event
		 */
		type = udev_device_get_property_value(dev, "DEVTYPE");
		part = udev_device_get_property_value(dev,
		    "ID_PART_TABLE_TYPE");
		if (type != NULL && type[0] != '\0' &&
		    strcmp(type, "disk") == 0 &&
		    part != NULL && part[0] != '\0') {
			/* skip and wait for partition event */
			udev_device_unref(dev);
			continue;
		}

		/*
		 * ignore small partitions
		 */
		sectors = udev_device_get_property_value(dev,
		    "ID_PART_ENTRY_SIZE");
		if (sectors == NULL)
			sectors = udev_device_get_sysattr_value(dev, "size");
		if (sectors != NULL &&
		    strtoull(sectors, NULL, 10) < MINIMUM_SECTORS) {
			udev_device_unref(dev);
			continue;
		}

		/*
		 * If the blkid probe didn't find ZFS, then a persistent
		 * device id string is required in the message schema
		 * for matching with vdevs. Preflight here for expected
		 * udev information.
		 */
		bus = udev_device_get_property_value(dev, "ID_BUS");
		uuid = udev_device_get_property_value(dev, "DM_UUID");
		if (!is_zfs && (bus == NULL && uuid == NULL)) {
			zed_log_msg(LOG_INFO, "zed_udev_monitor: %s no devid "
			    "source", udev_device_get_devnode(dev));
			udev_device_unref(dev);
			continue;
		}

		action = udev_device_get_action(dev);
		if (strcmp(action, "add") == 0) {
			class = EC_DEV_ADD;
			subclass = ESC_DISK;
		} else if (strcmp(action, "remove") == 0) {
			class = EC_DEV_REMOVE;
			subclass = ESC_DISK;
		} else if (strcmp(action, "change") == 0) {
			class = EC_DEV_STATUS;
			subclass = ESC_DEV_DLE;
		} else {
			zed_log_msg(LOG_WARNING, "zed_udev_monitor: %s unknown",
			    action);
			udev_device_unref(dev);
			continue;
		}

		/*
		 * Special case an EC_DEV_ADD for multipath devices
		 *
		 * When a multipath device is created, udev reports the
		 * following:
		 *
		 * 1.	"add" event of the dm device for the multipath device
		 *	(like /dev/dm-3).
		 * 2.	"change" event to create the actual multipath device
		 *	symlink (like /dev/mapper/mpatha).  The event also
		 *	passes back the relevant DM vars we care about, like
		 *	DM_UUID.
		 * 3.	Another "change" event identical to #2 (that we ignore).
		 *
		 * To get the behavior we want, we treat the "change" event
		 * in #2 as a "add" event; as if "/dev/mapper/mpatha" was
		 * a new disk being added.
		 */
		if (strcmp(class, EC_DEV_STATUS) == 0 &&
		    udev_device_get_property_value(dev, "DM_UUID") &&
		    udev_device_get_property_value(dev, "MPATH_SBIN_PATH")) {
			tmp = (char *)udev_device_get_devnode(dev);
			tmp2 = zfs_get_underlying_path(tmp);
			if (tmp && tmp2 && (strcmp(tmp, tmp2) != 0)) {
				/*
				 * We have a real underlying device, which
				 * means that this multipath "change" event is
				 * an "add" event.
				 *
				 * If the multipath device and the underlying
				 * dev are the same name (i.e. /dev/dm-5), then
				 * there is no real underlying disk for this
				 * multipath device, and so this "change" event
				 * really is a multipath removal.
				 */
				class = EC_DEV_ADD;
				subclass = ESC_DISK;
			} else {
				tmp = (char *)
				    udev_device_get_property_value(dev,
				    "DM_NR_VALID_PATHS");
				/* treat as a multipath remove */
				if (tmp != NULL && strcmp(tmp, "0") == 0) {
					class = EC_DEV_REMOVE;
					subclass = ESC_DISK;
				}
			}
			free(tmp2);
		}

		/*
		 * Special case an EC_DEV_ADD for scsi_debug devices
		 *
		 * These devices require a udevadm trigger command after
		 * creation in order to register the vdev_id scsidebug alias
		 * rule (adds a persistent path (phys_path) used for fault
		 * management automated tests in the ZFS test suite.
		 *
		 * After udevadm trigger command, event registers as a "change"
		 * event but needs to instead be handled as another "add" event
		 * to allow for disk labeling and partitioning to occur.
		 */
		if (strcmp(class, EC_DEV_STATUS) == 0 &&
		    udev_device_get_property_value(dev, "ID_VDEV") &&
		    udev_device_get_property_value(dev, "ID_MODEL")) {
			const char *id_model, *id_model_sd = "scsi_debug";

			id_model = udev_device_get_property_value(dev,
			    "ID_MODEL");
			if (strcmp(id_model, id_model_sd) == 0) {
				class = EC_DEV_ADD;
				subclass = ESC_DISK;
			}
		}

		if ((nvl = dev_event_nvlist(dev)) != NULL) {
			zed_udev_event(class, subclass, nvl);
			nvlist_free(nvl);
		}

		udev_device_unref(dev);
	}

	return (NULL);
}

int
zed_disk_event_init()
{
	int fd, fflags;

	/*
	 * Cray Specific - Cray has a Disk Watcher Daemon that monitors
	 * every storage device in the system and will provide telemetry
	 * that we can consume to get early warning of e.g. drive failures.
	 * Spawn off a thread to monitor the telemetry from the Cray DWD.
	 */
	if (cray_dwd_watcher_init() < 0) {
		zed_log_msg(LOG_WARNING, "dwd watcher init failed");
		return (-1);
	}

	if ((g_udev = udev_new()) == NULL) {
		zed_log_msg(LOG_WARNING, "udev_new failed (%d)", errno);
		return (-1);
	}

	/* Set up a udev monitor for block devices */
	g_mon = udev_monitor_new_from_netlink(g_udev, "udev");
	udev_monitor_filter_add_match_subsystem_devtype(g_mon, "block", "disk");
	udev_monitor_filter_add_match_subsystem_devtype(g_mon, "block",
	    "partition");
	udev_monitor_enable_receiving(g_mon);

	/* Make sure monitoring socket is blocking */
	fd = udev_monitor_get_fd(g_mon);
	if ((fflags = fcntl(fd, F_GETFL)) & O_NONBLOCK)
		(void) fcntl(fd, F_SETFL, fflags & ~O_NONBLOCK);

	/* spawn a thread to monitor events */
	if (pthread_create(&g_mon_tid, NULL, zed_udev_monitor, g_mon) != 0) {
		udev_monitor_unref(g_mon);
		udev_unref(g_udev);
		zed_log_msg(LOG_WARNING, "pthread_create failed");
		return (-1);
	}

	zed_log_msg(LOG_INFO, "zed_disk_event_init");

	return (0);
}

void
zed_disk_event_fini()
{
	/* cancel monitor thread at recvmsg() */
	(void) pthread_cancel(g_mon_tid);
	(void) pthread_join(g_mon_tid, NULL);

	/* cleanup udev resources */
	udev_monitor_unref(g_mon);
	udev_unref(g_udev);

	zed_log_msg(LOG_INFO, "zed_disk_event_fini");
}

#else

#include "zed_disk_event.h"

int
zed_disk_event_init()
{
	return (0);
}

void
zed_disk_event_fini()
{
}

#endif /* HAVE_LIBUDEV */
