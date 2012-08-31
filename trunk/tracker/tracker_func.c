/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

//tracker_func.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <grp.h>
#include <pwd.h>
#include "fdfs_define.h"
#include "logger.h"
#include "fdfs_global.h"
#include "sockopt.h"
#include "shared_func.h"
#include "ini_file_reader.h"
#include "tracker_types.h"
#include "tracker_proto.h"
#include "tracker_global.h"
#include "tracker_func.h"
#include "tracker_mem.h"
#include "local_ip_func.h"

#ifdef WITH_HTTPD
#include "fdfs_http_shared.h"
#endif

static int tracker_load_store_lookup(const char *filename, \
		IniContext *pItemContext)
{
	char *pGroupName;
	g_groups.store_lookup = iniGetIntValue(NULL, "store_lookup", \
			pItemContext, FDFS_STORE_LOOKUP_ROUND_ROBIN);
	if (g_groups.store_lookup == FDFS_STORE_LOOKUP_ROUND_ROBIN)
	{
		g_groups.store_group[0] = '\0';
		return 0;
	}

	if (g_groups.store_lookup == FDFS_STORE_LOOKUP_LOAD_BALANCE)
	{
		g_groups.store_group[0] = '\0';
		return 0;
	}

	if (g_groups.store_lookup != FDFS_STORE_LOOKUP_SPEC_GROUP)
	{
		logError("file: "__FILE__", line: %d, " \
			"conf file \"%s\", the value of \"store_lookup\" is " \
			"invalid, value=%d!", \
			__LINE__, filename, g_groups.store_lookup);
		return EINVAL;
	}

	pGroupName = iniGetStrValue(NULL, "store_group", pItemContext);
	if (pGroupName == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"conf file \"%s\" must have item " \
			"\"store_group\"!", __LINE__, filename);
		return ENOENT;
	}
	if (pGroupName[0] == '\0')
	{
		logError("file: "__FILE__", line: %d, " \
			"conf file \"%s\", " \
			"store_group is empty!", __LINE__, filename);
		return EINVAL;
	}

	snprintf(g_groups.store_group, sizeof(g_groups.store_group), \
			"%s", pGroupName);
	if (fdfs_validate_group_name(g_groups.store_group) != 0) \
	{
		logError("file: "__FILE__", line: %d, " \
			"conf file \"%s\", " \
			"the group name \"%s\" is invalid!", \
			__LINE__, filename, g_groups.store_group);
		return EINVAL;
	}

	return 0;
}

static int tracker_cmp_group_name_and_ip(const void *p1, const void *p2)
{
	int result;
	result = strcmp(((FDFSStorageIdInfo *)p1)->group_name,
		((FDFSStorageIdInfo *)p2)->group_name);
	if (result != 0)
	{
		return result;
	}

	return ((FDFSStorageIdInfo *)p1)->ip_addr - \
		((FDFSStorageIdInfo *)p2)->ip_addr;
}

FDFSStorageIdInfo *tracker_get_storage_id_info(const char *group_name, \
		const char *pIpAddr)
{
	FDFSStorageIdInfo target;
	memset(&target, 0, sizeof(FDFSStorageIdInfo));
	snprintf(target.group_name, sizeof(target.group_name), "%s", group_name);
	target.ip_addr = getIpaddrByName(pIpAddr, NULL, 0);
	return (FDFSStorageIdInfo *)bsearch(&target, g_storage_ids, \
		g_storage_id_count, sizeof(FDFSStorageIdInfo), \
		tracker_cmp_group_name_and_ip);
}

