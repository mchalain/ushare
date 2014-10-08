/*
 * ushare.c : GeeXboX uShare UPnP Media Server.
 * Originally developped for the GeeXboX project.
 * Parts of the code are originated from GMediaServer from Oskar Liljeblad.
 * Copyright (C) 2005-2007 Benjamin Zores <ben@geexbox.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#if (defined(BSD) || defined(__FreeBSD__) || defined(__APPLE__))
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <net/if_dl.h>
#endif

#if (defined(__APPLE__))
#include <net/route.h>
#endif

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <fcntl.h>

#ifdef HAVE_IFADDRS_H
#include <ifaddrs.h>
#endif

#if (defined(__unix__) || defined(unix)) && !defined(USG)
#include <sys/param.h>
#endif

#include "config.h"

#if (defined(HAVE_SETLOCALE) && defined(CONFIG_NLS))
# include <locale.h>
#endif

#include "ushare.h"
#include "metadata.h"
#include "util_iconv.h"
#include "content.h"
#include "cfgparser.h"
#include "gettext.h"
#include "trace.h"
#include "buffer.h"
#include "ctrl_telnet.h"
#ifdef HAVE_FAM
#include "ufam.h"
#endif /* HAVE_FAM */

#include <upnp/ffmpeg_profiler.h>
#include <upnp/mpg123_profiler.h>

ushare_t *ut = NULL;

static ushare_t *
ushare_new (void)
{
  ushare_t *ut = malloc (sizeof (ushare_t));
  if (!ut)
    return NULL;

  ut->name = strdup (DEFAULT_USHARE_NAME);
  ut->interface = strdup (DEFAULT_USHARE_IFACE);
  ut->model_name = strdup (DEFAULT_USHARE_NAME);
  ut->contentlist = NULL;
  ut->init = 0;
  ut->udn = NULL;
  ut->port = 0; /* Randomly attributed by libupnp */
  ut->telnet_port = CTRL_TELNET_PORT;
  ut->presentation = NULL;
  ut->use_presentation = true;
  ut->use_telnet = true;
  ut->dlna = NULL;
  ut->dlna_flags = DLNA_ORG_FLAG_STREAMING_TRANSFER_MODE |
                   DLNA_ORG_FLAG_BACKGROUND_TRANSFERT_MODE |
                   DLNA_ORG_FLAG_CONNECTION_STALL |
                   DLNA_ORG_FLAG_DLNA_V15;
  ut->caps = DLNA_CAPABILITY_UPNP_AV;
  ut->verbose = false;
  ut->daemon = false;
  ut->override_iconv_err = false;
  ut->cfg_file = NULL;
#ifdef HAVE_FAM
  ut->ufam = ufam_init ();
#endif /* HAVE_FAM */

  pthread_mutex_init (&ut->termination_mutex, NULL);
  pthread_cond_init (&ut->termination_cond, NULL);

  return ut;
}

static void
ushare_free (ushare_t *ut)
{
  if (!ut)
    return;

  if (ut->name)
    free (ut->name);
  if (ut->interface)
    free (ut->interface);
  if (ut->model_name)
    free (ut->model_name);
  if (ut->contentlist)
    content_free (ut->contentlist);
  if (ut->udn)
    free (ut->udn);
  if (ut->presentation)
    buffer_free (ut->presentation);
  if (ut->dlna)
    dlna_uninit (ut->dlna);
  ut->dlna = NULL;
  if (ut->cfg_file)
    free (ut->cfg_file);

#ifdef HAVE_FAM
  if (ut->ufam)
    ufam_free (ut->ufam);
#endif /* HAVE_FAM */

  pthread_cond_destroy (&ut->termination_cond);
  pthread_mutex_destroy (&ut->termination_mutex);

  free (ut);
}

static void
ushare_signal_exit (void)
{
  pthread_mutex_lock (&ut->termination_mutex);
  pthread_cond_signal (&ut->termination_cond);
  pthread_mutex_unlock (&ut->termination_mutex);
}

static int
finish_upnp (ushare_t *ut)
{
  if (!ut)
    return -1;

  log_info (_("Stopping UPnP Service ...\n"));
#ifdef HAVE_FAM
  ufam_stop (ut->ufam);
#endif /* HAVE_FAM */
  dlna_stop (ut->dlna);

  return 0;
}

