/*
 * Open-MX
 * Copyright © INRIA 2007 (see AUTHORS file)
 *
 * The development of this software has been funded by Myricom, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU Lesser General Public License in COPYING.LGPL for more details.
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "omx_lib.h"

#define OMX_ZOMBIE_MAX_DEFAULT 512

struct omx__globals omx__globals = { 0 };
struct omx_driver_desc * omx__driver_desc = NULL;

omx_return_t
omx__init_api(int api)
{
  omx_return_t ret;
  char *env;
  int err;

  if (omx__globals.initialized)
    return OMX_ALREADY_INITIALIZED;

  err = open(OMX_DEVNAME, O_RDONLY);
  if (err < 0)
#if 0
    return omx__errno_to_return("init open control fd");
#else
    {
      /* FIXME: the standard error handler exits by default */
      fprintf(stderr, "Cannot open Open-MX device: code %d\n", 
              omx__errno_to_return("init open control fd"));
      exit(1);
    }
#endif

  omx__globals.control_fd = err;

  omx__driver_desc = mmap(NULL, OMX_DRIVER_DESC_SIZE, PROT_READ, MAP_SHARED,
			  omx__globals.control_fd, OMX_DRIVER_DESC_FILE_OFFSET);
  if (omx__driver_desc == MAP_FAILED) {
    ret = omx__errno_to_return("mmap driver descriptor");
    goto out_with_fd;
  }

  if (omx__driver_desc->abi_version > OMX_DRIVER_ABI_VERSION) {
    fprintf(stderr, "Library (ABI 0x%x) is too old for driver (ABI 0x%x), did you relink your program with the new library?\n",
	    OMX_DRIVER_ABI_VERSION, omx__driver_desc->abi_version);
    ret = OMX_BAD_ERROR;
    goto out_with_fd;
  } else if (omx__driver_desc->abi_version < OMX_DRIVER_ABI_VERSION) {
    fprintf(stderr, "Driver (ABI 0x%x) is too old for library (ABI 0x%x), did you rebuild/reload the new driver?\n",
	    omx__driver_desc->abi_version, OMX_DRIVER_ABI_VERSION);
    ret = OMX_BAD_ERROR;
    goto out_with_fd;
  }

  omx__globals.ack_delay = omx__ack_jiffies_delay();
  omx__globals.resend_delay = omx__resend_jiffies_delay();
  omx__globals.retransmits_max = 1000;

#ifdef OMX_DEBUG
  omx__globals.verbose = 0;
  env = getenv("OMX_VERBOSE");
#ifdef OMX_MX_API_COMPAT
  if (!env) {
    env = getenv("MX_VERBOSE");
    if (env) {
      printf("Emulating MX_VERBOSE as OMX_VERBOSE=\"\"\n");
      env = "";
    }
  }
#endif
  if (env) {
    char *next;
    unsigned long val = strtoul(env, &next, 0);
    if (env == next) {
      int i;
      val = OMX_VERBOSE_MAIN;
      for(i=0; env[i]!='\0'; i++) {
	switch (env[i]) {
	case 'P': val |= OMX_VERBOSE_ENDPOINT; break;
	case 'C': val |= OMX_VERBOSE_CONNECT; break;
	case 'S': val |= OMX_VERBOSE_SEND; break;
	case 'L': val |= OMX_VERBOSE_LARGE; break;
	case 'M': val |= OMX_VERBOSE_MEDIUM; break;
	case 'Q': val |= OMX_VERBOSE_SEQNUM; break;
	case 'R': val |= OMX_VERBOSE_RECV; break;
	case 'U': val |= OMX_VERBOSE_UNEXP; break;
	case 'E': val |= OMX_VERBOSE_EARLY; break;
	case 'A': val |= OMX_VERBOSE_ACK; break;
	case 'T': val |= OMX_VERBOSE_EVENT; break;
	case 'W': val |= OMX_VERBOSE_WAIT; break;
	case 'V': val |= OMX_VERBOSE_VECT; break;
	default: omx__abort("Unknown verbose character '%c'\n", env[i]);
	}
      }
    }
    omx__globals.verbose = val;
  }
#endif /* OMX_DEBUG */

  /* regcache configuration */
  omx__globals.regcache = 0;
  env = getenv("OMX_RCACHE");
#ifdef OMX_MX_API_COMPAT
  if (!env) {
    env = getenv("MX_RCACHE");
    if (env)
      omx__debug_printf(MAIN, "Emulating MX_RCACHE as OMX_RCACHE\n");
  }
#endif
  if (env) {
    omx__globals.regcache = atoi(env);
    omx__debug_printf(MAIN, "Forcing regcache to %s\n",
		      omx__globals.regcache ? "enabled" : "disabled");
  }

  /* waitspin configuration */
  omx__globals.waitspin = 0;
  env = getenv("OMX_WAITSPIN");
  /* could be enabled by MX_MONOTHREAD */
  if (env) {
    omx__globals.waitspin = atoi(env);
    omx__debug_printf(MAIN, "Forcing waitspin to %s\n",
		      omx__globals.waitspin ? "enabled" : "disabled");
  }

  /* zombie send configuration */
  omx__globals.zombie_max = OMX_ZOMBIE_MAX_DEFAULT;
  env = getenv("OMX_ZOMBIE_SEND");
#ifdef OMX_MX_API_COMPAT
  if (!env) {
    env = getenv("MX_ZOMBIE_SEND");
    if (env)
      omx__debug_printf(MAIN, "Emulating MX_ZOMBIE_SEND as OMX_ZOMBIE_SEND\n");
  }
#endif
  if (env) {
    omx__globals.zombie_max = atoi(env);
    omx__debug_printf(MAIN, "Forcing zombie max to %d\n",
		      omx__globals.zombie_max);
  }

  /* self comm configuration */
#ifndef OMX_DISABLE_SELF
  omx__globals.selfcomms = 1;
  env = getenv("OMX_DISABLE_SELF");
#ifdef OMX_MX_API_COMPAT
  if (!env) {
    env = getenv("MX_DISABLE_SELF");
    if (env)
      omx__debug_printf(MAIN, "Emulating MX_DISABLE_SELF as OMX_DISABLE_SELF\n");
  }
#endif
  if (env) {
    omx__globals.selfcomms = !atoi(env);
    omx__debug_printf(MAIN, "Forcing self comms to %s\n",
		      omx__globals.selfcomms ? "enabled" : "disabled");
  }
#endif

  /* shared comm configuration */
#ifndef OMX_DISABLE_SHARED
  omx__globals.sharedcomms = 1;
  env = getenv("OMX_DISABLE_SHARED");
#ifdef OMX_MX_API_COMPAT
  if (!env) {
    env = getenv("MX_DISABLE_SHMEM");
    if (env)
      omx__debug_printf(MAIN, "Emulating MX_DISABLE_SHMEM as OMX_DISABLE_SHARED\n");
  }
#endif
  if (env) {
    omx__globals.sharedcomms = !atoi(env);
    omx__debug_printf(MAIN, "Forcing shared comms to %s\n",
		      omx__globals.sharedcomms ? "enabled" : "disabled");
  }
#endif

  omx__globals.initialized = 1;
  return OMX_SUCCESS;

 out_with_fd:
  close(omx__globals.control_fd);
  return ret;
}

omx_return_t
omx_finalize(void)
{
  /* FIXME: check that no endpoint is still open */

  close(omx__globals.control_fd);

  omx__globals.initialized = 0;
  return OMX_SUCCESS;
}
