/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * ---------------------------------------
 */

/**
 * \file    nfs_stats_thread.c
 * \author  $Author: deniel $
 * \date    $Date: 2006/02/22 12:01:58 $
 * \version $Revision: 1.6 $
 * \brief   The file that contain the 'stats_thread' routine for the nfsd.
 *
 * nfs_stats_thread.c : The file that contain the 'stats_thread' routine for the nfsd.
 *
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include "nfs_core.h"
#include "nfs_stat.h"
#include "nfs_tools.h"
#include "nfs_exports.h"
#include "fsal.h"
#include "ganesha_rpc.h"
#include "abstract_mem.h"

#define DEFAULT_PORT "10401"

#define BACKLOG 10

#define  CONF_STAT_EXPORTER_LABEL  "STAT_EXPORTER"
#define STRCMP   strcasecmp

/* Make sure this is <= the same macro in support/exports.c */
#define EXPORT_MAX_CLIENTS 20

int stat_export_check_access(sockaddr_t                * hostaddr,
                             exportlist_client_t       * clients,
                             exportlist_client_entry_t * pclient_found)
{
  sockaddr_t alt_hostaddr;
  sockaddr_t * puse_hostaddr;

  /* For now, no matching client is found */
  memset(pclient_found, 0, sizeof(exportlist_client_entry_t));

  puse_hostaddr = check_convert_ipv6_to_ipv4(hostaddr, &alt_hostaddr);

  if(isFullDebug(COMPONENT_MAIN))
    {
      char ipstring[SOCK_NAME_MAX];
      int ipvalid = sprint_sockip(puse_hostaddr, ipstring, sizeof(ipstring));

      if(ipvalid)
        LogFullDebug(COMPONENT_MAIN, "Check for address %s", ipstring);
    }

  return export_client_match_any(puse_hostaddr,
                                 clients,
                                 pclient_found,
                                 EXPORT_OPTION_RW_ACCESS);

}                               /* stat_export_check_access */

int get_stat_exporter_conf(config_file_t in_config, external_tools_parameter_t * out_parameter)
{
  int err;
  int var_max, var_index;
  char *key_name;
  char *key_value;
  config_item_t block;
  config_item_t item;

  if(strmaxcpy(out_parameter->stat_export.export_stat_port,
               DEFAULT_PORT,
               sizeof(out_parameter->stat_export.export_stat_port)) == -1)
    {
      LogFatal(COMPONENT_CONFIG,
               "Default export stat port %s too long",
               DEFAULT_PORT);
    }

   /* Get the config BLOCK */
 if((block = config_FindItemByName(in_config, CONF_STAT_EXPORTER_LABEL)) == NULL)
    {
      /* cannot read item */
      LogCrit(COMPONENT_CONFIG,
              "STAT_EXPORTER: Cannot read item \"%s\" from configuration file",
              CONF_STAT_EXPORTER_LABEL);
      /* Expected to be a block */
      return ENOENT;
    }
  else if(config_ItemType(block) != CONFIG_ITEM_BLOCK)
     {
       LogCrit(COMPONENT_CONFIG,
               "STAT_EXPORTER: Cannot read item \"%s\" from configuration file",
               CONF_STAT_EXPORTER_LABEL);
      /* Expected to be a block */
       return ENOENT;
     }

  /* makes an iteration on the (key, value) couplets */
  var_max = config_GetNbItems(block);

  for(var_index = 0; var_index < var_max; var_index++)
    {
       /* retrieve key's name */
      item = config_GetItemByIndex(block, var_index);
      err = config_GetKeyValue(item, &key_name, &key_value);

      if(err)
        {
          LogCrit(COMPONENT_CONFIG,
                  "STAT_EXPORTER: ERROR reading key[%d] from section \"%s\" of configuration file.",
                  var_index, CONF_LABEL_FS_SPECIFIC);
          return err;
        }

      if(!STRCMP(key_name, "Access"))
        {
	  parseAccessParam(key_name,
	                   key_value,
	                   &(out_parameter->stat_export.allowed_clients),
	                   EXPORT_OPTION_READ_ACCESS | EXPORT_OPTION_WRITE_ACCESS,
	                   CONF_STAT_EXPORTER_LABEL);
        }
      else if(!STRCMP(key_name, "Port"))
        {
          if(strmaxcpy(out_parameter->stat_export.export_stat_port,
                       key_value,
                       sizeof(out_parameter->stat_export.export_stat_port)) == -1)
            {
              LogCrit(COMPONENT_CONFIG,
                      "%s %s=\"%s\" too long",
                      CONF_STAT_EXPORTER_LABEL, key_name, key_value);
            }
        }
      else
        {
          LogCrit(COMPONENT_CONFIG,
                  "STAT_EXPORTER LOAD PARAMETER: ERROR: Unknown or unsettable key: %s (item %s)",
                  key_name, CONF_LABEL_FS_SPECIFIC);
          return EINVAL;
        }
    }
  return 0;
}

