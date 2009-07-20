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

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <inttypes.h>
//#include <fcntl.h>
//#include <sys/ioctl.h>
#include <pthread.h>
#include "th_queue.h"
#include "datapack.h"

#include "hddspacemgr.h"
#include "replicator.h"

#define JHASHSIZE 0x400
#define JHASHPOS(id) ((id)&0x3FF)

enum {
	OP_EXIT,
	OP_INVAL,
	OP_CHUNKOP,
	OP_OPEN,
	OP_CLOSE,
	OP_READ,
	OP_WRITE,
	OP_REPLICATE
};

// for OP_CHUNKOP
typedef struct _chunk_op_args {
	uint64_t chunkid,copychunkid;
	uint32_t version,newversion,copyversion;
	uint32_t length;
} chunk_op_args;

// for OP_OPEN and OP_CLOSE
typedef struct _chunk_oc_args {
	uint64_t chunkid;
} chunk_oc_args;

// for OP_READ
typedef struct _chunk_rd_args {
	uint64_t chunkid;
	uint32_t version;
	uint32_t offset,size;
	uint16_t blocknum;
	uint8_t *buffer;
	uint8_t *crcbuff;
} chunk_rd_args;

// for OP_WRITE
typedef struct _chunk_wr_args {
	uint64_t chunkid;
	uint32_t version;
	uint32_t offset,size;
	uint16_t blocknum;
	const uint8_t *buffer;
	const uint8_t *crcbuff;
} chunk_wr_args;

typedef struct _chunk_rp_args {
	uint64_t chunkid;
	uint32_t version;
	uint8_t srccnt;
} chunk_rp_args;

typedef struct _job {
	uint32_t jobid;
	void (*callback)(uint8_t status,void *extra);
	void *extra;
	struct _job *next;
} job;

typedef struct _jobpool {
	int rpipe,wpipe;
	uint8_t workers;
	pthread_t *workerthreads;
	pthread_mutex_t pipelock;
	pthread_mutex_t jobslock;
	void *jobqueue;
	void *statusqueue;
	job* jobhash[JHASHSIZE];
	uint32_t nextjobid;
} jobpool;

static inline void job_send_status(jobpool *jp,uint32_t jobid,uint8_t status) {
	pthread_mutex_lock(&(jp->pipelock));
	if (queue_isempty(jp->statusqueue)) {	// first status
		if (write(jp->wpipe,&status,1)!=1) {	// write anything to wake up select
			syslog(LOG_ERR,"can't write to bgjobs pipe !!!: %m");
		}
	}
	queue_put(jp->statusqueue,jobid,status,NULL,1);
	pthread_mutex_unlock(&(jp->pipelock));
	return;
}

static inline int job_receive_status(jobpool *jp,uint32_t *jobid,uint8_t *status) {
	uint32_t qstatus;
	pthread_mutex_lock(&(jp->pipelock));
	queue_get(jp->statusqueue,jobid,&qstatus,NULL,NULL);
	*status = qstatus;
	if (queue_isempty(jp->statusqueue)) {
		if (read(jp->rpipe,&qstatus,1)!=1) {	// make pipe empty
			syslog(LOG_ERR,"can't read from bgjobs pipe !!!: %m");
		}
		pthread_mutex_unlock(&(jp->pipelock));
		return 0;	// last element
	}
	pthread_mutex_unlock(&(jp->pipelock));
	return 1;	// not last
}

#define opargs ((chunk_op_args*)args)
#define ocargs ((chunk_oc_args*)args)
#define rdargs ((chunk_rd_args*)args)
#define wrargs ((chunk_wr_args*)args)
#define rpargs ((chunk_rp_args*)args)
void* job_worker(void *th_arg) {
	jobpool *jp = (jobpool*)th_arg;
	uint8_t *args,status;
	uint32_t jobid;
	uint32_t op;
	for (;;) {
		queue_get(jp->jobqueue,&jobid,&op,&args,NULL);
		switch (op) {
		case OP_INVAL:
			status = ERROR_EINVAL;
			break;
		case OP_CHUNKOP:
			status = hdd_chunkop(opargs->chunkid,opargs->version,opargs->newversion,opargs->copychunkid,opargs->copyversion,opargs->length);
			break;
		case OP_OPEN:
			status = hdd_open(ocargs->chunkid);
			break;
		case OP_CLOSE:
			status = hdd_close(ocargs->chunkid);
			break;
		case OP_READ:
			status = hdd_read(rdargs->chunkid,rdargs->version,rdargs->blocknum,rdargs->buffer,rdargs->offset,rdargs->size,rdargs->crcbuff);
			break;
		case OP_WRITE:
			status = hdd_write(wrargs->chunkid,wrargs->version,wrargs->blocknum,wrargs->buffer,wrargs->offset,wrargs->size,wrargs->crcbuff);
			break;
		case OP_REPLICATE:
			status = replicate(rpargs->chunkid,rpargs->version,rpargs->srccnt,((uint8_t*)args)+sizeof(chunk_rp_args));
			break;
		default: // OP_EXIT
			pthread_exit(NULL);
			return NULL;
		}
		job_send_status(jp,jobid,status);
		if (args!=NULL) {
			free(args);
		}
	}
}

