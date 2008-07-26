/* Copyright (C) 2003-2007 Datapark corp. All rights reserved.
   Based on Electric Fence 2.2 by Bruce Perens

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
*/
/*
 * Electric Fence - Red-Zone memory allocator.
 * Bruce Perens, 1988, 1993
 * 
 * This is a special version of malloc() and company for debugging software
 * that is suspected of overrunning or underrunning the boundaries of a
 * malloc buffer, or touching free memory.
 *
 * It arranges for each malloc buffer to be followed (or preceded)
 * in the address space by an inaccessable virtual memory page,
 * and for free memory to be inaccessable. If software touches the
 * inaccessable page, it will get an immediate segmentation
 * fault. It is then trivial to uncover the offending code using a debugger.
 *
 * An advantage of this product over most malloc debuggers is that this one
 * detects reading out of bounds as well as writing, and this one stops on
 * the exact instruction that causes the error, rather than waiting until the
 * next boundary check.
 *
 * There is one product that debugs malloc buffer overruns
 * better than Electric Fence: "Purify" from Purify Systems, and that's only
 * a small part of what Purify does. I'm not affiliated with Purify, I just
 * respect a job well done.
 *
 * This version of malloc() should not be linked into production software,
 * since it tremendously increases the time and memory overhead of malloc().
 * Each malloc buffer will consume a minimum of two virtual memory pages,
 * this is 16 kilobytes on many systems. On some systems it will be necessary
 * to increase the amount of swap space in order to debug large programs that
 * perform lots of allocation, because of the per-buffer overhead.
 */
#include "dps_common.h"

#ifdef EFENCE

#include "dps_efence.h"
#include "dps_charsetutils.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <memory.h>
#include <string.h>
#ifdef HAVE_PTHREAD
# include <pthread.h>
# include <semaphore.h>
#endif

#ifdef __linux__
#include <sys/time.h>
#include <sys/resource.h>
#endif

/*
#ifdef	malloc
#undef	malloc
#endif

#ifdef	calloc
#undef	calloc
#endif
*/
static const char	version[] = "\n  Internal memory debugger based on Electric Fence 2.2.0"
 " Copyright (C) 1987-1999 Bruce Perens\n";

/*
 * MEMORY_CREATION_SIZE is the amount of memory to get from the operating
 * system at one time. We'll break that memory down into smaller pieces for
 * malloc buffers. One megabyte is probably a good value.
 */
#define			MEMORY_CREATION_SIZE	1 * 1024 * 1024

/*
 * Enum Mode indicates the status of a malloc buffer.
 */
enum _Mode {
	NOT_IN_USE = 0,	/* Available to represent a malloc buffer. */
	FREE,		/* A free buffer. */
	ALLOCATED,	/* A buffer that is in use. */
	PROTECTED,	/* A freed buffer that can not be allocated again. */
	INTERNAL_USE	/* A buffer used internally by malloc(). */
};
typedef enum _Mode	Mode;

#define DPS_FILENAMELEN 32

/*
 * Struct Slot contains all of the information about a malloc buffer except
 * for the contents of its memory.
 */
struct _Slot {
	void *		userAddress;
	void *		internalAddress;
	size_t		userSize;
	size_t		internalSize;
	Mode		mode;
        char            filename[DPS_FILENAMELEN];
        size_t          fileline;
};
typedef struct _Slot	Slot;

/*
 * EF_ALIGNMENT is a global variable used to control the default alignment
 * of buffers returned by malloc(), calloc(), and realloc(). It is all-caps
 * so that its name matches the name of the environment variable that is used
 * to set it. This gives the programmer one less name to remember.
 * If the value is -1, it will be set from the environment or sizeof(int)
 * at run time.
 */
int		EF_ALIGNMENT = -1;

/*
 * EF_PROTECT_FREE is a global variable used to control the disposition of
 * memory that is released using free(). It is all-caps so that its name
 * matches the name of the environment variable that is used to set it.
 * If its value is greater non-zero, memory released by free is made
 * inaccessable and never allocated again. Any software that touches free
 * memory will then get a segmentation fault. If its value is zero, freed
 * memory will be available for reallocation, but will still be inaccessable
 * until it is reallocated.
 * If the value is -1, it will be set from the environment or to 0 at run-time.
 */