void merge_stats(nfs_request_stat_item_t  * global_stat_items,
                 nfs_request_stat_item_t ** workers_stat_items,
                 int                        function_index,
                 int                        detail_flag)
{
  unsigned int i = 0;

  for(i = 0; i < nfs_param.core_param.nb_worker; i++)
    {
      if(i == 0)
        {
          global_stat_items[function_index].total =
              workers_stat_items[i][function_index].total;
          global_stat_items[function_index].success =
              workers_stat_items[i][function_index].success;
          global_stat_items[function_index].dropped =
              workers_stat_items[i][function_index].dropped;
          global_stat_items[function_index].tot_latency =
              workers_stat_items[i][function_index].tot_latency;
          global_stat_items[function_index].min_latency =
              workers_stat_items[i][function_index].min_latency;
          global_stat_items[function_index].max_latency =
              workers_stat_items[i][function_index].max_latency;
#ifdef _USE_QUEUE_TIMER
          if(detail_flag)
            {
              global_stat_items[function_index].tot_await_time =
                  workers_stat_items[i][function_index].tot_await_time;
            }
#endif
        }
      else
        {
          global_stat_items[function_index].total +=
              workers_stat_items[i][function_index].total;
          global_stat_items[function_index].success +=
              workers_stat_items[i][function_index].success;
          global_stat_items[function_index].dropped +=
              workers_stat_items[i][function_index].dropped;
          global_stat_items[function_index].tot_latency +=
              workers_stat_items[i][function_index].tot_latency;
          if(workers_stat_items[i][function_index].total != 0)
            {
              set_min_latency(&(global_stat_items[function_index].min_latency),
                  workers_stat_items[i][function_index].min_latency);
              set_max_latency(&(global_stat_items[function_index].max_latency),
                  workers_stat_items[i][function_index].max_latency);
            }
#ifdef _USE_QUEUE_TIMER
          if(detail_flag)
            {
              global_stat_items[function_index].tot_await_time +=
                  workers_stat_items[i][function_index].tot_await_time;
            }
#endif
        }
    }
}

void write_stats(struct display_buffer   * dspbuf,
                 int                       num_cmds,
                 char                   ** function_names,
                 nfs_request_stat_item_t * global_stat_items,
                 int                       detail_flag)
{
  unsigned int   i = 0;
  char         * name = NULL;
  char         * ver __attribute__((unused)) = NULL;
  char         * call = NULL;
  char         * saveptr = NULL;
  int            b_left = display_start(dspbuf);

  for(i = 0; i < num_cmds && b_left > 0; i++)
    {
      /* Extract call name from function name. */
      name = gsh_strdup(function_names[i]);
      ver = strtok_r(name, "_", &saveptr);
      call = strtok_r(NULL, "_", &saveptr);

#ifdef _USE_QUEUE_TIMER
      if(detail_flag)
        b_left = display_printf(dspbuf,
                                "_%s_ %u %"PRIu64" %"PRIu64,
                                call,
                                global_stat_items[i].total,
                                global_stat_items[i].tot_latency,
                                global_stat_items[i].tot_await_time);
      else
#endif
        b_left = display_printf(dspbuf,
                                "_%s_ %u %"PRIu64,
                                call,
                                global_stat_items[i].total,
                                global_stat_items[i].tot_latency);

      if(i != num_cmds - 1 && b_left > 0)
        b_left = display_cat(dspbuf, " ");

      gsh_free(name);
    }
}