static int tracker_load_storage_ids(const char *filename, \
		IniContext *pItemContext)
{
	char *pStorageIdsFilename;
	char *content;
	char **lines;
	char *line;
	char *id;
	char *group_name;
	char *pIpAddr;
	FDFSStorageIdInfo *pStorageIdInfo;
	int64_t file_size;
	int alloc_bytes;
	int result;
	int line_count;
	int i;

	g_use_storage_id = iniGetBoolValue(NULL, "use_storage_id", \
				pItemContext, false);
	if (!g_use_storage_id)
	{
		return 0;
	}

	pStorageIdsFilename = iniGetStrValue(NULL, "storage_ids_filename", \
				pItemContext);
	if (pStorageIdsFilename == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"conf file \"%s\" must have item " \
			"\"storage_ids_filename\"!", __LINE__, filename);
		return ENOENT;
	}

	if (*pStorageIdsFilename == '\0')
	{
		logError("file: "__FILE__", line: %d, " \
			"conf file \"%s\", storage_ids_filename is emtpy!", \
			__LINE__, filename);
		return EINVAL;
	}

	if (*pStorageIdsFilename == '/')  //absolute path
	{
		result = getFileContent(pStorageIdsFilename, \
				&content, &file_size);
	}
	else
	{
		const char *lastSlash = strrchr(filename, '/');
		if (lastSlash == NULL)
		{
			result = getFileContent(pStorageIdsFilename, \
					&content, &file_size);
		}
		else
		{
			char filepath[MAX_PATH_SIZE];
			char full_filename[MAX_PATH_SIZE];
			int len;

			len = lastSlash - filename;
			if (len >= sizeof(filepath))
			{
				logError("file: "__FILE__", line: %d, " \
					"conf filename: \"%s\" is too long!", \
					__LINE__, filename);
				return ENOSPC;
			}
			memcpy(filepath, filename, len);
			*(filepath + len) = '\0';
			snprintf(full_filename, sizeof(full_filename), \
				"%s/%s", filepath, pStorageIdsFilename);
			result = getFileContent(full_filename, \
					&content, &file_size);
		}
	}
	if (result != 0)
	{
		return result;
	}

	lines = split(content, '\n', 0, &line_count);
	if (lines == NULL)
	{
		free(content);
		return ENOMEM;
	}

	do
	{
		g_storage_id_count = 0;
		for (i=0; i<line_count; i++)
		{
			trim(lines[i]);
			if (*lines[i] == '\0' || *lines[i] == '#')
			{
				continue;
			}
			g_storage_id_count++;
		}

		if (g_storage_id_count == 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"config file: %s, no storage id!", \
				__LINE__, pStorageIdsFilename);
			result = ENOENT;
			break;
		}

		alloc_bytes = sizeof(FDFSStorageIdInfo) * g_storage_id_count;
		g_storage_ids = (FDFSStorageIdInfo *)malloc(alloc_bytes);
		if (g_storage_ids == NULL)
		{
			result = errno != 0 ? errno : ENOMEM;
			logError("file: "__FILE__", line: %d, " \
				"malloc %d bytes fail, " \
				"errno: %d, error info: %s", __LINE__, \
				alloc_bytes, result, STRERROR(result));
			break;
		}
		memset(g_storage_ids, 0, alloc_bytes);

		pStorageIdInfo = g_storage_ids;
		for (i=0; i<line_count; i++)
		{
			line = lines[i];
			if (*line == '\0' || *line == '#')
			{
				continue;
			}

			id = line;
			group_name = line;
			while (!(*group_name == ' ' || *group_name == '\t' \
				|| *group_name == '\0'))
			{
				group_name++;
			}

			if (*group_name == '\0')
			{
				logError("file: "__FILE__", line: %d, " \
					"config file: %s, line no: %d, " \
					"content: %s, invalid format, " \
					"expect group name and ip address!", \
					__LINE__, pStorageIdsFilename, \
					i + 1, line);
				result = EINVAL;
				break;
			}

			*group_name = '\0';
			group_name++;  //skip space char
			while (*group_name == ' ' || *group_name == '\t')
			{
				group_name++;
			}
		
			pIpAddr = group_name;
			while (!(*pIpAddr == ' ' || *pIpAddr == '\t' \
				|| *pIpAddr == '\0'))
			{
				pIpAddr++;
			}

			if (*pIpAddr == '\0')
			{
				logError("file: "__FILE__", line: %d, " \
					"config file: %s, line no: %d, " \
					"content: %s, invalid format, " \
					"expect ip address!", __LINE__, \
					pStorageIdsFilename, i + 1, line);
				result = EINVAL;
				break;
			}

			*pIpAddr = '\0';
			pIpAddr++;  //skip space char
			while (*pIpAddr == ' ' || *pIpAddr == '\t')
			{
				pIpAddr++;
			}

			pStorageIdInfo->ip_addr = getIpaddrByName( \
							pIpAddr, NULL, 0);
			if (pStorageIdInfo->ip_addr == INADDR_NONE)
			{
				logError("file: "__FILE__", line: %d, " \
					"invalid host name: %s", \
					__LINE__, pIpAddr);
				result = EINVAL;
				break;
			}

			snprintf(pStorageIdInfo->id, \
				sizeof(pStorageIdInfo->id), "%s", id);
			snprintf(pStorageIdInfo->group_name, \
				sizeof(pStorageIdInfo->group_name), \
				"%s", group_name);
			pStorageIdInfo++;
		}
	} while (0);

	free(content);
	freeSplit(lines);

	if (result != 0)
	{
		return result;
	}

	logInfo("g_storage_id_count: %d", g_storage_id_count);
	pStorageIdInfo = g_storage_ids;
	for (i=0; i<g_storage_id_count; i++)
	{
		char szIpAddr[IP_ADDRESS_SIZE];

		logInfo("%s  %s  %s", pStorageIdInfo->id, \
			pStorageIdInfo->group_name, inet_ntop( \
			AF_INET, &pStorageIdInfo->ip_addr, szIpAddr, \
			sizeof(szIpAddr)));

		pStorageIdInfo++;
	}
	
	qsort(g_storage_ids, g_storage_id_count, sizeof(FDFSStorageIdInfo), \
		tracker_cmp_group_name_and_ip);
	return result;
}