int		EF_PROTECT_FREE = -1;

/*
 * EF_PROTECT_BELOW is used to modify the behavior of the allocator. When
 * its value is non-zero, the allocator will place an inaccessable page
 * immediately _before_ the malloc buffer in the address space, instead
 * of _after_ it. Use this to detect malloc buffer under-runs, rather than
 * over-runs. It won't detect both at the same time, so you should test your
 * software twice, once with this value clear, and once with it set.
 * If the value is -1, it will be set from the environment or to zero at
 * run-time
 */
int		EF_PROTECT_BELOW = -1;

/*
 * EF_ALLOW_MALLOC_0 is set if Electric Fence is to allow malloc(0). I
 * trap malloc(0) by default because it is a common source of bugs.
 */
int		EF_ALLOW_MALLOC_0 = -1;

/*
 * EF_FILL is set to 0-255 if Electric Fence should fill all new allocated
 * memory with the specified value.
 */
int		EF_FILL = -1;

/*
 * allocationList points to the array of slot structures used to manage the
 * malloc arena.
 */
static Slot *		allocationList = 0;

/*
 * allocationListSize is the size of the allocation list. This will always
 * be a multiple of the page size.
 */
static size_t		allocationListSize = 0;

/*
 * slotCount is the number of Slot structures in allocationList.
 */
static size_t		slotCount = 0;

/*
 * unUsedSlots is the number of Slot structures that are currently available
 * to represent new malloc buffers. When this number gets too low, we will
 * create new slots.
 */
static size_t		unUsedSlots = 0;

/*
 * slotsPerPage is the number of slot structures that fit in a virtual
 * memory page.
 */
static size_t		slotsPerPage = 0;

/*
 * internalUse is set when allocating and freeing the allocatior-internal
 * data structures.
 */
static int		internalUse = 0;

/*
 * noAllocationListProtection is set to tell malloc() and free() not to
 * manipulate the protection of the allocation list. This is only set in
 * realloc(), which does it to save on slow system calls, and in
 * allocateMoreSlots(), which does it because it changes the allocation list.
 */
static int		noAllocationListProtection = 0;

#ifdef HAVE_PTHREAD
/*
 * EF_sem is a semaphore used to allow one thread at a time into
 * these routines.
 * Also, we use semEnabled as a boolean to see if we should be
 * using the semaphore.
 * semThread is set to the thread id of the thread that currently
 * has the semaphore so that when/if it tries to get the semaphore
 * again (realloc calling malloc/free) - nothing will happen to the
 * semaphore.
 * semDepth is used to keep track of how many times the same thread
 * gets the semaphore - so we know when it is actually freed.
 */
static sem_t      EF_sem = { 0 };
static int        semEnabled = 0;
static pthread_t  semThread = (pthread_t) -1;
static int        semDepth = 0;
#endif

/*
 * bytesPerPage is set at run-time to the number of bytes per virtual-memory
 * page, as returned by Page_Size().
 */
static size_t		bytesPerPage = 0;

/*
static int cmp_Slot(const Slot *s1, const Slot *s2) {
  if (s1->userAddress < s2->userAddress) return -1;
  if (s1->userAddress > s2->userAddress) return 1;
  return 0;
}
*/

static void lock(void) {
#ifdef HAVE_PTHREAD
	/* Are we using a semaphore? */
	if (!semEnabled)
		return;

	/* Do we already have the semaphore? */
	if (semThread == pthread_self()) {
		/* Increment semDepth - push one stack level */
		semDepth++;
		return;
	}

	/* Wait for the semaphore. */
	while (sem_wait(&EF_sem) < 0)
		/* try again */;

	/* Let everyone know who has the semaphore. */
	semThread = pthread_self();
	semDepth++;
#endif	/* HAVE_PTHREAD */
}

