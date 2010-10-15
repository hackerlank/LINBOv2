#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <pthread.h>
#include <errno.h>

#include "log.h"
#include "fifo.h"
#include "socklib.h"
#include "udpc-protoc.h"
#include "udp-receiver.h"
#include "util.h"
#include "statistics.h"
#include "fec.h"

#define DEBUG 0

#define ADR(x, bs) (fifo->dataBuffer + \
	(slice->base+(x)*bs) % fifo->dataBufSize)

#define SLICEMAGIC 0x41424344
#define NR_SLICES 4

typedef enum slice_state {
    SLICE_FREE,
    SLICE_RECEIVING,
    SLICE_DONE
#ifdef BB_FEATURE_UDPCAST_FEC
    ,
    SLICE_FEC,
    SLICE_FEC_DONE
#endif
} slice_state_t;

#ifdef BB_FEATURE_UDPCAST_FEC
struct fec_desc {
    unsigned char *adr; /* address of FEC block */
    int fecBlockNo; /* number of FEC block */
    int erasedBlockNo; /* erased data block */
};
#endif

typedef struct slice {
    int magic;
    volatile slice_state_t state;
    int base; /* base offset of beginning of slice */
    int sliceNo; /* current slice number */
    int blocksTransferred; /* blocks transferred during this slice */
    int dataBlocksTransferred; /* data blocks transferred during this slice */
    int bytes; /* number of bytes in this slice (or 0, if unknown) */
    int bytesKnown; /* is number of bytes known yet? */
    int freePos; /* where the next data part will be stored to */
    struct retransmit retransmit;


    /* How many data blocks are there missing per stripe? */
    short missing_data_blocks[MAX_FEC_INTERLEAVE]; 

#ifdef BB_FEATURE_UDPCAST_FEC
    int fec_stripes; /* number of stripes for FEC */

    /* How many FEC blocs do we have per stripe? */
    short fec_blocks[MAX_FEC_INTERLEAVE];

    struct fec_desc fec_descs[MAX_SLICE_SIZE];
#endif
} *slice_t;

struct clientState {
    struct fifo *fifo;
    struct client_config *client_config;
    struct net_config *net_config;
    union serverDataMsg Msg;

    struct msghdr data_hdr;

    /* pre-prepared messages */
    struct iovec data_iov[2];

    struct slice *currentSlice;
    int currentSliceNo;
    receiver_stats_t stats;
    
    produconsum_t free_slices_pc;
    struct slice slices[NR_SLICES];

    /* Completely received slices */
    int receivedPtr;
    int receivedSliceNo;

    int use_fec; /* do we use forward error correction ? */
    produconsum_t fec_data_pc;
    struct slice *fec_slices[NR_SLICES];
    pthread_t fec_thread;

    /* A reservoir of free blocks for FEC */
    produconsum_t freeBlocks_pc;
    char **blockAddresses; /* adresses of blocks in local queue */

    char **localBlockAddresses; /* local blocks: freed FEC blocks after we
				 * have received the corresponding data */
    int localPos;

    char *blockData;
    char *nextBlock;

    int notifyPipeFd; /* pipe used to notify keyboard thread that we are now 
		       * started */

    int endReached; /* end of transmission reached:
		       0: transmission in progress
		       1: network transmission finished
		       2: network transmission _and_ FEC 
		          processing finished 
		    */

#ifdef BB_FEATURE_UDPCAST_FEC
    fec_code_t fec_code;
#endif
};

static void printMissedBlockMap(struct clientState *clst, slice_t slice)
{
    int i, first=1;
    int blocksInSlice = (slice->bytes +  clst->net_config->blockSize - 1) / 
	clst->net_config->blockSize;

    for(i=0; i< blocksInSlice ; i++) {
	if(!BIT_ISSET(i,slice->retransmit.map)) {
	    if(first)
		fprintf(stderr, "Missed blocks: ");
	    else
		fprintf(stderr, ",");
	    fprintf(stderr, "%d",i);
	    first=0;
	}
    }
    if(!first)
	fprintf(stderr, "\n");
    first=1;
#ifdef BB_FEATURE_UDPCAST_FEC
    if(slice->fec_stripes != 0) {
	for(i=0; i<MAX_SLICE_SIZE; i++) {
	    if(i / slice->fec_stripes < 
	       slice->fec_blocks[i % slice->fec_stripes]) {
		if(first)
		    fprintf(stderr, "FEC blocks: ");
		else
		    fprintf(stderr, ",");
		fprintf(stderr, "%d",slice->fec_descs[i].fecBlockNo);
		first=0;
	    }	    
	}
    }
#endif
    if(!first)
	fprintf(stderr, "\n");
    fprintf(stderr, "Blocks received: %d/%d/%d\n", 
	    slice->dataBlocksTransferred, slice->blocksTransferred,
	    blocksInSlice);
#ifdef BB_FEATURE_UDPCAST_FEC
    for(i=0; i<slice->fec_stripes; i++) {
	fprintf(stderr, "Stripe %2d: %3d/%3d %s\n", i,
		slice->missing_data_blocks[i],
		slice->fec_blocks[i],
		slice->missing_data_blocks[i] > slice->fec_blocks[i] ?
		"**************" :"");
    }
#endif
}