static inline uint32_t job_new(jobpool *jp,uint32_t op,void *args,void (*callback)(uint8_t status,void *extra),void *extra) {
//	jobpool* jp = (jobpool*)jpool;
	uint32_t jobid = jp->nextjobid;
	uint32_t jhpos = JHASHPOS(jobid);
	job *jptr;
	jptr = malloc(sizeof(job));
	if (jptr==NULL) {
		return 0;
	}
	jptr->jobid = jobid;
	jptr->callback = callback;
	jptr->extra = extra;
	jptr->next = jp->jobhash[jhpos];
	jp->jobhash[jhpos] = jptr;
	queue_put(jp->jobqueue,jobid,op,args,1);
	jp->nextjobid++;
	if (jp->nextjobid==0) {
		jp->nextjobid=1;
	}
	return jobid;
}

#if 0
int job_setnonblock(int fd) {
# ifdef O_NONBLOCK
	int flags = fcntl(sock, F_GETFL, 0);
	if (flags == -1) return -1;
	return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
# else /* O_NONBLOCK */
#  ifdef FIONBIO
	int yes = 1;
	return ioctl(sock, FIONBIO, &yes);
#  else /* FIONBIO */
#   ifdef ENOTSUP
	errno = ENOTSUP;
#   else /* ENOTSUP */
#    ifdef ENODEV
	errno = ENODEV;
#    endif /* ENODEV */
#   endif /* ENOTSUP */
	return -1;
#  endif /* FIONBIO */
# endif /* O_NONBLOCK */
}
#endif

/* interface */

void* job_pool_new(uint8_t workers,uint32_t jobs,int *wakeupdesc) {
	int fd[2];
	uint32_t i;
	jobpool* jp;
	if (pipe(fd)<0) {
		return NULL;
	}
//	if (job_setnonblock(fd[0])<0) {
//		return NULL;
//	}
       	jp=malloc(sizeof(jobpool));
	*wakeupdesc = fd[0];
	jp->rpipe = fd[0];
	jp->wpipe = fd[1];
	jp->workers = workers;
	jp->workerthreads = malloc(sizeof(pthread_t)*workers);
	pthread_mutex_init(&(jp->pipelock),NULL);
	pthread_mutex_init(&(jp->jobslock),NULL);
	jp->jobqueue = queue_new(jobs);
	jp->statusqueue = queue_new(0);
	for (i=0 ; i<JHASHSIZE ; i++) {
		jp->jobhash[i]=NULL;
	}
	jp->nextjobid = 1;
	for (i=0 ; i<workers ; i++) {
		pthread_create(jp->workerthreads+i,NULL,job_worker,jp);
	}
	return jp;
}

int job_pool_can_add(void *jpool) {
	jobpool* jp = (jobpool*)jpool;
	return queue_isfull(jp->jobqueue)?0:1;
}


void job_pool_change_callback(void *jpool,uint32_t jobid,void (*callback)(uint8_t status,void *extra),void *extra) {
	jobpool* jp = (jobpool*)jpool;
	uint32_t jhpos = JHASHPOS(jobid);
	job *jptr;
	for (jptr = jp->jobhash[jhpos] ; jptr ; jptr=jptr->next) {
		if (jptr->jobid==jobid) {
			jptr->callback=callback;
			jptr->extra=extra;
		}
	}
}

void job_pool_check_jobs(void *jpool) {
	jobpool* jp = (jobpool*)jpool;
	uint32_t jobid,jhpos;
	uint8_t status;
	int notlast;
	job **jhandle,*jptr;
	do {
		notlast = job_receive_status(jp,&jobid,&status);
		jhpos = JHASHPOS(jobid);
		jhandle = jp->jobhash+jhpos;
		while ((jptr = *jhandle)) {
			if (jptr->jobid==jobid) {
				if (jptr->callback) {
					jptr->callback(status,jptr->extra);
				}
				*jhandle = jptr->next;
				free(jptr);
				break;
			} else {
				jhandle = &(jptr->next);
			}
		}
	} while (notlast);
}

void job_pool_delete(void *jpool) {
	jobpool* jp = (jobpool*)jpool;
	uint32_t i;
	for (i=0 ; i<jp->workers ; i++) {
		queue_put(jp->jobqueue,0,OP_EXIT,NULL,0);
	}
	for (i=0 ; i<jp->workers ; i++) {
		pthread_join(jp->workerthreads[i],NULL);
	}
	if (!queue_isempty(jp->statusqueue)) {
		job_pool_check_jobs(jp);
	}
	queue_delete(jp->jobqueue);
	queue_delete(jp->statusqueue);
	pthread_mutex_destroy(&(jp->pipelock));
	pthread_mutex_destroy(&(jp->jobslock));
	free(jp->workerthreads);
	close(jp->rpipe);
	close(jp->wpipe);
	free(jp);
}