static void release(void) {
#ifdef	HAVE_PTHREAD
	/* Are we using a semaphore? */
	if (!semEnabled)
		return;

	/* Do we have the semaphore?  Cannot free it if we don't. */
	if (semThread != pthread_self()) {
		if ( semThread == 0 )
			EF_InternalError(
			 "Releasing semaphore that wasn't locked.");

		else
			EF_InternalError(
			 "Semaphore doesn't belong to thread.");
	}

	/* Make sure this is positive as well. */
	if (semDepth <= 0)
		EF_InternalError("Semaphore depth");
	/* Decrement semDepth - popping one stack level */
	semDepth--;

	/* Only actually free the semaphore when we've reached the top */
	/* of our call stack. */
	if (semDepth == 0) {
		/* Zero this before actually free'ing the semaphore. */
		semThread = (pthread_t) 0;
		if (sem_post(&EF_sem) < 0)
			EF_InternalError("Failed to post the semaphore.");
	}
#endif /* HAVE_PTHREAD */
}

/*
 * initialize sets up the memory allocation arena and the run-time
 * configuration information.
 */
static void initialize(void) {
	size_t	size = MEMORY_CREATION_SIZE;
	size_t	slack;
	char *	string;
	Slot *	slot;

	EF_Print(version);

#ifdef __linux__
	{
	  struct rlimit nolimit = { RLIM_INFINITY, RLIM_INFINITY };
	  int rc = setrlimit( RLIMIT_AS, &nolimit);
	}
#endif

	lock();

	/*
	 * Import the user's environment specification of the default
	 * alignment for malloc(). We want that alignment to be under
	 * user control, since smaller alignment lets us catch more bugs,
	 * however some software will break if malloc() returns a buffer
	 * that is not word-aligned.
	 *
	 * I would like
	 * alignment to be zero so that we could catch all one-byte
	 * overruns, however if malloc() is asked to allocate an odd-size
	 * buffer and returns an address that is not word-aligned, or whose
	 * size is not a multiple of the word size, software breaks.
	 * This was the case with the Sun string-handling routines,
	 * which can do word fetches up to three bytes beyond the end of a
	 * string. I handle this problem in part by providing
	 * byte-reference-only versions of the string library functions, but
	 * there are other functions that break, too. Some in X Windows, one
	 * in Sam Leffler's TIFF library, and doubtless many others.
	 */
	if ( EF_ALIGNMENT == -1 ) {
		if ( (string = getenv("EF_ALIGNMENT")) != 0 )
			EF_ALIGNMENT = (size_t)atoi(string);
		else
			EF_ALIGNMENT = sizeof(int);
	}

	/*
	 * See if the user wants to protect the address space below a buffer,
	 * rather than that above a buffer.
	 */
	if ( EF_PROTECT_BELOW == -1 ) {
		if ( (string = getenv("EF_PROTECT_BELOW")) != 0 )
			EF_PROTECT_BELOW = (atoi(string) != 0);
		else
			EF_PROTECT_BELOW = 0;
	}

	/*
	 * See if the user wants to protect memory that has been freed until
	 * the program exits, rather than until it is re-allocated.
	 */
	if ( EF_PROTECT_FREE == -1 ) {
		if ( (string = getenv("EF_PROTECT_FREE")) != 0 )
			EF_PROTECT_FREE = (atoi(string) != 0);
		else
			EF_PROTECT_FREE = 0;
	}

	/*
	 * See if the user wants to allow malloc(0).
	 */
	if ( EF_ALLOW_MALLOC_0 == -1 ) {
		if ( (string = getenv("EF_ALLOW_MALLOC_0")) != 0 )
			EF_ALLOW_MALLOC_0 = (atoi(string) != 0);
		else
			EF_ALLOW_MALLOC_0 = 0;
	}

	
	/*
	 * Check if we should be filling new memory with a value.
	 */
	if ( EF_FILL == -1 ) {
		if ( (string = getenv("EF_FILL")) != 0)
			EF_FILL = (unsigned char) atoi(string);
	}

	/*
	 * Get the run-time configuration of the virtual memory page size.
 	 */
	bytesPerPage = Page_Size();

	/*
	 * Figure out how many Slot structures to allocate at one time.
	 */
	slotCount = slotsPerPage = bytesPerPage / sizeof(Slot);
	allocationListSize = bytesPerPage;

	if ( allocationListSize > size )
		size = allocationListSize;

	if ( (slack = size % bytesPerPage) != 0 )
		size += bytesPerPage - slack;

	/*
	 * Allocate memory, and break it up into two malloc buffers. The
	 * first buffer will be used for Slot structures, the second will
	 * be marked free.
	 */
	slot = allocationList = (Slot *)Page_Create(size);
	memset((char *)allocationList, 0, allocationListSize);

	slot[0].internalSize = slot[0].userSize = allocationListSize;
	slot[0].internalAddress = slot[0].userAddress = allocationList;
	slot[0].mode = INTERNAL_USE;
	if ( size > allocationListSize ) {
		slot[1].internalAddress = slot[1].userAddress
		 = ((char *)slot[0].internalAddress) + slot[0].internalSize;
		slot[1].internalSize
		 = slot[1].userSize = size - slot[0].internalSize;
		slot[1].mode = FREE;
	}

	/*
	 * Deny access to the free page, so that we will detect any software
	 * that treads upon free memory.
	 */
	Page_DenyAccess(slot[1].internalAddress, slot[1].internalSize);

	/*
	 * Account for the two slot structures that we've used.
	 */
	unUsedSlots = slotCount - 2;

/*	if (slotCount > 1) DpsSort(allocationList, slotCount, sizeof(Slot), (qsort_cmp)cmp_Slot);*/

	release();
#ifdef HAVE_PTHREAD
	if (!semEnabled) {
		semEnabled = 1;
		if (sem_init(&EF_sem, 0, 1) < 0) {
		  semEnabled = 0;
		}
	}
#endif
}