void merge_nfs_stats_by_share(struct display_buffer * dspbuf,
                              nfs_stat_client_req_t * stat_client_req,
                              nfs_worker_stat_t     * global_data,
                              nfs_worker_stat_t     * workers_stat)
{
  unsigned int i = 0;
  unsigned int num_cmds = 0;
  nfs_request_stat_item_t *global_stat_items = NULL;
  nfs_request_stat_item_t *workers_stat_items[nfs_param.core_param.nb_worker];
  char **function_names = NULL;

  switch(stat_client_req->nfs_version)
    {
      case 2:
        num_cmds = NFS_V2_NB_COMMAND;
        global_stat_items = (global_data->stat_req.stat_req_nfs2);
        for(i = 0; i < nfs_param.core_param.nb_worker; i++)
          {
            workers_stat_items[i] = (workers_stat[i].stat_req.stat_req_nfs2);
          }
        function_names = nfsv2_function_names;
      break;

      case 3:
        num_cmds = NFS_V3_NB_COMMAND;
        global_stat_items = (global_data->stat_req.stat_req_nfs3);
        for(i = 0; i < nfs_param.core_param.nb_worker; i++)
          {
            workers_stat_items[i] = (workers_stat[i].stat_req.stat_req_nfs3);
          }
        function_names = nfsv3_function_names;
      break;

      case 4:
        num_cmds = NFS_V4_NB_COMMAND;
        global_stat_items = (global_data->stat_req.stat_req_nfs4);
        for(i = 0; i < nfs_param.core_param.nb_worker; i++)
          {
            workers_stat_items[i] = (workers_stat[i].stat_req.stat_req_nfs4);
          }
        function_names = nfsv4_function_names;
      break;

      default:
        // TODO: Invalid NFS version handling
        LogCrit(COMPONENT_MAIN, "Error: Invalid NFS version.");
      break;
    }

  switch(stat_client_req->stat_type)
    {
      case PER_SERVER:
      case PER_SHARE:
        for(i = 0; i < num_cmds; i++)
          merge_stats(global_stat_items, workers_stat_items, i, 0);

        write_stats(dspbuf, num_cmds, function_names, global_stat_items, 0);
      break;

      case PER_SERVER_DETAIL:
      case PER_SHARE_DETAIL:
        for(i = 0; i < num_cmds; i++)
          merge_stats(global_stat_items, workers_stat_items, i, 1);

        write_stats(dspbuf, num_cmds, function_names, global_stat_items, 1);
      break;

      case PER_CLIENT:
      break;

      case PER_CLIENTSHARE:
      break;

      default:
        // TODO: Invalid stat type handling
        LogCrit(COMPONENT_MAIN, "Error: Invalid stat type.");
      break;
        }
}


void merge_nfs_stats(struct display_buffer * dspbuf,
                     nfs_stat_client_req_t * stat_client_req,
                     nfs_worker_stat_t     * global_data,
                     nfs_worker_data_t     * workers_data)
{
  unsigned int i = 0;
  unsigned int num_cmds = 0;
  nfs_request_stat_item_t *global_stat_items = NULL;
  nfs_request_stat_item_t *workers_stat_items[nfs_param.core_param.nb_worker];
  char **function_names = NULL;

  switch(stat_client_req->nfs_version)
    {
      case 2:
        num_cmds = NFS_V2_NB_COMMAND;
        global_stat_items = (global_data->stat_req.stat_req_nfs2);
        for(i = 0; i < nfs_param.core_param.nb_worker; i++)
          {
            workers_stat_items[i] = (workers_data[i].stats.stat_req.stat_req_nfs2);
          }
        function_names = nfsv2_function_names;
      break;

      case 3:
        num_cmds = NFS_V3_NB_COMMAND;
        global_stat_items = (global_data->stat_req.stat_req_nfs3);
        for(i = 0; i < nfs_param.core_param.nb_worker; i++)
          {
            workers_stat_items[i] = (workers_data[i].stats.stat_req.stat_req_nfs3);
          }
        function_names = nfsv3_function_names;
      break;

      case 4:
        num_cmds = NFS_V4_NB_COMMAND;
        global_stat_items = (global_data->stat_req.stat_req_nfs4);
        for(i = 0; i < nfs_param.core_param.nb_worker; i++)
          {
            workers_stat_items[i] = (workers_data[i].stats.stat_req.stat_req_nfs4);
          }
        function_names = nfsv4_function_names;
      break;

      default:
        // TODO: Invalid NFS version handling
        LogCrit(COMPONENT_MAIN, "Error: Invalid NFS version.");
      break;
    }

  switch(stat_client_req->stat_type)
    {
      case PER_SERVER:
      case PER_SHARE:
        for(i = 0; i < num_cmds; i++)
          merge_stats(global_stat_items, workers_stat_items, i, 0);

        write_stats(dspbuf, num_cmds, function_names, global_stat_items, 0);
      break;

      case PER_SERVER_DETAIL:
      case PER_SHARE_DETAIL:
        for(i = 0; i < num_cmds; i++)
          merge_stats(global_stat_items, workers_stat_items, i, 1);

        write_stats(dspbuf, num_cmds, function_names, global_stat_items, 1);
      break;

      case PER_CLIENT:
      break;

      case PER_CLIENTSHARE:
      break;

      default:
        // TODO: Invalid stat type handling
        LogCrit(COMPONENT_MAIN, "Error: Invalid stat type.");
      break;
        }
}