static int
init_upnp (ushare_t *ut)
{
  int res;
#ifdef WEB_INT
  extern dlna_http_callback_t ushare_http_callbacks;
#endif
  dlna_org_flags_t flags;
  
  if (!ut || !ut->name || !ut->udn || !ut->dlna || !ut->vfs)
    return -1;

  flags = DLNA_ORG_FLAG_STREAMING_TRANSFER_MODE |
          DLNA_ORG_FLAG_BACKGROUND_TRANSFERT_MODE |
          DLNA_ORG_FLAG_CONNECTION_STALL |
          DLNA_ORG_FLAG_DLNA_V15;
  
  dlna_set_verbosity (ut->dlna, ut->verbose ? 1 : 0);
  dlna_set_extension_check (ut->dlna, 0);
  
  dlna_set_capability_mode (ut->dlna, ut->caps);
  if (ut->caps &= DLNA_CAPABILITY_DLNA)
    log_info (_("Starting in DLNA compliant profile ...\n"));

  dlna_set_interface (ut->dlna, ut->interface);
  dlna_set_port (ut->dlna, ut->port);
  
  log_info (_("Initializing UPnP subsystem ...\n"));

  dlna_vfs_set_mode (ut->vfs, flags);
  dlna_vfs_add_protocol (ut->vfs, http_protocol_new (ut->dlna));

  /* set some UPnP device properties */
  dlna_device_t *device;

  device = dlna_device_new (ut->caps);
  dlna_device_set_type (device, DLNA_DEVICE_TYPE_DMS,"DMS");
  dlna_device_set_friendly_name (device, ut->name);
  dlna_device_set_manufacturer (device, "GeeXboX Team");
  dlna_device_set_manufacturer_url (device, "http://ushare.geexbox.org/");
  dlna_device_set_model_description (device, "uShare : DLNA Media Server");
  dlna_device_set_model_name (device, ut->model_name);
  dlna_device_set_model_number (device, "001");
  dlna_device_set_model_url (device, "http://ushare.geexbox.org/");
  dlna_device_set_serial_number (device, "USHARE-01");
  dlna_device_set_uuid (device, ut->udn);
#ifdef WEB_INT
  dlna_device_set_presentation_url (device, "ushare.html", &ushare_http_callbacks);
#endif

  /* set default default service */
  dlna_service_register (device, cms_service_new(ut->dlna));
  dlna_service_register (device, cds_service_new(ut->dlna, ut->vfs));
  if (ut->caps &= DLNA_CAPABILITY_UPNP_AV_XBOX)
  {
    log_info (_("Starting in XboX 360 compliant profile ...\n"));
    dlna_service_register (device, msr_service_new(ut->dlna));
  }

  dlna_set_device (ut->dlna, device);

  build_metadata_list (ut);

  res = dlna_start (ut->dlna);
  if (res != DLNA_ST_OK)
  {
    log_error (_("Cannot initialize UPnP subsystem\n"));
    return -1;
  }

  log_info (_("Listening for control point connections ...\n"));

#ifdef HAVE_FAM
  ufam_start (ut);
#endif /* HAVE_FAM */

  return 0;
}