/*
 * allocateMoreSlots is called when there are only enough slot structures
 * left to support the allocation of a single malloc buffer.
 */
static void
allocateMoreSlots(void)
{
	size_t	newSize = allocationListSize + bytesPerPage;
	void *	newAllocation;
	void *	oldAllocation = allocationList;

	Page_AllowAccess(allocationList, allocationListSize);
	noAllocationListProtection = 1;
	internalUse = 1;

	newAllocation = DpsMalloc(newSize);
	dps_memmove(newAllocation, allocationList, allocationListSize);
	memset(&(((char *)newAllocation)[allocationListSize]), 0, bytesPerPage);

	allocationList = (Slot *)newAllocation;
	allocationListSize = newSize;
	slotCount += slotsPerPage;
	unUsedSlots += slotsPerPage;

/*	DpsSort(allocationList, slotCount, sizeof(Slot), (qsort_cmp)cmp_Slot);*/
	DpsFree(oldAllocation);

	/*
	 * Keep access to the allocation list open at this point, because
	 * I am returning to memalign(), which needs that access.
 	 */
	noAllocationListProtection = 0;
	internalUse = 0;
}



/*
 * This is the memory allocator. When asked to allocate a buffer, allocate
 * it in such a way that the end of the buffer is followed by an inaccessable
 * memory page. If software overruns that buffer, it will touch the bad page
 * and get an immediate segmentation fault. It's then easy to zero in on the
 * offending code with a debugger.
 *
 * There are a few complications. If the user asks for an odd-sized buffer,
 * we would have to have that buffer start on an odd address if the byte after
 * the end of the buffer was to be on the inaccessable page. Unfortunately,
 * there is lots of software that asks for odd-sized buffers and then
 * requires that the returned address be word-aligned, or the size of the
 * buffer be a multiple of the word size. An example are the string-processing
 * functions on Sun systems, which do word references to the string memory
 * and may refer to memory up to three bytes beyond the end of the string.
 * For this reason, I take the alignment requests to memalign() and valloc()
 * seriously, and 
 * 
 * Electric Fence wastes lots of memory. I do a best-fit allocator here
 * so that it won't waste even more. It's slow, but thrashing because your
 * working set is too big for a system's RAM is even slower. 
 */