static int sendOk(struct client_config *client_config, unsigned int sliceNo)
{
    struct ok ok;
    unsigned int endianness = client_config->endianness;
    ok.opCode = htoxs(CMD_OK);
    ok.reserved = 0;
    ok.sliceNo = htoxl(sliceNo);
    return SSEND(ok);
}

static int sendRetransmit(struct clientState *clst,
			  struct slice *slice,
			  int rxmit) {
    struct client_config *client_config = clst->client_config;
    int endianness = client_config->endianness;

    assert(slice->magic == SLICEMAGIC);
    slice->retransmit.opCode = htoxs(CMD_RETRANSMIT);
    slice->retransmit.reserved = 0;
    slice->retransmit.sliceNo = htoxl(slice->sliceNo);
    slice->retransmit.rxmit = htoxl(rxmit);
    return SSEND(slice->retransmit);
}


static char *getBlockSpace(struct clientState *clst)
{
    int pos;

    if(clst->localPos) {
	clst->localPos--;
	return clst->localBlockAddresses[clst->localPos];
    }

    pc_consume(clst->freeBlocks_pc, 1);
    pos = pc_getConsumerPosition(clst->freeBlocks_pc);
    pc_consumed(clst->freeBlocks_pc, 1);
    return clst->blockAddresses[pos];
}

#ifdef BB_FEATURE_UDPCAST_FEC
static void freeBlockSpace(struct clientState *clst, char *block)
{
    int pos = pc_getProducerPosition(clst->freeBlocks_pc);
    assert(block != 0);
    clst->blockAddresses[pos] = block;
    pc_produce(clst->freeBlocks_pc, 1);
}
#endif

static void setNextBlock(struct clientState *clst)
{
    clst->nextBlock = getBlockSpace(clst);
}

static struct slice *newSlice(struct clientState *clst, int sliceNo)
{
    struct slice *slice=NULL;
    int i;

#if DEBUG
    flprintf("Getting new slice %d\n", 
	     pc_getConsumerPosition(clst->free_slices_pc));
#endif
    pc_consume(clst->free_slices_pc, 1);
#if DEBUG
    flprintf("Got new slice\n");
#endif
    i = pc_getConsumerPosition(clst->free_slices_pc);
    pc_consumed(clst->free_slices_pc, 1);
    slice = &clst->slices[i];
    assert(slice->state == SLICE_FREE);
    BZERO(*slice);
    slice->magic = SLICEMAGIC;
    slice->state = SLICE_RECEIVING;
    slice->blocksTransferred = 0;
    slice->dataBlocksTransferred = 0;
    BZERO(slice->retransmit.map);
    slice->freePos = 0;
    slice->bytes = 0;
    slice->bytesKnown = 0;
    slice->sliceNo = sliceNo;
    if(clst->currentSlice != NULL && !clst->currentSlice->bytesKnown) {
	udpc_fatal(1, "Previous slice size not known\n");
    }
    if(clst->currentSliceNo != sliceNo-1) {
	udpc_fatal(1, "Slice no mismatch %d <-> %d\n",
		   sliceNo, clst->currentSliceNo);
    }

    /* wait for free data memory */
    slice->base = pc_getConsumerPosition(clst->fifo->freeMemQueue);
#if DEBUG
    if(pc_consume(clst->fifo->freeMemQueue, 0) < 
       clst->net_config->blockSize * MAX_SLICE_SIZE)
	flprintf("Pipeline full\n");
#endif
    pc_consume(clst->fifo->freeMemQueue, 
	       clst->net_config->blockSize * MAX_SLICE_SIZE);
    clst->currentSlice = slice;
    clst->currentSliceNo = sliceNo;
    return slice;
}


