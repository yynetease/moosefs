/*
   Copyright 2008 Gemius SA.

   This file is part of MooseFS.

   MooseFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   MooseFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with MooseFS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "hddspacemgr.h"
#include "masterconn.h"
#include "csserv.h"
#include "cstocsconn.h"
#include "stats.h"

#include "config.h"

#define STR_AUX(x) #x
#define STR(x) STR_AUX(x)
const char id1[]="@(#) version: " STR(VERSMAJ) "." STR(VERSMID) "." STR(VERSMIN) ", written by Jakub Kruszona-Zawadzki";
const char id2[]="@(#) Copyright 2005 by Gemius S.A.";

/* Run Tab */
typedef int (*runfn)(void);
struct {
	runfn fn;
	char *name;
} RunTab[]={
	{hdd_init,"hdd space manager"},
	{csserv_init,"chunkserver server"},	/* heve to be before "masterconn" */
	{cstocsconn_init,"connection with chunkserver"},
	{masterconn_init,"connection with master"},
	{stats_init,"statistics/charts module"},
	{(runfn)0,"****"}
};