static void * _DpsMemalign(size_t alignment, size_t userSize, char *filename, size_t fileline) {
        register Slot   *slot, *slot2;
	register size_t	count;
	Slot *		fullSlot = 0;
	Slot *		emptySlots[2];
	size_t		internalSize;
	size_t		slack;
	char *		address;

	if ( allocationList == 0 )
		initialize();

	lock();

	if ( userSize == 0 && !EF_ALLOW_MALLOC_0 )
	  EF_Abort("Allocating 0 bytes, probably a bug at %s:%d.", filename, fileline);

	/*
	 * If EF_PROTECT_BELOW is set, all addresses returned by malloc()
	 * and company will be page-aligned.
 	 */
	if ( !EF_PROTECT_BELOW && alignment > 1 ) {
		if ( (slack = userSize % alignment) != 0 )
			userSize += alignment - slack;
	}

	/*
	 * The internal size of the buffer is rounded up to the next page-size
	 * boudary, and then we add another page's worth of memory for the
	 * dead page.
	 */
	internalSize = userSize + bytesPerPage;
	if ( (slack = internalSize % bytesPerPage) != 0 )
		internalSize += bytesPerPage - slack;

	/*
	 * These will hold the addresses of two empty Slot structures, that
	 * can be used to hold information for any memory I create, and any
	 * memory that I mark free.
	 */
	emptySlots[0] = 0;
	emptySlots[1] = 0;

	/*
	 * The internal memory used by the allocator is currently
	 * inaccessable, so that errant programs won't scrawl on the
	 * allocator's arena. I'll un-protect it here so that I can make
	 * a new allocation. I'll re-protect it before I return.
 	 */
	if ( !noAllocationListProtection )
		Page_AllowAccess(allocationList, allocationListSize);

	/*
	 * If I'm running out of empty slots, create some more before
	 * I don't have enough slots left to make an allocation.
	 */
	if ( !internalUse && unUsedSlots < 7 ) {
		allocateMoreSlots();
	}
	
	/*
	 * Iterate through all of the slot structures. Attempt to find a slot
	 * containing free memory of the exact right size. Accept a slot with
	 * more memory than we want, if the exact right size is not available.
	 * Find two slot structures that are not in use. We will need one if
	 * we split a buffer into free and allocated parts, and the second if
	 * we have to create new memory and mark it as free.
	 *
	 */
	slot = allocationList; slot2 = &slot[slotCount - 1];
	while (slot <= slot2) {
		if ( slot->mode == FREE
		 && slot->internalSize >= internalSize ) {
			if ( !fullSlot
			 ||slot->internalSize < fullSlot->internalSize){
				fullSlot = slot;
				if ( slot->internalSize == internalSize
				 && emptySlots[0] )
					break;	/* All done, */
			}
		}
		else if ( slot->mode == NOT_IN_USE ) {
			if ( !emptySlots[0] )
				emptySlots[0] = slot;
			else if ( !emptySlots[1] )
				emptySlots[1] = slot;
			else if ( fullSlot
			 && fullSlot->internalSize == internalSize )
				break;	/* All done. */
		}
		
		if ( slot2->mode == FREE
		 && slot2->internalSize >= internalSize ) {
			if ( !fullSlot
			 ||slot2->internalSize < fullSlot->internalSize){
				fullSlot = slot2;
				if ( slot2->internalSize == internalSize
				 && emptySlots[0] )
					break;	/* All done, */
			}
		}
		else if ( slot2->mode == NOT_IN_USE ) {
			if ( !emptySlots[0] )
				emptySlots[0] = slot2;
			else if ( !emptySlots[1] )
				emptySlots[1] = slot2;
			else if ( fullSlot
			 && fullSlot->internalSize == internalSize )
				break;	/* All done. */
		}
		slot++; slot2--;
	}

/*	
	for ( slot = allocationList, count = slotCount ; count > 0; count-- ) {
		if ( slot->mode == FREE
		 && slot->internalSize >= internalSize ) {
			if ( !fullSlot
			 ||slot->internalSize < fullSlot->internalSize){
				fullSlot = slot;
				if ( slot->internalSize == internalSize
				 && emptySlots[0] )
					break;	*//* All done, *//*
			}
		}
		else if ( slot->mode == NOT_IN_USE ) {
			if ( !emptySlots[0] )
				emptySlots[0] = slot;
			else if ( !emptySlots[1] )
				emptySlots[1] = slot;
			else if ( fullSlot
			 && fullSlot->internalSize == internalSize )
				break;	*//* All done. *//*
		}
		slot++;
	}
*/
	if ( !emptySlots[0] )
		EF_InternalError("No empty slot 0.");

	if ( !fullSlot ) {
		/*
		 * I get here if I haven't been able to find a free buffer
		 * with all of the memory I need. I'll have to create more
		 * memory. I'll mark it all as free, and then split it into
		 * free and allocated portions later.
		 */
		size_t	chunkSize = MEMORY_CREATION_SIZE;

		if ( !emptySlots[1] )
			EF_InternalError("No empty slot 1.");

		if ( chunkSize < internalSize )
			chunkSize = internalSize;

		if ( (slack = chunkSize % bytesPerPage) != 0 )
			chunkSize += bytesPerPage - slack;

		/* Use up one of the empty slots to make the full slot. */
		fullSlot = emptySlots[0];
		emptySlots[0] = emptySlots[1];
		fullSlot->internalAddress = Page_Create(chunkSize);
		fullSlot->internalSize = chunkSize;
		fullSlot->mode = FREE;
		unUsedSlots--;
		
		/* Fill the slot if it was specified to do so. */
		if ( EF_FILL != -1 )
			memset(
			 (char *)fullSlot->internalAddress
			,EF_FILL
			,chunkSize);
	}
  
	/*
	 * If I'm allocating memory for the allocator's own data structures,
	 * mark it INTERNAL_USE so that no errant software will be able to
	 * free it.
	 */
	if ( internalUse )
		fullSlot->mode = INTERNAL_USE;
	else
		fullSlot->mode = ALLOCATED;

	/*
	 * If the buffer I've found is larger than I need, split it into
	 * an allocated buffer with the exact amount of memory I need, and
	 * a free buffer containing the surplus memory.
	 */
	if ( fullSlot->internalSize > internalSize ) {
		emptySlots[0]->internalSize
		 = fullSlot->internalSize - internalSize;
		emptySlots[0]->internalAddress
		 = ((char *)fullSlot->internalAddress) + internalSize;
		emptySlots[0]->mode = FREE;
		fullSlot->internalSize = internalSize;
		unUsedSlots--;
	}

	if ( !EF_PROTECT_BELOW ) {
		/*
		 * Arrange the buffer so that it is followed by an inaccessable
		 * memory page. A buffer overrun that touches that page will
		 * cause a segmentation fault.
		 */
		address = (char *)fullSlot->internalAddress;

		/* Set up the "live" page. */
		if ( internalSize - bytesPerPage > 0 )
				Page_AllowAccess(
				 fullSlot->internalAddress
				,internalSize - bytesPerPage);
			
		address += internalSize - bytesPerPage;

		/* Set up the "dead" page. */
		if ( EF_PROTECT_FREE )
			Page_Delete(address, bytesPerPage);
		else
			Page_DenyAccess(address, bytesPerPage);

		/* Figure out what address to give the user. */
		address -= userSize;
	}
	else {	/* EF_PROTECT_BELOW != 0 */
		/*
		 * Arrange the buffer so that it is preceded by an inaccessable
		 * memory page. A buffer underrun that touches that page will
		 * cause a segmentation fault.
		 */
		address = (char *)fullSlot->internalAddress;

		/* Set up the "dead" page. */
		if ( EF_PROTECT_FREE )
			Page_Delete(address, bytesPerPage);
		else
			Page_DenyAccess(address, bytesPerPage);
			
		address += bytesPerPage;

		/* Set up the "live" page. */
		if ( internalSize - bytesPerPage > 0 )
			Page_AllowAccess(address, internalSize - bytesPerPage);
	}

	fullSlot->userAddress = address;
	fullSlot->userSize = userSize;
	fullSlot->fileline = fileline;
	dps_strncpy(fullSlot->filename, filename, DPS_FILENAMELEN);

/*	if (slotCount > 1) DpsSort(allocationList, slotCount, sizeof(Slot), (qsort_cmp)cmp_Slot);*/

	/*
	 * Make the pool's internal memory inaccessable, so that the program
	 * being debugged can't stomp on it.
	 */
	if ( !internalUse )
		Page_DenyAccess(allocationList, allocationListSize);

	release();

/*	if (address == 0x292d3000) { 
	  int r = 1 / 0; 
	  printf("Error r:%d\n");
	}*/

	return address;
}