static struct slice *findSlice(struct clientState *clst, int sliceNo)
{
    if(sliceNo <= clst->currentSliceNo) {
	struct slice *slice = clst->currentSlice;
	int pos = slice - clst->slices;
	assert(slice == NULL || slice->magic == SLICEMAGIC);
	while(slice->sliceNo != sliceNo) {
	    if(slice->state == SLICE_FREE)
		return NULL;
	    assert(slice->magic == SLICEMAGIC);
	    pos--;
	    if(pos < 0)
		pos += NR_SLICES;
	    slice = &clst->slices[pos];
	}
	return slice;
    }
    if(sliceNo != clst->currentSliceNo + 1) {
	udpc_flprintf("Slice %d skipped\n", sliceNo-1);
	exit(1);
    }
    if(sliceNo > clst->receivedSliceNo + 2) {
	slice_t slice = findSlice(clst, clst->receivedSliceNo+1);
	udpc_flprintf("Dropped by server now=%d last=%d\n", sliceNo,
		      clst->receivedSliceNo);
	if(slice != NULL)
	    printMissedBlockMap(clst, slice);
	exit(1);
    }
    return newSlice(clst, sliceNo);
}

static void setSliceBytes(struct slice *slice, 
			  struct fifo *fifo,
			  int bytes) {
    assert(slice->magic == SLICEMAGIC);
    if(slice->bytesKnown) {
	if(slice->bytes != bytes) {
	    udpc_fatal(1, "Byte number mismatch %d <-> %d\n",
		       bytes, slice->bytes);
	}
    } else {
	slice->bytesKnown = 1;
	slice->bytes = bytes;	    
	pc_consumed(fifo->freeMemQueue, bytes);
    }
}

/**
 * Advance pointer of received slices
 */
static void advanceReceivedPointer(struct clientState *clst) {
    int pos = clst->receivedPtr;
    while(1) {	
	slice_t slice = &clst->slices[pos];
	if(
#ifdef BB_FEATURE_UDPCAST_FEC
	   slice->state != SLICE_FEC &&
#endif
	   slice->state != SLICE_DONE)
	    break;
	pos++;
	clst->receivedSliceNo = slice->sliceNo;
	if(pos >= NR_SLICES)
	    pos -= NR_SLICES;
    }
    clst->receivedPtr = pos;
}



/**
 * Cleans up all finished slices. At first, invoked by the net receiver
 * thread. However, once FEC has become active, net receiver will no longer
 * call it, and instead it will be called by the fec thread.
 */
static void cleanupSlices(struct clientState *clst, unsigned int doneState)
{
    while(1) {	
	int pos = pc_getProducerPosition(clst->free_slices_pc);
	int bytes;
	slice_t slice = &clst->slices[pos];
#if DEBUG
	flprintf("Attempting to clean slice %d %d %d %d at %d\n", 
		 slice->sliceNo,
		 slice->state, doneState, clst->use_fec, pos);
#endif
	if(slice->state != doneState)
	    break;
	udpc_receiverStatsAddBytes(clst->stats, slice->bytes);
	udpc_displayReceiverStats(clst->stats);
	bytes = slice->bytes;

	/* signal data received */
	if(bytes == 0) {
	    pc_produceEnd(clst->fifo->data);
	} else
	    pc_produce(clst->fifo->data, slice->bytes);

	/* free up slice structure */
	clst->slices[pos].state = SLICE_FREE;
#if DEBUG
	flprintf("Giving back slice %d => %d %p\n",
		 clst->slices[pos].sliceNo, pos, &clst->slices[pos]);
#endif
	pc_produce(clst->free_slices_pc, 1);
	
	/* if at end, exit this thread */
	if(!bytes) {
	    clst->endReached = 2;
	}
    }
}

/**
 * Check whether this slice is sufficiently complete
 */
static void checkSliceComplete(struct clientState *clst,
			       struct slice *slice)
{
    int blocksInSlice;

    assert(slice->magic == SLICEMAGIC);
    if(slice->state != SLICE_RECEIVING) 
	/* bad starting state */
	return; 

    /* is this slice ready ? */
    assert(clst->net_config->blockSize != 0);
    blocksInSlice = (slice->bytes + clst->net_config->blockSize - 1) / 
	clst->net_config->blockSize;
    if(blocksInSlice == slice->blocksTransferred) {
	if(blocksInSlice == slice->dataBlocksTransferred)
	    slice->state = SLICE_DONE;
	else {
#ifdef BB_FEATURE_UDPCAST_FEC
	    assert(clst->use_fec == 1);
	    slice->state = SLICE_FEC;
#else
	    assert(0);
#endif
	}
	advanceReceivedPointer(clst);
	if(clst->use_fec) {
	    int n = pc_getProducerPosition(clst->fec_data_pc);
#ifdef BB_FEATURE_UDPCAST_FEC
	    assert(slice->state == SLICE_DONE || slice->state == SLICE_FEC);
#else
	    assert(slice->state == SLICE_DONE);
#endif
	    clst->fec_slices[n] = slice;
	    pc_produce(clst->fec_data_pc, 1);
	} else
	    cleanupSlices(clst, SLICE_DONE);
    }
}

