/*
 * Copyright (c) 2014, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

/**
 * Contains definitions of the structures used to pack NVIDIA
 * officially-released firmwares.
 */

#ifndef __GRAPH_NETLIST_H__
#define __GRAPH_NETLIST_H__

#include <core/os.h>

/* netlist regions */
#define NETLIST_REGIONID_FECS_UCODE_DATA        0
#define NETLIST_REGIONID_FECS_UCODE_INST        1
#define NETLIST_REGIONID_GPCCS_UCODE_DATA       2
#define NETLIST_REGIONID_GPCCS_UCODE_INST       3
#define NETLIST_REGIONID_SW_BUNDLE_INIT         4
#define NETLIST_REGIONID_SW_CTX_LOAD            5
#define NETLIST_REGIONID_SW_NON_CTX_LOAD        6
#define NETLIST_REGIONID_SW_METHOD_INIT         7
#define NETLIST_REGIONID_CTXREG_SYS             8
#define NETLIST_REGIONID_CTXREG_GPC             9
#define NETLIST_REGIONID_CTXREG_TPC             10
#define NETLIST_REGIONID_CTXREG_ZCULL_GPC       11
#define NETLIST_REGIONID_CTXREG_PM_SYS          12
#define NETLIST_REGIONID_CTXREG_PM_GPC          13
#define NETLIST_REGIONID_CTXREG_PM_TPC          14
#define NETLIST_REGIONID_MAJORV                 15
#define NETLIST_REGIONID_BUFFER_SIZE            16
#define NETLIST_REGIONID_CTXSW_REG_BASE_INDEX   17
#define NETLIST_REGIONID_NETLIST_NUM            18
#define NETLIST_REGIONID_CTXREG_PPC             19
#define NETLIST_REGIONID_CTXREG_PMPPC           20

struct netlist_region
{
	u32 region_id;
	u32 data_size;
	u32 data_offset;
};

struct netlist_image_header
{
	u32 version;
	u32 regions;
};

struct netlist_image
{
	struct netlist_image_header header;
	struct netlist_region regions[1];
};

#endif