int tracker_load_from_conf_file(const char *filename, \
		char *bind_addr, const int addr_size)
{
	char *pBasePath;
	char *pBindAddr;
	char *pStorageReserved;
	char *pRunByGroup;
	char *pRunByUser;
	char *pThreadStackSize;
	char *pSlotMinSize;
	char *pSlotMaxSize;
	char *pSpaceThreshold;
	char *pTrunkFileSize;
#ifdef WITH_HTTPD
	char *pHttpCheckUri;
	char *pHttpCheckType;
#endif
	IniContext iniContext;
	int result;
	int64_t storage_reserved;
	int64_t thread_stack_size;
	int64_t trunk_file_size;
	int64_t slot_min_size;
	int64_t slot_max_size;
	char reserved_space_str[32];

	memset(&g_groups, 0, sizeof(FDFSGroups));
	memset(&iniContext, 0, sizeof(IniContext));
	if ((result=iniLoadFromFile(filename, &iniContext)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"load conf file \"%s\" fail, ret code: %d", \
			__LINE__, filename, result);
		return result;
	}

	//iniPrintItems(&iniContext);

	do
	{
		if (iniGetBoolValue(NULL, "disabled", &iniContext, false))
		{
			logError("file: "__FILE__", line: %d, " \
				"conf file \"%s\" disabled=true, exit", \
				__LINE__, filename);
			result = ECANCELED;
			break;
		}

		pBasePath = iniGetStrValue(NULL, "base_path", &iniContext);
		if (pBasePath == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"conf file \"%s\" must have item " \
				"\"base_path\"!", __LINE__, filename);
			result = ENOENT;
			break;
		}

		snprintf(g_fdfs_base_path, sizeof(g_fdfs_base_path), "%s", pBasePath);
		chopPath(g_fdfs_base_path);
		if (!fileExists(g_fdfs_base_path))
		{
			logError("file: "__FILE__", line: %d, " \
				"\"%s\" can't be accessed, error info: %s", \
				__LINE__, g_fdfs_base_path, STRERROR(errno));
			result = errno != 0 ? errno : ENOENT;
			break;
		}
		if (!isDir(g_fdfs_base_path))
		{
			logError("file: "__FILE__", line: %d, " \
				"\"%s\" is not a directory!", \
				__LINE__, g_fdfs_base_path);
			result = ENOTDIR;
			break;
		}

		load_log_level(&iniContext);
		if ((result=log_set_prefix(g_fdfs_base_path, \
				TRACKER_ERROR_LOG_FILENAME)) != 0)
		{
			break;
		}

		g_fdfs_connect_timeout = iniGetIntValue(NULL, "connect_timeout", \
				&iniContext, DEFAULT_CONNECT_TIMEOUT);
		if (g_fdfs_connect_timeout <= 0)
		{
			g_fdfs_connect_timeout = DEFAULT_CONNECT_TIMEOUT;
		}

		g_fdfs_network_timeout = iniGetIntValue(NULL, "network_timeout", \
				&iniContext, DEFAULT_NETWORK_TIMEOUT);
		if (g_fdfs_network_timeout <= 0)
		{
			g_fdfs_network_timeout = DEFAULT_NETWORK_TIMEOUT;
		}
		g_network_tv.tv_sec = g_fdfs_network_timeout;

		g_server_port = iniGetIntValue(NULL, "port", &iniContext, \
				FDFS_TRACKER_SERVER_DEF_PORT);
		if (g_server_port <= 0)
		{
			g_server_port = FDFS_TRACKER_SERVER_DEF_PORT;
		}

		pBindAddr = iniGetStrValue(NULL, "bind_addr", &iniContext);
		if (pBindAddr == NULL)
		{
			bind_addr[0] = '\0';
		}
		else
		{
			snprintf(bind_addr, addr_size, "%s", pBindAddr);
		}

		if ((result=tracker_load_store_lookup(filename, \
			&iniContext)) != 0)
		{
			break;
		}

		g_groups.store_server = (byte)iniGetIntValue(NULL, \
				"store_server",  &iniContext, \
				FDFS_STORE_SERVER_ROUND_ROBIN);
		if (!(g_groups.store_server == FDFS_STORE_SERVER_FIRST_BY_IP ||\
			g_groups.store_server == FDFS_STORE_SERVER_FIRST_BY_PRI||
			g_groups.store_server == FDFS_STORE_SERVER_ROUND_ROBIN))
		{
			logWarning("file: "__FILE__", line: %d, " \
				"store_server 's value %d is invalid, " \
				"set to %d (round robin)!", \
				__LINE__, g_groups.store_server, \
				FDFS_STORE_SERVER_ROUND_ROBIN);

			g_groups.store_server = FDFS_STORE_SERVER_ROUND_ROBIN;
		}

		g_groups.download_server = (byte)iniGetIntValue(NULL, \
			"download_server", &iniContext, \
			FDFS_DOWNLOAD_SERVER_ROUND_ROBIN);
		if (!(g_groups.download_server==FDFS_DOWNLOAD_SERVER_ROUND_ROBIN
			|| g_groups.download_server == 
				FDFS_DOWNLOAD_SERVER_SOURCE_FIRST))
		{
			logWarning("file: "__FILE__", line: %d, " \
				"download_server 's value %d is invalid, " \
				"set to %d (round robin)!", \
				__LINE__, g_groups.download_server, \
				FDFS_DOWNLOAD_SERVER_ROUND_ROBIN);

			g_groups.download_server = \
				FDFS_DOWNLOAD_SERVER_ROUND_ROBIN;
		}

		g_groups.store_path = (byte)iniGetIntValue(NULL, "store_path", \
			&iniContext, FDFS_STORE_PATH_ROUND_ROBIN);
		if (!(g_groups.store_path == FDFS_STORE_PATH_ROUND_ROBIN || \
			g_groups.store_path == FDFS_STORE_PATH_LOAD_BALANCE))
		{
			logWarning("file: "__FILE__", line: %d, " \
				"store_path 's value %d is invalid, " \
				"set to %d (round robin)!", \
				__LINE__, g_groups.store_path , \
				FDFS_STORE_PATH_ROUND_ROBIN);
			g_groups.store_path = FDFS_STORE_PATH_ROUND_ROBIN;
		}

		pStorageReserved = iniGetStrValue(NULL, \
					"reserved_storage_space", &iniContext);
		if (pStorageReserved == NULL)
		{
			g_storage_reserved_space.flag = \
					TRACKER_STORAGE_RESERVED_SPACE_FLAG_MB;
			g_storage_reserved_space.rs.mb = \
					FDFS_DEF_STORAGE_RESERVED_MB;
		}
		else if (*pStorageReserved == '\0')
		{
			logError("file: "__FILE__", line: %d, " \
				"item \"reserved_storage_space\" is empty!", \
				__LINE__);
			result = EINVAL;
                        break;
		}
		else if (*(pStorageReserved+strlen(pStorageReserved)-1) == '%')
		{
			char *endptr;
			g_storage_reserved_space.flag = \
				TRACKER_STORAGE_RESERVED_SPACE_FLAG_RATIO;
			endptr = NULL;
			*(pStorageReserved+strlen(pStorageReserved)-1) = '\0';
			g_storage_reserved_space.rs.ratio = \
					strtod(pStorageReserved, &endptr);
			if (endptr != NULL && *endptr != '\0')
			{
				logError("file: "__FILE__", line: %d, " \
					"item \"reserved_storage_space\": %s%%"\
					" is invalid!", __LINE__, \
					pStorageReserved);
				result = EINVAL;
        	                break;
			}

			if (g_storage_reserved_space.rs.ratio <= 0.00 || \
				g_storage_reserved_space.rs.ratio >= 100.00)
			{
				logError("file: "__FILE__", line: %d, " \
					"item \"reserved_storage_space\": %s%%"\
					" is invalid!", __LINE__, \
					pStorageReserved);
				result = EINVAL;
        	                break;
			}
		}
		else
		{
			if ((result=parse_bytes(pStorageReserved, 1, \
					&storage_reserved)) != 0)
			{
				return result;
			}

			g_storage_reserved_space.flag = \
					TRACKER_STORAGE_RESERVED_SPACE_FLAG_MB;
			g_storage_reserved_space.rs.mb = \
					storage_reserved / FDFS_ONE_MB;
		}

		g_max_connections = iniGetIntValue(NULL, "max_connections", \
				&iniContext, DEFAULT_MAX_CONNECTONS);
		if (g_max_connections <= 0)
		{
			g_max_connections = DEFAULT_MAX_CONNECTONS;
		}
	
		g_work_threads = iniGetIntValue(NULL, "work_threads", \
				&iniContext, DEFAULT_WORK_THREADS);
		if (g_work_threads <= 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"item \"work_threads\" is invalid, " \
				"value: %d <= 0!", __LINE__, g_work_threads);
			result = EINVAL;
                        break;
		}

		if ((result=set_rlimit(RLIMIT_NOFILE, g_max_connections)) != 0)
		{
			break;
		}
	
		pRunByGroup = iniGetStrValue(NULL, "run_by_group", &iniContext);
		pRunByUser = iniGetStrValue(NULL, "run_by_user", &iniContext);
		if (pRunByGroup == NULL)
		{
			*g_run_by_group = '\0';
		}
		else
		{
			snprintf(g_run_by_group, sizeof(g_run_by_group), \
				"%s", pRunByGroup);
		}
		if (*g_run_by_group == '\0')
		{
			g_run_by_gid = getegid();
		}
		else
		{
			struct group *pGroup;

     			pGroup = getgrnam(g_run_by_group);
			if (pGroup == NULL)
			{
				result = errno != 0 ? errno : ENOENT;
				logError("file: "__FILE__", line: %d, " \
					"getgrnam fail, errno: %d, " \
					"error info: %s", __LINE__, \
					result, STRERROR(result));
				return result;
			}

			g_run_by_gid = pGroup->gr_gid;
		}


		if (pRunByUser == NULL)
		{
			*g_run_by_user = '\0';
		}
		else
		{
			snprintf(g_run_by_user, sizeof(g_run_by_user), \
				"%s", pRunByUser);
		}
		if (*g_run_by_user == '\0')
		{
			g_run_by_uid = geteuid();
		}
		else
		{
			struct passwd *pUser;

     			pUser = getpwnam(g_run_by_user);
			if (pUser == NULL)
			{
				result = errno != 0 ? errno : ENOENT;
				logError("file: "__FILE__", line: %d, " \
					"getpwnam fail, errno: %d, " \
					"error info: %s", __LINE__, \
					result, STRERROR(result));
				return result;
			}

			g_run_by_uid = pUser->pw_uid;
		}

		if ((result=load_allow_hosts(&iniContext, \
                	 &g_allow_ip_addrs, &g_allow_ip_count)) != 0)
		{
			return result;
		}

		g_sync_log_buff_interval = iniGetIntValue(NULL, \
				"sync_log_buff_interval", &iniContext, \
				SYNC_LOG_BUFF_DEF_INTERVAL);
		if (g_sync_log_buff_interval <= 0)
		{
			g_sync_log_buff_interval = SYNC_LOG_BUFF_DEF_INTERVAL;
		}

		g_check_active_interval = iniGetIntValue(NULL, \
				"check_active_interval", &iniContext, \
				CHECK_ACTIVE_DEF_INTERVAL);
		if (g_check_active_interval <= 0)
		{
			g_check_active_interval = CHECK_ACTIVE_DEF_INTERVAL;
		}

		pThreadStackSize = iniGetStrValue(NULL, \
			"thread_stack_size", &iniContext);
		if (pThreadStackSize == NULL)
		{
			thread_stack_size = 64 * 1024;
		}
		else if ((result=parse_bytes(pThreadStackSize, 1, \
				&thread_stack_size)) != 0)
		{
			return result;
		}
		g_thread_stack_size = (int)thread_stack_size;

		g_storage_ip_changed_auto_adjust = iniGetBoolValue(NULL, \
				"storage_ip_changed_auto_adjust", \
				&iniContext, true);

		g_storage_sync_file_max_delay = iniGetIntValue(NULL, \
				"storage_sync_file_max_delay", &iniContext, \
				DEFAULT_STORAGE_SYNC_FILE_MAX_DELAY);
		if (g_storage_sync_file_max_delay <= 0)
		{
			g_storage_sync_file_max_delay = \
					DEFAULT_STORAGE_SYNC_FILE_MAX_DELAY;
		}

		g_storage_sync_file_max_time = iniGetIntValue(NULL, \
				"storage_sync_file_max_time", &iniContext, \
				DEFAULT_STORAGE_SYNC_FILE_MAX_TIME);
		if (g_storage_sync_file_max_time <= 0)
		{
			g_storage_sync_file_max_time = \
				DEFAULT_STORAGE_SYNC_FILE_MAX_TIME;
		}

		g_if_use_trunk_file = iniGetBoolValue(NULL, \
			"use_trunk_file", &iniContext, false);

		pSlotMinSize = iniGetStrValue(NULL, \
			"slot_min_size", &iniContext);
		if (pSlotMinSize == NULL)
		{
			slot_min_size = 256;
		}
		else if ((result=parse_bytes(pSlotMinSize, 1, \
				&slot_min_size)) != 0)
		{
			return result;
		}
		g_slot_min_size = (int)slot_min_size;
		if (g_slot_min_size <= 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"item \"slot_min_size\" %d is invalid, " \
				"which <= 0", __LINE__, g_slot_min_size);
			result = EINVAL;
			break;
		}
		if (g_slot_min_size > 64 * 1024)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"item \"slot_min_size\" %d is too large, " \
				"change to 64KB", __LINE__, g_slot_min_size);
			g_slot_min_size = 64 * 1024;
		}

		pTrunkFileSize = iniGetStrValue(NULL, \
			"trunk_file_size", &iniContext);
		if (pTrunkFileSize == NULL)
		{
			trunk_file_size = 64 * 1024 * 1024;
		}
		else if ((result=parse_bytes(pTrunkFileSize, 1, \
				&trunk_file_size)) != 0)
		{
			return result;
		}
		g_trunk_file_size = (int)trunk_file_size;
		if (g_trunk_file_size < 4 * 1024 * 1024)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"item \"trunk_file_size\" %d is too small, " \
				"change to 4MB", __LINE__, g_trunk_file_size);
			g_trunk_file_size = 4 * 1024 * 1024;
		}

		pSlotMaxSize = iniGetStrValue(NULL, \
			"slot_max_size", &iniContext);
		if (pSlotMaxSize == NULL)
		{
			slot_max_size = g_trunk_file_size / 2;
		}
		else if ((result=parse_bytes(pSlotMaxSize, 1, \
				&slot_max_size)) != 0)
		{
			return result;
		}
		g_slot_max_size = (int)slot_max_size;
		if (g_slot_max_size <= g_slot_min_size)
		{
			logError("file: "__FILE__", line: %d, " \
				"item \"slot_max_size\" %d is invalid, " \
				"which <= slot_min_size: %d", \
				__LINE__, g_slot_max_size, g_slot_min_size);
			result = EINVAL;
			break;
		}
		if (g_slot_max_size > g_trunk_file_size / 2)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"item \"slot_max_size\": %d is too large, " \
				"change to %d", __LINE__, g_slot_max_size, \
				g_trunk_file_size / 2);
			g_slot_max_size = g_trunk_file_size / 2;
		}

		g_trunk_create_file_advance = iniGetBoolValue(NULL, \
			"trunk_create_file_advance", &iniContext, false);
		if ((result=get_time_item_from_conf(&iniContext, \
                	"trunk_create_file_time_base", \
			&g_trunk_create_file_time_base, 2, 0)) != 0)
		{
			return result;
		}

		g_trunk_create_file_interval = iniGetIntValue(NULL, \
				"trunk_create_file_interval", &iniContext, \
				86400);
		pSpaceThreshold = iniGetStrValue(NULL, \
			"trunk_create_file_space_threshold", &iniContext);
		if (pSpaceThreshold == NULL)
		{
			g_trunk_create_file_space_threshold = 0;
		}
		else if ((result=parse_bytes(pSpaceThreshold, 1, \
				&g_trunk_create_file_space_threshold)) != 0)
		{
			return result;
		}

		g_trunk_init_check_occupying = iniGetBoolValue(NULL, \
			"trunk_init_check_occupying", &iniContext, false);

		g_trunk_init_reload_from_binlog = iniGetBoolValue(NULL, \
			"trunk_init_reload_from_binlog", &iniContext, false);

		if ((result=tracker_load_storage_ids(filename, &iniContext)) != 0)
		{
			return result;
		}
