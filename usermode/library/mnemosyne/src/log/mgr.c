/**
 * \brief Log manager
 *
 * Recovers and flushes registered logs. 
 * Expects logs to follow a specific interface. See m_log_ops_t
 */

#include <sys/mman.h>
#include <pthread.h>
#include <malloc.h>
#include <stdint.h>
#include <result.h>
#include <debug.h>
#include <list.h>
#include "log.h"
#include "staticlogs.h"
#include "../segment.h"
#include "../pregionlayout.h"

__attribute__ ((section("PERSISTENT"))) uintptr_t   log_pool = 0x0;

#define LOG_NUM 32


typedef struct m_logtype_entry_s m_logtype_entry_t;
struct m_logtype_entry_s {
	int              type;
	m_log_ops_t      *ops;
	struct list_head list;
};


static pthread_mutex_t      logmgr_init_lock = PTHREAD_MUTEX_INITIALIZER;
static m_logmgr_t           *logmgr = NULL;
static volatile char        logmgr_initialized = 0; /* reads and writes to single-byte memory locations are guaranteed to be atomic. Don't need to bother with alignment. */


#define NULL_LOG_OPS { NULL, NULL, NULL, NULL, NULL}

/**
 * Static log operations.
 *
 * The operation on log types known to the log manager at compilation time. 
 */
static m_log_ops_t static_log_ops[LF_TYPE_VALIDVALUES] =
{
	NULL_LOG_OPS
};


static m_result_t register_static_logtypes(m_logmgr_t *mgr);
static m_result_t do_recovery(m_logmgr_t *mgr);


/**
 * \brief Creates the log pool if doesn't exist and then initializes the
 * necessary volatile data structures to access the log pool. 
 *
 * A log descriptor volatile structure is created per non-volatile persistent
 * log but the actual volatile log structure is created when the log is 
 * later recovered or allocated by a client. 
 */
static
m_result_t
create_log_pool(m_logmgr_t *mgr)
{
	uintptr_t        metadata_start_addr;
	uintptr_t        logs_start_addr;
	int              metadata_section_size;
	int              physical_log_size;
	void             *addr;
	m_log_dsc_t    *log_dscs;
	m_segidx_entry_t *segidx_entry;
	int              i;

	if (!log_pool) {
		/* 
		 * Check whether the segment already exists. This is possible if
		 * there was a crash right after segment was created but before 
		 * log_pool was written.
		 */
		if (m_segment_find_using_addr((void *) LOG_POOL_START, &segidx_entry) 
		    != M_R_SUCCESS) 
		{
			addr = m_pmap2((void *) LOG_POOL_START, LOG_POOL_SIZE, 
			               PROT_READ|PROT_WRITE, MAP_FIXED);
			if (addr == MAP_FAILED) {
				M_INTERNALERROR("Could not allocate logs pool segment.\n");
			}
		}
		PCM_STORE(&log_pool, (uintptr_t) addr);
		PCM_BARRIER
	}
	
	/* 
	 * Now read the non-volatile log metadata and non-volatile physical logs.
	 * 
	 * Physical logs should be page aligned to get maximum bandwidth from the 
	 * system. Since sizeof(metadata) much smaller than sizeof(PAGE) we 
	 * aggregate all the metadata together.
	 */
	metadata_start_addr = LOG_POOL_START; /* this is already page aligned */
	metadata_section_size = PAGE_ALIGN(LOG_NUM * sizeof(m_log_nvmd_t));
	logs_start_addr = metadata_start_addr + metadata_section_size;
   	physical_log_size = PAGE_ALIGN(PHYSICAL_LOG_SIZE);
	assert(metadata_section_size + LOG_NUM*physical_log_size <= LOG_POOL_SIZE);
	log_dscs = (m_log_dsc_t *) calloc(LOG_NUM, sizeof(m_log_dsc_t));
	for (i=0; i<LOG_NUM; i++) {
		log_dscs[i].nvmd = (m_log_nvmd_t *) (metadata_start_addr + 
		                                        sizeof(m_log_nvmd_t)*i);
		log_dscs[i].nvphlog = (scm_word_t *) (logs_start_addr + 
		                                         physical_log_size*i);
		log_dscs[i].log = NULL;
		log_dscs[i].ops = NULL;
		log_dscs[i].logorder = INV_LOG_ORDER;
		if ((log_dscs[i].nvmd->generic_flags & LF_TYPE_MASK) == 
		    LF_TYPE_FREE) 
		{
			list_add_tail(&(log_dscs[i].list), &(mgr->free_logs_list));
		} else {
			list_add_tail(&(log_dscs[i].list), &(mgr->pending_logs_list));
		}
	}

}