int process_stat_request(int new_fd)
{
  int                     rc = ERR_STAT_NO_ERROR;
  char                    cmd_buf[4096];
  char                    stat_buf[4096];
  struct display_buffer   dspbuf = {sizeof(stat_buf), stat_buf, stat_buf};
  char                  * token = NULL;
  char                  * key = NULL;
  char                  * value = NULL;
  char                  * saveptr1 = NULL;
  char                  * saveptr2 = NULL;

  exportlist_t *pexport = NULL;

  nfs_worker_stat_t global_worker_stat;
  nfs_stat_client_req_t stat_client_req;
  memset(&stat_client_req, 0, sizeof(nfs_stat_client_req_t));
  memset(cmd_buf, 0, sizeof(cmd_buf));

  if((rc = recv(new_fd, cmd_buf, 4096, 0)) == -1)
    {
      LogError(COMPONENT_MAIN, ERR_SYS, errno, rc);
      return rc;
    }

  /* Parse command options. */
  token = strtok_r(cmd_buf, ",", &saveptr1);
  while(token != NULL)
  {
    key = strtok_r(token, "=", &saveptr2);
    value = strtok_r(NULL, "=", &saveptr2);

    if(key != NULL && value != NULL)
    {
      if(strcmp(key, "version") == 0)
      {
        stat_client_req.nfs_version = atoi(value);
      }
      else if(strcmp(key, "type") == 0)
      {
        if(strcmp(value, "all") == 0)
          {
            stat_client_req.stat_type = PER_SERVER;
          }
        else if(strcmp(value, "all_detail") == 0)
          {
            stat_client_req.stat_type = PER_SERVER_DETAIL;
          }
        else if(strcmp(value, "share") == 0)
          {
            stat_client_req.stat_type = PER_SHARE;
          }
        else if(strcmp(value, "share_detail") == 0)
          {
            stat_client_req.stat_type = PER_SHARE_DETAIL;
          }
      }
      else if(strcmp(key, "path") == 0)
        {
          if(strmaxcpy(stat_client_req.share_path,
                       value,
                       sizeof(stat_client_req.share_path)) == -1)
            {
              LogCrit(COMPONENT_CONFIG,
                      "%s=\"%s\" too long",
                      key, value);
            }
      }
    }

    token = strtok_r(NULL, ",", &saveptr1);
  }

  memset(stat_buf, 0, sizeof(stat_buf));

  if(stat_client_req.stat_type == PER_SHARE ||
     stat_client_req.stat_type == PER_SHARE_DETAIL)
    {
      LogDebug(COMPONENT_MAIN, "share path %s",
               stat_client_req.share_path);

      // Get export entry
      pexport = GetExportEntry(stat_client_req.share_path);
      if(!pexport)
        {
          LogEvent(COMPONENT_MAIN, "Invalid export, discard stat request");
          goto exit;
        }
      else
        LogDebug(COMPONENT_MAIN, "Got export entry, pexport %p", pexport);

      merge_nfs_stats_by_share(&dspbuf, &stat_client_req, &global_worker_stat,
                               pexport->worker_stats);
    }
  else
    {
      merge_nfs_stats(&dspbuf, &stat_client_req, &global_worker_stat,
                      workers_data);
    }

  /** @todo FSF: could that be display_buffer_len(&dspbuf) instead of sizeof(stat_buf)? */
  if((rc = send(new_fd, stat_buf, sizeof(stat_buf), 0)) == -1)
    LogError(COMPONENT_MAIN, ERR_SYS, errno, rc);

exit:
  close(new_fd);

  return rc;
}