static bool
has_iface (char *interface)
{
#ifdef HAVE_IFADDRS_H
  struct ifaddrs *itflist, *itf;

  if (!interface)
    return false;

  if (getifaddrs (&itflist) < 0)
  {
    perror ("getifaddrs");
    return false;
  }

  itf = itflist;
  while (itf)
  {
    if ((itf->ifa_flags & IFF_UP)
        && !strncmp (itf->ifa_name, interface, IFNAMSIZ))
    {
      freeifaddrs (itflist);
      return true;
    }
    itf = itf->ifa_next;
  }

  freeifaddrs (itflist);
#else  
  int sock, i, n;
  struct ifconf ifc;
  struct ifreq ifr;
  char buff[8192];

  if (!interface)
    return false;

  /* determine UDN according to MAC address */
  sock = socket (AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
  {
    perror ("socket");
    return false;
  }

  /* get list of available interfaces */
  ifc.ifc_len = sizeof (buff);
  ifc.ifc_buf = buff;

  if (ioctl (sock, SIOCGIFCONF, &ifc) < 0)
  {
    perror ("ioctl");
    close (sock);
    return false;
  }

  n = ifc.ifc_len / sizeof (struct ifreq);
  for (i = n - 1 ; i >= 0 ; i--)
  {
    ifr = ifc.ifc_req[i];

    if (strncmp (ifr.ifr_name, interface, IFNAMSIZ))
      continue;

    if (ioctl (sock, SIOCGIFFLAGS, &ifr) < 0)
    {
      perror ("ioctl");
      close (sock);
      return false;
    }

    if (!(ifr.ifr_flags & IFF_UP))
    {
      /* interface is down */
      log_error (_("Interface %s is down.\n"), interface);
      log_error (_("Recheck uShare's configuration and try again !\n"));
      close (sock);
      return false;
    }

    /* found right interface */
    close (sock);
    return true;
  }
  close (sock);
#endif
  
  log_error (_("Can't find interface %s.\n"),interface);
  log_error (_("Recheck uShare's configuration and try again !\n"));

  return false;
}

static char *
create_udn (char *interface)
{
  int sock = -1;
  char *buf;
  unsigned char *ptr;

#if (defined(BSD) || defined(__FreeBSD__) || defined(__APPLE__))
  int mib[6];
  size_t len;
  struct if_msghdr *ifm;
  struct sockaddr_dl *sdl;
#else /* Linux */
  struct ifreq ifr;
#endif

  if (!interface)
    return NULL;

#if (defined(BSD) || defined(__FreeBSD__) || defined(__APPLE__))
  mib[0] = CTL_NET;
  mib[1] = AF_ROUTE;
  mib[2] = 0;
  mib[3] = AF_LINK;
  mib[4] = NET_RT_IFLIST;

  mib[5] = if_nametoindex (interface);
  if (mib[5] == 0)
  {
    perror ("if_nametoindex");
    return NULL;
  }

  if (sysctl (mib, 6, NULL, &len, NULL, 0) < 0)
  {
    perror ("sysctl");
    return NULL;
  }

  buf = malloc (len);
  if (sysctl (mib, 6, buf, &len, NULL, 0) < 0)
  {
    perror ("sysctl");
    return NULL;
  }

  ifm = (struct if_msghdr *) buf;
  sdl = (struct sockaddr_dl*) (ifm + 1);
  ptr = (unsigned char *) LLADDR (sdl);
#else /* Linux */
  /* determine UDN according to MAC address */
  sock = socket (AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
  {
    perror ("socket");
    return NULL;
  }

  strcpy (ifr.ifr_name, interface);
  strcpy (ifr.ifr_hwaddr.sa_data, "");

  if (ioctl (sock, SIOCGIFHWADDR, &ifr) < 0)
  {
    perror ("ioctl");
    return NULL;
  }

  buf = (char *) malloc (64 * sizeof (char));
  memset (buf, 0, 64);
  ptr = (unsigned char *) ifr.ifr_hwaddr.sa_data;
#endif /* (defined(BSD) || defined(__FreeBSD__)) */

  snprintf (buf, 64, "%s-%02x%02x%02x%02x%02x%02x", DEFAULT_UUID,
            (ptr[0] & 0377), (ptr[1] & 0377), (ptr[2] & 0377),
            (ptr[3] & 0377), (ptr[4] & 0377), (ptr[5] & 0377));

  if (sock)
    close (sock);

  return buf;
}

static int
restart_upnp (ushare_t *ut)
{
  finish_upnp (ut);

  if (ut->udn)
    free (ut->udn);
  ut->udn = create_udn (ut->interface);
  if (!ut->udn)
    return -1;

  return (init_upnp (ut));
}

static void
UPnPBreak (int s __attribute__ ((unused)))
{
  ushare_signal_exit ();
}

static void
reload_config (int s __attribute__ ((unused)))
{
  ushare_t *ut2;
  bool reload = false;

  log_info (_("Reloading configuration...\n"));

  ut2 = ushare_new ();
  if (!ut || !ut2)
    return;

  if (parse_config_file (ut2) < 0)
    return;

  if (ut->name && strcmp (ut->name, ut2->name))
  {
    free (ut->name);
    ut->name = ut2->name;
    ut2->name = NULL;
    reload = true;
  }

  if (ut->interface && strcmp (ut->interface, ut2->interface))
  {
    if (!has_iface (ut2->interface))
    {
      ushare_free (ut2);
      raise (SIGINT);
    }
    else
    {
      free (ut->interface);
      ut->interface = ut2->interface;
      ut2->interface = NULL;
      reload = true;
    }
  }

  if (ut->port != ut2->port)
  {
    ut->port = ut2->port;
    reload = true;
  }

  if (reload)
  {
    if (restart_upnp (ut) < 0)
    {
      ushare_free (ut2);
      raise (SIGINT);
    }
  }

  if (ut->contentlist)
    content_free (ut->contentlist);
  ut->contentlist = ut2->contentlist;
  ut2->contentlist = NULL;
  ushare_free (ut2);

  if (ut->contentlist)
  {
    free_metadata_list (ut);
    build_metadata_list (ut);
  }
  else
  {
    log_error (_("Error: no content directory to be shared.\n"));
    raise (SIGINT);
  }
}

inline void
display_headers (void)
{
  printf (_("%s (version %s), a lightweight UPnP A/V and DLNA Media Server.\n"),
          PACKAGE_NAME, VERSION);
  printf (_("Benjamin Zores (C) 2005-2007, for GeeXboX Team.\n"));
  printf (_("Marc Chalain (C) 2014-2016.\n"));
  printf (_("See http://ushare.geexbox.org/ for updates.\n"));
  printf (_("See http://github.com/mchalain/ushare for updates.\n"));
}

inline static void
setup_i18n(void)
{
#ifdef CONFIG_NLS
#ifdef HAVE_SETLOCALE
  setlocale (LC_ALL, "");
#endif
#if (!defined(BSD) && !defined(__FreeBSD__))
  bindtextdomain (PACKAGE, LOCALEDIR);
#endif
  textdomain (PACKAGE);
#endif
}

#define SHUTDOWN_MSG _("Server is shutting down: other clients will be notified soon, Bye bye ...\n")

static void
ushare_kill (ctrl_telnet_client_t *client,
             int argc __attribute__((unused)),
             char **argv __attribute__((unused)))
{
  if (ut->use_telnet)
  {
    ctrl_telnet_client_send (client, SHUTDOWN_MSG);
    client->exiting = true;
  }
  ushare_signal_exit ();
}

int
main (int argc, char **argv)
{
  const dlna_profiler_t *profiler;

  ut = ushare_new ();
  if (!ut)
    return EXIT_FAILURE;

  setup_i18n ();
  setup_iconv ();

  ut->dlna = dlna_init ();
  ut->vfs = dlna_vfs_new (ut->dlna);

  profiler = &mpg123_profiler;
  dlna_add_profiler (ut->dlna, profiler);
//  profiler = &ffmpeg_profiler;
//  dlna_add_profiler (ut->dlna, profiler);

  /* Parse args before cfg file, as we may override the default file */
  if (parse_command_line (ut, argc, argv) < 0)
  {
    dlna_uninit (ut->dlna);
    ushare_free (ut);
    return EXIT_SUCCESS;
  }

  if (parse_config_file (ut) < 0)
  {
    /* fprintf here, because syslog not yet ready */
    fprintf (stderr, _("Warning: can't parse file \"%s\".\n"),
             ut->cfg_file ? ut->cfg_file : SYSCONFDIR "/" USHARE_CONFIG_FILE);
  }

  if (ut->daemon)
  {
    /* starting syslog feature as soon as possible */
    start_log ();
  }

  if (!ut->contentlist)
  {
    log_error (_("Error: no content directory to be shared.\n"));
    dlna_uninit (ut->dlna);
    ushare_free (ut);
    return EXIT_FAILURE;
  }

  if (!has_iface (ut->interface))
  {
    dlna_uninit (ut->dlna);
    ushare_free (ut);
    return EXIT_FAILURE;
  }

  ut->udn = create_udn (ut->interface);
  if (!ut->udn)
  {
    dlna_uninit (ut->dlna);
    ushare_free (ut);
    return EXIT_FAILURE;
  }

  if (ut->daemon)
  {
    int err;
    err = daemon (0, 0);
    if (err == -1)
    {
      log_error (_("Error: failed to daemonize program : %s\n"),
                 strerror (err));
      dlna_uninit (ut->dlna);
      ushare_free (ut);
      return EXIT_FAILURE;
    }
  }
  else
  {
    display_headers ();
  }

  signal (SIGINT, UPnPBreak);
  signal (SIGHUP, reload_config);

  if (ut->use_telnet)
  {
    if (ctrl_telnet_start (ut->telnet_port) < 0)
    {
      dlna_uninit (ut->dlna);
      ushare_free (ut);
      return EXIT_FAILURE;
    }
    
    ctrl_telnet_register ("kill", ushare_kill,
                          _("Terminates the uShare server"));
  }
  
  if (init_upnp (ut) < 0)
  {
    finish_upnp (ut);
    dlna_uninit (ut->dlna);
    ushare_free (ut);
    return EXIT_FAILURE;
  }

  /* Let main sleep until it's time to die... */
  pthread_mutex_lock (&ut->termination_mutex);
  pthread_cond_wait (&ut->termination_cond, &ut->termination_mutex);
  pthread_mutex_unlock (&ut->termination_mutex);

  if (ut->use_telnet)
    ctrl_telnet_stop ();
  finish_upnp (ut);
  dlna_uninit (ut->dlna);
  ushare_free (ut);
  finish_iconv ();

  /* it should never be executed */
  return EXIT_SUCCESS;
}