/*
 * Find the slot structure for a user address.
 */
static Slot *
slotForUserAddress(void * address) {
	register Slot *	slot = allocationList;
	register Slot * slot2 = &allocationList[slotCount - 1];
/*	register size_t	count = slotCount;*/


	while(slot <= slot2) {
		if ( slot->userAddress == address )
			return slot;
		slot++;
		if ( slot2->userAddress == address )
			return slot2;
		slot2--;
	}


/*
	for ( ; count > 0; count-- ) {
		if ( slot->userAddress == address )
			return slot;
		slot++;
	}
*/
	return 0;

}

/*
 * Find the slot structure for an internal address.
 */
static Slot *
slotForInternalAddress(void * address) {
	register Slot *	slot = allocationList;
	register Slot * slot2 = &allocationList[slotCount - 1];

	while(slot <= slot2) {
		if ( slot->internalAddress == address )
			return slot;
		slot++;
		if ( slot2->internalAddress == address )
			return slot2;
		slot2--;
	}
/*
	register size_t	count = slotCount;
	
	for ( ; count > 0; count-- ) {
		if ( slot->internalAddress == address )
			return slot;
		slot++;
	}
*/

	return 0;
}

/*
 * Given the internal address of a buffer, find the buffer immediately
 * before that buffer in the address space. This is used by free() to
 * coalesce two free buffers into one.
 */