int check_permissions() {
  return 0;
}

void *stat_exporter_thread(void *UnusedArg)
{
  int sockfd, new_fd;
  struct addrinfo hints, *servinfo, *p;
  sockaddr_t their_addr;
  socklen_t sin_size;
  int yes = 1;
  char s[INET6_ADDRSTRLEN];
  int rc;
  exportlist_client_entry_t pclient_found;

  SetNameFunction("statistics_exporter");

  memset(&hints, 0, sizeof hints);

#ifndef _USE_TIRPC_IPV6
  hints.ai_family = AF_INET;
#else
  hints.ai_family = AF_INET6;
#endif
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  if((rc = getaddrinfo(NULL, nfs_param.extern_param.stat_export.export_stat_port, &hints, &servinfo)) != 0)
    {
      LogCrit(COMPONENT_MAIN, "getaddrinfo Failed: %s", gai_strerror(rc));
      return NULL;
    }
  for(p = servinfo; p != NULL; p = p->ai_next)
    {
      if((sockfd = socket(p->ai_family, p->ai_socktype,
                          p->ai_protocol)) == -1)
        {
          LogError(COMPONENT_MAIN, ERR_SYS, errno, sockfd);
          continue;
        }

      if((rc = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
                          sizeof(int))) == -1)
        {
          LogError(COMPONENT_MAIN, ERR_SYS, errno, rc);
          freeaddrinfo(servinfo);
          return NULL;
        }

      if((rc = bind(sockfd, p->ai_addr, p->ai_addrlen)) == -1)
        {
          close(sockfd);
          LogError(COMPONENT_MAIN, ERR_SYS, errno, rc);
          continue;
        }

      break;
    }

  if(p == NULL)
    {
      LogCrit(COMPONENT_MAIN, "server: failed to bind");
      freeaddrinfo(servinfo);
      return NULL;
    }

  freeaddrinfo(servinfo);
  if((rc = listen(sockfd, BACKLOG)) == -1)
    {
      LogError(COMPONENT_MAIN, ERR_SYS, errno, rc);
      return NULL;
    }
  LogInfo(COMPONENT_MAIN, "Stat export server: Waiting for connections...");

  while(1)
    {
      sin_size = sizeof their_addr;
      new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
      if(new_fd == -1)
        {
          LogError(COMPONENT_MAIN, ERR_SYS, errno, new_fd);
          continue;
        }

      sprint_sockip((sockaddr_t *)&their_addr, s, sizeof s);

      if (stat_export_check_access(&their_addr,
                                   &(nfs_param.extern_param.stat_export.allowed_clients),
                                   &pclient_found)) {
        LogDebug(COMPONENT_MAIN, "Stat export server: Access granted to %s", s);
        process_stat_request(new_fd);
      } else {
        LogWarn(COMPONENT_MAIN, "Stat export server: Access denied to %s", s);
      }
    }                           /* while ( 1 ) */

  return NULL;
}                               /* stat_exporter_thread */

void *long_processing_thread(void *UnusedArg)
{
  msectimer_t timer_end;
  msectimer_t timer_diff;
  msectimer_t timer_diff_sec;
  int i;

  SetNameFunction("long_processing");

  while(1)
    {
      sleep(1);

      timer_end = timer_get();

      for(i = 0; i < nfs_param.core_param.nb_worker; i++)
        {
          timer_diff = timer_end - atomic_fetch_msectimer_t(&workers_data[i].timer_start);
          timer_diff_sec = timer_diff / MSEC_PER_SEC;

          if(timer_diff_sec == nfs_param.core_param.long_processing_threshold ||
             timer_diff_sec == nfs_param.core_param.long_processing_threshold * 10)
            LogEvent(COMPONENT_DISPATCH,
                     "Worker#%d: Function %s has been running for %lu.%03lu seconds",
                     i, workers_data[i].funcdesc->funcname,
                     timer_diff / MSEC_PER_SEC,
                     timer_diff % MSEC_PER_SEC);
        }
    }

  return NULL;
}                               /* long_processing_thread */