#ifdef BB_FEATURE_UDPCAST_FEC
static int getSliceBlocks(struct slice *slice, struct net_config *net_config)
{
    assert(net_config->blockSize != 0);
    return (slice->bytes + net_config->blockSize - 1) / net_config->blockSize;
}

static void fec_decode_one_stripe(struct clientState *clst,
				  struct slice *slice,
				  int stripe,
				  int bytes,
				  int stripes,
				  short nr_fec_blocks,
				  struct fec_desc *fec_descs) {
    struct fifo *fifo = clst->fifo;
    struct net_config *config = clst->net_config;
    char *map = slice->retransmit.map;

//    int nrBlocks = (bytes + data->blockSize - 1) / data->blockSize;
    int nrBlocks = getSliceBlocks(slice, config);
    int leftOver = bytes % config->blockSize;
    int j;

    unsigned char *fec_blocks[nr_fec_blocks];
    unsigned int fec_block_nos[nr_fec_blocks];
    unsigned int erased_blocks[nr_fec_blocks];
    unsigned char *data_blocks[128];

    int erasedIdx = stripe;
    int i;
    for(i=stripe, j=0; i<nrBlocks; i+= stripes) {
	if(!BIT_ISSET(i, map)) {
#if DEBUG
	    flprintf("Repairing block %d with %d@%p\n",
		     i, 
		     fec_descs[erasedIdx].fecBlockNo,
		     fec_descs[erasedIdx].adr);
#endif
	    fec_descs[erasedIdx].erasedBlockNo=i;
	    erased_blocks[j++] = i/stripes;
	    erasedIdx += stripes;
	}
    }
    assert(erasedIdx == stripe+nr_fec_blocks*stripes);

    for(i=stripe, j=0; j<nr_fec_blocks; i+=stripes, j++) {
	fec_block_nos[j] = fec_descs[i].fecBlockNo/stripes;
	fec_blocks[j] = fec_descs[i].adr;
    }

    if(leftOver) {
	char *lastBlock = ADR(nrBlocks - 1, config->blockSize);
	bzero(lastBlock+leftOver, config->blockSize-leftOver);
    }

    for(i=stripe, j=0; i< nrBlocks; i+=stripes, j++)
	data_blocks[j] = ADR(i, config->blockSize);
    fec_decode(config->blockSize,  data_blocks, j, 
	       fec_blocks,  fec_block_nos, erased_blocks, nr_fec_blocks);
}


static void *fecMain(void *args0)
{
    struct clientState *clst = (struct clientState *) args0;
    int pos;
    struct fifo *fifo = clst->fifo;
    struct net_config *config = clst->net_config;
    
    assert(fifo->dataBufSize % config->blockSize == 0);
    assert(config->blockSize != 0);

    while(clst->endReached < 2) {
	struct slice *slice;
	pc_consume(clst->fec_data_pc, 1);
	pos = pc_getConsumerPosition(clst->fec_data_pc);
	slice = clst->fec_slices[pos];
	pc_consumed(clst->fec_data_pc, 1);
	if(slice->state != SLICE_FEC &&
	   slice->state != SLICE_DONE)
	    /* This can happen if a SLICE_DONE was enqueued after a SLICE_FEC:
	     * the cleanup after SLICE_FEC also cleaned away the SLICE_DONE (in
	     * main queue), and thus we will find it as SLICE_FREE in the
	     * fec queue. Or worse receiving, or whatever if it made full
	     * circle ... */
	    continue;
	if(slice->state == SLICE_FEC) {
	    int stripes = slice->fec_stripes;
	    struct fec_desc *fec_descs = slice->fec_descs;
	    int stripe;
	    
	    /* Record the addresses of FEC blocks */
	    for(stripe=0; stripe<stripes; stripe++) {
		assert(config->blockSize != 0);
		fec_decode_one_stripe(clst, slice,
				      stripe,
				      slice->bytes,
				      slice->fec_stripes,
				      slice->fec_blocks[stripe],
				      fec_descs);
	    }

	    slice->state = SLICE_FEC_DONE;
	    for(stripe=0; stripe<stripes; stripe++) {
		int i;
		assert(slice->missing_data_blocks[stripe] >=
		       slice->fec_blocks[stripe]);
		for(i=0; i<slice->fec_blocks[stripe]; i++) {
		    freeBlockSpace(clst,fec_descs[stripe+i*stripes].adr);
		    fec_descs[stripe+i*stripes].adr=0;
		}
	    }
	}  else if(slice->state == SLICE_DONE) {
	    slice->state = SLICE_FEC_DONE;
	}
	assert(slice->state == SLICE_FEC_DONE);
	cleanupSlices(clst, SLICE_FEC_DONE);
    }
    return NULL;
}