/**
 * \brief Reincarnates the pool of logs and recovers and log types known when
 * to the log manager when it was compiled.
 */
static
m_result_t
logmgr_init()
{
	m_result_t rv = M_R_FAILURE;
	m_logmgr_t *mgr;

	pthread_mutex_lock(&logmgr_init_lock);
	if (logmgr_initialized) {
		rv = M_R_SUCCESS;
		goto out;
	}

	if (!(mgr = (m_logmgr_t *) malloc(sizeof(m_logmgr_t)))) {
		rv = M_R_NOMEMORY;
		goto out;
	}
	pthread_mutex_init(&(mgr->mutex), NULL);
	INIT_LIST_HEAD(&(mgr->known_logtypes_list));
	INIT_LIST_HEAD(&(mgr->free_logs_list));
	INIT_LIST_HEAD(&(mgr->active_logs_list));
	INIT_LIST_HEAD(&(mgr->pending_logs_list));
	create_log_pool(mgr);
	register_static_logtypes(mgr);
	do_recovery(mgr); /* will recover any known log types so far. */

	/* 
	 * Be careful, order matters. 
	 * 
	 * x86 does not reorder STORE ops so we know that if someone sees variable
	 * 'logmgr_initialized' set then it is guaranteed to see the assignment 
	 * to 'logmgr'.
	 */
	logmgr = mgr;
	logmgr_initialized = 1; 
	m_logtrunc_init(logmgr);
	rv = M_R_SUCCESS;

out:
	pthread_mutex_unlock(&logmgr_init_lock);
	return rv;
}


m_result_t
m_logmgr_init()
{
	return logmgr_init();
}



/**
 * \brief Shutdowns the log manager.
 *
 * It flushes any dirty logs.
 */
m_result_t
m_logmgr_fini()
{
	//TODO
}



static
m_result_t
register_logtype(m_logmgr_t *mgr, int type, m_log_ops_t *ops, int lock)
{
	m_result_t        rv = M_R_FAILURE;
	m_logtype_entry_t *logtype_entry;
	m_log_dsc_t       *log_dsc;

	if (lock) {
		pthread_mutex_lock(&(mgr->mutex));
	}
	/* first check that the type is not already registered. */
	list_for_each_entry(logtype_entry, &(mgr->known_logtypes_list), list) {
		if (logtype_entry->type == type) {
			/* already registered, nothing need to be done */
			rv = M_R_SUCCESS;
			goto out;
		}
	}
	logtype_entry = NULL;
	if (!(logtype_entry = malloc(sizeof(m_logtype_entry_t)))) {
		rv = M_R_NOMEMORY;
		goto out;
	}
	logtype_entry->type = type;
	logtype_entry->ops = ops;
	list_add_tail(&(logtype_entry->list), &(mgr->known_logtypes_list));
	/* Update the ops field of any pending log of the newly registered type and allocate a log. */
	list_for_each_entry(log_dsc, &(mgr->pending_logs_list), list) {
		if ((log_dsc->nvmd->generic_flags & LF_TYPE_MASK)  == type) {
			log_dsc->ops = ops;
			assert(log_dsc->ops->alloc(log_dsc) == M_R_SUCCESS);
		}	
	}

	rv = M_R_SUCCESS;
out:
	if (lock) {
		pthread_mutex_unlock(&(mgr->mutex));
	}
	return rv;
}


static
m_result_t
register_static_logtypes(m_logmgr_t *mgr)
{
	int i;

	for (i=1; i<LF_TYPE_VALIDVALUES; i++) {
		assert(register_logtype(mgr, i, &static_log_ops[i], 0) == M_R_SUCCESS);
	}
	
	return M_R_SUCCESS;
}


m_result_t
m_logmgr_register_logtype(int type, m_log_ops_t *ops)
{
	if (!logmgr) {
		logmgr_init();
	}
	return register_logtype((m_logmgr_t *)logmgr, type, ops, 1);
}


/**
 * \brief It checks the unknown logs list and recovers any newly known 
 * log types.
 */