static Slot *
slotForInternalAddressPreviousTo(void * address) {
	register Slot *	slot = allocationList;
	register Slot * slot2 = &allocationList[slotCount - 1];

	while(slot <= slot2) {
		if ( ((char *)slot->internalAddress)
		 + slot->internalSize == address )
			return slot;
		slot++;
		if ( ((char *)slot2->internalAddress)
		 + slot2->internalSize == address )
			return slot2;
		slot2--;
	}
/*
	register size_t	count = slotCount;
	
	for ( ; count > 0; count-- ) {
		if ( ((char *)slot->internalAddress)
		 + slot->internalSize == address )
			return slot;
		slot++;
	}
*/
	return 0;
}

extern C_LINKAGE void _DpsFree(void * address, char *filename, size_t fileline) {
	Slot *	slot;
	Slot *	previousSlot = 0;
	Slot *	nextSlot = 0;

	if ( address == 0 )
		return;

/*	fprintf(stderr, "DpsFree: %x at %s:%d\n", address, filename, fileline);*/

	if ( allocationList == 0 )
	  EF_Abort("DpsFree() called before first DpsMalloc() at %s:%d.", filename, fileline);

	lock();

	if ( !noAllocationListProtection )
		Page_AllowAccess(allocationList, allocationListSize);

	slot = slotForUserAddress(address);

	if ( !slot )
	  EF_Abort("DpsFree(%a): address not from DpsMalloc() at %s:%d.", address, filename, fileline);

	if ( slot->mode != ALLOCATED ) {
		if ( internalUse && slot->mode == INTERNAL_USE )
			/* Do nothing. */;
		else {
		  EF_Abort("DpsFree(%a): freeing free memory at %s:%d.", address, filename, fileline);
		}
	}

	if ( EF_PROTECT_FREE )
		slot->mode = NOT_IN_USE /*PROTECTED*/;
	else
		slot->mode = FREE;

	/*
	 * Free memory is _always_ set to deny access. When EF_PROTECT_FREE
	 * is true, free memory is never reallocated, so it remains access
	 * denied for the life of the process. When EF_PROTECT_FREE is false, 
	 * the memory may be re-allocated, at which time access to it will be
	 * allowed again.
	 *
	 * Some operating systems allow munmap() with single-page resolution,
	 * and allow you to un-map portions of a region, rather than the
	 * entire region that was mapped with mmap(). On those operating
	 * systems, we can release protected free pages with Page_Delete(),
	 * in the hope that the swap space attached to those pages will be
	 * released as well.
	 */
	if ( EF_PROTECT_FREE )
	    Page_Delete(slot->internalAddress, slot->internalSize);
	else
	    Page_DenyAccess(slot->internalAddress, slot->internalSize);

	previousSlot = slotForInternalAddressPreviousTo(slot->internalAddress);
	nextSlot = slotForInternalAddress(
	 ((char *)slot->internalAddress) + slot->internalSize);

	if ( previousSlot && previousSlot->mode == slot->mode ) {
		/* Coalesce previous slot with this one. */
		previousSlot->internalSize += slot->internalSize;
		slot->internalAddress = slot->userAddress = 0;
		slot->internalSize = slot->userSize = 0;
		slot->mode = NOT_IN_USE;
		slot = previousSlot;
		unUsedSlots++;
	}
	if ( nextSlot && nextSlot->mode == slot->mode ) {
		/* Coalesce next slot with this one. */
		slot->internalSize += nextSlot->internalSize;
		nextSlot->internalAddress = nextSlot->userAddress = 0;
		nextSlot->internalSize = nextSlot->userSize = 0;
		nextSlot->mode = NOT_IN_USE;
		unUsedSlots++;
	}

	slot->userAddress = slot->internalAddress;
	slot->userSize = slot->internalSize;

	if ( !noAllocationListProtection )
		Page_DenyAccess(allocationList, allocationListSize);

	release();

/*	fprintf(stderr, "DpsFree Done\n");*/
}