static void initClstForFec(struct clientState *clst)
{
    clst->use_fec = 1;
    pthread_create(&clst->fec_thread, NULL, fecMain, clst);
}

static void initSliceForFec(struct clientState *clst, struct slice *slice)
{
    int i, j;
    int blocksInSlice;

    assert(slice->magic == SLICEMAGIC);

    /* make this client ready for fec */
    if(!clst->use_fec)
	initClstForFec(clst);

    /* is this slice ready ? */
    assert(clst->net_config->blockSize != 0);
    blocksInSlice = (slice->bytes + clst->net_config->blockSize - 1) / clst->net_config->blockSize;

    for(i=0; i<slice->fec_stripes; i++) {
	slice->missing_data_blocks[i]=0;
	slice->fec_blocks[i]=0;
    }

    for(i=0; i< (blocksInSlice+7)/8 ; i++) {
	if(slice->retransmit.map[i] != 0xff) {
	    int max = i*8+8;
	    if(max > blocksInSlice)
		max = blocksInSlice;
	    for(j=i*8; j < max; j++)
		if(!BIT_ISSET(j, slice->retransmit.map))
		    slice->missing_data_blocks[j % slice->fec_stripes]++;
	}
    }
}



static int processFecBlock(struct clientState *clst,
			   int stripes,
			   int sliceNo,
			   int blockNo,
			   int bytes)
{
    struct slice *slice = findSlice(clst, sliceNo);
    char *shouldAddress, *isAddress;
    int stripe;
    struct fec_desc *desc;
    int adr;

#if DEBUG
    flprintf("Handling FEC packet %d %d %d %d\n",
	     stripes, sliceNo, blockNo, bytes);
#endif
    assert(slice == NULL || slice->magic == SLICEMAGIC);
    if(slice == NULL || 
       slice->state == SLICE_FREE ||
       slice->state == SLICE_DONE ||
       slice->state == SLICE_FEC) {
	/* an old slice. Ignore */
	return 0;
    }

    shouldAddress = clst->nextBlock;
    isAddress = clst->data_hdr.msg_iov[1].iov_base;

    setSliceBytes(slice, clst->fifo, bytes);

    if(slice->fec_stripes == 0) {
	slice->fec_stripes = stripes;
	initSliceForFec(clst, slice);
    } else if(slice->fec_stripes != stripes) {
	udpc_flprintf("Interleave mismatch %d <-> %d", 
		      slice->fec_stripes, stripes);
	return 0;
    }
    stripe = blockNo % slice->fec_stripes;
    if(slice->missing_data_blocks[stripe] <=
       slice->fec_blocks[stripe]) {
	/* not useful */
	/* FIXME: we should forget block here */

	checkSliceComplete(clst, slice);
	advanceReceivedPointer(clst);
	if(!clst->use_fec)
	    cleanupSlices(clst, SLICE_DONE);
	return 0;
    }

    adr = slice->fec_blocks[stripe]*stripes+stripe;

    {
	int i;
	/* check for duplicates, in case of retransmission... */
	for(i=stripe; i<adr; i+= stripes) {
	    desc = &slice->fec_descs[i];
	    if(desc->fecBlockNo == blockNo) {
		udpc_flprintf("**** duplicate block...\n");
		return 0;
	    }
	}
    }

    if(shouldAddress != isAddress)
	/* copy message to the correct place */
	bcopy(isAddress,  shouldAddress, clst->net_config->blockSize);

    desc = &slice->fec_descs[adr];
    desc->adr = shouldAddress;
    desc->fecBlockNo = blockNo;
    slice->fec_blocks[stripe]++;	   
    slice->blocksTransferred++;
    setNextBlock(clst);
    slice->freePos = MAX_SLICE_SIZE;
    checkSliceComplete(clst, slice);
    advanceReceivedPointer(clst);
    return 0;
}
#endif

