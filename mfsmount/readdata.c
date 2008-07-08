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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <syslog.h>
#include <inttypes.h>
#include <pthread.h>

#include "MFSCommunication.h"
#include "sockets.h"
#include "datapack.h"
#include "mastercomm.h"
#include "cscomm.h"

#define RETRIES 30
#define REFRESHTIMEOUT 5000000
#define READDELAY 1000000

#define MAPBITS 10
#define MAPSIZE (1<<(MAPBITS))
#define MAPMASK (MAPSIZE-1)
#define MAPINDX(inode) (inode&MAPMASK)

typedef struct _readrec {
	uint8_t *rbuff;
	uint32_t rbuffsize;
	uint32_t inode;
	uint64_t fleng;
	uint32_t indx;
	uint64_t chunkid;
	uint32_t version;
	int fd;
	struct timeval vtime;
	struct timeval atime;
	int valid;
	pthread_mutex_t lock;
	struct _readrec *next;
	struct _readrec *mapnext;
} readrec;

static readrec *rdinodemap[MAPSIZE];
static readrec *rdhead=NULL;
static pthread_t pthid;
static pthread_mutex_t *mainlock;

#define TIMEDIFF(tv1,tv2) (((int64_t)((tv1).tv_sec-(tv2).tv_sec))*1000000LL+(int64_t)((tv1).tv_usec-(tv2).tv_usec))

void* read_data_delayed_ops(void *arg) {
	struct timeval now;
	readrec *rrec,**rrecp;
	readrec **rrecmap;
	(void)arg;
	for (;;) {
		pthread_mutex_lock(mainlock);
		rrecp = &rdhead;
		while ((rrec=*rrecp)!=NULL) {
			if (rrec->valid==0) {
				pthread_mutex_lock(&(rrec->lock));
				pthread_mutex_unlock(&(rrec->lock));
				pthread_mutex_destroy(&(rrec->lock));
				*rrecp = rrec->next;
				rrecmap = &(rdinodemap[MAPINDX(rrec->inode)]);
				while (*rrecmap) {
					if ((*rrecmap)==rrec) {
						*rrecmap = rrec->mapnext;
					} else {
						rrecmap = &((*rrecmap)->mapnext);
					}
				}
				free(rrec);
			} else {
				pthread_mutex_lock(&(rrec->lock));
				gettimeofday(&now,NULL);
				if (rrec->fd>=0 && (TIMEDIFF(now,rrec->atime)>READDELAY || TIMEDIFF(now,rrec->vtime)>REFRESHTIMEOUT)) {
					tcpclose(rrec->fd);
					rrec->fd=-1;
				}
				pthread_mutex_unlock(&(rrec->lock));
				rrecp = &(rrec->next);
			}
		}
		pthread_mutex_unlock(mainlock);
		usleep(READDELAY/2);
	}
}

void* read_data_new(uint32_t inode) {
	readrec *rrec;
	rrec = malloc(sizeof(readrec));
	rrec->rbuff = NULL;
	rrec->rbuffsize = 0;
	rrec->inode = inode;
	rrec->fleng = 0;
	rrec->indx = 0;
	rrec->chunkid = 0;
	rrec->version = 0;
	rrec->fd = -1;
	rrec->atime.tv_sec = 0;
	rrec->atime.tv_usec = 0;
	rrec->vtime.tv_sec = 0;
	rrec->vtime.tv_usec = 0;
	rrec->valid = 1;
	pthread_mutex_init(&(rrec->lock),NULL);
	pthread_mutex_lock(mainlock);
	rrec->next = rdhead;
	rdhead = rrec;
	rrec->mapnext = rdinodemap[MAPINDX(inode)];
	rdinodemap[MAPINDX(inode)] = rrec;
	pthread_mutex_unlock(mainlock);
//	fprintf(stderr,"read_data_new (%p)\n",rrec);
	return rrec;
}

void read_data_end(void* rr) {
	readrec *rrec = (readrec*)rr;
//	fprintf(stderr,"read_data_end (%p)\n",rr);
	pthread_mutex_lock(&(rrec->lock));
	if (rrec->fd>=0) {
		tcpclose(rrec->fd);
		rrec->fd=-1;
	}
	if (rrec->rbuff!=NULL) {
		free(rrec->rbuff);
	}
	rrec->valid = 0;
	pthread_mutex_unlock(&(rrec->lock));
}

void read_data_init(void) {
	uint32_t i;
	for (i=0 ; i<MAPSIZE ; i++) {
		rdinodemap[i]=NULL;
	}
	mainlock = malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(mainlock,NULL);
	pthread_create(&pthid,NULL,read_data_delayed_ops,NULL);
}