extern C_LINKAGE void * _DpsRealloc(void * oldBuffer, size_t newSize, char *filename, size_t fileline) {
	void *	newBuffer = 0;

	if ( allocationList == 0 )
		initialize();	/* This sets EF_ALIGNMENT */

/*	fprintf(stderr, "DpsRealloc: %x at %s:%d\n", oldBuffer, filename, fileline);*/

	lock();

	newBuffer = _DpsMalloc(newSize, filename, fileline);

	if ( oldBuffer ) {
		size_t	size;
		Slot *	slot;


		Page_AllowAccess(allocationList, allocationListSize);
		noAllocationListProtection = 1;
		
		slot = slotForUserAddress(oldBuffer);

		if ( slot == 0 )
		  EF_Abort("DpsRealloc(%a, %d): address not from DpsMalloc() at %s:%d.", oldBuffer, newSize, filename, fileline);

		if ( newSize < (size = slot->userSize) )
			size = newSize;

		if ( size > 0 )
			dps_memmove(newBuffer, oldBuffer, size);

		DpsFree(oldBuffer);
		noAllocationListProtection = 0;
		Page_DenyAccess(allocationList, allocationListSize);

		if ( size < newSize )
			memset(&(((char *)newBuffer)[size]), 0, newSize - size);
		
		/* Internal memory was re-protected in free() */
	}

	release();

	return newBuffer;
}

extern C_LINKAGE void *_DpsMalloc(size_t size, char *filename, size_t fileline) {
	if ( allocationList == 0 )
		initialize();	/* This sets EF_ALIGNMENT */

	return _DpsMemalign(EF_ALIGNMENT, size, filename, fileline);
}

extern C_LINKAGE void *_DpsCalloc(size_t nelem, size_t elsize, char *filename, size_t fileline) {
	size_t	size = nelem * elsize;
	void *	allocation = _DpsMalloc(size, filename, fileline);

	memset(allocation, 0, size);
	return allocation;
}

/*
 * This will catch more bugs if you remove the page alignment, but it
 * will break some software.
 */
extern C_LINKAGE void *_DpsValloc (size_t size, char *filename, size_t fileline) {
	return _DpsMemalign(bytesPerPage, size, filename, fileline);
}

/*
*/
extern C_LINKAGE char * _DpsStrdup(const char *str, char *filename, size_t fileline) {
        size_t len;
        char *copy;

        len = dps_strlen(str) + 1;
        if ((copy = _DpsMalloc(len, filename, fileline)) == NULL)
                return (NULL);
        dps_memmove(copy, str, len);
        return (copy);
}


extern C_LINKAGE void DpsEfenceCheckLeaks(void) {
	register Slot *	slot = allocationList;
	register size_t	count = slotCount;

	if ( allocationList == 0 )
		EF_Abort("DpsEfenceCheckLeaks() called before first DpsMalloc().");

	lock();

	if ( !noAllocationListProtection )
		Page_AllowAccess(allocationList, allocationListSize);


	for ( ; count > 0; count-- ) {
	  if ( slot->mode == ALLOCATED ) {
	    fprintf(stderr, "Unfried memory at 0x%x size:%d at %s:%d\n", slot->userAddress, slot->userSize, slot->filename, slot->fileline);
	  }
	  slot++;
	}


	if ( !noAllocationListProtection )
		Page_DenyAccess(allocationList, allocationListSize);

	release();

  return;
}



#ifdef __hpux
/*
 * HP-UX 8/9.01 strcat reads a word past source when doing unaligned copies!
 * Work around it here. The bug report has been filed with HP.
 */
char *strcat(char *d, const char *s)
{
	dps_strcpy(d + dps_strlen(d), s);
	return d;
}
#endif


#endif /* EFENCE */