static int processDataBlock(struct clientState *clst,
			    int sliceNo,
			    int blockNo,
			    int bytes)
{
    struct fifo *fifo = clst->fifo;
    struct slice *slice = findSlice(clst, sliceNo);
    char *shouldAddress, *isAddress;

    assert(slice == NULL || slice->magic == SLICEMAGIC);

    if(slice == NULL || 
       slice->state == SLICE_FREE ||
       slice->state == SLICE_DONE
#ifdef BB_FEATURE_UDPCAST_FEC
       ||
       slice->state == SLICE_FEC ||
       slice->state == SLICE_FEC_DONE
#endif
       ) {
	/* an old slice. Ignore */
	return 0;
    }

    if(sliceNo > clst->currentSliceNo+2)
	udpc_fatal(1, "We have been dropped by sender\n");

    if(BIT_ISSET(blockNo, slice->retransmit.map)) {
	/* we already have this packet, ignore */
#if 0
	flprintf("Packet %d:%d not for us\n", sliceNo, blockNo);
#endif
	return 0;
    }

    if(slice->base % clst->net_config->blockSize) {
	udpc_fatal(1, "Bad base %d, not multiple of block size %d\n",
		   slice->base, clst->net_config->blockSize);
    }

    shouldAddress = ADR(blockNo, clst->net_config->blockSize);
    isAddress = clst->data_hdr.msg_iov[1].iov_base;
    if(shouldAddress != isAddress) {
	/* copy message to the correct place */
	bcopy(isAddress,  shouldAddress, clst->net_config->blockSize);
    }

    if(clst->client_config->server_is_newgen &&  bytes != 0)
	setSliceBytes(slice, clst->fifo, bytes);

    SET_BIT(blockNo, slice->retransmit.map);
#ifdef BB_FEATURE_UDPCAST_FEC
    if(slice->fec_stripes) {
	int stripe = blockNo % slice->fec_stripes;
	slice->missing_data_blocks[stripe]--;
	assert(slice->missing_data_blocks[stripe] >= 0);
	if(slice->missing_data_blocks[stripe] <
	   slice->fec_blocks[stripe]) {
	    int blockIdx;
	    /* FIXME: FEC block should be enqueued in local queue here...*/
	    slice->fec_blocks[stripe]--;
	    blockIdx = stripe+slice->fec_blocks[stripe]*slice->fec_stripes;
	    assert(slice->fec_descs[blockIdx].adr != 0);
	    clst->localBlockAddresses[clst->localPos++] = 
		slice->fec_descs[blockIdx].adr;
	    slice->fec_descs[blockIdx].adr=0;
	    slice->blocksTransferred--;
	}
    }
#endif
    slice->dataBlocksTransferred++;
    slice->blocksTransferred++;
    while(slice->freePos < MAX_SLICE_SIZE && 
	  BIT_ISSET(slice->freePos, slice->retransmit.map))
	slice->freePos++;
    checkSliceComplete(clst, slice);
    return 0;
}

static int processReqAck(struct clientState *clst,
			 int sliceNo, int bytes, int rxmit)
{   
    struct slice *slice = findSlice(clst, sliceNo);
    int blocksInSlice;
    char *readySet = (char *) clst->data_hdr.msg_iov[1].iov_base;

#if DEBUG
    flprintf("Received REQACK (sn=%d, rxmit=%d sz=%d) %d\n", 
	     sliceNo, rxmit,  bytes, (slice - &clst->slices[0]));
#endif

    assert(slice == NULL || slice->magic == SLICEMAGIC);

    {
	struct timeval tv;
	gettimeofday(&tv, 0);
	/* usleep(1); DEBUG: FIXME */
    }
    if(BIT_ISSET(clst->client_config->clientNumber, readySet)) {
	/* not for us */
#if DEBUG
	flprintf("Not for us\n");
#endif
	return 0;
    }

    if(slice == NULL) {
	/* an old slice => send ok */
#if DEBUG
	flprintf("old slice => sending ok\n");
#endif
	return sendOk(clst->client_config, sliceNo);
    }

    setSliceBytes(slice, clst->fifo, bytes);
    assert(clst->net_config->blockSize != 0);
    blocksInSlice = (slice->bytes + clst->net_config->blockSize - 1) / 
	clst->net_config->blockSize;
    if (blocksInSlice == slice->blocksTransferred) {
	/* send ok */
	sendOk(clst->client_config, slice->sliceNo);
    } else {
#if DEBUG
	flprintf("Ask for retransmission (%d/%d %d)\n",
		slice->blocksTransferred, blocksInSlice, bytes);
#endif
	sendRetransmit(clst, slice, rxmit);
    }
#if DEBUG
    flprintf("Received reqack %d %d\n", slice->sliceNo, bytes);
#endif
    checkSliceComplete(clst, slice); /* needed for the final 0 sized slice */
    advanceReceivedPointer(clst);
    if(!clst->use_fec)
	cleanupSlices(clst, SLICE_DONE);
    return 0;
}

