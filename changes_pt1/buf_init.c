/*-------------------------------------------------------------------------
 *
 * buf_init.c
 *	  buffer manager initialization routines
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_init.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/bufmgr.h"
#include "storage/buf_internals.h"


BufferDescPadded *BufferDescriptors;
char	   *BufferBlocks;
LWLockMinimallyPadded *BufferIOLWLockArray = NULL;
LWLockTranche BufferIOLWLockTranche;
LWLockTranche BufferContentLWLockTranche;
WritebackContext BackendWritebackContext;
CkptSortItem *CkptBufferIds;


/*
 * Data Structures:
 *		buffers live in a freelist and a lookup data structure.
 *
 *
 * Buffer Lookup:
 *		Two important notes.  First, the buffer has to be
 *		available for lookup BEFORE an IO begins.  Otherwise
 *		a second process trying to read the buffer will
 *		allocate its own copy and the buffer pool will
 *		become inconsistent.
 *
 * Buffer Replacement:
 *		see freelist.c.  A buffer cannot be replaced while in
 *		use either by data manager or during IO.
 *
 *
 * Synchronization/Locking:
 *
 * IO_IN_PROGRESS -- this is a flag in the buffer descriptor.
 *		It must be set when an IO is initiated and cleared at
 *		the end of the IO.  It is there to make sure that one
 *		process doesn't start to use a buffer while another is
 *		faulting it in.  see WaitIO and related routines.
 *
 * refcount --	Counts the number of processes holding pins on a buffer.
 *		A buffer is pinned during IO and immediately after a BufferAlloc().
 *		Pins must be released before end of transaction.  For efficiency the
 *		shared refcount isn't increased if an individual backend pins a buffer
 *		multiple times. Check the PrivateRefCount infrastructure in bufmgr.c.
 */


/*
 * Initialize shared buffer pool
 *
 * This is called once during shared-memory initialization (either in the
 * postmaster, or in a standalone backend).
 */
void
InitBufferPool(void)
{
	bool		foundBufs,
				foundDescs,
				foundIOLocks,
				foundBufCkpt;

	/* Align descriptors to a cacheline boundary. */
	BufferDescriptors = (BufferDescPadded *)
		ShmemInitStruct("Buffer Descriptors",
						NBuffers * sizeof(BufferDescPadded),
						&foundDescs);

	BufferBlocks = (char *)
		ShmemInitStruct("Buffer Blocks",
						NBuffers * (Size) BLCKSZ, &foundBufs);

	/* Align lwlocks to cacheline boundary */
	BufferIOLWLockArray = (LWLockMinimallyPadded *)
		ShmemInitStruct("Buffer IO Locks",
						NBuffers * (Size) sizeof(LWLockMinimallyPadded),
						&foundIOLocks);

	BufferIOLWLockTranche.name = "buffer_io";
	BufferIOLWLockTranche.array_base = BufferIOLWLockArray;
	BufferIOLWLockTranche.array_stride = sizeof(LWLockMinimallyPadded);
	LWLockRegisterTranche(LWTRANCHE_BUFFER_IO_IN_PROGRESS,
						  &BufferIOLWLockTranche);

	BufferContentLWLockTranche.name = "buffer_content";
	BufferContentLWLockTranche.array_base =
		((char *) BufferDescriptors) + offsetof(BufferDesc, content_lock);
	BufferContentLWLockTranche.array_stride = sizeof(BufferDescPadded);
	LWLockRegisterTranche(LWTRANCHE_BUFFER_CONTENT,
						  &BufferContentLWLockTranche);

	/*
	 * The array used to sort to-be-checkpointed buffer ids is located in
	 * shared memory, to avoid having to allocate significant amounts of
	 * memory at runtime. As that'd be in the middle of a checkpoint, or when
	 * the checkpointer is restarted, memory allocation failures would be
	 * painful.
	 */
	CkptBufferIds = (CkptSortItem *)
		ShmemInitStruct("Checkpoint BufferIds",
						NBuffers * sizeof(CkptSortItem), &foundBufCkpt);

	if (foundDescs || foundBufs || foundIOLocks || foundBufCkpt)
	{
		/* should find all of these, or none of them */
		Assert(foundDescs && foundBufs && foundIOLocks && foundBufCkpt);
		/* note: this path is only taken in EXEC_BACKEND case */
	}
	else
	{
		int			i;

		/*
		 * Initialize all the buffer headers.
		 */
		for (i = 0; i < NBuffers; i++)
		{
			BufferDesc *buf = GetBufferDescriptor(i);
			
			//local timestamps ADDED!!!
			buf->lrutimestamp = 0;
			buf->fifotimestamp = 0;
			
			
			CLEAR_BUFFERTAG(buf->tag);

			pg_atomic_init_u32(&buf->state, 0);
			buf->wait_backend_pid = 0;

			buf->buf_id = i;

			/*
			 * Initially link all the buffers together as unused. Subsequent
			 * management of this list is done by freelist.c.
			 */
			buf->freeNext = i + 1;

			LWLockInitialize(BufferDescriptorGetContentLock(buf),
							 LWTRANCHE_BUFFER_CONTENT);

			LWLockInitialize(BufferDescriptorGetIOLock(buf),
							 LWTRANCHE_BUFFER_IO_IN_PROGRESS);
		}

		/* Correct last entry of linked list */
		GetBufferDescriptor(NBuffers - 1)->freeNext = FREENEXT_END_OF_LIST;
	}

	/* Init other shared buffer-management stuff */
	StrategyInitialize(!foundDescs);

	/* Initialize per-backend file flush context */
	WritebackContextInit(&BackendWritebackContext,
						 &backend_flush_after);
}

/*
 * BufferShmemSize
 *
 * compute the size of shared memory for the buffer pool including
 * data pages, buffer descriptors, hash tables, etc.
 */
Size
BufferShmemSize(void)
{
	Size		size = 0;

	/* size of buffer descriptors */
	size = add_size(size, mul_size(NBuffers, sizeof(BufferDescPadded)));
	/* to allow aligning buffer descriptors */
	size = add_size(size, PG_CACHE_LINE_SIZE);

	/* size of data pages */
	size = add_size(size, mul_size(NBuffers, BLCKSZ));

	/* size of stuff controlled by freelist.c */
	size = add_size(size, StrategyShmemSize());

	/*
	 * It would be nice to include the I/O locks in the BufferDesc, but that
	 * would increase the size of a BufferDesc to more than one cache line,
	 * and benchmarking has shown that keeping every BufferDesc aligned on a
	 * cache line boundary is important for performance.  So, instead, the
	 * array of I/O locks is allocated in a separate tranche.  Because those
	 * locks are not highly contentended, we lay out the array with minimal
	 * padding.
	 */
	size = add_size(size, mul_size(NBuffers, sizeof(LWLockMinimallyPadded)));
	/* to allow aligning the above */
	size = add_size(size, PG_CACHE_LINE_SIZE);

	/* size of checkpoint sort array in bufmgr.c */
	size = add_size(size, mul_size(NBuffers, sizeof(CkptSortItem)));

	return size;
}
