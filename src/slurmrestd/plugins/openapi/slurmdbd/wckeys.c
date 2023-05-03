/*****************************************************************************\
 *  wckeys.c - Slurm REST API acct wckey http operations handlers
 *****************************************************************************
 *  Copyright (C) 2020 SchedMD LLC.
 *  Written by Nathan Rini <nate@schedmd.com>
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "config.h"

#include <stdint.h>

#include "slurm/slurm.h"
#include "slurm/slurmdb.h"

#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/parse_time.h"
#include "src/common/ref.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurmdbd_defs.h"
#include "src/common/strlcpy.h"
#include "src/common/uid.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/slurmrestd/operations.h"
#include "api.h"

#define MAGIC_FOREACH_WCKEY 0xb3a2faf2

typedef struct {
	int magic; /* MAGIC_FOREACH_WCKEY */
	data_t *wckeys;
	ctxt_t *ctxt;
} foreach_wckey_t;

static int _foreach_wckey(void *x, void *arg)
{
	slurmdb_wckey_rec_t *wckey = x;
	foreach_wckey_t *args = arg;

	xassert(args->magic == MAGIC_FOREACH_WCKEY);

	if (DATA_DUMP(args->ctxt->parser, WCKEY, *wckey,
		      data_list_append(args->wckeys)))
		return SLURM_ERROR;

	return SLURM_SUCCESS;
}

static void _dump_wckeys(ctxt_t *ctxt, char *wckey)
{
	slurmdb_wckey_cond_t wckey_cond = {
		.with_deleted = true,
	};
	foreach_wckey_t args = {
		.magic = MAGIC_FOREACH_WCKEY,
		.ctxt = ctxt,
	};
	List wckey_list = NULL;

	args.wckeys = data_set_list(data_key_set(ctxt->resp, "wckeys"));

	if (wckey) {
		wckey_cond.name_list = list_create(NULL);
		list_append(wckey_cond.name_list, wckey);
	}

	if (!db_query_list(ctxt, &wckey_list, slurmdb_wckeys_get,
			   &wckey_cond) &&
	    wckey_list)
		list_for_each(wckey_list, _foreach_wckey, &args);

	FREE_NULL_LIST(wckey_list);
	FREE_NULL_LIST(wckey_cond.name_list);
}

static int _foreach_del_wckey(void *x, void *arg)
{
	char *wckey = x;
	data_t *wckeys = arg;

	data_set_string(data_list_append(wckeys), wckey);
	return SLURM_SUCCESS;
}

static void _delete_wckey(ctxt_t *ctxt)
{
	List wckey_list = NULL;
	slurmdb_wckey_cond_t wckey_cond = {
		.with_deleted = true,
	};
	char *wckey = get_str_param("wckey", true, ctxt);
	data_t *wckeys =
		data_set_list(data_key_set(ctxt->resp, "deleted_wckeys"));

	if (!wckey || !wckey[0]) {
		resp_error(ctxt, ESLURM_REST_INVALID_QUERY, __func__,
			   "wckey name must be provided for delete operation");
		goto cleanup;
	}

	wckey_cond.name_list = list_create(NULL);
	list_append(wckey_cond.name_list, wckey);

	if (!db_query_list(ctxt, &wckey_list, slurmdb_wckeys_remove,
			   &wckey_cond))
		db_query_commit(ctxt);

	if (!ctxt->rc && wckey_list) {
		list_for_each(wckey_list, _foreach_del_wckey, wckeys);
	}

cleanup:
	FREE_NULL_LIST(wckey_list);
	FREE_NULL_LIST(wckey_cond.name_list);
}

static void _update_wckeys(ctxt_t *ctxt, bool commit)
{
	data_t *parent_path = NULL;
	data_t *dwckeys = get_query_key_list("wckeys", ctxt, &parent_path);
	list_t *wckey_list = NULL;

	if (!dwckeys) {
		resp_warn(ctxt, __func__,
			  "ignoring empty or non-existant wckeys array for update");
		goto cleanup;
	}

	if (DATA_PARSE(ctxt->parser, WCKEY_LIST, wckey_list, dwckeys,
		       parent_path))
		goto cleanup;

	if (!db_query_rc(ctxt, wckey_list, slurmdb_wckeys_add) && commit)
		db_query_commit(ctxt);

cleanup:
	FREE_NULL_LIST(wckey_list);
	FREE_NULL_DATA(parent_path);
}

extern int op_handler_wckey(ctxt_t *ctxt)
{
	char *wckey = get_str_param("wckey", true, ctxt);

	if (!wckey) {
		resp_error(ctxt, ESLURM_REST_INVALID_QUERY, __func__,
			   "wckey required for singular query");
	} else if (ctxt->method == HTTP_REQUEST_GET) {
		_dump_wckeys(ctxt, wckey);
	} else if (ctxt->method == HTTP_REQUEST_DELETE) {
		_delete_wckey(ctxt);
	} else {
		resp_error(ctxt, ESLURM_REST_INVALID_QUERY, __func__,
			   "Unsupported HTTP method requested: %s",
			   get_http_method_string(ctxt->method));
	}

	return SLURM_SUCCESS;
}

extern int op_handler_wckeys(ctxt_t *ctxt)
{
	if (ctxt->method == HTTP_REQUEST_GET)
		_dump_wckeys(ctxt, NULL);
	else if (ctxt->method == HTTP_REQUEST_POST)
		_update_wckeys(ctxt, (ctxt->tag != CONFIG_OP_TAG));
	else {
		resp_error(ctxt, ESLURM_REST_INVALID_QUERY, __func__,
			   "Unsupported HTTP method requested: %s",
			   get_http_method_string(ctxt->method));
	}

	return SLURM_SUCCESS;
}

extern void init_op_wckeys(void)
{
	bind_handler("/slurmdb/{data_parser}/wckeys/", op_handler_wckeys, 0);
	bind_handler("/slurmdb/{data_parser}/wckey/{wckey}", op_handler_wckey,
		     0);
}

extern void destroy_op_wckeys(void)
{
	unbind_operation_ctxt_handler(op_handler_wckeys);
	unbind_operation_ctxt_handler(op_handler_wckey);
}