/* Receives a message from network, and dispatches it
 * Only called in network reception thread
 */
static int dispatchMessage(struct clientState *clst)
{
    int ret;
    struct sockaddr lserver;
    struct fifo *fifo = clst->fifo;
    int fd = clst->client_config->toServer;

    /* set up message header */
    if (clst->currentSlice != NULL &&
	clst->currentSlice->freePos < MAX_SLICE_SIZE) {
	struct slice *slice = clst->currentSlice;
	assert(slice == NULL || slice->magic == SLICEMAGIC);
	clst->data_iov[1].iov_base = 
	    ADR(slice->freePos, clst->net_config->blockSize);
    } else {
	clst->data_iov[1].iov_base = clst->nextBlock;
    }

    clst->data_iov[1].iov_len = clst->net_config->blockSize;
    clst->data_hdr.msg_iovlen = 2;

    clst->data_hdr.msg_name = &lserver;
    clst->data_hdr.msg_namelen = sizeof(struct sockaddr);

    while(clst->endReached) {
	int oldEndReached = clst->endReached;
	int nr_desc;
	struct timeval tv;
	fd_set read_set;
	FD_ZERO(&read_set);
	FD_SET(fd, &read_set);
	tv.tv_sec = clst->net_config->exitWait / 1000;
	tv.tv_usec = (clst->net_config->exitWait % 1000)*1000;
	nr_desc = select(fd+1,  &read_set, 0, 0,  &tv);
	if(nr_desc < 0) {
	    flprintf("Select error: %s\n", strerror(errno));
	    break;
	}
	if(FD_ISSET(fd, &read_set))
	    break;
	/* Timeout expired */
	if(oldEndReached >= 2) {
	    clst->endReached = 3;
	    return 0;
	}
    }

#ifdef LOSSTEST
    loseRecvPacket(fd);
    ret=RecvMsg(fd, &clst->data_hdr, 0);
#else
    ret=recvmsg(fd, &clst->data_hdr, 0);
#endif
    if (ret < 0) {
#if DEBUG
	flprintf("data recvfrom %d: %s\n", fd, strerror(errno));
#endif
	return -1;
    }


#if 0
    flprintf("received packet for slice %d, block %d\n", 
	     ntohl(Msg.sliceNo), ntohs(db.blockNo));
#endif

    if(!udpc_isAddressEqual(&lserver, 
			    &clst->client_config->serverAddr)) {
	char buffer1[16], buffer2[16];
	udpc_flprintf("Rogue packet received %s:%d, expecting %s:%d\n",
		      udpc_getIpString(&lserver, buffer1), getPort(&lserver),
		      udpc_getIpString(&clst->client_config->serverAddr, buffer2),
		      udpc_getPort(&clst->client_config->serverAddr));
	return -1;
    }

    switch(ntohs(clst->Msg.opCode)) {
	case CMD_DATA:
	    udpc_receiverStatsStartTimer(clst->stats);
	    clst->client_config->isStarted = 1;
	    clst->client_config->endianness = NET_ENDIAN;
	    return processDataBlock(clst,
				    ntohl(clst->Msg.dataBlock.sliceNo),
				    ntohs(clst->Msg.dataBlock.blockNo),
				    ntohl(clst->Msg.dataBlock.bytes));
#ifdef BB_FEATURE_UDPCAST_FEC
	case CMD_FEC:
	    receiverStatsStartTimer(clst->stats);
	    clst->client_config->isStarted = 1;
	    return processFecBlock(clst,
				   ntohs(clst->Msg.fecBlock.stripes),
				   ntohl(clst->Msg.fecBlock.sliceNo),
				   ntohs(clst->Msg.fecBlock.blockNo),
				   ntohl(clst->Msg.fecBlock.bytes));
#endif
	case CMD_REQACK:
	    receiverStatsStartTimer(clst->stats);
	    clst->client_config->isStarted = 1;
	    clst->client_config->endianness = NET_ENDIAN;
	    return processReqAck(clst,
				 ntohl(clst->Msg.reqack.sliceNo),
				 ntohl(clst->Msg.reqack.bytes),
				 ntohl(clst->Msg.reqack.rxmit));
	case CMD_HELLO:
	    /* retransmission of hello to find other participants ==> ignore */
	    return 0;
	default:
	    break;
    }

    switch(pctohs(clst->Msg.opCode)) {
	case CMD_DATA:
	    receiverStatsStartTimer(clst->stats);
	    clst->client_config->isStarted = 1;
	    clst->client_config->endianness = PC_ENDIAN;
	    /*flprintf("Process little endian data block\n");*/
	    return processDataBlock(clst,
				    pctohl(clst->Msg.dataBlock.sliceNo),
				    pctohs(clst->Msg.dataBlock.blockNo),
				    ntohl(clst->Msg.dataBlock.bytes));
	case CMD_REQACK:
	    receiverStatsStartTimer(clst->stats);
	    clst->client_config->isStarted = 1;	    
	    clst->client_config->endianness = PC_ENDIAN;
	    /*flprintf("Process little endian reqack\n");*/
	    return processReqAck(clst,
				 pctohl(clst->Msg.reqack.sliceNo),
				 pctohl(clst->Msg.reqack.bytes),
				 pctohl(clst->Msg.reqack.rxmit));
	case CMD_HELLO:
	    /* retransmission of hello to find other participants ==> ignore */
	    return 0;
	default:
	    break;
    }

    udpc_flprintf("Unexpected opcode %04x\n", 
		  (unsigned short) clst->Msg.opCode);
    return -1;
}