static int read_data_refresh_connection(readrec *rrec) {
	uint32_t ip;
	uint16_t port;
	uint8_t status;
//	fprintf(stderr,"read_data_refresh_connection (%p)\n",rrec);
	if (rrec->fd>=0) {
		tcpclose(rrec->fd);
		rrec->fd = -1;
	}
	status = fs_readchunk(rrec->inode,rrec->indx,&(rrec->fleng),&(rrec->chunkid),&(rrec->version),&ip,&port);
	if (status!=0) {
		syslog(LOG_WARNING,"file: %u, index: %u, chunk: %llu, version: %u - fs_readchunk returns status %u",rrec->inode,rrec->indx,(unsigned long long int)(rrec->chunkid),rrec->version,status);
		if (status==ERROR_ENOENT) {
			return -2;	// stale handle
		}
		return -1;
	}
//	fprintf(stderr,"(%u,%u,%llu,%llu,%u,%u,%u)\n",rrec->inode,rrec->indx,rrec->fleng,rrec->chunkid,rrec->version,ip,port);
	if (rrec->chunkid==0 && ip==0 && port==0) {
		return 0;
	}
	if (ip==0 || port==0) {
		syslog(LOG_WARNING,"file: %u, index: %u, chunk: %llu, version: %u - there are no valid copies",rrec->inode,rrec->indx,(unsigned long long int)(rrec->chunkid),rrec->version);
		return -3;
	}
	gettimeofday(&(rrec->vtime),NULL);
	rrec->fd = tcpsocket();
	if (rrec->fd<0) {
		syslog(LOG_WARNING,"can't create tcp socket: %m");
		return -1;
	}
	if (tcpnodelay(rrec->fd)<0) {
		syslog(LOG_WARNING,"can't set TCP_NODELAY: %m");
	}
	if (tcpnumconnect(rrec->fd,ip,port)<0) {
		syslog(LOG_WARNING,"can't connect to (%08X:%u)",ip,port);
		tcpclose(rrec->fd);
		rrec->fd = -1;
		return -1;
	}
	return 0;
}

void read_inode_ops(uint32_t inode) {	// other operations such as truncate or write can change something (especially file length), so close connections (force fs_readchunk)
	readrec *rrec;
	pthread_mutex_lock(mainlock);
	for (rrec = rdinodemap[MAPINDX(inode)] ; rrec ; rrec=rrec->mapnext) {
		if (rrec->inode==inode) {
			pthread_mutex_lock(&(rrec->lock));
			if (rrec->fd>=0) {
				tcpclose(rrec->fd);
				rrec->fd = -1;
			}
			pthread_mutex_unlock(&(rrec->lock));
		}
	}
	pthread_mutex_unlock(mainlock);
}

int read_data(void *rr, uint64_t offset, uint32_t *size, uint8_t **buff) {
	uint8_t *buffptr;
	uint64_t curroff;
	uint32_t currsize;
	uint32_t indx;
	uint8_t cnt;
	uint32_t chunkoffset;
	uint32_t chunksize;
	int err;
	readrec *rrec = (readrec*)rr;

//	fprintf(stderr,"read_data (%p,%llu,%u)\n",rrec,offset,*size);
	pthread_mutex_lock(&(rrec->lock));
	if (*size==0) {
		*buff = NULL;
		return 0;
	}

	if (*size>rrec->rbuffsize) {
		if (rrec->rbuff!=NULL) {
			free(rrec->rbuff);
		}
		rrec->rbuffsize = *size;
		rrec->rbuff = malloc(rrec->rbuffsize);
		if (rrec->rbuff==NULL) {
			rrec->rbuffsize = 0;
			syslog(LOG_WARNING,"file: %u, index: %u - out of memory",rrec->inode,rrec->indx);
			return -4;	// out of memory
		}
	}

	err = -1;
	cnt = 0;
	buffptr = rrec->rbuff;
	curroff = offset;
	currsize = *size;
	while (currsize>0) {
		indx = (curroff>>26);
		if (rrec->fd<0 || rrec->indx != indx) {
			rrec->indx = indx;
			while (cnt<RETRIES) {
				cnt++;
				err = read_data_refresh_connection(rrec);
				if (err==0) {
					break;
				}
				syslog(LOG_WARNING,"file: %u, index: %u - can't connect to proper chunkserver (try counter: %u)",rrec->inode,rrec->indx,cnt);
				if (err==-2) {	// no such inode - it's unrecoverable error
					return err;
				}
				if (err==-3) {	// chunk not available - unrecoverable, but wait longer, and make less retries
					sleep(60);
					cnt+=9;
				} else {
					sleep(1+cnt/5);
				}
			}
			if (cnt>=RETRIES) {
				return err;
			}
		}
		if (curroff>=rrec->fleng) {
			break;
		}
		if (curroff+currsize>rrec->fleng) {
			currsize = rrec->fleng-curroff;
		}
		chunkoffset = (curroff&0x3FFFFFF);
		if (chunkoffset+currsize>0x4000000) {
			chunksize = 0x4000000-chunkoffset;
		} else {
			chunksize = currsize;
		}
		if (rrec->chunkid>0) {
			// fprintf(stderr,"(%d,%llu,%u,%u,%u,%p)\n",rrec->fd,rrec->chunkid,rrec->version,chunkoffset,chunksize,buffptr);
			if (cs_readblock(rrec->fd,rrec->chunkid,rrec->version,chunkoffset,chunksize,buffptr)<0) {
				syslog(LOG_WARNING,"file: %u, index: %u, chunk: %llu, version: %u - readblock error (try counter: %u)",rrec->inode,rrec->indx,(unsigned long long int)(rrec->chunkid),rrec->version,cnt);
				tcpclose(rrec->fd);
				rrec->fd = -1;
				sleep(1+cnt/5);
			} else {
				curroff+=chunksize;
				currsize-=chunksize;
				buffptr+=chunksize;
			}
		} else {
			memset(buffptr,0,chunksize);
			curroff+=chunksize;
			currsize-=chunksize;
			buffptr+=chunksize;
		}
	}

	if (rrec->fleng<=offset) {
		*buff = NULL;
		*size = 0;
	} else if (rrec->fleng<(offset+(*size))) {
		*buff = rrec->rbuff;
		*size = rrec->fleng - offset;
	} else {
		*buff = rrec->rbuff;
	}
	gettimeofday(&(rrec->atime),NULL);
	return 0;
}

void read_data_freebuff(void *rr) {
	readrec *rrec = (readrec*)rr;
	pthread_mutex_unlock(&(rrec->lock));
}