static
m_result_t
do_recovery(m_logmgr_t *mgr)
{
	m_log_dsc_t       *log_dsc;
	m_log_dsc_t       *log_dsc_tmp;
	m_log_dsc_t       *log_dsc_to_recover;
	struct list_head  recovery_list;

	/* 
	 * First collect all logs which are to be recovered and prepare
	 * each log for recovery. After a log is prepared, it might pass 
	 * back a recovery order number if it cares about the order 
	 * the recovery is performed with respect to other logs.
	 */
	/* FIXME: Collect and recover logs by type. */
	INIT_LIST_HEAD(&recovery_list);
	list_for_each_entry_safe(log_dsc, log_dsc_tmp, &(mgr->pending_logs_list), list) {
		if (log_dsc->ops && log_dsc->ops->recovery_init) {
			log_dsc->ops->recovery_init(log_dsc);
			list_del_init(&(log_dsc->list));
			list_add(&(log_dsc->list), &recovery_list);
		}
	}

	/* 
	 * Find the next log to recover, recover it, update its recovery
	 * order, and repeat until there are no more logs to recover.
	 */
	do {
		log_dsc_to_recover = NULL; 
		list_for_each_entry(log_dsc, &recovery_list, list) {
			printf("log_dsc           = %p\n", log_dsc);
			printf("log_dsc->logorder = 0x%lX\n", log_dsc->logorder);
			if (log_dsc->logorder == INV_LOG_ORDER) {
				continue;
			}
			if (log_dsc_to_recover == NULL) {
				log_dsc_to_recover = log_dsc;
			} else {
				if (log_dsc_to_recover->logorder > log_dsc->logorder) {
					log_dsc_to_recover = log_dsc;
				}
			}
		}
		if (log_dsc_to_recover) {
			assert(log_dsc_to_recover->ops);
			assert(log_dsc_to_recover->ops->recovery_do);
			assert(log_dsc_to_recover->ops->recovery_prepare_next);
			log_dsc_to_recover->ops->recovery_do(log_dsc_to_recover);
			log_dsc_to_recover->ops->recovery_prepare_next(log_dsc_to_recover);
		}	
	} while(log_dsc_to_recover);

	/* Make the recovered logs available for reuse */
	list_splice(&recovery_list, &(mgr->free_logs_list));

	return M_R_SUCCESS;
}


m_result_t 
m_logmgr_do_recovery()
{
	return do_recovery(logmgr);
}


/**
 * \brief Allocates a new log and places it in the active logs list.
 */
m_result_t
m_logmgr_alloc_log(int type, m_log_dsc_t **log_dscp)
{
	m_result_t        rv = M_R_FAILURE;
	m_log_dsc_t       *log_dsc;
	m_log_dsc_t       *free_log_dsc = NULL;
	m_log_dsc_t       *free_log_dsc_notype = NULL;
	m_logtype_entry_t *logtype_entry;

	pthread_mutex_lock(&logmgr_init_lock);
	list_for_each_entry(log_dsc, &(logmgr->free_logs_list), list) {
		if (((log_dsc->nvmd->generic_flags & LF_TYPE_MASK) ==  type) &&
		    free_log_dsc == NULL) 
		{
			free_log_dsc = log_dsc;
		}
		if (((log_dsc->nvmd->generic_flags & LF_TYPE_MASK) ==  LF_TYPE_FREE) &&
		    free_log_dsc_notype == NULL) 
		{
			free_log_dsc_notype = log_dsc;
		}
	}
	/* Prefer using a log descriptor of the same type */
	if (free_log_dsc) {
		log_dsc = free_log_dsc;
	} else if (free_log_dsc_notype) {
		log_dsc = free_log_dsc_notype;
		list_for_each_entry(logtype_entry, &(logmgr->known_logtypes_list), list) {
			if (logtype_entry->type == type) {
				log_dsc->ops = logtype_entry->ops;
				assert(log_dsc->ops->alloc(log_dsc) == M_R_SUCCESS);
				break;
			}
		}
		if (!log_dsc->ops) {
			/* unknown type */
			rv = M_R_FAILURE;
			goto out;
		}
	} else {
		/* 
		 * TODO: there might be an available log in the free list but 
		 * be of different type. Need to get one out of the free list 
		 * and clean it.
		 */
		rv = M_R_FAILURE;
		goto out;
	}

	list_del_init(&(log_dsc->list));
	list_add_tail(&(log_dsc->list), &(logmgr->active_logs_list));

	/* Finally, initialize the log */
	assert(log_dsc->ops && log_dsc->ops->init);
	assert(log_dsc->ops->init(log_dsc->log, log_dsc) == M_R_SUCCESS);
	PCM_STORE(&(log_dsc->nvmd->generic_flags), (log_dsc->nvmd->generic_flags & ~LF_TYPE_MASK) | type);
	PCM_BARRIER


	*log_dscp = log_dsc;
	rv = M_R_SUCCESS;
out:
	pthread_mutex_unlock(&logmgr_init_lock);
	return rv;
}