static int setupMessages(struct clientState *clst) {
    /* the messages received from the server */
    clst->data_iov[0].iov_base = (void *)&clst->Msg;
    clst->data_iov[0].iov_len = sizeof(clst->Msg);
    
    /* namelen set just before reception */
    clst->data_hdr.msg_iov = clst->data_iov;
    /* iovlen set just before reception */

#ifndef __CYGWIN__
    clst->data_hdr.msg_control = 0;
    clst->data_hdr.msg_controllen = 0;
    clst->data_hdr.msg_flags = 0;
#endif
    return 0;
}

static void *netReceiverMain(void *args0)
{    
    struct clientState *clst = (struct clientState *) args0;
#ifdef BB_FEATURE_UDPCAST_FEC
    void *returnValue;
#endif

    clst->currentSliceNo = 0;
    setupMessages(clst);
    
    clst->currentSliceNo = -1;
    clst->currentSlice = NULL;
    newSlice(clst, 0);

    while(clst->endReached < 3) {
	if(clst->notifyPipeFd != -1 && clst->client_config->isStarted) {
	    close(clst->notifyPipeFd);
	    clst->notifyPipeFd=-1;
	}
	dispatchMessage(clst);
    }
#ifdef BB_FEATURE_UDPCAST_FEC
    if(clst->use_fec)
	pthread_join(clst->fec_thread, &returnValue);
#endif
    return NULL;
}

int spawnNetReceiver(struct fifo *fifo,
		     struct client_config *client_config,
		     struct net_config *net_config,
		     receiver_stats_t stats,
		     int notifyPipeFd)
{
    int i;
    struct clientState  *clst = MALLOC(struct clientState);
    clst->fifo = fifo;
    clst->client_config = client_config;
    clst->net_config = net_config;
    clst->stats = stats;
    clst->notifyPipeFd = notifyPipeFd;
    clst->endReached = 0;

    clst->free_slices_pc = pc_makeProduconsum(NR_SLICES, "free slices");
    pc_produce(clst->free_slices_pc, NR_SLICES);
    for(i = 0; i <NR_SLICES; i++)
	clst->slices[i].state = SLICE_FREE;
    clst->receivedPtr = 0;
    clst->receivedSliceNo = 0;

#ifdef BB_FEATURE_UDPCAST_FEC
    fec_init(); /* fec new involves memory
		 * allocation. Better do it here */
    clst->use_fec = 0;
    clst->fec_data_pc = pc_makeProduconsum(NR_SLICES, "fec data");
    clst->fec_thread = 0;
#endif

#define NR_BLOCKS 4096
    clst->freeBlocks_pc = pc_makeProduconsum(NR_BLOCKS, "free blocks");
    pc_produce(clst->freeBlocks_pc, NR_BLOCKS);
    clst->blockAddresses = calloc(NR_BLOCKS, sizeof(char *));
    clst->localBlockAddresses = calloc(NR_BLOCKS, sizeof(char *));
    clst->blockData = xmalloc(NR_BLOCKS * net_config->blockSize);
    for(i = 0; i < NR_BLOCKS; i++)
	clst->blockAddresses[i] = clst->blockData + i * net_config->blockSize;
    clst->localPos=0;

    setNextBlock(clst);
    return pthread_create(&client_config->thread, NULL, netReceiverMain, clst);
}