#ifdef WITH_HTTPD
		if ((result=fdfs_http_params_load(&iniContext, \
				filename, &g_http_params)) != 0)
		{
			return result;
		}

		g_http_check_interval = iniGetIntValue(NULL, \
			"http.check_alive_interval", &iniContext, 30);

		pHttpCheckType = iniGetStrValue(NULL, \
			"http.check_alive_type", &iniContext);
		if (pHttpCheckType != NULL && \
			strcasecmp(pHttpCheckType, "http") == 0)
		{
			g_http_check_type = FDFS_HTTP_CHECK_ALIVE_TYPE_HTTP;
		}
		else
		{
			g_http_check_type = FDFS_HTTP_CHECK_ALIVE_TYPE_TCP;
		}

		pHttpCheckUri = iniGetStrValue(NULL, \
			"http.check_alive_uri", &iniContext);
		if (pHttpCheckUri == NULL)
		{
			*g_http_check_uri = '/';
			*(g_http_check_uri+1) = '\0';
		}
		else if (*pHttpCheckUri == '/')
		{
			snprintf(g_http_check_uri, sizeof(g_http_check_uri), \
				"%s", pHttpCheckUri);
		}
		else
		{
			snprintf(g_http_check_uri, sizeof(g_http_check_uri), \
				"/%s", pHttpCheckUri);
		}