uint32_t job_inval(void *jpool,void (*callback)(uint8_t status,void *extra),void *extra) {
	jobpool* jp = (jobpool*)jpool;
	return job_new(jp,OP_INVAL,NULL,callback,extra);
}

uint32_t job_chunkop(void *jpool,void (*callback)(uint8_t status,void *extra),void *extra,uint64_t chunkid,uint32_t version,uint32_t newversion,uint64_t copychunkid,uint32_t copyversion,uint32_t length) {
	jobpool* jp = (jobpool*)jpool;
	chunk_op_args *args;
	args = malloc(sizeof(chunk_op_args));
	args->chunkid = chunkid;
	args->version = version;
	args->newversion = newversion;
	args->copychunkid = copychunkid;
	args->copyversion = copyversion;
	args->length = length;
	return job_new(jp,OP_CHUNKOP,args,callback,extra);
}

uint32_t job_open(void *jpool,void (*callback)(uint8_t status,void *extra),void *extra,uint64_t chunkid) {
	jobpool* jp = (jobpool*)jpool;
	chunk_oc_args *args;
	args = malloc(sizeof(chunk_oc_args));
	args->chunkid = chunkid;
	return job_new(jp,OP_OPEN,args,callback,extra);
}

uint32_t job_close(void *jpool,void (*callback)(uint8_t status,void *extra),void *extra,uint64_t chunkid) {
	jobpool* jp = (jobpool*)jpool;
	chunk_oc_args *args;
	args = malloc(sizeof(chunk_oc_args));
	args->chunkid = chunkid;
	return job_new(jp,OP_CLOSE,args,callback,extra);
}

uint32_t job_read(void *jpool,void (*callback)(uint8_t status,void *extra),void *extra,uint64_t chunkid,uint32_t version,uint16_t blocknum,uint8_t *buffer,uint32_t offset,uint32_t size,uint8_t *crcbuff) {
	jobpool* jp = (jobpool*)jpool;
	chunk_rd_args *args;
	args = malloc(sizeof(chunk_rd_args));
	args->chunkid = chunkid;
	args->version = version;
	args->blocknum = blocknum;
	args->buffer = buffer;
	args->offset = offset;
	args->size = size;
	args->crcbuff = crcbuff;
	return job_new(jp,OP_READ,args,callback,extra);
}

uint32_t job_write(void *jpool,void (*callback)(uint8_t status,void *extra),void *extra,uint64_t chunkid,uint32_t version,uint16_t blocknum,const uint8_t *buffer,uint32_t offset,uint32_t size,const uint8_t *crcbuff) {
	jobpool* jp = (jobpool*)jpool;
	chunk_wr_args *args;
	args = malloc(sizeof(chunk_wr_args));
	args->chunkid = chunkid;
	args->version = version;
	args->blocknum = blocknum;
	args->buffer = buffer;
	args->offset = offset;
	args->size = size;
	args->crcbuff = crcbuff;
	return job_new(jp,OP_WRITE,args,callback,extra);
}

uint32_t job_replicate(void *jpool,void (*callback)(uint8_t status,void *extra),void *extra,uint64_t chunkid,uint32_t version,uint8_t srccnt,const uint8_t *srcs) {
	jobpool* jp = (jobpool*)jpool;
	chunk_rp_args *args;
	uint8_t *ptr;
	ptr = malloc(sizeof(chunk_rp_args)+srccnt*18);
	args = (chunk_rp_args*)ptr;
	ptr += sizeof(chunk_rp_args);
	args->chunkid = chunkid;
	args->version = version;
	args->srccnt = srccnt;
	memcpy(ptr,srcs,srccnt*18);
	return job_new(jp,OP_REPLICATE,args,callback,extra);
}

uint32_t job_replicate_simple(void *jpool,void (*callback)(uint8_t status,void *extra),void *extra,uint64_t chunkid,uint32_t version,uint32_t ip,uint16_t port) {
	jobpool* jp = (jobpool*)jpool;
	chunk_rp_args *args;
	uint8_t *ptr;
	ptr = malloc(sizeof(chunk_rp_args)+18);
	args = (chunk_rp_args*)ptr;
	ptr += sizeof(chunk_rp_args);
	args->chunkid = chunkid;
	args->version = version;
	args->srccnt = 1;
	put64bit(&ptr,chunkid);
	put32bit(&ptr,version);
	put32bit(&ptr,ip);
	put16bit(&ptr,port);
	return job_new(jp,OP_REPLICATE,args,callback,extra);
}
