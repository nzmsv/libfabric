/*
 * Copyright (c) 2004, 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2006, 2007 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2017-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#include <alloca.h>
#include <errno.h>

#include <rdma/fi_errno.h>

#include "efa.h"

#ifdef _WIN32
#include "efawin.h"
#endif

static struct efa_context **ctx_list;
static int dev_cnt;

static struct efa_context *efa_device_open(struct ibv_device *device)
{
	struct efa_context *ctx;

	ctx = calloc(1, sizeof(struct efa_context));
	if (!ctx) {
		errno = ENOMEM;
		return NULL;
	}

	ctx->ibv_ctx = ibv_open_device(device);
	if (!ctx->ibv_ctx)
		goto err_free_ctx;

	return ctx;

err_free_ctx:
	free(ctx);
	return NULL;
}

static int efa_device_close(struct efa_context *ctx)
{
	ibv_close_device(ctx->ibv_ctx);
	free(ctx);

	return 0;
}

#ifndef _WIN32

int efa_lib_init(void)
{
	return 0;
}

#else // _WIN32

int efa_lib_init(void)
{
	int ret;
	/*
	* On Windows we need to load efawin dll to interact with
 	* efa device as there is no built-in verbs integration in the OS.
	* efawin dll provides all the ibv_* functions on Windows.
	* efa_load_efawin_lib function will replace stub ibv_* functions with
	* functions from efawin dll
	*/
	ret = efa_load_efawin_lib();

	return ret;
}

#endif // _WIN32

int efa_device_init(void)
{
	struct ibv_device **device_list;
	int ctx_idx;
	int ret;

	ofi_spin_init(&pd_list_lock);

	ret = efa_lib_init();
	if (ret != 0) {
		return ret;
	}

	device_list = ibv_get_device_list(&dev_cnt);
	if (device_list == NULL)
		return -ENOMEM;
	if (dev_cnt <= 0) {
		ret = -ENODEV;
		goto err_free_dev_list;
	}

	ctx_list = calloc(dev_cnt, sizeof(*ctx_list));
	if (!ctx_list) {
		ret = -ENOMEM;
		goto err_free_dev_list;
	}

	pd_list = calloc(dev_cnt, sizeof(*pd_list));
	if (!pd_list) {
		ret = -ENOMEM;
		goto err_free_ctx_list;
	}

	for (ctx_idx = 0; ctx_idx < dev_cnt; ctx_idx++) {
		ctx_list[ctx_idx] = efa_device_open(device_list[ctx_idx]);
		if (!ctx_list[ctx_idx]) {
			ret = -ENODEV;
			goto err_close_devs;
		}
		ctx_list[ctx_idx]->dev_idx = ctx_idx;
	}

	ibv_free_device_list(device_list);

	return 0;

err_close_devs:
	for (ctx_idx--; ctx_idx >= 0; ctx_idx--)
		efa_device_close(ctx_list[ctx_idx]);
	free(pd_list);
err_free_ctx_list:
	free(ctx_list);
err_free_dev_list:
	ibv_free_device_list(device_list);
	dev_cnt = 0;
	return ret;
}

bool efa_device_support_rdma_read(void)
{
#ifdef HAVE_RDMA_SIZE
	int err;
	struct efadv_device_attr efadv_attr;

	if (dev_cnt <=0)
		return false;

	assert(dev_cnt > 0);
	err = efadv_query_device(ctx_list[0]->ibv_ctx, &efadv_attr, sizeof(efadv_attr));
	if (err)
		return false;

	return efadv_attr.device_caps & EFADV_DEVICE_ATTR_CAPS_RDMA_READ;
#else
	return false;
#endif
}

#ifndef _WIN32

void efa_lib_close(void) {
	// Nothing to do when we are not compiling for Windows
}

#else // _WIN32

void efa_lib_close(void) {
	efa_free_efawin_lib();
}

#endif // _WIN32

void efa_device_free(void)
{
	int i;

	for (i = 0; i < dev_cnt; i++)
		efa_device_close(ctx_list[i]);

	free(pd_list);
	free(ctx_list);
	dev_cnt = 0;
	efa_lib_close();
	ofi_spin_destroy(&pd_list_lock);
}

struct efa_context **efa_device_get_context_list(int *num_ctx)
{
	struct efa_context **devs = NULL;
	int i;

	devs = calloc(dev_cnt, sizeof(*devs));
	if (!devs)
		goto out;

	for (i = 0; i < dev_cnt; i++)
		devs[i] = ctx_list[i];
out:
	*num_ctx = devs ? dev_cnt : 0;
	return devs;
}

void efa_device_free_context_list(struct efa_context **list)
{
	free(list);
}