#endif

		if (g_storage_reserved_space.flag == \
				TRACKER_STORAGE_RESERVED_SPACE_FLAG_MB)
		{
			sprintf(reserved_space_str, "%dMB", \
				g_storage_reserved_space.rs.mb);
		}
		else
		{
			sprintf(reserved_space_str, "%.2f%%", \
				g_storage_reserved_space.rs.ratio);
		}
	
		logInfo("FastDFS v%d.%02d, base_path=%s, " \
			"run_by_group=%s, run_by_user=%s, " \
			"connect_timeout=%ds, "    \
			"network_timeout=%ds, "    \
			"port=%d, bind_addr=%s, " \
			"max_connections=%d, "    \
			"work_threads=%d, "    \
			"store_lookup=%d, store_group=%s, " \
			"store_server=%d, store_path=%d, " \
			"reserved_storage_space=%s, " \
			"download_server=%d, " \
			"allow_ip_count=%d, sync_log_buff_interval=%ds, " \
			"check_active_interval=%ds, " \
			"thread_stack_size=%d KB, " \
			"storage_ip_changed_auto_adjust=%d, "  \
			"storage_sync_file_max_delay=%ds, " \
			"storage_sync_file_max_time=%ds, "  \
			"use_trunk_file=%d, " \
			"slot_min_size=%d, " \
			"slot_max_size=%d MB, " \
			"trunk_file_size=%d MB, " \
			"trunk_create_file_advance=%d, " \
			"trunk_create_file_time_base=%02d:%02d, " \
			"trunk_create_file_interval=%d, " \
			"trunk_create_file_space_threshold=%d GB, " \
			"trunk_init_check_occupying=%d, " \
			"trunk_init_reload_from_binlog=%d, " \
			"use_storage_id=%d, storage_id_count=%d", \
			g_fdfs_version.major, g_fdfs_version.minor,  \
			g_fdfs_base_path, g_run_by_group, g_run_by_user, \
			g_fdfs_connect_timeout, \
			g_fdfs_network_timeout, g_server_port, bind_addr, \
			g_max_connections, g_work_threads, \
			g_groups.store_lookup, g_groups.store_group, \
			g_groups.store_server, g_groups.store_path, \
			reserved_space_str, g_groups.download_server, \
			g_allow_ip_count, g_sync_log_buff_interval, \
			g_check_active_interval, g_thread_stack_size / 1024, \
			g_storage_ip_changed_auto_adjust, \
			g_storage_sync_file_max_delay, \
			g_storage_sync_file_max_time, \
			g_if_use_trunk_file, g_slot_min_size, \
			g_slot_max_size / FDFS_ONE_MB, \
			g_trunk_file_size / FDFS_ONE_MB, \
			g_trunk_create_file_advance, \
			g_trunk_create_file_time_base.hour, \
			g_trunk_create_file_time_base.minute, \
			g_trunk_create_file_interval, \
			(int)(g_trunk_create_file_space_threshold / \
			(FDFS_ONE_MB * 1024)), g_trunk_init_check_occupying, \
			g_trunk_init_reload_from_binlog, \
			g_use_storage_id, g_storage_id_count);

#ifdef WITH_HTTPD
		if (!g_http_params.disabled)
		{
			logInfo("HTTP supported: " \
				"server_port=%d, " \
				"default_content_type=%s, " \
				"anti_steal_token=%d, " \
				"token_ttl=%ds, " \
				"anti_steal_secret_key length=%d, "  \
				"token_check_fail content_type=%s, " \
				"token_check_fail buff length=%d, "  \
				"check_active_interval=%d, " \
				"check_active_type=%s, " \
				"check_active_uri=%s",  \
				g_http_params.server_port, \
				g_http_params.default_content_type, \
				g_http_params.anti_steal_token, \
				g_http_params.token_ttl, \
				g_http_params.anti_steal_secret_key.length, \
				g_http_params.token_check_fail_content_type, \
				g_http_params.token_check_fail_buff.length, \
				g_http_check_interval, g_http_check_type == \
				FDFS_HTTP_CHECK_ALIVE_TYPE_TCP ? "tcp":"http",\
				g_http_check_uri);
		}
#endif

	} while (0);

	iniFreeContext(&iniContext);

	load_local_host_ip_addrs();

	return result;
}

