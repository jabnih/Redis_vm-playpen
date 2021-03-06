/*
 * Copyright (c) 2006-2009, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define REDIS_VERSION "1.3.3"

#include "fmacros.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define __USE_POSIX199309
#include <signal.h>

#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#include <ucontext.h>
#endif /* HAVE_BACKTRACE */

#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>

#if defined(__sun)
#include "solarisfixes.h"
#endif

#include "redis.h"
#include "ae.h"     /* Event driven programming library */
#include "sds.h"    /* Dynamic safe strings */
#include "anet.h"   /* Networking the easy way */
#include "dict.h"   /* Hash tables */
#include "adlist.h" /* Linked lists */
#include "zmalloc.h" /* total memory usage aware version of malloc/free */
#include "lzf.h"    /* LZF compression library */
#include "pqsort.h" /* Partial qsort for SORT+LIMIT */

/* Error codes */
#define REDIS_OK                0
#define REDIS_ERR               -1

/* Static server configuration */
#define REDIS_SERVERPORT        6379    /* TCP port */
#define REDIS_MAXIDLETIME       (60*5)  /* default client timeout */
#define REDIS_IOBUF_LEN         1024
#define REDIS_LOADBUF_LEN       1024
#define REDIS_STATIC_ARGS       4
#define REDIS_DEFAULT_DBNUM     16
#define REDIS_CONFIGLINE_MAX    1024
#define REDIS_OBJFREELIST_MAX   1000000 /* Max number of objects to cache */
#define REDIS_MAX_SYNC_TIME     60      /* Slave can't take more to sync */
#define REDIS_EXPIRELOOKUPS_PER_CRON    100 /* try to expire 100 keys/second */
#define REDIS_MAX_WRITE_PER_EVENT (1024*64)
#define REDIS_REQUEST_MAX_SIZE (1024*1024*256) /* max bytes in inline command */

/* If more then REDIS_WRITEV_THRESHOLD write packets are pending use writev */
#define REDIS_WRITEV_THRESHOLD      3
/* Max number of iovecs used for each writev call */
#define REDIS_WRITEV_IOVEC_COUNT    256

/* Hash table parameters */
#define REDIS_HT_MINFILL        10      /* Minimal hash table fill 10% */

// Redis命令标识
/* Command flags */
#define REDIS_CMD_BULK          1       /* Bulk write command */
#define REDIS_CMD_INLINE        2       /* Inline command */
/* REDIS_CMD_DENYOOM reserves a longer comment: all the commands marked with
   this flags will return an error when the 'maxmemory' option is set in the
   config file and the server is using more than maxmemory bytes of memory.
   In short this commands are denied on low memory conditions. */
// 在内存不足的情况下，如果有此标识，则直接拒绝（用于在内存不足的情况下提供读服务）
#define REDIS_CMD_DENYOOM       4

/* Object types */
#define REDIS_STRING 0
#define REDIS_LIST 1
#define REDIS_SET 2
#define REDIS_ZSET 3
#define REDIS_HASH 4

/* Objects encoding */
#define REDIS_ENCODING_RAW 0    /* Raw representation */
#define REDIS_ENCODING_INT 1    /* Encoded as integer */

/* Object types only used for dumping to disk */
#define REDIS_EXPIRETIME 253
#define REDIS_SELECTDB 254
#define REDIS_EOF 255

/* Defines related to the dump file format. To store 32 bits lengths for short
 * keys requires a lot of space, so we check the most significant 2 bits of
 * the first byte to interpreter the length:
 *
 * 00|000000 => if the two MSB are 00 the len is the 6 bits of this byte
 * 01|000000 00000000 =>  01, the len is 14 byes, 6 bits + 8 bits of next byte
 * 10|000000 [32 bit integer] => if it's 01, a full 32 bit len will follow
 * 11|000000 this means: specially encoded object will follow. The six bits
 *           number specify the kind of object that follows.
 *           See the REDIS_RDB_ENC_* defines.
 *
 * Lenghts up to 63 are stored using a single byte, most DB keys, and may
 * values, will fit inside. */
#define REDIS_RDB_6BITLEN 0
#define REDIS_RDB_14BITLEN 1
#define REDIS_RDB_32BITLEN 2
#define REDIS_RDB_ENCVAL 3
#define REDIS_RDB_LENERR UINT_MAX

/* When a length of a string object stored on disk has the first two bits
 * set, the remaining two bits specify a special encoding for the object
 * accordingly to the following defines: */
#define REDIS_RDB_ENC_INT8 0        /* 8 bit signed integer */
#define REDIS_RDB_ENC_INT16 1       /* 16 bit signed integer */
#define REDIS_RDB_ENC_INT32 2       /* 32 bit signed integer */
#define REDIS_RDB_ENC_LZF 3         /* string compressed with FASTLZ */

/* Virtual memory object->where field. */
#define REDIS_VM_MEMORY 0       /* The object is on memory */
#define REDIS_VM_SWAPPED 1      /* The object is on disk */
#define REDIS_VM_SWAPPING 2     /* Redis is swapping this object on disk */
#define REDIS_VM_LOADING 3      /* Redis is loading this object from disk */

/* Virtual memory static configuration stuff.
 * Check vmFindContiguousPages() to know more about this magic numbers. */
#define REDIS_VM_MAX_NEAR_PAGES 65536
#define REDIS_VM_MAX_RANDOM_JUMP 4096
#define REDIS_VM_MAX_THREADS 32
#define REDIS_THREAD_STACK_SIZE (1024*1024*4)
/* The following is the *percentage* of completed I/O jobs to process when the
 * handelr is called. While Virtual Memory I/O operations are performed by
 * threads, this operations must be processed by the main thread when completed
 * in order to take effect. */
#define REDIS_MAX_COMPLETED_JOBS_PROCESSED 1

/* Client flags */
#define REDIS_CLOSE 1       /* This client connection should be closed ASAP */
#define REDIS_SLAVE 2       /* This client is a slave server */
#define REDIS_MASTER 4      /* This client is a master server */
#define REDIS_MONITOR 8      /* This client is a slave monitor, see MONITOR */
#define REDIS_MULTI 16      /* This client is in a MULTI context */
#define REDIS_BLOCKED 32    /* The client is waiting in a blocking operation */
#define REDIS_IO_WAIT 64    /* The client is waiting for Virtual Memory I/O */

/* Slave replication state - slave side */
#define REDIS_REPL_NONE 0   /* No active replication */
#define REDIS_REPL_CONNECT 1    /* Must connect to master */
#define REDIS_REPL_CONNECTED 2  /* Connected to master */

/* Slave replication state - from the point of view of master
 * Note that in SEND_BULK and ONLINE state the slave receives new updates
 * in its output queue. In the WAIT_BGSAVE state instead the server is waiting
 * to start the next background saving in order to send updates to it. */
#define REDIS_REPL_WAIT_BGSAVE_START 3 /* master waits bgsave to start feeding it */
#define REDIS_REPL_WAIT_BGSAVE_END 4 /* master waits bgsave to start bulk DB transmission */
#define REDIS_REPL_SEND_BULK 5 /* master is sending the bulk DB */
#define REDIS_REPL_ONLINE 6 /* bulk DB already transmitted, receive updates */

/* List related stuff */
#define REDIS_HEAD 0
#define REDIS_TAIL 1

/* Sort operations */
#define REDIS_SORT_GET 0
#define REDIS_SORT_ASC 1
#define REDIS_SORT_DESC 2
#define REDIS_SORTKEY_MAX 1024

/* Log levels */
#define REDIS_DEBUG 0
#define REDIS_VERBOSE 1
#define REDIS_NOTICE 2
#define REDIS_WARNING 3

/* Anti-warning macro... */
#define REDIS_NOTUSED(V) ((void) V)

#define ZSKIPLIST_MAXLEVEL 32 /* Should be enough for 2^32 elements */
#define ZSKIPLIST_P 0.25      /* Skiplist P = 1/4 */

/* Append only defines */
#define APPENDFSYNC_NO 0
#define APPENDFSYNC_ALWAYS 1
#define APPENDFSYNC_EVERYSEC 2

/* We can print the stacktrace, so our assert is defined this way: */
#define redisAssert(_e) ((_e)?(void)0 : (_redisAssert(#_e,__FILE__,__LINE__),exit(1)))
static void _redisAssert(char *estr, char *file, int line);

/*================================= Data types ============================== */

/* A redis object, that is a type able to hold a string / list / set */

/* The VM object structure */
struct redisObjectVM {
	off_t page;         /* the page at witch the object is stored on disk */
	off_t usedpages;    /* number of pages used on disk */
	time_t atime;       /* Last access time */
} vm;

/* The actual Redis Object */
// Redis中的对象，存储的基本单元
typedef struct redisObject {
	// 保存的值
	void *ptr;
	// 值的类型
	unsigned char type;
	// 编码类型（用于压缩）
	unsigned char encoding;
	unsigned char storage;  /* If this object is a key, where is the value?
                             * REDIS_VM_MEMORY, REDIS_VM_SWAPPED, ... */
	unsigned char vtype; /* If this object is a key, and value is swapped out,
                          * this is the type of the swapped out object. */
	// 引用次数，只有当引用次数为0时才释放内存
	int refcount;
	/* VM fields, this are only allocated if VM is active, otherwise the
	 * object allocation function will just allocate
	 * sizeof(redisObjct) minus sizeof(redisObjectVM), so using
	 * Redis without VM active will not have any overhead. */
	struct redisObjectVM vm;
} robj;

/* Macro used to initalize a Redis object allocated on the stack.
 * Note that this macro is taken near the structure definition to make sure
 * we'll update it when the structure is changed, to avoid bugs like
 * bug #85 introduced exactly in this way. */
#define initStaticStringObject(_var,_ptr) do { \
    _var.refcount = 1; \
    _var.type = REDIS_STRING; \
    _var.encoding = REDIS_ENCODING_RAW; \
    _var.ptr = _ptr; \
    if (server.vm_enabled) _var.storage = REDIS_VM_MEMORY; \
} while(0);

// Redis数据库结构
typedef struct redisDb {
	// 保存所有数据的字典结构
	dict *dict;                 /* The keyspace for this DB */
	// 用于保存设置了过期时间的键
	dict *expires;              /* Timeout of keys with a timeout set */
	// 存放当前阻塞的Key到客户端的映射（用于实现一个Key存放数据后，可以快速的找到对应的客户端）
	// Keys -> List(redisClient) 默认先取第一个先等待的客户端
	dict *blockingkeys;         /* Keys with clients waiting for data (BLPOP) */
	// ID,标识当前库，用于区分多个库，从0开始
	int id;
} redisDb;

/* Client MULTI/EXEC state */
// 客户端执行MULTI时保存的单条命令
typedef struct multiCmd {
	robj **argv;
	int argc;
	struct redisCommand *cmd;
} multiCmd;

typedef struct multiState {
	// 客户端执行MULTI时的所有命令
	multiCmd *commands;     /* Array of MULTI commands */
	// 命令的个数
	int count;              /* Total number of MULTI commands */
} multiState;

/* With multiplexing we need to take per-clinet state.
 * Clients are taken in a liked list. */
// 客户端
typedef struct redisClient {
	// 客户端使用的fd
	int fd;
	// 当前使用的数据库
	redisDb *db;
	// 当前使用的数据库id
	int dictid;
	// 客户端请求缓冲区
	sds querybuf;
	// 保存当前解析请求的参数
	robj **argv, **mbargv;
	int argc, mbargc;
	int bulklen;            /* bulk read len. -1 if not in bulk read mode */
	int multibulk;          /* multi bulk command format active */
	// 待发给客户端的应答
	list *reply;
	// 记录当前列表头元素对象已经发送的数据长度（用于数据量大的对象分多次发送）
	int sentlen;
	// 最后一次交互的时间，用于关闭超时的客户端
	time_t lastinteraction; /* time of the last interaction, used for timeout */
	int flags;              /* REDIS_CLOSE | REDIS_SLAVE | REDIS_MONITOR */
	/* REDIS_MULTI */
	int slaveseldb;         /* slave selected db, if this client is a slave */
	int authenticated;      /* when requirepass is non-NULL */
	int replstate;          /* replication state if this is a slave */
	int repldbfd;           /* replication DB file descriptor */
	long repldboff;         /* replication DB file offset */
	off_t repldbsize;       /* replication DB file size */
	// 执行MULTI时保存的状态信息（所有的请求命令）
	multiState mstate;      /* MULTI/EXEC state */
	// 当前阻塞等待的键（用于实现BLPOP类似的命令）
	robj **blockingkeys;    /* The key we waiting to terminate a blocking
                             * operation such as BLPOP. Otherwise NULL. */
	// 等待阻塞键的个数
	int blockingkeysnum;    /* Number of blocking keys */
	// 阻塞等待的超时时间
	time_t blockingto;      /* Blocking operation timeout. If UNIX current time
                             * is >= blockingto then the operation timed out. */
	list *io_keys;          /* Keys this client is waiting to be loaded from the
                             * swap file in order to continue. */
} redisClient;

/*
* 这个用于保存Redis触发RDB的阈值，
* 可以在配置文件中设置，
* 如save 900 1，表示在1s内变动900次则触发RDB
*/
struct saveparam {
	time_t seconds;
	int changes;
};

/* Global server state structure */
struct redisServer {
	int port;  // Redis监听端口
	int fd;    // 服务器fd
	redisDb *db;   // Redis数据库
	// 用于缓存经常使用的共享对象，用于节省内存以及减少malloc次数，提高性能
	dict *sharingpool;          /* Poll used for object sharing */
	// 共享对象池的大小
	unsigned int sharingpoolsize;
	// 数据变动的次数，当RDB进行持久化后就会置0
	long long dirty;            /* changes to DB from the last save */
	// 当前连接的活动客户端
	list *clients;
	// slave为所有备机的列表，
	// 而monitors则是用于监控Redis所有命令执行的命令
	list *slaves, *monitors;
	// 网络错误信息提示
	char neterr[ANET_ERR_LEN];
	// 事件循环句柄，用于处理所有的客户端请求
	aeEventLoop *el;
	// cronServer定时任务执行的次数
	int cronloops;              /* number of times the cron function run */
	// 对象内存池，当对象释放的时候会尝试放到该链表中，用于防止多次malloc操作
	list *objfreelist;          /* A list of freed objects to avoid malloc() */
	// 最后一次执行RDB持久化的时间
	time_t lastsave;            /* Unix time of last save succeeede */
	/* Fields used only for stats */
	// 服务器启动时间
	time_t stat_starttime;         /* server start time */
	// 处理的命令个数
	long long stat_numcommands;    /* number of processed commands */
	// 总连接数，包括之前断开的
	long long stat_numconnections; /* number of connections received */
	/* Configuration */
	// 日志过滤级别
	int verbosity;
	// 标志位，1表示开启，将应答消息组合起来批量发送出去
	int glueoutputbuf;
	// 客户端最大空闲时间
	int maxidletime;
	// 数据库个数
	int dbnum;
	// 后台执行标志
	int daemonize;
	// AOF开启标志
	int appendonly;
	// fsync同步时机标志，fsync函数只对指定文件生效，会等待写入磁盘成功后才返回
	int appendfsync;
	// 最后一次执行fsync时间
	time_t lastfsync;
	// aof文件fd
	int appendfd;
	// 执行aof时当前的db
	int appendseldb;
	// 当前Redis实例执行时，写入pid的文件
	char *pidfile;
	// 后台执行RDB保存的进程pid
	pid_t bgsavechildpid;
	pid_t bgrewritechildpid;
	sds bgrewritebuf; /* buffer taken by parent during oppend only rewrite */
	// 触发RDB阈值列表，save参数
	struct saveparam *saveparams;
	// save参数的个数
	int saveparamslen;
	// 日志文件
	char *logfile;
	// 绑定的IP地址
	char *bindaddr;
	// RDB文件名
	char *dbfilename;
	// AOF文件名
	char *appendfilename;
	// 认证密码
	char *requirepass;
	int shareobjects;
	int rdbcompression;
	/* Replication related */
	int isslave;
	// Master主机的认证密码
	char *masterauth;
	// Master地址
	char *masterhost;
	// Master端口
	int masterport;
	// 作为Master客户端的链接
	redisClient *master;    /* client that is master for this slave */
	// 当前Replication的状态（Slave）
	int replstate;
	// 最大客户端连接数
	unsigned int maxclients;
	// 最大使用内存
	unsigned long long maxmemory;
	unsigned int blockedclients;
	/* Sort parameters - qsort_r() is only available under BSD so we
	 * have to take this state global, in order to pass it to sortCompare() */
	int sort_desc;
	int sort_alpha;
	int sort_bypattern;
	/* Virtual memory configuration */
	int vm_enabled;
	char *vm_swap_file;
	off_t vm_page_size;
	off_t vm_pages;
	unsigned long long vm_max_memory;
	/* Virtual memory state */
	FILE *vm_fp;
	int vm_fd;
	off_t vm_next_page; /* Next probably empty page */
	off_t vm_near_pages; /* Number of pages allocated sequentially */
	unsigned char *vm_bitmap; /* Bitmap of free/used pages */
	time_t unixtime;    /* Unix time sampled every second. */
	/* Virtual memory I/O threads stuff */
	/* An I/O thread process an element taken from the io_jobs queue and
	 * put the result of the operation in the io_done list. While the
	 * job is being processed, it's put on io_processing queue. */
	list *io_newjobs; /* List of VM I/O jobs yet to be processed */
	list *io_processing; /* List of VM I/O jobs being processed */
	list *io_processed; /* List of VM I/O jobs already processed */
	list *io_clients; /* All the clients waiting for SWAP I/O operations */
	pthread_mutex_t io_mutex; /* lock to access io_jobs/io_done/io_thread_job */
	pthread_mutex_t obj_freelist_mutex; /* safe redis objects creation/free */
	pthread_mutex_t io_swapfile_mutex; /* So we can lseek + write */
	pthread_attr_t io_threads_attr; /* attributes for threads creation */
	int io_active_threads; /* Number of running I/O threads */
	int vm_max_threads; /* Max number of I/O threads running at the same time */
	/* Our main thread is blocked on the event loop, locking for sockets ready
	 * to be read or written, so when a threaded I/O operation is ready to be
	 * processed by the main thread, the I/O thread will use a unix pipe to
	 * awake the main thread. The followings are the two pipe FDs. */
	int io_ready_pipe_read;
	int io_ready_pipe_write;
	/* Virtual memory stats */
	unsigned long long vm_stats_used_pages;
	unsigned long long vm_stats_swapped_objects;
	unsigned long long vm_stats_swapouts;
	unsigned long long vm_stats_swapins;
	FILE *devnull;
};

// Redis命令
typedef void redisCommandProc(redisClient *c);
struct redisCommand {
	char *name;
	redisCommandProc *proc;
	// 参数要求（正的则为必须要参数个数等于该数值，负的则是参数最少要求个数）
	int arity;
	int flags;
};

struct redisFunctionSym {
	char *name;
	unsigned long pointer;
};

typedef struct _redisSortObject {
	robj *obj;
	union {
		double score;
		robj *cmpobj;
	} u;
} redisSortObject;

typedef struct _redisSortOperation {
	int type;
	robj *pattern;
} redisSortOperation;

/* ZSETs use a specialized version of Skiplists */

typedef struct zskiplistNode {
	struct zskiplistNode **forward;
	struct zskiplistNode *backward;
	double score;
	robj *obj;
} zskiplistNode;

typedef struct zskiplist {
	struct zskiplistNode *header, *tail;
	unsigned long length;
	int level;
} zskiplist;

typedef struct zset {
	dict *dict;
	zskiplist *zsl;
} zset;

/* Our shared "common" objects */
// 共享通用的对象
struct sharedObjectsStruct {
	robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *pong, *space,
	     *colon, *nullbulk, *nullmultibulk, *queued,
	     *emptymultibulk, *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr,
	     *outofrangeerr, *plus,
	     *select0, *select1, *select2, *select3, *select4,
	     *select5, *select6, *select7, *select8, *select9;
} shared;

/* Global vars that are actally used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. */

static double R_Zero, R_PosInf, R_NegInf, R_Nan;

/* VM threaded I/O request message */
#define REDIS_IOJOB_LOAD 0          /* Load from disk to memory */
#define REDIS_IOJOB_PREPARE_SWAP 1  /* Compute needed pages */
#define REDIS_IOJOB_DO_SWAP 2       /* Swap from memory to disk */
typedef struct iojon {
	int type;   /* Request type, REDIS_IOJOB_* */
	redisDb *db;/* Redis database */
	robj *key;  /* This I/O request is about swapping this key */
	robj *val;  /* the value to swap for REDIS_IOREQ_*_SWAP, otherwise this
                 * field is populated by the I/O thread for REDIS_IOREQ_LOAD. */
	off_t page; /* Swap page where to read/write the object */
	off_t pages; /* Swap pages needed to safe object. PREPARE_SWAP return val */
	int canceled; /* True if this command was canceled by blocking side of VM */
	pthread_t thread; /* ID of the thread processing this entry */
} iojob;

/*================================ Prototypes =============================== */

static void freeStringObject(robj *o);
static void freeListObject(robj *o);
static void freeSetObject(robj *o);
static void decrRefCount(void *o);
static robj *createObject(int type, void *ptr);
static void freeClient(redisClient *c);
static int rdbLoad(char *filename);
static void addReply(redisClient *c, robj *obj);
static void addReplySds(redisClient *c, sds s);
static void incrRefCount(robj *o);
static int rdbSaveBackground(char *filename);
static robj *createStringObject(char *ptr, size_t len);
static robj *dupStringObject(robj *o);
static void replicationFeedSlaves(list *slaves, struct redisCommand *cmd, int dictid, robj **argv, int argc);
static void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc);
static int syncWithMaster(void);
static robj *tryObjectSharing(robj *o);
static int tryObjectEncoding(robj *o);
static robj *getDecodedObject(robj *o);
static int removeExpire(redisDb *db, robj *key);
static int expireIfNeeded(redisDb *db, robj *key);
static int deleteIfVolatile(redisDb *db, robj *key);
static int deleteIfSwapped(redisDb *db, robj *key);
static int deleteKey(redisDb *db, robj *key);
static time_t getExpire(redisDb *db, robj *key);
static int setExpire(redisDb *db, robj *key, time_t when);
static void updateSlavesWaitingBgsave(int bgsaveerr);
static void freeMemoryIfNeeded(void);
static int processCommand(redisClient *c);
static void setupSigSegvAction(void);
static void rdbRemoveTempFile(pid_t childpid);
static void aofRemoveTempFile(pid_t childpid);
static size_t stringObjectLen(robj *o);
static void processInputBuffer(redisClient *c);
static zskiplist *zslCreate(void);
static void zslFree(zskiplist *zsl);
static void zslInsert(zskiplist *zsl, double score, robj *obj);
static void sendReplyToClientWritev(aeEventLoop *el, int fd, void *privdata, int mask);
static void initClientMultiState(redisClient *c);
static void freeClientMultiState(redisClient *c);
static void queueMultiCommand(redisClient *c, struct redisCommand *cmd);
static void unblockClientWaitingData(redisClient *c);
static int handleClientsWaitingListPush(redisClient *c, robj *key, robj *ele);
static void vmInit(void);
static void vmMarkPagesFree(off_t page, off_t count);
static robj *vmLoadObject(robj *key);
static robj *vmPreviewObject(robj *key);
static int vmSwapOneObjectBlocking(void);
static int vmSwapOneObjectThreaded(void);
static int vmCanSwapOut(void);
static int tryFreeOneObjectFromFreelist(void);
static void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
static void vmThreadedIOCompletedJob(aeEventLoop *el, int fd, void *privdata, int mask);
static void vmCancelThreadedIOJob(robj *o);
static void lockThreadedIO(void);
static void unlockThreadedIO(void);
static int vmSwapObjectThreaded(robj *key, robj *val, redisDb *db);
static void freeIOJob(iojob *j);
static void queueIOJob(iojob *j);
static int vmWriteObjectOnSwap(robj *o, off_t page);
static robj *vmReadObjectFromSwap(off_t page, int type);
static void waitEmptyIOJobsQueue(void);
static void vmReopenSwapFile(void);
static int vmFreePage(off_t page);

static void authCommand(redisClient *c);
static void pingCommand(redisClient *c);
static void echoCommand(redisClient *c);
static void setCommand(redisClient *c);
static void setnxCommand(redisClient *c);
static void getCommand(redisClient *c);
static void delCommand(redisClient *c);
static void existsCommand(redisClient *c);
static void incrCommand(redisClient *c);
static void decrCommand(redisClient *c);
static void incrbyCommand(redisClient *c);
static void decrbyCommand(redisClient *c);
static void selectCommand(redisClient *c);
static void randomkeyCommand(redisClient *c);
static void keysCommand(redisClient *c);
static void dbsizeCommand(redisClient *c);
static void lastsaveCommand(redisClient *c);
static void saveCommand(redisClient *c);
static void bgsaveCommand(redisClient *c);
static void bgrewriteaofCommand(redisClient *c);
static void shutdownCommand(redisClient *c);
static void moveCommand(redisClient *c);
static void renameCommand(redisClient *c);
static void renamenxCommand(redisClient *c);
static void lpushCommand(redisClient *c);
static void rpushCommand(redisClient *c);
static void lpopCommand(redisClient *c);
static void rpopCommand(redisClient *c);
static void llenCommand(redisClient *c);
static void lindexCommand(redisClient *c);
static void lrangeCommand(redisClient *c);
static void ltrimCommand(redisClient *c);
static void typeCommand(redisClient *c);
static void lsetCommand(redisClient *c);
static void saddCommand(redisClient *c);
static void sremCommand(redisClient *c);
static void smoveCommand(redisClient *c);
static void sismemberCommand(redisClient *c);
static void scardCommand(redisClient *c);
static void spopCommand(redisClient *c);
static void srandmemberCommand(redisClient *c);
static void sinterCommand(redisClient *c);
static void sinterstoreCommand(redisClient *c);
static void sunionCommand(redisClient *c);
static void sunionstoreCommand(redisClient *c);
static void sdiffCommand(redisClient *c);
static void sdiffstoreCommand(redisClient *c);
static void syncCommand(redisClient *c);
static void flushdbCommand(redisClient *c);
static void flushallCommand(redisClient *c);
static void sortCommand(redisClient *c);
static void lremCommand(redisClient *c);
static void rpoplpushcommand(redisClient *c);
static void infoCommand(redisClient *c);
static void mgetCommand(redisClient *c);
static void monitorCommand(redisClient *c);
static void expireCommand(redisClient *c);
static void expireatCommand(redisClient *c);
static void getsetCommand(redisClient *c);
static void ttlCommand(redisClient *c);
static void slaveofCommand(redisClient *c);
static void debugCommand(redisClient *c);
static void msetCommand(redisClient *c);
static void msetnxCommand(redisClient *c);
static void zaddCommand(redisClient *c);
static void zincrbyCommand(redisClient *c);
static void zrangeCommand(redisClient *c);
static void zrangebyscoreCommand(redisClient *c);
static void zrevrangeCommand(redisClient *c);
static void zcardCommand(redisClient *c);
static void zremCommand(redisClient *c);
static void zscoreCommand(redisClient *c);
static void zremrangebyscoreCommand(redisClient *c);
static void multiCommand(redisClient *c);
static void execCommand(redisClient *c);
static void blpopCommand(redisClient *c);
static void brpopCommand(redisClient *c);

/*================================= Globals ================================= */

/* Global vars */
static struct redisServer server; /* server global state */
static struct redisCommand cmdTable[] = {
	{"get", getCommand, 2, REDIS_CMD_INLINE},
	{"set", setCommand, 3, REDIS_CMD_BULK | REDIS_CMD_DENYOOM},
	{"setnx", setnxCommand, 3, REDIS_CMD_BULK | REDIS_CMD_DENYOOM},
	{"del", delCommand, -2, REDIS_CMD_INLINE},
	{"exists", existsCommand, 2, REDIS_CMD_INLINE},
	{"incr", incrCommand, 2, REDIS_CMD_INLINE | REDIS_CMD_DENYOOM},
	{"decr", decrCommand, 2, REDIS_CMD_INLINE | REDIS_CMD_DENYOOM},
	{"mget", mgetCommand, -2, REDIS_CMD_INLINE},
	{"rpush", rpushCommand, 3, REDIS_CMD_BULK | REDIS_CMD_DENYOOM},
	{"lpush", lpushCommand, 3, REDIS_CMD_BULK | REDIS_CMD_DENYOOM},
	{"rpop", rpopCommand, 2, REDIS_CMD_INLINE},
	{"lpop", lpopCommand, 2, REDIS_CMD_INLINE},
	{"brpop", brpopCommand, -3, REDIS_CMD_INLINE},
	{"blpop", blpopCommand, -3, REDIS_CMD_INLINE},
	{"llen", llenCommand, 2, REDIS_CMD_INLINE},
	{"lindex", lindexCommand, 3, REDIS_CMD_INLINE},
	{"lset", lsetCommand, 4, REDIS_CMD_BULK | REDIS_CMD_DENYOOM},
	{"lrange", lrangeCommand, 4, REDIS_CMD_INLINE},
	{"ltrim", ltrimCommand, 4, REDIS_CMD_INLINE},
	{"lrem", lremCommand, 4, REDIS_CMD_BULK},
	{"rpoplpush", rpoplpushcommand, 3, REDIS_CMD_INLINE | REDIS_CMD_DENYOOM},
	{"sadd", saddCommand, 3, REDIS_CMD_BULK | REDIS_CMD_DENYOOM},
	{"srem", sremCommand, 3, REDIS_CMD_BULK},
	{"smove", smoveCommand, 4, REDIS_CMD_BULK},
	{"sismember", sismemberCommand, 3, REDIS_CMD_BULK},
	{"scard", scardCommand, 2, REDIS_CMD_INLINE},
	{"spop", spopCommand, 2, REDIS_CMD_INLINE},
	{"srandmember", srandmemberCommand, 2, REDIS_CMD_INLINE},
	{"sinter", sinterCommand, -2, REDIS_CMD_INLINE | REDIS_CMD_DENYOOM},
	{"sinterstore", sinterstoreCommand, -3, REDIS_CMD_INLINE | REDIS_CMD_DENYOOM},
	{"sunion", sunionCommand, -2, REDIS_CMD_INLINE | REDIS_CMD_DENYOOM},
	{"sunionstore", sunionstoreCommand, -3, REDIS_CMD_INLINE | REDIS_CMD_DENYOOM},
	{"sdiff", sdiffCommand, -2, REDIS_CMD_INLINE | REDIS_CMD_DENYOOM},
	{"sdiffstore", sdiffstoreCommand, -3, REDIS_CMD_INLINE | REDIS_CMD_DENYOOM},
	{"smembers", sinterCommand, 2, REDIS_CMD_INLINE},
	{"zadd", zaddCommand, 4, REDIS_CMD_BULK | REDIS_CMD_DENYOOM},
	{"zincrby", zincrbyCommand, 4, REDIS_CMD_BULK | REDIS_CMD_DENYOOM},
	{"zrem", zremCommand, 3, REDIS_CMD_BULK},
	{"zremrangebyscore", zremrangebyscoreCommand, 4, REDIS_CMD_INLINE},
	{"zrange", zrangeCommand, -4, REDIS_CMD_INLINE},
	{"zrangebyscore", zrangebyscoreCommand, -4, REDIS_CMD_INLINE},
	{"zrevrange", zrevrangeCommand, -4, REDIS_CMD_INLINE},
	{"zcard", zcardCommand, 2, REDIS_CMD_INLINE},
	{"zscore", zscoreCommand, 3, REDIS_CMD_BULK | REDIS_CMD_DENYOOM},
	{"incrby", incrbyCommand, 3, REDIS_CMD_INLINE | REDIS_CMD_DENYOOM},
	{"decrby", decrbyCommand, 3, REDIS_CMD_INLINE | REDIS_CMD_DENYOOM},
	{"getset", getsetCommand, 3, REDIS_CMD_BULK | REDIS_CMD_DENYOOM},
	{"mset", msetCommand, -3, REDIS_CMD_BULK | REDIS_CMD_DENYOOM},
	{"msetnx", msetnxCommand, -3, REDIS_CMD_BULK | REDIS_CMD_DENYOOM},
	{"randomkey", randomkeyCommand, 1, REDIS_CMD_INLINE},
	{"select", selectCommand, 2, REDIS_CMD_INLINE},
	{"move", moveCommand, 3, REDIS_CMD_INLINE},
	{"rename", renameCommand, 3, REDIS_CMD_INLINE},
	{"renamenx", renamenxCommand, 3, REDIS_CMD_INLINE},
	{"expire", expireCommand, 3, REDIS_CMD_INLINE},
	{"expireat", expireatCommand, 3, REDIS_CMD_INLINE},
	{"keys", keysCommand, 2, REDIS_CMD_INLINE},
	{"dbsize", dbsizeCommand, 1, REDIS_CMD_INLINE},
	{"auth", authCommand, 2, REDIS_CMD_INLINE},
	{"ping", pingCommand, 1, REDIS_CMD_INLINE},
	{"echo", echoCommand, 2, REDIS_CMD_BULK},
	{"save", saveCommand, 1, REDIS_CMD_INLINE},
	{"bgsave", bgsaveCommand, 1, REDIS_CMD_INLINE},
	{"bgrewriteaof", bgrewriteaofCommand, 1, REDIS_CMD_INLINE},
	{"shutdown", shutdownCommand, 1, REDIS_CMD_INLINE},
	{"lastsave", lastsaveCommand, 1, REDIS_CMD_INLINE},
	{"type", typeCommand, 2, REDIS_CMD_INLINE},
	{"multi", multiCommand, 1, REDIS_CMD_INLINE},
	{"exec", execCommand, 1, REDIS_CMD_INLINE},
	{"sync", syncCommand, 1, REDIS_CMD_INLINE},
	{"flushdb", flushdbCommand, 1, REDIS_CMD_INLINE},
	{"flushall", flushallCommand, 1, REDIS_CMD_INLINE},
	{"sort", sortCommand, -2, REDIS_CMD_INLINE | REDIS_CMD_DENYOOM},
	{"info", infoCommand, 1, REDIS_CMD_INLINE},
	{"monitor", monitorCommand, 1, REDIS_CMD_INLINE},
	{"ttl", ttlCommand, 2, REDIS_CMD_INLINE},
	{"slaveof", slaveofCommand, 3, REDIS_CMD_INLINE},
	{"debug", debugCommand, -2, REDIS_CMD_INLINE},
	{NULL, NULL, 0, 0}
};

/*============================ Utility functions ============================ */

/* Glob-style pattern matching. */
int stringmatchlen(const char *pattern, int patternLen,
                   const char *string, int stringLen, int nocase)
{
	while (patternLen) {
		switch (pattern[0]) {
		case '*':
			while (pattern[1] == '*') {
				pattern++;
				patternLen--;
			}
			if (patternLen == 1)
				return 1; /* match */
			while (stringLen) {
				if (stringmatchlen(pattern + 1, patternLen - 1,
				                   string, stringLen, nocase))
					return 1; /* match */
				string++;
				stringLen--;
			}
			return 0; /* no match */
			break;
		case '?':
			if (stringLen == 0)
				return 0; /* no match */
			string++;
			stringLen--;
			break;
		case '[':
		{
			int not, match;

			pattern++;
			patternLen--;
			not = pattern[0] == '^';
			if (not) {
				pattern++;
				patternLen--;
			}
			match = 0;
			while (1) {
				if (pattern[0] == '\\') {
					pattern++;
					patternLen--;
					if (pattern[0] == string[0])
						match = 1;
				} else if (pattern[0] == ']') {
					break;
				} else if (patternLen == 0) {
					pattern--;
					patternLen++;
					break;
				} else if (pattern[1] == '-' && patternLen >= 3) {
					int start = pattern[0];
					int end = pattern[2];
					int c = string[0];
					if (start > end) {
						int t = start;
						start = end;
						end = t;
					}
					if (nocase) {
						start = tolower(start);
						end = tolower(end);
						c = tolower(c);
					}
					pattern += 2;
					patternLen -= 2;
					if (c >= start && c <= end)
						match = 1;
				} else {
					if (!nocase) {
						if (pattern[0] == string[0])
							match = 1;
					} else {
						if (tolower((int)pattern[0]) == tolower((int)string[0]))
							match = 1;
					}
				}
				pattern++;
				patternLen--;
			}
			if (not)
				match = !match;
			if (!match)
				return 0; /* no match */
			string++;
			stringLen--;
			break;
		}
		case '\\':
			if (patternLen >= 2) {
				pattern++;
				patternLen--;
			}
		/* fall through */
		default:
			if (!nocase) {
				if (pattern[0] != string[0])
					return 0; /* no match */
			} else {
				if (tolower((int)pattern[0]) != tolower((int)string[0]))
					return 0; /* no match */
			}
			string++;
			stringLen--;
			break;
		}
		pattern++;
		patternLen--;
		if (stringLen == 0) {
			while (*pattern == '*') {
				pattern++;
				patternLen--;
			}
			break;
		}
	}
	if (patternLen == 0 && stringLen == 0)
		return 1;
	return 0;
}

static void redisLog(int level, const char *fmt, ...) {
	va_list ap;
	FILE *fp;

	fp = (server.logfile == NULL) ? stdout : fopen(server.logfile, "a");
	if (!fp) return;

	va_start(ap, fmt);
	if (level >= server.verbosity) {
		char *c = ".-*";
		char buf[64];
		time_t now;

		now = time(NULL);
		strftime(buf, 64, "%d %b %H:%M:%S", localtime(&now));
		fprintf(fp, "[%d] %s %c ", (int)getpid(), buf, c[level]);
		vfprintf(fp, fmt, ap);
		fprintf(fp, "\n");
		fflush(fp);
	}
	va_end(ap);

	if (server.logfile) fclose(fp);
}

/*====================== Hash table type implementation  ==================== */

/* This is an hash table type that uses the SDS dynamic strings libary as
 * keys and radis objects as values (objects can hold SDS strings,
 * lists, sets). */

static void dictVanillaFree(void *privdata, void *val)
{
	DICT_NOTUSED(privdata);
	zfree(val);
}

static void dictListDestructor(void *privdata, void *val)
{
	DICT_NOTUSED(privdata);
	listRelease((list*)val);
}

static int sdsDictKeyCompare(void *privdata, const void *key1,
                             const void *key2)
{
	int l1, l2;
	DICT_NOTUSED(privdata);

	l1 = sdslen((sds)key1);
	l2 = sdslen((sds)key2);
	if (l1 != l2) return 0;
	return memcmp(key1, key2, l1) == 0;
}

static void dictRedisObjectDestructor(void *privdata, void *val)
{
	DICT_NOTUSED(privdata);

	if (val == NULL) return; /* Values of swapped out keys as set to NULL */
	decrRefCount(val);
}

static int dictObjKeyCompare(void *privdata, const void *key1,
                             const void *key2)
{
	const robj *o1 = key1, *o2 = key2;
	return sdsDictKeyCompare(privdata, o1->ptr, o2->ptr);
}

static unsigned int dictObjHash(const void *key) {
	const robj *o = key;
	return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}

static int dictEncObjKeyCompare(void *privdata, const void *key1,
                                const void *key2)
{
	robj *o1 = (robj*) key1, *o2 = (robj*) key2;
	int cmp;

	o1 = getDecodedObject(o1);
	o2 = getDecodedObject(o2);
	cmp = sdsDictKeyCompare(privdata, o1->ptr, o2->ptr);
	decrRefCount(o1);
	decrRefCount(o2);
	return cmp;
}

static unsigned int dictEncObjHash(const void *key) {
	robj *o = (robj*) key;

	o = getDecodedObject(o);
	unsigned int hash = dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
	decrRefCount(o);
	return hash;
}

/* Sets type and expires */
static dictType setDictType = {
	dictEncObjHash,            /* hash function */
	NULL,                      /* key dup */
	NULL,                      /* val dup */
	dictEncObjKeyCompare,      /* key compare */
	dictRedisObjectDestructor, /* key destructor */
	NULL                       /* val destructor */
};

/* Sorted sets hash (note: a skiplist is used in addition to the hash table) */
static dictType zsetDictType = {
	dictEncObjHash,            /* hash function */
	NULL,                      /* key dup */
	NULL,                      /* val dup */
	dictEncObjKeyCompare,      /* key compare */
	dictRedisObjectDestructor, /* key destructor */
	dictVanillaFree            /* val destructor of malloc(sizeof(double)) */
};

/* Db->dict */
static dictType hashDictType = {
	dictObjHash,                /* hash function */
	NULL,                       /* key dup */
	NULL,                       /* val dup */
	dictObjKeyCompare,          /* key compare */
	dictRedisObjectDestructor,  /* key destructor */
	dictRedisObjectDestructor   /* val destructor */
};

/* Db->expires */
static dictType keyptrDictType = {
	dictObjHash,               /* hash function */
	NULL,                      /* key dup */
	NULL,                      /* val dup */
	dictObjKeyCompare,         /* key compare */
	dictRedisObjectDestructor, /* key destructor */
	NULL                       /* val destructor */
};

/* Keylist hash table type has unencoded redis objects as keys and
 * lists as values. It's used for blocking operations (BLPOP) */
static dictType keylistDictType = {
	dictObjHash,                /* hash function */
	NULL,                       /* key dup */
	NULL,                       /* val dup */
	dictObjKeyCompare,          /* key compare */
	dictRedisObjectDestructor,  /* key destructor */
	dictListDestructor          /* val destructor */
};

/* ========================= Random utility functions ======================= */

/* Redis generally does not try to recover from out of memory conditions
 * when allocating objects or strings, it is not clear if it will be possible
 * to report this condition to the client since the networking layer itself
 * is based on heap allocation for send buffers, so we simply abort.
 * At least the code will be simpler to read... */
static void oom(const char *msg) {
	redisLog(REDIS_WARNING, "%s: Out of memory\n", msg);
	sleep(1);
	abort();
}

/* ====================== Redis server networking stuff ===================== */
// 处理客户端超时的情况（空闲或者阻塞）
static void closeTimedoutClients(void) {
	redisClient *c;
	listNode *ln;
	time_t now = time(NULL);
	listIter li;

	listRewind(server.clients, &li);
	while ((ln = listNext(&li)) != NULL) {
		c = listNodeValue(ln);
		if (server.maxidletime &&
		        !(c->flags & REDIS_SLAVE) &&    /* no timeout for slaves */
		        !(c->flags & REDIS_MASTER) &&   /* no timeout for masters */
		        (now - c->lastinteraction > server.maxidletime))
		{
			// 处理客户端空闲超时情况
			// 即使客户端在阻塞状态等待，但是超过最大空闲时间，也会关闭连接
			redisLog(REDIS_VERBOSE, "Closing idle client");
			freeClient(c);
		} else if (c->flags & REDIS_BLOCKED) {
			// 处理客户端阻塞超时情况
			if (c->blockingto != 0 && c->blockingto < now) {
				// 超时，应答Null
				addReply(c, shared.nullmultibulk);
				unblockClientWaitingData(c);
			}
		}
	}
}

static int htNeedsResize(dict *dict) {
	long long size, used;

	size = dictSlots(dict);
	used = dictSize(dict);
	return (size && used && size > DICT_HT_INITIAL_SIZE &&
	        (used * 100 / size < REDIS_HT_MINFILL));
}

/* If the percentage of used slots in the HT reaches REDIS_HT_MINFILL
 * we resize the hash table to save memory */
static void tryResizeHashTables(void) {
	int j;

	for (j = 0; j < server.dbnum; j++) {
		if (htNeedsResize(server.db[j].dict)) {
			redisLog(REDIS_VERBOSE, "The hash table %d is too sparse, resize it...", j);
			dictResize(server.db[j].dict);
			redisLog(REDIS_VERBOSE, "Hash table %d resized.", j);
		}
		if (htNeedsResize(server.db[j].expires))
			dictResize(server.db[j].expires);
	}
}

/* A background saving child (BGSAVE) terminated its work. Handle this. */
void backgroundSaveDoneHandler(int statloc) {
	int exitcode = WEXITSTATUS(statloc);
	int bysignal = WIFSIGNALED(statloc);

	if (!bysignal && exitcode == 0) {
		redisLog(REDIS_NOTICE,
		         "Background saving terminated with success");
		server.dirty = 0;
		server.lastsave = time(NULL);
	} else if (!bysignal && exitcode != 0) {
		redisLog(REDIS_WARNING, "Background saving error");
	} else {
		redisLog(REDIS_WARNING,
		         "Background saving terminated by signal");
		rdbRemoveTempFile(server.bgsavechildpid);
	}
	server.bgsavechildpid = -1;
	/* Possibly there are slaves waiting for a BGSAVE in order to be served
	 * (the first stage of SYNC is a bulk transfer of dump.rdb) */
	updateSlavesWaitingBgsave(exitcode == 0 ? REDIS_OK : REDIS_ERR);
}

/* A background append only file rewriting (BGREWRITEAOF) terminated its work.
 * Handle this. */
void backgroundRewriteDoneHandler(int statloc) {
	int exitcode = WEXITSTATUS(statloc);
	int bysignal = WIFSIGNALED(statloc);

	if (!bysignal && exitcode == 0) {
		int fd;
		char tmpfile[256];

		redisLog(REDIS_NOTICE,
		         "Background append only file rewriting terminated with success");
		/* Now it's time to flush the differences accumulated by the parent */
		snprintf(tmpfile, 256, "temp-rewriteaof-bg-%d.aof", (int) server.bgrewritechildpid);
		fd = open(tmpfile, O_WRONLY | O_APPEND);
		if (fd == -1) {
			redisLog(REDIS_WARNING, "Not able to open the temp append only file produced by the child: %s", strerror(errno));
			goto cleanup;
		}
		/* Flush our data... */
		if (write(fd, server.bgrewritebuf, sdslen(server.bgrewritebuf)) !=
		        (signed) sdslen(server.bgrewritebuf)) {
			redisLog(REDIS_WARNING, "Error or short write trying to flush the parent diff of the append log file in the child temp file: %s", strerror(errno));
			close(fd);
			goto cleanup;
		}
		redisLog(REDIS_NOTICE, "Parent diff flushed into the new append log file with success (%lu bytes)", sdslen(server.bgrewritebuf));
		/* Now our work is to rename the temp file into the stable file. And
		 * switch the file descriptor used by the server for append only. */
		if (rename(tmpfile, server.appendfilename) == -1) {
			redisLog(REDIS_WARNING, "Can't rename the temp append only file into the stable one: %s", strerror(errno));
			close(fd);
			goto cleanup;
		}
		/* Mission completed... almost */
		redisLog(REDIS_NOTICE, "Append only file successfully rewritten.");
		if (server.appendfd != -1) {
			/* If append only is actually enabled... */
			close(server.appendfd);
			server.appendfd = fd;
			fsync(fd);
			server.appendseldb = -1; /* Make sure it will issue SELECT */
			redisLog(REDIS_NOTICE, "The new append only file was selected for future appends.");
		} else {
			/* If append only is disabled we just generate a dump in this
			 * format. Why not? */
			close(fd);
		}
	} else if (!bysignal && exitcode != 0) {
		redisLog(REDIS_WARNING, "Background append only file rewriting error");
	} else {
		redisLog(REDIS_WARNING,
		         "Background append only file rewriting terminated by signal");
	}
cleanup:
	sdsfree(server.bgrewritebuf);
	server.bgrewritebuf = sdsempty();
	aofRemoveTempFile(server.bgrewritechildpid);
	server.bgrewritechildpid = -1;
}

static int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
	int j, loops = server.cronloops++;
	REDIS_NOTUSED(eventLoop);
	REDIS_NOTUSED(id);
	REDIS_NOTUSED(clientData);

	/* We take a cached value of the unix time in the global state because
	 * with virtual memory and aging there is to store the current time
	 * in objects at every object access, and accuracy is not needed.
	 * To access a global var is faster than calling time(NULL) */
	server.unixtime = time(NULL);

	/* Show some info about non-empty databases */
	for (j = 0; j < server.dbnum; j++) {
		long long size, used, vkeys;

		size = dictSlots(server.db[j].dict);
		used = dictSize(server.db[j].dict);
		vkeys = dictSize(server.db[j].expires);
		if (!(loops % 5) && (used || vkeys)) {
			redisLog(REDIS_VERBOSE, "DB %d: %lld keys (%lld volatile) in %lld slots HT.", j, used, vkeys, size);
			/* dictPrintStats(server.dict); */
		}
	}

	/* We don't want to resize the hash tables while a bacground saving
	 * is in progress: the saving child is created using fork() that is
	 * implemented with a copy-on-write semantic in most modern systems, so
	 * if we resize the HT while there is the saving child at work actually
	 * a lot of memory movements in the parent will cause a lot of pages
	 * copied. */
	if (server.bgsavechildpid == -1) tryResizeHashTables();

	/* Show information about connected clients */
	if (!(loops % 5)) {
		redisLog(REDIS_VERBOSE, "%d clients connected (%d slaves), %zu bytes in use, %d shared objects",
		         listLength(server.clients) - listLength(server.slaves),
		         listLength(server.slaves),
		         zmalloc_used_memory(),
		         dictSize(server.sharingpool));
	}

	/* Close connections of timedout clients */
	if ((server.maxidletime && !(loops % 10)) || server.blockedclients)
		closeTimedoutClients();

	/* Check if a background saving or AOF rewrite in progress terminated */
	if (server.bgsavechildpid != -1 || server.bgrewritechildpid != -1) {
		int statloc;
		pid_t pid;

		if ((pid = wait3(&statloc, WNOHANG, NULL)) != 0) {
			if (pid == server.bgsavechildpid) {
				backgroundSaveDoneHandler(statloc);
			} else {
				backgroundRewriteDoneHandler(statloc);
			}
		}
	} else {
		/* If there is not a background saving in progress check if
		 * we have to save now */
		time_t now = time(NULL);
		for (j = 0; j < server.saveparamslen; j++) {
			struct saveparam *sp = server.saveparams + j;

			if (server.dirty >= sp->changes &&
			        now - server.lastsave > sp->seconds) {
				redisLog(REDIS_NOTICE, "%d changes in %d seconds. Saving...",
				         sp->changes, sp->seconds);
				rdbSaveBackground(server.dbfilename);
				break;
			}
		}
	}

	/* Try to expire a few timed out keys. The algorithm used is adaptive and
	 * will use few CPU cycles if there are few expiring keys, otherwise
	 * it will get more aggressive to avoid that too much memory is used by
	 * keys that can be removed from the keyspace. */
	for (j = 0; j < server.dbnum; j++) {
		int expired;
		redisDb *db = server.db + j;

		/* Continue to expire if at the end of the cycle more than 25%
		 * of the keys were expired. */
		do {
			long num = dictSize(db->expires);
			time_t now = time(NULL);

			expired = 0;
			if (num > REDIS_EXPIRELOOKUPS_PER_CRON)
				num = REDIS_EXPIRELOOKUPS_PER_CRON;
			while (num--) {
				dictEntry *de;
				time_t t;

				if ((de = dictGetRandomKey(db->expires)) == NULL) break;
				t = (time_t) dictGetEntryVal(de);
				if (now > t) {
					deleteKey(db, dictGetEntryKey(de));
					expired++;
				}
			}
		} while (expired > REDIS_EXPIRELOOKUPS_PER_CRON / 4);
	}

	/* Swap a few keys on disk if we are over the memory limit and VM
	 * is enbled. Try to free objects from the free list first. */
	if (vmCanSwapOut()) {
		while (server.vm_enabled && zmalloc_used_memory() >
		        server.vm_max_memory)
		{
			int retval;

			if (tryFreeOneObjectFromFreelist() == REDIS_OK) continue;
			retval = (server.vm_max_threads == 0) ?
			         vmSwapOneObjectBlocking() :
			         vmSwapOneObjectThreaded();
			if (retval == REDIS_ERR && (loops % 30) == 0 &&
			        zmalloc_used_memory() >
			        (server.vm_max_memory + server.vm_max_memory / 10))
			{
				redisLog(REDIS_WARNING, "WARNING: vm-max-memory limit exceeded by more than 10%% but unable to swap more objects out!");
			}
			/* Note that when using threade I/O we free just one object,
			 * because anyway when the I/O thread in charge to swap this
			 * object out will finish, the handler of completed jobs
			 * will try to swap more objects if we are still out of memory. */
			if (retval == REDIS_ERR || server.vm_max_threads > 0) break;
		}
	}

	/* Check if we should connect to a MASTER */
	if (server.replstate == REDIS_REPL_CONNECT) {
		redisLog(REDIS_NOTICE, "Connecting to MASTER...");
		if (syncWithMaster() == REDIS_OK) {
			redisLog(REDIS_NOTICE, "MASTER <-> SLAVE sync succeeded");
		}
	}
	return 1000;
}

static void createSharedObjects(void) {
	shared.crlf = createObject(REDIS_STRING, sdsnew("\r\n"));
	shared.ok = createObject(REDIS_STRING, sdsnew("+OK\r\n"));
	shared.err = createObject(REDIS_STRING, sdsnew("-ERR\r\n"));
	shared.emptybulk = createObject(REDIS_STRING, sdsnew("$0\r\n\r\n"));
	shared.czero = createObject(REDIS_STRING, sdsnew(":0\r\n"));
	shared.cone = createObject(REDIS_STRING, sdsnew(":1\r\n"));
	shared.nullbulk = createObject(REDIS_STRING, sdsnew("$-1\r\n"));
	shared.nullmultibulk = createObject(REDIS_STRING, sdsnew("*-1\r\n"));
	shared.emptymultibulk = createObject(REDIS_STRING, sdsnew("*0\r\n"));
	shared.pong = createObject(REDIS_STRING, sdsnew("+PONG\r\n"));
	shared.queued = createObject(REDIS_STRING, sdsnew("+QUEUED\r\n"));
	shared.wrongtypeerr = createObject(REDIS_STRING, sdsnew(
	                                       "-ERR Operation against a key holding the wrong kind of value\r\n"));
	shared.nokeyerr = createObject(REDIS_STRING, sdsnew(
	                                   "-ERR no such key\r\n"));
	shared.syntaxerr = createObject(REDIS_STRING, sdsnew(
	                                    "-ERR syntax error\r\n"));
	shared.sameobjecterr = createObject(REDIS_STRING, sdsnew(
	                                        "-ERR source and destination objects are the same\r\n"));
	shared.outofrangeerr = createObject(REDIS_STRING, sdsnew(
	                                        "-ERR index out of range\r\n"));
	shared.space = createObject(REDIS_STRING, sdsnew(" "));
	shared.colon = createObject(REDIS_STRING, sdsnew(":"));
	shared.plus = createObject(REDIS_STRING, sdsnew("+"));
	shared.select0 = createStringObject("select 0\r\n", 10);
	shared.select1 = createStringObject("select 1\r\n", 10);
	shared.select2 = createStringObject("select 2\r\n", 10);
	shared.select3 = createStringObject("select 3\r\n", 10);
	shared.select4 = createStringObject("select 4\r\n", 10);
	shared.select5 = createStringObject("select 5\r\n", 10);
	shared.select6 = createStringObject("select 6\r\n", 10);
	shared.select7 = createStringObject("select 7\r\n", 10);
	shared.select8 = createStringObject("select 8\r\n", 10);
	shared.select9 = createStringObject("select 9\r\n", 10);
}

static void appendServerSaveParams(time_t seconds, int changes) {
	server.saveparams = zrealloc(server.saveparams, sizeof(struct saveparam) * (server.saveparamslen + 1));
	server.saveparams[server.saveparamslen].seconds = seconds;
	server.saveparams[server.saveparamslen].changes = changes;
	server.saveparamslen++;
}

// 清空掉save参数
static void resetServerSaveParams() {
	zfree(server.saveparams);
	server.saveparams = NULL;
	server.saveparamslen = 0;
}

// 初始化默认配置
static void initServerConfig() {
	server.dbnum = REDIS_DEFAULT_DBNUM;    //16
	server.port = REDIS_SERVERPORT;        //6379
	server.verbosity = REDIS_VERBOSE;
	server.maxidletime = REDIS_MAXIDLETIME;//60*5 5分钟
	server.saveparams = NULL;
	server.logfile = NULL; /* NULL = log on standard output */
	server.bindaddr = NULL;
	server.glueoutputbuf = 1;
	server.daemonize = 0;
	server.appendonly = 0;
	// 在写aof后总是执行fsync,
	server.appendfsync = APPENDFSYNC_ALWAYS;
	server.lastfsync = time(NULL);
	server.appendfd = -1;
	server.appendseldb = -1; /* Make sure the first time will not match */
	server.pidfile = "/var/run/redis.pid";
	server.dbfilename = "dump.rdb";
	server.appendfilename = "appendonly.aof";
	server.requirepass = NULL;
	server.shareobjects = 0;
	server.rdbcompression = 1;
	server.sharingpoolsize = 1024;
	server.maxclients = 0; // 0为没有限制
	server.blockedclients = 0;
	server.maxmemory = 0;
	server.vm_enabled = 0;
	server.vm_swap_file = zstrdup("/tmp/redis-%p.vm");
	server.vm_page_size = 256;          /* 256 bytes per page */
	server.vm_pages = 1024 * 1024 * 100; /* 104 millions of pages */
	server.vm_max_memory = 1024LL * 1024 * 1024 * 1; /* 1 GB of RAM */
	server.vm_max_threads = 4;

	resetServerSaveParams();

	// 默认有三个触发阈值
	// 一小时改变一次
	appendServerSaveParams(60 * 60, 1); /* save after 1 hour and 1 change */
	// 5分钟改变100次
	appendServerSaveParams(300, 100); /* save after 5 minutes and 100 changes */
	// 1分钟改变10000次
	appendServerSaveParams(60, 10000); /* save after 1 minute and 10000 changes */
	/* Replication related */
	server.isslave = 0;
	server.masterauth = NULL;
	server.masterhost = NULL;
	server.masterport = 6379;
	server.master = NULL;
	server.replstate = REDIS_REPL_NONE;

	/* Double constants initialization */
	R_Zero = 0.0;
	R_PosInf = 1.0 / R_Zero;
	R_NegInf = -1.0 / R_Zero;
	R_Nan = R_Zero / R_Zero;
}

static void initServer() {
	int j;

	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	setupSigSegvAction();

	server.devnull = fopen("/dev/null", "w");
	if (server.devnull == NULL) {
		redisLog(REDIS_WARNING, "Can't open /dev/null: %s", server.neterr);
		exit(1);
	}
	server.clients = listCreate();
	server.slaves = listCreate();
	server.monitors = listCreate();
	server.objfreelist = listCreate();
	createSharedObjects();
	// 创建事件循环
	server.el = aeCreateEventLoop();
	server.db = zmalloc(sizeof(redisDb) * server.dbnum);
	server.sharingpool = dictCreate(&setDictType, NULL);
	// 监听端口
	server.fd = anetTcpServer(server.neterr, server.port, server.bindaddr);
	if (server.fd == -1) {
		redisLog(REDIS_WARNING, "Opening TCP port: %s", server.neterr);
		exit(1);
	}
	// 依次创建数据库
	for (j = 0; j < server.dbnum; j++) {
		server.db[j].dict = dictCreate(&hashDictType, NULL);
		server.db[j].expires = dictCreate(&keyptrDictType, NULL);
		server.db[j].blockingkeys = dictCreate(&keylistDictType, NULL);
		server.db[j].id = j;
	}
	server.cronloops = 0;
	server.bgsavechildpid = -1;
	server.bgrewritechildpid = -1;
	server.bgrewritebuf = sdsempty();
	server.lastsave = time(NULL);
	server.dirty = 0;
	server.stat_numcommands = 0;
	server.stat_numconnections = 0;
	server.stat_starttime = time(NULL);
	server.unixtime = time(NULL);
	// 创建定时器，1ms执行一次（不精确）
	aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL);
	// 创建客户端接入处理句柄
	if (aeCreateFileEvent(server.el, server.fd, AE_READABLE,
	                      acceptHandler, NULL) == AE_ERR) oom("creating file event");

	if (server.appendonly) {
		// 如果开启了AOF，则预先打开文件
		server.appendfd = open(server.appendfilename, O_WRONLY | O_APPEND | O_CREAT, 0644);
		if (server.appendfd == -1) {
			redisLog(REDIS_WARNING, "Can't open the append-only file: %s",
			         strerror(errno));
			exit(1);
		}
	}

	if (server.vm_enabled) vmInit();
}

/* Empty the whole database */
static long long emptyDb() {
	int j;
	long long removed = 0;

	for (j = 0; j < server.dbnum; j++) {
		removed += dictSize(server.db[j].dict);
		dictEmpty(server.db[j].dict);
		dictEmpty(server.db[j].expires);
	}
	return removed;
}

static int yesnotoi(char *s) {
	if (!strcasecmp(s, "yes")) return 1;
	else if (!strcasecmp(s, "no")) return 0;
	else return -1;
}

/* I agree, this is a very rudimental way to load a configuration...
   will improve later if the config gets more complex */
static void loadServerConfig(char *filename) {
	FILE *fp;
	char buf[REDIS_CONFIGLINE_MAX + 1], *err = NULL;
	int linenum = 0;
	sds line = NULL;

	if (filename[0] == '-' && filename[1] == '\0')
		fp = stdin;
	else {
		if ((fp = fopen(filename, "r")) == NULL) {
			redisLog(REDIS_WARNING, "Fatal error, can't open config file");
			exit(1);
		}
	}

	while (fgets(buf, REDIS_CONFIGLINE_MAX + 1, fp) != NULL) {
		sds *argv;
		int argc, j;

		linenum++;
		line = sdsnew(buf);
		line = sdstrim(line, " \t\r\n");

		/* Skip comments and blank lines*/
		if (line[0] == '#' || line[0] == '\0') {
			sdsfree(line);
			continue;
		}

		/* Split into arguments */
		argv = sdssplitlen(line, sdslen(line), " ", 1, &argc);
		sdstolower(argv[0]);

		/* Execute config directives */
		if (!strcasecmp(argv[0], "timeout") && argc == 2) {
			server.maxidletime = atoi(argv[1]);
			if (server.maxidletime < 0) {
				err = "Invalid timeout value"; goto loaderr;
			}
		} else if (!strcasecmp(argv[0], "port") && argc == 2) {
			server.port = atoi(argv[1]);
			if (server.port < 1 || server.port > 65535) {
				err = "Invalid port"; goto loaderr;
			}
		} else if (!strcasecmp(argv[0], "bind") && argc == 2) {
			server.bindaddr = zstrdup(argv[1]);
		} else if (!strcasecmp(argv[0], "save") && argc == 3) {
			int seconds = atoi(argv[1]);
			int changes = atoi(argv[2]);
			if (seconds < 1 || changes < 0) {
				err = "Invalid save parameters"; goto loaderr;
			}
			appendServerSaveParams(seconds, changes);
		} else if (!strcasecmp(argv[0], "dir") && argc == 2) {
			if (chdir(argv[1]) == -1) {
				redisLog(REDIS_WARNING, "Can't chdir to '%s': %s",
				         argv[1], strerror(errno));
				exit(1);
			}
		} else if (!strcasecmp(argv[0], "loglevel") && argc == 2) {
			if (!strcasecmp(argv[1], "debug")) server.verbosity = REDIS_DEBUG;
			else if (!strcasecmp(argv[1], "verbose")) server.verbosity = REDIS_VERBOSE;
			else if (!strcasecmp(argv[1], "notice")) server.verbosity = REDIS_NOTICE;
			else if (!strcasecmp(argv[1], "warning")) server.verbosity = REDIS_WARNING;
			else {
				err = "Invalid log level. Must be one of debug, notice, warning";
				goto loaderr;
			}
		} else if (!strcasecmp(argv[0], "logfile") && argc == 2) {
			FILE *logfp;

			server.logfile = zstrdup(argv[1]);
			if (!strcasecmp(server.logfile, "stdout")) {
				zfree(server.logfile);
				server.logfile = NULL;
			}
			if (server.logfile) {
				/* Test if we are able to open the file. The server will not
				 * be able to abort just for this problem later... */
				logfp = fopen(server.logfile, "a");
				if (logfp == NULL) {
					err = sdscatprintf(sdsempty(),
					                   "Can't open the log file: %s", strerror(errno));
					goto loaderr;
				}
				fclose(logfp);
			}
		} else if (!strcasecmp(argv[0], "databases") && argc == 2) {
			server.dbnum = atoi(argv[1]);
			if (server.dbnum < 1) {
				err = "Invalid number of databases"; goto loaderr;
			}
		} else if (!strcasecmp(argv[0], "maxclients") && argc == 2) {
			server.maxclients = atoi(argv[1]);
		} else if (!strcasecmp(argv[0], "maxmemory") && argc == 2) {
			server.maxmemory = strtoll(argv[1], NULL, 10);
		} else if (!strcasecmp(argv[0], "slaveof") && argc == 3) {
			server.masterhost = sdsnew(argv[1]);
			server.masterport = atoi(argv[2]);
			server.replstate = REDIS_REPL_CONNECT;
		} else if (!strcasecmp(argv[0], "masterauth") && argc == 2) {
			server.masterauth = zstrdup(argv[1]);
		} else if (!strcasecmp(argv[0], "glueoutputbuf") && argc == 2) {
			if ((server.glueoutputbuf = yesnotoi(argv[1])) == -1) {
				err = "argument must be 'yes' or 'no'"; goto loaderr;
			}
		} else if (!strcasecmp(argv[0], "shareobjects") && argc == 2) {
			if ((server.shareobjects = yesnotoi(argv[1])) == -1) {
				err = "argument must be 'yes' or 'no'"; goto loaderr;
			}
		} else if (!strcasecmp(argv[0], "rdbcompression") && argc == 2) {
			if ((server.rdbcompression = yesnotoi(argv[1])) == -1) {
				err = "argument must be 'yes' or 'no'"; goto loaderr;
			}
		} else if (!strcasecmp(argv[0], "shareobjectspoolsize") && argc == 2) {
			server.sharingpoolsize = atoi(argv[1]);
			if (server.sharingpoolsize < 1) {
				err = "invalid object sharing pool size"; goto loaderr;
			}
		} else if (!strcasecmp(argv[0], "daemonize") && argc == 2) {
			if ((server.daemonize = yesnotoi(argv[1])) == -1) {
				err = "argument must be 'yes' or 'no'"; goto loaderr;
			}
		} else if (!strcasecmp(argv[0], "appendonly") && argc == 2) {
			if ((server.appendonly = yesnotoi(argv[1])) == -1) {
				err = "argument must be 'yes' or 'no'"; goto loaderr;
			}
		} else if (!strcasecmp(argv[0], "appendfsync") && argc == 2) {
			if (!strcasecmp(argv[1], "no")) {
				server.appendfsync = APPENDFSYNC_NO;
			} else if (!strcasecmp(argv[1], "always")) {
				server.appendfsync = APPENDFSYNC_ALWAYS;
			} else if (!strcasecmp(argv[1], "everysec")) {
				server.appendfsync = APPENDFSYNC_EVERYSEC;
			} else {
				err = "argument must be 'no', 'always' or 'everysec'";
				goto loaderr;
			}
		} else if (!strcasecmp(argv[0], "requirepass") && argc == 2) {
			server.requirepass = zstrdup(argv[1]);
		} else if (!strcasecmp(argv[0], "pidfile") && argc == 2) {
			server.pidfile = zstrdup(argv[1]);
		} else if (!strcasecmp(argv[0], "dbfilename") && argc == 2) {
			server.dbfilename = zstrdup(argv[1]);
		} else if (!strcasecmp(argv[0], "vm-enabled") && argc == 2) {
			if ((server.vm_enabled = yesnotoi(argv[1])) == -1) {
				err = "argument must be 'yes' or 'no'"; goto loaderr;
			}
		} else if (!strcasecmp(argv[0], "vm-swap-file") && argc == 2) {
			zfree(server.vm_swap_file);
			server.vm_swap_file = zstrdup(argv[1]);
		} else if (!strcasecmp(argv[0], "vm-max-memory") && argc == 2) {
			server.vm_max_memory = strtoll(argv[1], NULL, 10);
		} else if (!strcasecmp(argv[0], "vm-page-size") && argc == 2) {
			server.vm_page_size = strtoll(argv[1], NULL, 10);
		} else if (!strcasecmp(argv[0], "vm-pages") && argc == 2) {
			server.vm_pages = strtoll(argv[1], NULL, 10);
		} else if (!strcasecmp(argv[0], "vm-max-threads") && argc == 2) {
			server.vm_max_threads = strtoll(argv[1], NULL, 10);
		} else {
			err = "Bad directive or wrong number of arguments"; goto loaderr;
		}
		for (j = 0; j < argc; j++)
			sdsfree(argv[j]);
		zfree(argv);
		sdsfree(line);
	}
	if (fp != stdin) fclose(fp);
	return;

loaderr:
	fprintf(stderr, "\n*** FATAL CONFIG FILE ERROR ***\n");
	fprintf(stderr, "Reading the configuration file, at line %d\n", linenum);
	fprintf(stderr, ">>> '%s'\n", line);
	fprintf(stderr, "%s\n", err);
	exit(1);
}

static void freeClientArgv(redisClient *c) {
	int j;

	for (j = 0; j < c->argc; j++)
		decrRefCount(c->argv[j]);
	for (j = 0; j < c->mbargc; j++)
		decrRefCount(c->mbargv[j]);
	c->argc = 0;
	c->mbargc = 0;
}

static void freeClient(redisClient *c) {
	listNode *ln;

	/* Note that if the client we are freeing is blocked into a blocking
	 * call, we have to set querybuf to NULL *before* to call
	 * unblockClientWaitingData() to avoid processInputBuffer() will get
	 * called. Also it is important to remove the file events after
	 * this, because this call adds the READABLE event. */
	sdsfree(c->querybuf);
	c->querybuf = NULL;
	if (c->flags & REDIS_BLOCKED)
		unblockClientWaitingData(c);

	aeDeleteFileEvent(server.el, c->fd, AE_READABLE);
	aeDeleteFileEvent(server.el, c->fd, AE_WRITABLE);
	listRelease(c->reply);
	freeClientArgv(c);
	close(c->fd);
	/* Remove from the list of clients */
	ln = listSearchKey(server.clients, c);
	redisAssert(ln != NULL);
	listDelNode(server.clients, ln);
	/* Remove from the list of clients waiting for VM operations */
	if (server.vm_enabled && listLength(c->io_keys)) {
		ln = listSearchKey(server.io_clients, c);
		if (ln) listDelNode(server.io_clients, ln);
		listRelease(c->io_keys);
	}
	listRelease(c->io_keys);
	/* Other cleanup */
	if (c->flags & REDIS_SLAVE) {
		if (c->replstate == REDIS_REPL_SEND_BULK && c->repldbfd != -1)
			close(c->repldbfd);
		list *l = (c->flags & REDIS_MONITOR) ? server.monitors : server.slaves;
		ln = listSearchKey(l, c);
		redisAssert(ln != NULL);
		listDelNode(l, ln);
	}
	if (c->flags & REDIS_MASTER) {
		server.master = NULL;
		server.replstate = REDIS_REPL_CONNECT;
	}
	zfree(c->argv);
	zfree(c->mbargv);
	freeClientMultiState(c);
	zfree(c);
}

#define GLUEREPLY_UP_TO (1024)
static void glueReplyBuffersIfNeeded(redisClient *c) {
	int copylen = 0;
	char buf[GLUEREPLY_UP_TO];
	listNode *ln;
	listIter li;
	robj *o;

	listRewind(c->reply, &li);
	while ((ln = listNext(&li))) {
		int objlen;

		o = ln->value;
		objlen = sdslen(o->ptr);
		if (copylen + objlen <= GLUEREPLY_UP_TO) {
			memcpy(buf + copylen, o->ptr, objlen);
			copylen += objlen;
			listDelNode(c->reply, ln);
		} else {
			// copylen为0表示第一个消息已经大于1024了，无需组合起来
			if (copylen == 0) return;
			break;
		}
	}
	/* Now the output buffer is empty, add the new single element */
	// 到了这里，表示buf中有数据，可能是由多条短消息组合而成的，封装成对象，重新放回到应答列表中
	o = createObject(REDIS_STRING, sdsnewlen(buf, copylen));
	listAddNodeHead(c->reply, o);
}

static void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
	redisClient *c = privdata;
	int nwritten = 0, totwritten = 0, objlen;
	robj *o;
	REDIS_NOTUSED(el);
	REDIS_NOTUSED(mask);

	/* 如果没有开启glueoutputbuf，但是当前的应答已经大于设定的阈值
	*  并且客户端不是Master，则会使用writev批量发送数据
	*/
	/* Use writev() if we have enough buffers to send */
	if (!server.glueoutputbuf &&
	        listLength(c->reply) > REDIS_WRITEV_THRESHOLD &&
	        !(c->flags & REDIS_MASTER))
	{
		sendReplyToClientWritev(el, fd, privdata, mask);
		return;
	}

	while (listLength(c->reply)) {
		// 如果开启了批量发送，并且有超过两个应答消息，则尽可能的将多条短小的消息组合成一条
		if (server.glueoutputbuf && listLength(c->reply) > 1)
			glueReplyBuffersIfNeeded(c);

		o = listNodeValue(listFirst(c->reply));
		objlen = sdslen(o->ptr);

		if (objlen == 0) {
			listDelNode(c->reply, listFirst(c->reply));
			continue;
		}

		if (c->flags & REDIS_MASTER) {
			/* Don't reply to a master */
			// 这里不发送，仅仅是记录为全部数据已发送出去（如果为Master，则sentlen的值为0）
			nwritten = objlen - c->sentlen;
		} else {
			// nwritten 此次发送出去的字节
			nwritten = write(fd, ((char*)o->ptr) + c->sentlen, objlen - c->sentlen);
			if (nwritten <= 0) break;
		}
		// 偏移此次发送出去的字节数
		c->sentlen += nwritten;
		// totwritten用于控制当次最大发送量，防止在写上阻塞过长时间
		totwritten += nwritten;
		/* If we fully sent the object on head go to the next one */
		if (c->sentlen == objlen) {
			// 当前对象的数据已经全部发送出去了，所以可以从应答列表中删除
			listDelNode(c->reply, listFirst(c->reply));
			c->sentlen = 0;
		}
		/* Note that we avoid to send more thank REDIS_MAX_WRITE_PER_EVENT
		 * bytes, in a single threaded server it's a good idea to serve
		 * other clients as well, even if a very large request comes from
		 * super fast link that is always able to accept data (in real world
		 * scenario think about 'KEYS *' against the loopback interfae) */
		// 一次最多发送64K
		if (totwritten > REDIS_MAX_WRITE_PER_EVENT) break;
	}
	if (nwritten == -1) {
		if (errno == EAGAIN) {
			nwritten = 0;
		} else {
			redisLog(REDIS_VERBOSE,
			         "Error writing to client: %s", strerror(errno));
			freeClient(c);
			return;
		}
	}
	// 只有当总发送字节数大于0，才会断定会这是一次有效的会话，设置最后交互时间
	if (totwritten > 0) c->lastinteraction = time(NULL);
	if (listLength(c->reply) == 0) {
		// 所有应答消息已经发送完毕，可以从事件循环中移除写句柄
		c->sentlen = 0;
		aeDeleteFileEvent(server.el, c->fd, AE_WRITABLE);
	}
}

static void sendReplyToClientWritev(aeEventLoop *el, int fd, void *privdata, int mask)
{
	redisClient *c = privdata;
	int nwritten = 0, totwritten = 0, objlen, willwrite;
	robj *o;
	struct iovec iov[REDIS_WRITEV_IOVEC_COUNT];
	int offset, ion = 0;
	REDIS_NOTUSED(el);
	REDIS_NOTUSED(mask);

	listNode *node;
	while (listLength(c->reply)) {
		// 获取当前对象已发送的偏移
		offset = c->sentlen;
		ion = 0;
		// 用于标识是否有数据要发送
		willwrite = 0;

		/* fill-in the iov[] array */
		for (node = listFirst(c->reply); node; node = listNextNode(node)) {
			// 下面循环填充iov缓冲区
			o = listNodeValue(node);
			objlen = sdslen(o->ptr);

			// 如果当前对象剩余的数据量(objlen-offset)已经大于64K，则不使用writev发送
			// 或者已经发送的数据加上剩余的大于64K，则退出（控制单次处理发送数量）
			if (totwritten + objlen - offset > REDIS_MAX_WRITE_PER_EVENT)
				break;

			// 一次最多256个
			if (ion == REDIS_WRITEV_IOVEC_COUNT)
				break; /* no more iovecs */

			iov[ion].iov_base = ((char*)o->ptr) + offset;
			iov[ion].iov_len = objlen - offset;
			willwrite += objlen - offset;
			offset = 0; /* just for the first item */
			ion++;
		}

		if (willwrite == 0)
			break;

		/* write all collected blocks at once */
		if ((nwritten = writev(fd, iov, ion)) < 0) {
			if (errno != EAGAIN) {
				redisLog(REDIS_VERBOSE,
				         "Error writing to client: %s", strerror(errno));
				freeClient(c);
				return;
			}
			break;
		}

		totwritten += nwritten;
		offset = c->sentlen;

		/* remove written robjs from c->reply */
		// 移除所有已发送的应答
		while (nwritten && listLength(c->reply)) {
			o = listNodeValue(listFirst(c->reply));
			objlen = sdslen(o->ptr);

			if (nwritten >= objlen - offset) {
				listDelNode(c->reply, listFirst(c->reply));
				nwritten -= objlen - offset;
				c->sentlen = 0;
			} else {
				/* partial write */
				// 只写了一半
				c->sentlen += nwritten;
				break;
			}
			offset = 0;
		}
	}

	if (totwritten > 0)
		c->lastinteraction = time(NULL);

	if (listLength(c->reply) == 0) {
		c->sentlen = 0;
		aeDeleteFileEvent(server.el, c->fd, AE_WRITABLE);
	}
}

static struct redisCommand *lookupCommand(char *name) {
	int j = 0;
	while (cmdTable[j].name != NULL) {
		if (!strcasecmp(name, cmdTable[j].name)) return &cmdTable[j];
		j++;
	}
	return NULL;
}

/* resetClient prepare the client to process the next command */
static void resetClient(redisClient *c) {
	freeClientArgv(c);
	c->bulklen = -1;
	c->multibulk = 0;
}

/* Call() is the core of Redis execution of a command */
static void call(redisClient *c, struct redisCommand *cmd) {
	long long dirty;

	// 保存当前数据变动情况，如果调用命令后，数据变动了，则做相应操作（如AOF，Replication）
	dirty = server.dirty;
	cmd->proc(c);
	if (server.appendonly && server.dirty - dirty)
		// 开启了AOF，数据改变了数据，将此命令追加到文件中
		feedAppendOnlyFile(cmd, c->db->id, c->argv, c->argc);
	if (server.dirty - dirty && listLength(server.slaves))
		// 数据由变动，并且有备机，则将变动的命令发送到备机
		replicationFeedSlaves(server.slaves, cmd, c->db->id, c->argv, c->argc);
	if (listLength(server.monitors))
		// 将所有执行的命令都发送到monitors
		replicationFeedSlaves(server.monitors, cmd, c->db->id, c->argv, c->argc);
	// 统计执行的命令个数
	server.stat_numcommands++;
}

/* If this function gets called we already read a whole
 * command, argments are in the client argv/argc fields.
 * processCommand() execute the command or prepare the
 * server for a bulk read from the client.
 *
 * If 1 is returned the client is still alive and valid and
 * and other operations can be performed by the caller. Otherwise
 * if 0 is returned the client was destroied (i.e. after QUIT). */
static int processCommand(redisClient *c) {
	struct redisCommand *cmd;

	/* Free some memory if needed (maxmemory setting) */
	if (server.maxmemory) freeMemoryIfNeeded();

	/* Handle the multi bulk command type. This is an alternative protocol
	 * supported by Redis in order to receive commands that are composed of
	 * multiple binary-safe "bulk" arguments. The latency of processing is
	 * a bit higher but this allows things like multi-sets, so if this
	 * protocol is used only for MSET and similar commands this is a big win. */
	if (c->multibulk == 0 && c->argc == 1 && ((char*)(c->argv[0]->ptr))[0] == '*') {
		c->multibulk = atoi(((char*)c->argv[0]->ptr) + 1);
		if (c->multibulk <= 0) {
			resetClient(c);
			return 1;
		} else {
			decrRefCount(c->argv[c->argc - 1]);
			c->argc--;
			return 1;
		}
	} else if (c->multibulk) {
		if (c->bulklen == -1) {
			if (((char*)c->argv[0]->ptr)[0] != '$') {
				addReplySds(c, sdsnew("-ERR multi bulk protocol error\r\n"));
				resetClient(c);
				return 1;
			} else {
				int bulklen = atoi(((char*)c->argv[0]->ptr) + 1);
				decrRefCount(c->argv[0]);
				if (bulklen < 0 || bulklen > 1024 * 1024 * 1024) {
					c->argc--;
					addReplySds(c, sdsnew("-ERR invalid bulk write count\r\n"));
					resetClient(c);
					return 1;
				}
				c->argc--;
				c->bulklen = bulklen + 2; /* add two bytes for CR+LF */
				return 1;
			}
		} else {
			c->mbargv = zrealloc(c->mbargv, (sizeof(robj*)) * (c->mbargc + 1));
			c->mbargv[c->mbargc] = c->argv[0];
			c->mbargc++;
			c->argc--;
			c->multibulk--;
			if (c->multibulk == 0) {
				robj **auxargv;
				int auxargc;

				/* Here we need to swap the multi-bulk argc/argv with the
				 * normal argc/argv of the client structure. */
				auxargv = c->argv;
				c->argv = c->mbargv;
				c->mbargv = auxargv;

				auxargc = c->argc;
				c->argc = c->mbargc;
				c->mbargc = auxargc;

				/* We need to set bulklen to something different than -1
				 * in order for the code below to process the command without
				 * to try to read the last argument of a bulk command as
				 * a special argument. */
				c->bulklen = 0;
				/* continue below and process the command */
			} else {
				c->bulklen = -1;
				return 1;
			}
		}
	}
	/* -- end of multi bulk commands processing -- */

	/* The QUIT command is handled as a special case. Normal command
	 * procs are unable to close the client connection safely */
	if (!strcasecmp(c->argv[0]->ptr, "quit")) {
		freeClient(c);
		return 0;
	}
	// 查找所需执行的命令
	cmd = lookupCommand(c->argv[0]->ptr);
	if (!cmd) {
		addReplySds(c,
		            sdscatprintf(sdsempty(), "-ERR unknown command '%s'\r\n",
		                         (char*)c->argv[0]->ptr));
		resetClient(c);
		return 1;
	} else if ((cmd->arity > 0 && cmd->arity != c->argc) ||
	           (c->argc < -cmd->arity)) {
		addReplySds(c,
		            sdscatprintf(sdsempty(),
		                         "-ERR wrong number of arguments for '%s' command\r\n",
		                         cmd->name));
		resetClient(c);
		return 1;
	} else if (server.maxmemory && cmd->flags & REDIS_CMD_DENYOOM && zmalloc_used_memory() > server.maxmemory) {
		// 内存超出限制
		addReplySds(c, sdsnew("-ERR command not allowed when used memory > 'maxmemory'\r\n"));
		resetClient(c);
		return 1;
	} else if (cmd->flags & REDIS_CMD_BULK && c->bulklen == -1) {
		int bulklen = atoi(c->argv[c->argc - 1]->ptr);

		decrRefCount(c->argv[c->argc - 1]);
		if (bulklen < 0 || bulklen > 1024 * 1024 * 1024) {
			c->argc--;
			addReplySds(c, sdsnew("-ERR invalid bulk write count\r\n"));
			resetClient(c);
			return 1;
		}
		c->argc--;
		c->bulklen = bulklen + 2; /* add two bytes for CR+LF */
		/* It is possible that the bulk read is already in the
		 * buffer. Check this condition and handle it accordingly.
		 * This is just a fast path, alternative to call processInputBuffer().
		 * It's a good idea since the code is small and this condition
		 * happens most of the times. */
		if ((signed)sdslen(c->querybuf) >= c->bulklen) {
			c->argv[c->argc] = createStringObject(c->querybuf, c->bulklen - 2);
			c->argc++;
			c->querybuf = sdsrange(c->querybuf, c->bulklen, -1);
		} else {
			return 1;
		}
	}
	/* Let's try to share objects on the command arguments vector */
	if (server.shareobjects) {
		// 开启了共享对象池，则尝试将命令的参数缓存起来
		int j;
		for (j = 1; j < c->argc; j++)
			c->argv[j] = tryObjectSharing(c->argv[j]);
	}
	/* Let's try to encode the bulk object to save space. */
	if (cmd->flags & REDIS_CMD_BULK)
		tryObjectEncoding(c->argv[c->argc - 1]);

	/* Check if the user is authenticated */
	// 密码认证
	if (server.requirepass && !c->authenticated && cmd->proc != authCommand) {
		addReplySds(c, sdsnew("-ERR operation not permitted\r\n"));
		resetClient(c);
		return 1;
	}

	/* Exec the command */
	if (c->flags & REDIS_MULTI && cmd->proc != execCommand) {
		// 如果客户端已经执行了MULTI命令，并且当前不是EXEC命令，则缓存起来
		queueMultiCommand(c, cmd);
		addReply(c, shared.queued);
	} else {
		// 调用命令执行
		call(c, cmd);
	}

	/* Prepare the client for the next command */
	if (c->flags & REDIS_CLOSE) {
		freeClient(c);
		return 0;
	}
	resetClient(c);
	return 1;
}

static void replicationFeedSlaves(list *slaves, struct redisCommand *cmd, int dictid, robj **argv, int argc) {
	listNode *ln;
	listIter li;
	int outc = 0, j;
	robj **outv;
	/* (args*2)+1 is enough room for args, spaces, newlines */
	robj *static_outv[REDIS_STATIC_ARGS * 2 + 1];

	if (argc <= REDIS_STATIC_ARGS) {
		outv = static_outv;
	} else {
		outv = zmalloc(sizeof(robj*) * (argc * 2 + 1));
	}

	for (j = 0; j < argc; j++) {
		if (j != 0) outv[outc++] = shared.space;
		if ((cmd->flags & REDIS_CMD_BULK) && j == argc - 1) {
			robj *lenobj;

			lenobj = createObject(REDIS_STRING,
			                      sdscatprintf(sdsempty(), "%lu\r\n",
			                                   (unsigned long) stringObjectLen(argv[j])));
			lenobj->refcount = 0;
			outv[outc++] = lenobj;
		}
		outv[outc++] = argv[j];
	}
	outv[outc++] = shared.crlf;

	/* Increment all the refcounts at start and decrement at end in order to
	 * be sure to free objects if there is no slave in a replication state
	 * able to be feed with commands */
	for (j = 0; j < outc; j++) incrRefCount(outv[j]);
	listRewind(slaves, &li);
	while ((ln = listNext(&li))) {
		redisClient *slave = ln->value;

		/* Don't feed slaves that are still waiting for BGSAVE to start */
		if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) continue;

		/* Feed all the other slaves, MONITORs and so on */
		if (slave->slaveseldb != dictid) {
			robj *selectcmd;

			switch (dictid) {
			case 0: selectcmd = shared.select0; break;
			case 1: selectcmd = shared.select1; break;
			case 2: selectcmd = shared.select2; break;
			case 3: selectcmd = shared.select3; break;
			case 4: selectcmd = shared.select4; break;
			case 5: selectcmd = shared.select5; break;
			case 6: selectcmd = shared.select6; break;
			case 7: selectcmd = shared.select7; break;
			case 8: selectcmd = shared.select8; break;
			case 9: selectcmd = shared.select9; break;
			default:
				selectcmd = createObject(REDIS_STRING,
				                         sdscatprintf(sdsempty(), "select %d\r\n", dictid));
				selectcmd->refcount = 0;
				break;
			}
			addReply(slave, selectcmd);
			slave->slaveseldb = dictid;
		}
		for (j = 0; j < outc; j++) addReply(slave, outv[j]);
	}
	for (j = 0; j < outc; j++) decrRefCount(outv[j]);
	if (outv != static_outv) zfree(outv);
}

static void processInputBuffer(redisClient *c) {
again:
	/* Before to process the input buffer, make sure the client is not
	 * waitig for a blocking operation such as BLPOP. Note that the first
	 * iteration the client is never blocked, otherwise the processInputBuffer
	 * would not be called at all, but after the execution of the first commands
	 * in the input buffer the client may be blocked, and the "goto again"
	 * will try to reiterate. The following line will make it return asap. */
	// 确保客户端不是处于被阻塞的状态
	if (c->flags & REDIS_BLOCKED || c->flags & REDIS_IO_WAIT) return;

	/* Redis请求数据封装协议分为两种，
	*  1.简单字符串（如PING 编码为PING\n或者PING\r\n等）
	*  2.数组（如"LLEN list"格式则为*2\r\n$4LLEN\r\n$4list\r\n）
	*/
	if (c->bulklen == -1) {
		/* Read the first line of the query */
		char *p = strchr(c->querybuf, '\n');
		size_t querylen;

		if (p) {
			sds query, *argv;
			int argc, j;

			query = c->querybuf;
			c->querybuf = sdsempty();
			querylen = 1 + (p - (query));
			if (sdslen(query) > querylen) {
				/* leave data after the first line of the query in the buffer */
				c->querybuf = sdscatlen(c->querybuf, query + querylen, sdslen(query) - querylen);
			}
			*p = '\0'; /* remove "\n" */
			if (*(p - 1) == '\r') *(p - 1) = '\0'; /* and "\r" if any */
			sdsupdatelen(query);

			/* Now we can split the query in arguments */
			argv = sdssplitlen(query, sdslen(query), " ", 1, &argc);
			sdsfree(query);

			if (c->argv) zfree(c->argv);
			c->argv = zmalloc(sizeof(robj*)*argc);

			for (j = 0; j < argc; j++) {
				if (sdslen(argv[j])) {
					c->argv[c->argc] = createObject(REDIS_STRING, argv[j]);
					c->argc++;
				} else {
					sdsfree(argv[j]);
				}
			}
			zfree(argv);
			if (c->argc) {
				/* Execute the command. If the client is still valid
				 * after processCommand() return and there is something
				 * on the query buffer try to process the next command. */
				if (processCommand(c) && sdslen(c->querybuf)) goto again;
			} else {
				/* Nothing to process, argc == 0. Just process the query
				 * buffer if it's not empty or return to the caller */
				if (sdslen(c->querybuf)) goto again;
			}
			return;
		} else if (sdslen(c->querybuf) >= REDIS_REQUEST_MAX_SIZE) {
			redisLog(REDIS_VERBOSE, "Client protocol error");
			freeClient(c);
			return;
		}
	} else {
		/* Bulk read handling. Note that if we are at this point
		   the client already sent a command terminated with a newline,
		   we are reading the bulk data that is actually the last
		   argument of the command. */
		int qbl = sdslen(c->querybuf);

		if (c->bulklen <= qbl) {
			/* Copy everything but the final CRLF as final argument */
			c->argv[c->argc] = createStringObject(c->querybuf, c->bulklen - 2);
			c->argc++;
			c->querybuf = sdsrange(c->querybuf, c->bulklen, -1);
			/* Process the command. If the client is still valid after
			 * the processing and there is more data in the buffer
			 * try to parse it. */
			if (processCommand(c) && sdslen(c->querybuf)) goto again;
			return;
		}
	}
}

static void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
	redisClient *c = (redisClient*) privdata;
	char buf[REDIS_IOBUF_LEN];
	int nread;
	REDIS_NOTUSED(el);
	REDIS_NOTUSED(mask);

	// 每次只读1K数据，也就是每次只处理1K数据（如果一次请求超过1K，则需要分次读取处理）
	nread = read(fd, buf, REDIS_IOBUF_LEN);
	if (nread == -1) {
		if (errno == EAGAIN) {
			nread = 0;
		} else {
			redisLog(REDIS_VERBOSE, "Reading from client: %s", strerror(errno));
			freeClient(c);
			return;
		}
	} else if (nread == 0) {
		redisLog(REDIS_VERBOSE, "Client closed connection");
		freeClient(c);
		return;
	}
	if (nread) {
		// 将读取到的数据拼接到querybuf缓冲区中
		c->querybuf = sdscatlen(c->querybuf, buf, nread);
		// 设置最后一次交互时间为当前（防止超时被关闭）
		c->lastinteraction = time(NULL);
	} else {
		return;
	}
	processInputBuffer(c);
}

static int selectDb(redisClient *c, int id) {
	if (id < 0 || id >= server.dbnum)
		return REDIS_ERR;
	c->db = &server.db[id];
	return REDIS_OK;
}

static void *dupClientReplyValue(void *o) {
	incrRefCount((robj*)o);
	return 0;
}

static redisClient *createClient(int fd) {
	redisClient *c = zmalloc(sizeof(*c));

	// 设置为非阻塞
	anetNonBlock(NULL, fd);
	// 关闭掉Nagel算法
	anetTcpNoDelay(NULL, fd);
	if (!c) return NULL;
	// 默认使用0号数据库
	selectDb(c, 0);
	c->fd = fd;
	c->querybuf = sdsempty();
	c->argc = 0;
	c->argv = NULL;
	c->bulklen = -1;
	c->multibulk = 0;
	c->mbargc = 0;
	c->mbargv = NULL;
	c->sentlen = 0;
	c->flags = 0;
	c->lastinteraction = time(NULL);
	c->authenticated = 0;
	c->replstate = REDIS_REPL_NONE;
	c->reply = listCreate();
	// 设置释放应答数据的方法，仅仅为减少对象的引用
	listSetFreeMethod(c->reply, decrRefCount);
	// 设置复制客户端应答数据的方法（用于Replica时复制）
	listSetDupMethod(c->reply, dupClientReplyValue);
	c->blockingkeys = NULL;
	c->blockingkeysnum = 0;
	c->io_keys = listCreate();
	listSetFreeMethod(c->io_keys, decrRefCount);
	// 设置客户端请求数据的处理方法
	if (aeCreateFileEvent(server.el, c->fd, AE_READABLE,
	                      readQueryFromClient, c) == AE_ERR) {
		freeClient(c);
		return NULL;
	}
	// 加入到服务器的当前客户端列表中
	listAddNodeTail(server.clients, c);
	// 初始化设置MUTI命令的状态
	initClientMultiState(c);
	return c;
}

// 添加客户端应答消息
static void addReply(redisClient *c, robj *obj) {

	// 如果当前没有应答消息（说明没有创建发送应答处理句柄）
	// 并且当前客户端不是Master，或者是Slave且处于在线状态，才创建发送应答方法
	if (listLength(c->reply) == 0 &&
	        (c->replstate == REDIS_REPL_NONE ||
	         c->replstate == REDIS_REPL_ONLINE) &&
	        aeCreateFileEvent(server.el, c->fd, AE_WRITABLE,
	                          sendReplyToClient, c) == AE_ERR) return;

	if (server.vm_enabled && obj->storage != REDIS_VM_MEMORY) {
		obj = dupStringObject(obj);
		obj->refcount = 0; /* getDecodedObject() will increment the refcount */
	}
	listAddNodeTail(c->reply, getDecodedObject(obj));
}

static void addReplySds(redisClient *c, sds s) {
	robj *o = createObject(REDIS_STRING, s);
	addReply(c, o);
	decrRefCount(o);
}

static void addReplyDouble(redisClient *c, double d) {
	char buf[128];

	snprintf(buf, sizeof(buf), "%.17g", d);
	addReplySds(c, sdscatprintf(sdsempty(), "$%lu\r\n%s\r\n",
	                            (unsigned long) strlen(buf), buf));
}

static void addReplyBulkLen(redisClient *c, robj *obj) {
	size_t len;

	if (obj->encoding == REDIS_ENCODING_RAW) {
		len = sdslen(obj->ptr);
	} else {
		long n = (long)obj->ptr;

		/* Compute how many bytes will take this integer as a radix 10 string */
		len = 1;
		if (n < 0) {
			len++;
			n = -n;
		}
		while ((n = n / 10) != 0) {
			len++;
		}
	}
	addReplySds(c, sdscatprintf(sdsempty(), "$%lu\r\n", (unsigned long)len));
}

// 客户端接入的时候处理句柄
static void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
	int cport, cfd;
	char cip[128];
	redisClient *c;
	REDIS_NOTUSED(el);
	REDIS_NOTUSED(mask);
	REDIS_NOTUSED(privdata);

	// 接收连接
	cfd = anetAccept(server.neterr, fd, cip, &cport);
	if (cfd == AE_ERR) {
		redisLog(REDIS_VERBOSE, "Accepting client connection: %s", server.neterr);
		return;
	}
	redisLog(REDIS_VERBOSE, "Accepted %s:%d", cip, cport);
	// 初始化客户端所需的信息
	if ((c = createClient(cfd)) == NULL) {
		redisLog(REDIS_WARNING, "Error allocating resoures for the client");
		close(cfd); /* May be already closed, just ingore errors */
		return;
	}
	/* If maxclient directive is set and this is one client more... close the
	 * connection. Note that we create the client instead to check before
	 * for this condition, since now the socket is already set in nonblocking
	 * mode and we can send an error for free using the Kernel I/O */
	// 判断连接是否超出了设置的阈值，如果是则响应错误并关闭该客户端
	if (server.maxclients && listLength(server.clients) > server.maxclients) {
		char *err = "-ERR max number of clients reached\r\n";

		/* That's a best effort error message, don't check write errors */
		if (write(c->fd, err, strlen(err)) == -1) {
			/* Nothing to do, Just to avoid the warning... */
		}
		freeClient(c);
		return;
	}
	// 增加连接数
	server.stat_numconnections++;
}

/* ======================= Redis objects implementation ===================== */

// 创建对象
static robj *createObject(int type, void *ptr) {
	robj *o;

	if (server.vm_enabled) pthread_mutex_lock(&server.obj_freelist_mutex);
	if (listLength(server.objfreelist)) {
		// 如果对象池中有对象，则直接取来用
		listNode *head = listFirst(server.objfreelist);
		o = listNodeValue(head);
		listDelNode(server.objfreelist, head);
		if (server.vm_enabled) pthread_mutex_unlock(&server.obj_freelist_mutex);
	} else {
		if (server.vm_enabled) {
			pthread_mutex_unlock(&server.obj_freelist_mutex);
			o = zmalloc(sizeof(*o));
		} else {
			// 没有才分配一个新的对象
			o = zmalloc(sizeof(*o) - sizeof(struct redisObjectVM));
		}
	}
	// 对象的类型（如String,List等）
	o->type = type;
	// 目前编码只有两种，INT和RAW，创建时默认为RAW，后续才会尝试转换编码
	o->encoding = REDIS_ENCODING_RAW;
	// 对象的值
	o->ptr = ptr;
	// 初始化引用个数为1
	o->refcount = 1;
	if (server.vm_enabled) {
		/* Note that this code may run in the context of an I/O thread
		 * and accessing to server.unixtime in theory is an error
		 * (no locks). But in practice this is safe, and even if we read
		 * garbage Redis will not fail, as it's just a statistical info */
		o->vm.atime = server.unixtime;
		o->storage = REDIS_VM_MEMORY;
	}
	return o;
}

// 创建String对象
static robj *createStringObject(char *ptr, size_t len) {
	return createObject(REDIS_STRING, sdsnewlen(ptr, len));
}

// 复制一个String对象（只复制元数据，浅复制，对应的值只复制"引用"（指针））
static robj *dupStringObject(robj *o) {
	assert(o->encoding == REDIS_ENCODING_RAW);
	return createStringObject(o->ptr, sdslen(o->ptr));
}

// 创建List对象
static robj *createListObject(void) {
	list *l = listCreate();

	// List内释放节点的值方法为decrRefCount
	listSetFreeMethod(l, decrRefCount);
	return createObject(REDIS_LIST, l);
}

static robj *createSetObject(void) {
	dict *d = dictCreate(&setDictType, NULL);
	return createObject(REDIS_SET, d);
}

static robj *createZsetObject(void) {
	zset *zs = zmalloc(sizeof(*zs));

	zs->dict = dictCreate(&zsetDictType, NULL);
	zs->zsl = zslCreate();
	return createObject(REDIS_ZSET, zs);
}

// 释放字符串对象
static void freeStringObject(robj *o) {
	if (o->encoding == REDIS_ENCODING_RAW) {
		sdsfree(o->ptr);
	}
}

// 释放链表对象
static void freeListObject(robj *o) {
	listRelease((list*) o->ptr);
}

static void freeSetObject(robj *o) {
	dictRelease((dict*) o->ptr);
}

static void freeZsetObject(robj *o) {
	zset *zs = o->ptr;

	dictRelease(zs->dict);
	zslFree(zs->zsl);
	zfree(zs);
}

static void freeHashObject(robj *o) {
	dictRelease((dict*) o->ptr);
}

// 增加对象的引用
static void incrRefCount(robj *o) {
	redisAssert(!server.vm_enabled || o->storage == REDIS_VM_MEMORY);
	o->refcount++;
}

// 降低对象的引用，只有当引用个数为0时才真正释放内存
static void decrRefCount(void *obj) {
	robj *o = obj;

	/* Object is a key of a swapped out value, or in the process of being
	 * loaded. */
	if (server.vm_enabled &&
	        (o->storage == REDIS_VM_SWAPPED || o->storage == REDIS_VM_LOADING))
	{
		if (o->storage == REDIS_VM_SWAPPED || o->storage == REDIS_VM_LOADING) {
			redisAssert(o->refcount == 1);
		}
		if (o->storage == REDIS_VM_LOADING) vmCancelThreadedIOJob(obj);
		redisAssert(o->type == REDIS_STRING);
		freeStringObject(o);
		vmMarkPagesFree(o->vm.page, o->vm.usedpages);
		pthread_mutex_lock(&server.obj_freelist_mutex);
		if (listLength(server.objfreelist) > REDIS_OBJFREELIST_MAX ||
		        !listAddNodeHead(server.objfreelist, o))
			zfree(o);
		pthread_mutex_unlock(&server.obj_freelist_mutex);
		server.vm_stats_swapped_objects--;
		return;
	}
	/* Object is in memory, or in the process of being swapped out. */
	if (--(o->refcount) == 0) {
		if (server.vm_enabled && o->storage == REDIS_VM_SWAPPING)
			vmCancelThreadedIOJob(obj);
		switch (o->type) {
		// 根据对象的类型来释放对象的值
		case REDIS_STRING: freeStringObject(o); break;
		case REDIS_LIST: freeListObject(o); break;
		case REDIS_SET: freeSetObject(o); break;
		case REDIS_ZSET: freeZsetObject(o); break;
		case REDIS_HASH: freeHashObject(o); break;
		default: redisAssert(0 != 0); break;
		}
		if (server.vm_enabled) pthread_mutex_lock(&server.obj_freelist_mutex);
		// 如果对象池中的对象个数没有大于设定的阈值，则将该对象先缓存起来，减少创建和释放的次数
		if (listLength(server.objfreelist) > REDIS_OBJFREELIST_MAX ||
		        !listAddNodeHead(server.objfreelist, o))
			zfree(o);
		if (server.vm_enabled) pthread_mutex_unlock(&server.obj_freelist_mutex);
	}
}

static robj *lookupKey(redisDb *db, robj *key) {
	dictEntry *de = dictFind(db->dict, key);
	if (de) {
		robj *key = dictGetEntryKey(de);
		robj *val = dictGetEntryVal(de);

		if (server.vm_enabled) {
			if (key->storage == REDIS_VM_MEMORY ||
			        key->storage == REDIS_VM_SWAPPING)
			{
				/* If we were swapping the object out, stop it, this key
				 * was requested. */
				if (key->storage == REDIS_VM_SWAPPING)
					vmCancelThreadedIOJob(key);
				/* Update the access time of the key for the aging algorithm. */
				key->vm.atime = server.unixtime;
			} else {
				/* Our value was swapped on disk. Bring it at home. */
				redisAssert(val == NULL);
				val = vmLoadObject(key);
				dictGetEntryVal(de) = val;
			}
		}
		return val;
	} else {
		return NULL;
	}
}

static robj *lookupKeyRead(redisDb *db, robj *key) {
	// 删除过期的数据
	expireIfNeeded(db, key);
	return lookupKey(db, key);
}

static robj *lookupKeyWrite(redisDb *db, robj *key) {
	// 当有需要写入的时候，会先将含有过期时间的键删除，无论有没有过期
	//（这样子设计很不合理，因为存在List含有过期时间，如果向其添加数据，则会将整个List删除，即便没有过期）
	deleteIfVolatile(db, key);
	return lookupKey(db, key);
}

// 根据给定的key删除DB里面的键值对
static int deleteKey(redisDb *db, robj *key) {
	int retval;

	/* We need to protect key from destruction: after the first dictDelete()
	 * it may happen that 'key' is no longer valid if we don't increment
	 * it's count. This may happen when we get the object reference directly
	 * from the hash table with dictRandomKey() or dict iterators */
	// 用来防止key被删除，因为可能使用的是共享池里面的对象
	incrRefCount(key);
	// 先删除过期字典的中键
	if (dictSize(db->expires)) dictDelete(db->expires, key);
	// 再删除DB中的键值对
	retval = dictDelete(db->dict, key);
	decrRefCount(key);

	return retval == DICT_OK;
}

/* Try to share an object against the shared objects pool */
// 尝试将对象缓存起来，用于共享该对象
static robj *tryObjectSharing(robj *o) {
	struct dictEntry *de;
	unsigned long c;

	if (o == NULL || server.shareobjects == 0) return o;

	redisAssert(o->type == REDIS_STRING);
	de = dictFind(server.sharingpool, o);
	if (de) {
		// 查找到
		robj *shared = dictGetEntryKey(de);

		// 使用次数+1，次数越多，保存时间越久
		c = ((unsigned long) dictGetEntryVal(de)) + 1;
		dictGetEntryVal(de) = (void*) c;
		// 增加共享对象的引用，并且将原有的对象减少引用，表明此次使用共享的对象
		incrRefCount(shared);
		decrRefCount(o);
		return shared;
	} else {
		/* Here we are using a stream algorihtm: Every time an object is
		 * shared we increment its count, everytime there is a miss we
		 * recrement the counter of a random object. If this object reaches
		 * zero we remove the object and put the current object instead. */
		// 没有找到
		if (dictSize(server.sharingpool) >=
		        server.sharingpoolsize) {
			// 对象池满了，则随机从共享对象池中获取一个对象并减少它的引用
			de = dictGetRandomKey(server.sharingpool);
			redisAssert(de != NULL);
			c = ((unsigned long) dictGetEntryVal(de)) - 1;
			dictGetEntryVal(de) = (void*) c;
			if (c == 0) {
				// 当次数减少到0的时候则直接从共享池中移除
				dictDelete(server.sharingpool, de->key);
			}
		} else {
			c = 0; /* If the pool is empty we want to add this object */
		}
		if (c == 0) {
			int retval;
			// 移除了旧的对象，将新的对象放进去
			retval = dictAdd(server.sharingpool, o, (void*)1);
			redisAssert(retval == DICT_OK);
			incrRefCount(o);
		}
		return o;
	}
}

/* Check if the nul-terminated string 's' can be represented by a long
 * (that is, is a number that fits into long without any other space or
 * character before or after the digits).
 *
 * If so, the function returns REDIS_OK and *longval is set to the value
 * of the number. Otherwise REDIS_ERR is returned */
static int isStringRepresentableAsLong(sds s, long *longval) {
	char buf[32], *endptr;
	long value;
	int slen;

	value = strtol(s, &endptr, 10);
	if (endptr[0] != '\0') return REDIS_ERR;
	slen = snprintf(buf, 32, "%ld", value);

	/* If the number converted back into a string is not identical
	 * then it's not possible to encode the string as integer */
	if (sdslen(s) != (unsigned)slen || memcmp(buf, s, slen)) return REDIS_ERR;
	if (longval) *longval = value;
	return REDIS_OK;
}

/* Try to encode a string object in order to save space */
static int tryObjectEncoding(robj *o) {
	long value;
	sds s = o->ptr;

	if (o->encoding != REDIS_ENCODING_RAW)
		return REDIS_ERR; /* Already encoded */

	/* It's not save to encode shared objects: shared objects can be shared
	 * everywhere in the "object space" of Redis. Encoded objects can only
	 * appear as "values" (and not, for instance, as keys) */
	if (o->refcount > 1) return REDIS_ERR;

	/* Currently we try to encode only strings */
	redisAssert(o->type == REDIS_STRING);

	/* Check if we can represent this string as a long integer */
	if (isStringRepresentableAsLong(s, &value) == REDIS_ERR) return REDIS_ERR;

	/* Ok, this object can be encoded */
	o->encoding = REDIS_ENCODING_INT;
	sdsfree(o->ptr);
	o->ptr = (void*) value;
	return REDIS_OK;
}

/* Get a decoded version of an encoded object (returned as a new object).
 * If the object is already raw-encoded just increment the ref count. */
// 将编码为整型的字符串解码后返回
static robj *getDecodedObject(robj *o) {
	robj *dec;

	if (o->encoding == REDIS_ENCODING_RAW) {
		incrRefCount(o);
		return o;
	}
	if (o->type == REDIS_STRING && o->encoding == REDIS_ENCODING_INT) {
		char buf[32];

		snprintf(buf, 32, "%ld", (long)o->ptr);
		dec = createStringObject(buf, strlen(buf));
		return dec;
	} else {
		redisAssert(1 != 1);
	}
}

/* Compare two string objects via strcmp() or alike.
 * Note that the objects may be integer-encoded. In such a case we
 * use snprintf() to get a string representation of the numbers on the stack
 * and compare the strings, it's much faster than calling getDecodedObject().
 *
 * Important note: if objects are not integer encoded, but binary-safe strings,
 * sdscmp() from sds.c will apply memcmp() so this function ca be considered
 * binary safe. */
static int compareStringObjects(robj *a, robj *b) {
	redisAssert(a->type == REDIS_STRING && b->type == REDIS_STRING);
	char bufa[128], bufb[128], *astr, *bstr;
	int bothsds = 1;

	if (a == b) return 0;
	if (a->encoding != REDIS_ENCODING_RAW) {
		snprintf(bufa, sizeof(bufa), "%ld", (long) a->ptr);
		astr = bufa;
		bothsds = 0;
	} else {
		astr = a->ptr;
	}
	if (b->encoding != REDIS_ENCODING_RAW) {
		snprintf(bufb, sizeof(bufb), "%ld", (long) b->ptr);
		bstr = bufb;
		bothsds = 0;
	} else {
		bstr = b->ptr;
	}
	return bothsds ? sdscmp(astr, bstr) : strcmp(astr, bstr);
}

static size_t stringObjectLen(robj *o) {
	redisAssert(o->type == REDIS_STRING);
	if (o->encoding == REDIS_ENCODING_RAW) {
		return sdslen(o->ptr);
	} else {
		char buf[32];

		return snprintf(buf, 32, "%ld", (long)o->ptr);
	}
}

/*============================ RDB saving/loading =========================== */

// RDB通过一个字节来表示value的编码类型（0:String,1:List,etc,还有0xFE指示Selector等）
static int rdbSaveType(FILE *fp, unsigned char type) {
	if (fwrite(&type, 1, 1, fp) == 0) return -1;
	return 0;
}

// 过期时间为4字节
static int rdbSaveTime(FILE *fp, time_t t) {
	int32_t t32 = (int32_t) t;
	if (fwrite(&t32, 4, 1, fp) == 0) return -1;
	return 0;
}

/**
* Redis对于长度采用的是扩展操作码的技术，其中前两个bit位用于指示：
* 00 : 后6bit代表长度
* 01 : 后14bit代表长度
* 10 : 后6bit不用，后续4字节代表长度
* 11 : 自定义扩展，后6bit用于代表编码的格式(11:REDIS_RDB_ENCVAL，如REDIS_RDB_ENC_INT8)
*/
/* check rdbLoadLen() comments for more info */
static int rdbSaveLen(FILE *fp, uint32_t len) {
	unsigned char buf[2];

	if (len < (1 << 6)) {
		/* Save a 6 bit len */
		buf[0] = (len & 0xFF) | (REDIS_RDB_6BITLEN << 6);
		if (fwrite(buf, 1, 1, fp) == 0) return -1;
	} else if (len < (1 << 14)) {
		/* Save a 14 bit len */
		buf[0] = ((len >> 8) & 0xFF) | (REDIS_RDB_14BITLEN << 6);
		buf[1] = len & 0xFF;
		if (fwrite(buf, 2, 1, fp) == 0) return -1;
	} else {
		/* Save a 32 bit len */
		buf[0] = (REDIS_RDB_32BITLEN << 6);
		if (fwrite(buf, 1, 1, fp) == 0) return -1;
		len = htonl(len);
		if (fwrite(&len, 4, 1, fp) == 0) return -1;
	}
	return 0;
}

/* String objects in the form "2391" "-100" without any space and with a
 * range of values that can fit in an 8, 16 or 32 bit signed value can be
 * encoded as integers to save space */
// 尝试将String类型转换为整型，用于节省空间
static int rdbTryIntegerEncoding(sds s, unsigned char *enc) {
	long long value;
	char *endptr, buf[32];

	/* Check if it's possible to encode this value as a number */
	// 将String转换为长整型，如果剩余的字符不为空（含有其它字符），则转换失败
	value = strtoll(s, &endptr, 10);
	if (endptr[0] != '\0') return 0;
	/* 转换成功后，再将长整型转换回字符串
	* 用于比较是否相同（即使存在0100.00字符串，转换为100，但是字符串不再相同了，也不允许转换，
	* 因为这样可能会导致客户端存进去的数据和取出来的数据不一致）
	*/
	snprintf(buf, 32, "%lld", value);

	/* If the number converted back into a string is not identical
	 * then it's not possible to encode the string as integer */
	if (strlen(buf) != sdslen(s) || memcmp(buf, s, sdslen(s))) return 0;

	/* Finally check if it fits in our ranges */
	if (value >= -(1 << 7) && value <= (1 << 7) - 1) {
		// 使用的是11自定义编码（见长度编码）
		enc[0] = (REDIS_RDB_ENCVAL << 6) | REDIS_RDB_ENC_INT8;
		enc[1] = value & 0xFF;
		return 2;
	} else if (value >= -(1 << 15) && value <= (1 << 15) - 1) {
		enc[0] = (REDIS_RDB_ENCVAL << 6) | REDIS_RDB_ENC_INT16;
		enc[1] = value & 0xFF;
		enc[2] = (value >> 8) & 0xFF;
		return 3;
	} else if (value >= -((long long)1 << 31) && value <= ((long long)1 << 31) - 1) {
		enc[0] = (REDIS_RDB_ENCVAL << 6) | REDIS_RDB_ENC_INT32;
		enc[1] = value & 0xFF;
		enc[2] = (value >> 8) & 0xFF;
		enc[3] = (value >> 16) & 0xFF;
		enc[4] = (value >> 24) & 0xFF;
		return 5;
	} else {
		return 0;
	}
}

// 使用LZF压缩算法压缩
static int rdbSaveLzfStringObject(FILE *fp, robj *obj) {
	unsigned int comprlen, outlen;
	unsigned char byte;
	void *out;

	/* We require at least four bytes compression for this to be worth it */
	outlen = sdslen(obj->ptr) - 4;
	if (outlen <= 0) return 0;
	if ((out = zmalloc(outlen + 1)) == NULL) return 0;
	comprlen = lzf_compress(obj->ptr, sdslen(obj->ptr), out, outlen);
	if (comprlen == 0) {
		zfree(out);
		return 0;
	}
	/* 格式为 使用长度编码方式的扩展类型11，后6bit指示LZF压缩
	* 后面保存压缩的长度和未压缩的长度，最后是压缩后的数据
	*/
	/* Data compressed! Let's save it on disk */
	byte = (REDIS_RDB_ENCVAL << 6) | REDIS_RDB_ENC_LZF;
	if (fwrite(&byte, 1, 1, fp) == 0) goto writeerr;
	if (rdbSaveLen(fp, comprlen) == -1) goto writeerr;
	if (rdbSaveLen(fp, sdslen(obj->ptr)) == -1) goto writeerr;
	if (fwrite(out, comprlen, 1, fp) == 0) goto writeerr;
	zfree(out);
	return comprlen;

writeerr:
	zfree(out);
	return -1;
}

/* Save a string objet as [len][data] on disk. If the object is a string
 * representation of an integer value we try to safe it in a special form */
/* 保存原始字符串
* 1. 长度小于11，尝试转换为整数编码
* 2. 长度大于20，尝试使用LZF压缩再保存
* 3. 如果上述都失败，那么直接保存
*/
static int rdbSaveStringObjectRaw(FILE *fp, robj *obj) {
	size_t len;
	int enclen;

	len = sdslen(obj->ptr);

	/* Try integer encoding */
	if (len <= 11) {
		unsigned char buf[5];
		if ((enclen = rdbTryIntegerEncoding(obj->ptr, buf)) > 0) {
			if (fwrite(buf, enclen, 1, fp) == 0) return -1;
			return 0;
		}
	}

	/* Try LZF compression - under 20 bytes it's unable to compress even
	 * aaaaaaaaaaaaaaaaaa so skip it */
	if (server.rdbcompression && len > 20) {
		int retval;

		retval = rdbSaveLzfStringObject(fp, obj);
		if (retval == -1) return -1;
		if (retval > 0) return 0;
		/* retval == 0 means data can't be compressed, save the old way */
	}

	/* Store verbatim */
	if (rdbSaveLen(fp, len) == -1) return -1;
	if (len && fwrite(obj->ptr, len, 1, fp) == 0) return -1;
	return 0;
}

/* Like rdbSaveStringObjectRaw() but handle encoded objects */
// 和上面的RAW类似，但是会处理编码过的对象（先解码再保存）
static int rdbSaveStringObject(FILE *fp, robj *obj) {
	int retval;

	/* Avoid incr/decr ref count business when possible.
	 * This plays well with copy-on-write given that we are probably
	 * in a child process (BGSAVE). Also this makes sure key objects
	 * of swapped objects are not incRefCount-ed (an assert does not allow
	 * this in order to avoid bugs) */
	if (obj->encoding != REDIS_ENCODING_RAW) {
		obj = getDecodedObject(obj);
		retval = rdbSaveStringObjectRaw(fp, obj);
		decrRefCount(obj);
	} else {
		retval = rdbSaveStringObjectRaw(fp, obj);
	}
	return retval;
}

/* Save a double value. Doubles are saved as strings prefixed by an unsigned
 * 8 bit integer specifing the length of the representation.
 * This 8 bit integer has special values in order to specify the following
 * conditions:
 * 253: not a number
 * 254: + inf
 * 255: - inf
 */
// 保存Double
static int rdbSaveDoubleValue(FILE *fp, double val) {
	unsigned char buf[128];
	int len;

	if (isnan(val)) {
		buf[0] = 253;
		len = 1;
	} else if (!isfinite(val)) {
		len = 1;
		buf[0] = (val < 0) ? 255 : 254;
	} else {
		snprintf((char*)buf + 1, sizeof(buf) - 1, "%.17g", val);
		buf[0] = strlen((char*)buf + 1);
		len = buf[0] + 1;
	}
	if (fwrite(buf, len, 1, fp) == 0) return -1;
	return 0;
}

/* Save a Redis object. */
// 保存Redis对象
//（在Redis里，所有数据（Key,Value）都是String（除非编码过），
// 只是数据结构不一样，所以需要遍历该数据结构，再依次保存）
static int rdbSaveObject(FILE *fp, robj *o) {
	if (o->type == REDIS_STRING) {
		/* Save a string value */
		if (rdbSaveStringObject(fp, o) == -1) return -1;
	} else if (o->type == REDIS_LIST) {
		/* Save a list value */
		list *list = o->ptr;
		listIter li;
		listNode *ln;

		if (rdbSaveLen(fp, listLength(list)) == -1) return -1;
		listRewind(list, &li);
		while ((ln = listNext(&li))) {
			robj *eleobj = listNodeValue(ln);

			if (rdbSaveStringObject(fp, eleobj) == -1) return -1;
		}
	} else if (o->type == REDIS_SET) {
		/* Save a set value */
		dict *set = o->ptr;
		dictIterator *di = dictGetIterator(set);
		dictEntry *de;

		if (rdbSaveLen(fp, dictSize(set)) == -1) return -1;
		while ((de = dictNext(di)) != NULL) {
			robj *eleobj = dictGetEntryKey(de);

			if (rdbSaveStringObject(fp, eleobj) == -1) return -1;
		}
		dictReleaseIterator(di);
	} else if (o->type == REDIS_ZSET) {
		/* Save a set value */
		zset *zs = o->ptr;
		dictIterator *di = dictGetIterator(zs->dict);
		dictEntry *de;

		if (rdbSaveLen(fp, dictSize(zs->dict)) == -1) return -1;
		while ((de = dictNext(di)) != NULL) {
			robj *eleobj = dictGetEntryKey(de);
			double *score = dictGetEntryVal(de);

			if (rdbSaveStringObject(fp, eleobj) == -1) return -1;
			if (rdbSaveDoubleValue(fp, *score) == -1) return -1;
		}
		dictReleaseIterator(di);
	} else {
		redisAssert(0 != 0);
	}
	return 0;
}

/* Return the length the object will have on disk if saved with
 * the rdbSaveObject() function. Currently we use a trick to get
 * this length with very little changes to the code. In the future
 * we could switch to a faster solution. */
// 保存对象长度，这里通过一个小技巧来完成：
// 传入rdbSavedObjectLen(o,null)，通过rewind将devnull重定向到文件开头，
// 将对象写入devnull中，然后通过ftello来获取当前偏移位置来取得对象长度
static off_t rdbSavedObjectLen(robj *o, FILE *fp) {
	if (fp == NULL) fp = server.devnull;
	rewind(fp);
	assert(rdbSaveObject(fp, o) != 1);
	return ftello(fp);
}

/* Return the number of pages required to save this object in the swap file */
static off_t rdbSavedObjectPages(robj *o, FILE *fp) {
	off_t bytes = rdbSavedObjectLen(o, fp);

	return (bytes + (server.vm_page_size - 1)) / server.vm_page_size;
}

/* Save the DB on disk. Return REDIS_ERR on error, REDIS_OK on success */
// 进行RDB持久化
static int rdbSave(char *filename) {
	dictIterator *di = NULL;
	dictEntry *de;
	FILE *fp;
	char tmpfile[256];
	int j;
	time_t now = time(NULL);

	/* Wait for I/O therads to terminate, just in case this is a
	 * foreground-saving, to avoid seeking the swap file descriptor at the
	 * same time. */
	if (server.vm_enabled)
		waitEmptyIOJobsQueue();

	snprintf(tmpfile, 256, "temp-%d.rdb", (int) getpid());
	fp = fopen(tmpfile, "w");
	if (!fp) {
		redisLog(REDIS_WARNING, "Failed saving the DB: %s", strerror(errno));
		return REDIS_ERR;
	}
	// 写入REDIS魔数用于快速判断一个文件是否是RDB文件，后面跟着4位版本号 0001
	if (fwrite("REDIS0001", 9, 1, fp) == 0) goto werr;
	for (j = 0; j < server.dbnum; j++) {
		redisDb *db = server.db + j;
		dict *d = db->dict;
		if (dictSize(d) == 0) continue;
		di = dictGetIterator(d);
		if (!di) {
			fclose(fp);
			return REDIS_ERR;
		}
		// RDB后续的格式为,指示ID的类型以及DB的id
		/* Write the SELECT DB opcode */
		if (rdbSaveType(fp, REDIS_SELECTDB) == -1) goto werr;
		if (rdbSaveLen(fp, j) == -1) goto werr;

		// 接着就是所有的key-value键值对
		//（如果含有过期时间，并且大于当前时间，则先写入过期时间，再写入key-value）
		/* Iterate this DB writing every entry */
		while ((de = dictNext(di)) != NULL) {
			robj *key = dictGetEntryKey(de);
			robj *o = dictGetEntryVal(de);
			time_t expiretime = getExpire(db, key);

			/* Save the expire time */
			if (expiretime != -1) {
				/* If this key is already expired skip it */
				if (expiretime < now) continue;
				if (rdbSaveType(fp, REDIS_EXPIRETIME) == -1) goto werr;
				if (rdbSaveTime(fp, expiretime) == -1) goto werr;
			}
			/* Save the key and associated value. This requires special
			 * handling if the value is swapped out. */
			if (!server.vm_enabled || key->storage == REDIS_VM_MEMORY ||
			        key->storage == REDIS_VM_SWAPPING) {
				/* Save type, key, value */
				if (rdbSaveType(fp, o->type) == -1) goto werr;
				if (rdbSaveStringObject(fp, key) == -1) goto werr;
				if (rdbSaveObject(fp, o) == -1) goto werr;
			} else {
				/* REDIS_VM_SWAPPED or REDIS_VM_LOADING */
				robj *po;
				/* Get a preview of the object in memory */
				po = vmPreviewObject(key);
				/* Save type, key, value */
				if (rdbSaveType(fp, key->vtype) == -1) goto werr;
				if (rdbSaveStringObject(fp, key) == -1) goto werr;
				if (rdbSaveObject(fp, po) == -1) goto werr;
				/* Remove the loaded object from memory */
				decrRefCount(po);
			}
		}
		dictReleaseIterator(di);
	}
	// 最后面是EOF结束符
	/* EOF opcode */
	if (rdbSaveType(fp, REDIS_EOF) == -1) goto werr;

	/* Make sure data will not remain on the OS's output buffers */
	fflush(fp);
	// 取得文件的fd，将写入文件的内容刷到磁盘中（高速缓存）（只对该文件进行持久化写入，并等待返回）
	fsync(fileno(fp));
	fclose(fp);

	/* Use RENAME to make sure the DB file is changed atomically only
	 * if the generate DB file is ok. */
	// 重命名文件，这里重命名后，之前打开的旧RDB依旧有效，只有当其文件引用变为0时才会真正删除
	if (rename(tmpfile, filename) == -1) {
		redisLog(REDIS_WARNING, "Error moving temp DB file on the final destination: %s", strerror(errno));
		unlink(tmpfile);
		return REDIS_ERR;
	}
	redisLog(REDIS_NOTICE, "DB saved on disk");
	// 这里有个问题，再进行RDB的时候，如果更改了数据，此时将dirty置为0是不是就丢失了这中间的次数？
	// 不过这应该影响比较小，只有在高并发的时候，更改次数比较频繁，才会受到影响
	server.dirty = 0;
	server.lastsave = time(NULL);
	return REDIS_OK;

werr:
	fclose(fp);
	unlink(tmpfile);
	redisLog(REDIS_WARNING, "Write error saving DB on disk: %s", strerror(errno));
	if (di) dictReleaseIterator(di);
	return REDIS_ERR;
}

// 通过fork一个进程，后台进行RDB持久化
static int rdbSaveBackground(char *filename) {
	pid_t childpid;

	if (server.bgsavechildpid != -1) return REDIS_ERR;
	if (server.vm_enabled) waitEmptyIOJobsQueue();
	if ((childpid = fork()) == 0) {
		/* Child */
		if (server.vm_enabled) vmReopenSwapFile();
		close(server.fd);
		if (rdbSave(filename) == REDIS_OK) {
			exit(0);
		} else {
			exit(1);
		}
	} else {
		/* Parent */
		if (childpid == -1) {
			redisLog(REDIS_WARNING, "Can't save in background: fork: %s",
			         strerror(errno));
			return REDIS_ERR;
		}
		redisLog(REDIS_NOTICE, "Background saving started by pid %d", childpid);
		server.bgsavechildpid = childpid;
		return REDIS_OK;
	}
	return REDIS_OK; /* unreached */
}

// 删除临时RDB文件
static void rdbRemoveTempFile(pid_t childpid) {
	char tmpfile[256];

	snprintf(tmpfile, 256, "temp-%d.rdb", (int) childpid);
	unlink(tmpfile);
}

static int rdbLoadType(FILE *fp) {
	unsigned char type;
	if (fread(&type, 1, 1, fp) == 0) return -1;
	return type;
}

static time_t rdbLoadTime(FILE *fp) {
	int32_t t32;
	if (fread(&t32, 4, 1, fp) == 0) return -1;
	return (time_t) t32;
}

/* Load an encoded length from the DB, see the REDIS_RDB_* defines on the top
 * of this file for a description of how this are stored on disk.
 *
 * isencoded is set to 1 if the readed length is not actually a length but
 * an "encoding type", check the above comments for more info */
static uint32_t rdbLoadLen(FILE *fp, int *isencoded) {
	unsigned char buf[2];
	uint32_t len;
	int type;

	if (isencoded) *isencoded = 0;
	if (fread(buf, 1, 1, fp) == 0) return REDIS_RDB_LENERR;
	type = (buf[0] & 0xC0) >> 6;
	if (type == REDIS_RDB_6BITLEN) {
		/* Read a 6 bit len */
		return buf[0] & 0x3F;
	} else if (type == REDIS_RDB_ENCVAL) {
		/* Read a 6 bit len encoding type */
		if (isencoded) *isencoded = 1;
		return buf[0] & 0x3F;
	} else if (type == REDIS_RDB_14BITLEN) {
		/* Read a 14 bit len */
		if (fread(buf + 1, 1, 1, fp) == 0) return REDIS_RDB_LENERR;
		return ((buf[0] & 0x3F) << 8) | buf[1];
	} else {
		/* Read a 32 bit len */
		if (fread(&len, 4, 1, fp) == 0) return REDIS_RDB_LENERR;
		return ntohl(len);
	}
}

static robj *rdbLoadIntegerObject(FILE *fp, int enctype) {
	unsigned char enc[4];
	long long val;

	if (enctype == REDIS_RDB_ENC_INT8) {
		if (fread(enc, 1, 1, fp) == 0) return NULL;
		val = (signed char)enc[0];
	} else if (enctype == REDIS_RDB_ENC_INT16) {
		uint16_t v;
		if (fread(enc, 2, 1, fp) == 0) return NULL;
		v = enc[0] | (enc[1] << 8);
		val = (int16_t)v;
	} else if (enctype == REDIS_RDB_ENC_INT32) {
		uint32_t v;
		if (fread(enc, 4, 1, fp) == 0) return NULL;
		v = enc[0] | (enc[1] << 8) | (enc[2] << 16) | (enc[3] << 24);
		val = (int32_t)v;
	} else {
		val = 0; /* anti-warning */
		redisAssert(0 != 0);
	}
	return createObject(REDIS_STRING, sdscatprintf(sdsempty(), "%lld", val));
}

static robj *rdbLoadLzfStringObject(FILE*fp) {
	unsigned int len, clen;
	unsigned char *c = NULL;
	sds val = NULL;

	if ((clen = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR) return NULL;
	if ((len = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR) return NULL;
	if ((c = zmalloc(clen)) == NULL) goto err;
	if ((val = sdsnewlen(NULL, len)) == NULL) goto err;
	if (fread(c, clen, 1, fp) == 0) goto err;
	if (lzf_decompress(c, clen, val, len) == 0) goto err;
	zfree(c);
	return createObject(REDIS_STRING, val);
err:
	zfree(c);
	sdsfree(val);
	return NULL;
}

static robj *rdbLoadStringObject(FILE*fp) {
	int isencoded;
	uint32_t len;
	sds val;

	len = rdbLoadLen(fp, &isencoded);
	if (isencoded) {
		switch (len) {
		case REDIS_RDB_ENC_INT8:
		case REDIS_RDB_ENC_INT16:
		case REDIS_RDB_ENC_INT32:
			return tryObjectSharing(rdbLoadIntegerObject(fp, len));
		case REDIS_RDB_ENC_LZF:
			return tryObjectSharing(rdbLoadLzfStringObject(fp));
		default:
			redisAssert(0 != 0);
		}
	}

	if (len == REDIS_RDB_LENERR) return NULL;
	val = sdsnewlen(NULL, len);
	if (len && fread(val, len, 1, fp) == 0) {
		sdsfree(val);
		return NULL;
	}
	return tryObjectSharing(createObject(REDIS_STRING, val));
}

/* For information about double serialization check rdbSaveDoubleValue() */
static int rdbLoadDoubleValue(FILE *fp, double *val) {
	char buf[128];
	unsigned char len;

	if (fread(&len, 1, 1, fp) == 0) return -1;
	switch (len) {
	case 255: *val = R_NegInf; return 0;
	case 254: *val = R_PosInf; return 0;
	case 253: *val = R_Nan; return 0;
	default:
		if (fread(buf, len, 1, fp) == 0) return -1;
		buf[len] = '\0';
		sscanf(buf, "%lg", val);
		return 0;
	}
}

/* Load a Redis object of the specified type from the specified file.
 * On success a newly allocated object is returned, otherwise NULL. */
static robj *rdbLoadObject(int type, FILE *fp) {
	robj *o;

	if (type == REDIS_STRING) {
		/* Read string value */
		if ((o = rdbLoadStringObject(fp)) == NULL) return NULL;
		tryObjectEncoding(o);
	} else if (type == REDIS_LIST || type == REDIS_SET) {
		/* Read list/set value */
		uint32_t listlen;

		if ((listlen = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR) return NULL;
		o = (type == REDIS_LIST) ? createListObject() : createSetObject();
		/* Load every single element of the list/set */
		while (listlen--) {
			robj *ele;

			if ((ele = rdbLoadStringObject(fp)) == NULL) return NULL;
			tryObjectEncoding(ele);
			if (type == REDIS_LIST) {
				listAddNodeTail((list*)o->ptr, ele);
			} else {
				dictAdd((dict*)o->ptr, ele, NULL);
			}
		}
	} else if (type == REDIS_ZSET) {
		/* Read list/set value */
		uint32_t zsetlen;
		zset *zs;

		if ((zsetlen = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR) return NULL;
		o = createZsetObject();
		zs = o->ptr;
		/* Load every single element of the list/set */
		while (zsetlen--) {
			robj *ele;
			double *score = zmalloc(sizeof(double));

			if ((ele = rdbLoadStringObject(fp)) == NULL) return NULL;
			tryObjectEncoding(ele);
			if (rdbLoadDoubleValue(fp, score) == -1) return NULL;
			dictAdd(zs->dict, ele, score);
			zslInsert(zs->zsl, *score, ele);
			incrRefCount(ele); /* added to skiplist */
		}
	} else {
		redisAssert(0 != 0);
	}
	return o;
}

static int rdbLoad(char *filename) {
	FILE *fp;
	robj *keyobj = NULL;
	uint32_t dbid;
	int type, retval, rdbver;
	dict *d = server.db[0].dict;
	redisDb *db = server.db + 0;
	char buf[1024];
	time_t expiretime = -1, now = time(NULL);
	long long loadedkeys = 0;

	fp = fopen(filename, "r");
	if (!fp) return REDIS_ERR;
	if (fread(buf, 9, 1, fp) == 0) goto eoferr;
	buf[9] = '\0';
	if (memcmp(buf, "REDIS", 5) != 0) {
		fclose(fp);
		redisLog(REDIS_WARNING, "Wrong signature trying to load DB from file");
		return REDIS_ERR;
	}
	rdbver = atoi(buf + 5);
	if (rdbver != 1) {
		fclose(fp);
		redisLog(REDIS_WARNING, "Can't handle RDB format version %d", rdbver);
		return REDIS_ERR;
	}
	while (1) {
		robj *o;

		/* Read type. */
		if ((type = rdbLoadType(fp)) == -1) goto eoferr;
		if (type == REDIS_EXPIRETIME) {
			if ((expiretime = rdbLoadTime(fp)) == -1) goto eoferr;
			/* We read the time so we need to read the object type again */
			if ((type = rdbLoadType(fp)) == -1) goto eoferr;
		}
		if (type == REDIS_EOF) break;
		/* Handle SELECT DB opcode as a special case */
		if (type == REDIS_SELECTDB) {
			if ((dbid = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)
				goto eoferr;
			if (dbid >= (unsigned)server.dbnum) {
				redisLog(REDIS_WARNING, "FATAL: Data file was created with a Redis server configured to handle more than %d databases. Exiting\n", server.dbnum);
				exit(1);
			}
			db = server.db + dbid;
			d = db->dict;
			continue;
		}
		/* Read key */
		if ((keyobj = rdbLoadStringObject(fp)) == NULL) goto eoferr;
		/* Read value */
		if ((o = rdbLoadObject(type, fp)) == NULL) goto eoferr;
		/* Add the new object in the hash table */
		retval = dictAdd(d, keyobj, o);
		if (retval == DICT_ERR) {
			redisLog(REDIS_WARNING, "Loading DB, duplicated key (%s) found! Unrecoverable error, exiting now.", keyobj->ptr);
			exit(1);
		}
		/* Set the expire time if needed */
		if (expiretime != -1) {
			setExpire(db, keyobj, expiretime);
			/* Delete this key if already expired */
			if (expiretime < now) deleteKey(db, keyobj);
			expiretime = -1;
		}
		keyobj = o = NULL;
		/* Handle swapping while loading big datasets when VM is on */
		loadedkeys++;
		if (server.vm_enabled && (loadedkeys % 5000) == 0) {
			while (zmalloc_used_memory() > server.vm_max_memory) {
				if (vmSwapOneObjectBlocking() == REDIS_ERR) break;
			}
		}
	}
	fclose(fp);
	return REDIS_OK;

eoferr: /* unexpected end of file is handled here with a fatal exit */
	if (keyobj) decrRefCount(keyobj);
	redisLog(REDIS_WARNING, "Short read or OOM loading DB. Unrecoverable error, aborting now.");
	exit(1);
	return REDIS_ERR; /* Just to avoid warning */
}

/*================================== Commands =============================== */

// 密码认证
static void authCommand(redisClient *c) {
	// 简单的比对字符串
	if (!server.requirepass || !strcmp(c->argv[1]->ptr, server.requirepass)) {
		c->authenticated = 1;
		addReply(c, shared.ok);
	} else {
		c->authenticated = 0;
		addReplySds(c, sdscatprintf(sdsempty(), "-ERR invalid password\r\n"));
	}
}

// PING命令
static void pingCommand(redisClient *c) {
	addReply(c, shared.pong);
}

// 回显
static void echoCommand(redisClient *c) {
	addReplyBulkLen(c, c->argv[1]);
	addReply(c, c->argv[1]);
	addReply(c, shared.crlf);
}

/*=================================== Strings =============================== */

static void setGenericCommand(redisClient *c, int nx) {
	int retval;

	// 如果使用SETNX 命令，则会将设置了过期时间的键删除，无论是有没有过期
	//（后续3.0改了，SETNX只有键不存在或者过期才会真正执行SET）
	if (nx) deleteIfVolatile(c->db, c->argv[1]);
	retval = dictAdd(c->db->dict, c->argv[1], c->argv[2]);
	if (retval == DICT_ERR) {
		if (!nx) {
			/* If the key is about a swapped value, we want a new key object
			 * to overwrite the old. So we delete the old key in the database.
			 * This will also make sure that swap pages about the old object
			 * will be marked as free. */
			if (deleteIfSwapped(c->db, c->argv[1]))
				incrRefCount(c->argv[1]);
			dictReplace(c->db->dict, c->argv[1], c->argv[2]);
			incrRefCount(c->argv[2]);
		} else {
			addReply(c, shared.czero);
			return;
		}
	} else {
		incrRefCount(c->argv[1]);
		incrRefCount(c->argv[2]);
	}
	server.dirty++;
	removeExpire(c->db, c->argv[1]);
	addReply(c, nx ? shared.cone : shared.ok);
}

static void setCommand(redisClient *c) {
	setGenericCommand(c, 0);
}

static void setnxCommand(redisClient *c) {
	setGenericCommand(c, 1);
}

static int getGenericCommand(redisClient *c) {
	robj *o = lookupKeyRead(c->db, c->argv[1]);

	if (o == NULL) {
		addReply(c, shared.nullbulk);
		return REDIS_OK;
	} else {
		if (o->type != REDIS_STRING) {
			addReply(c, shared.wrongtypeerr);
			return REDIS_ERR;
		} else {
			addReplyBulkLen(c, o);
			addReply(c, o);
			addReply(c, shared.crlf);
			return REDIS_OK;
		}
	}
}

static void getCommand(redisClient *c) {
	getGenericCommand(c);
}

static void getsetCommand(redisClient *c) {
	if (getGenericCommand(c) == REDIS_ERR) return;
	if (dictAdd(c->db->dict, c->argv[1], c->argv[2]) == DICT_ERR) {
		dictReplace(c->db->dict, c->argv[1], c->argv[2]);
	} else {
		incrRefCount(c->argv[1]);
	}
	incrRefCount(c->argv[2]);
	server.dirty++;
	removeExpire(c->db, c->argv[1]);
}

static void mgetCommand(redisClient *c) {
	int j;

	addReplySds(c, sdscatprintf(sdsempty(), "*%d\r\n", c->argc - 1));
	for (j = 1; j < c->argc; j++) {
		robj *o = lookupKeyRead(c->db, c->argv[j]);
		if (o == NULL) {
			addReply(c, shared.nullbulk);
		} else {
			if (o->type != REDIS_STRING) {
				addReply(c, shared.nullbulk);
			} else {
				addReplyBulkLen(c, o);
				addReply(c, o);
				addReply(c, shared.crlf);
			}
		}
	}
}

static void msetGenericCommand(redisClient *c, int nx) {
	int j, busykeys = 0;

	if ((c->argc % 2) == 0) {
		addReplySds(c, sdsnew("-ERR wrong number of arguments for MSET\r\n"));
		return;
	}
	/* Handle the NX flag. The MSETNX semantic is to return zero and don't
	 * set nothing at all if at least one already key exists. */
	if (nx) {
		for (j = 1; j < c->argc; j += 2) {
			if (lookupKeyWrite(c->db, c->argv[j]) != NULL) {
				busykeys++;
			}
		}
	}
	if (busykeys) {
		addReply(c, shared.czero);
		return;
	}

	for (j = 1; j < c->argc; j += 2) {
		int retval;

		tryObjectEncoding(c->argv[j + 1]);
		retval = dictAdd(c->db->dict, c->argv[j], c->argv[j + 1]);
		if (retval == DICT_ERR) {
			dictReplace(c->db->dict, c->argv[j], c->argv[j + 1]);
			incrRefCount(c->argv[j + 1]);
		} else {
			incrRefCount(c->argv[j]);
			incrRefCount(c->argv[j + 1]);
		}
		removeExpire(c->db, c->argv[j]);
	}
	server.dirty += (c->argc - 1) / 2;
	addReply(c, nx ? shared.cone : shared.ok);
}

static void msetCommand(redisClient *c) {
	msetGenericCommand(c, 0);
}

static void msetnxCommand(redisClient *c) {
	msetGenericCommand(c, 1);
}

static void incrDecrCommand(redisClient *c, long long incr) {
	long long value;
	int retval;
	robj *o;

	o = lookupKeyWrite(c->db, c->argv[1]);
	if (o == NULL) {
		value = 0;
	} else {
		if (o->type != REDIS_STRING) {
			value = 0;
		} else {
			char *eptr;

			if (o->encoding == REDIS_ENCODING_RAW)
				value = strtoll(o->ptr, &eptr, 10);
			else if (o->encoding == REDIS_ENCODING_INT)
				value = (long)o->ptr;
			else
				redisAssert(1 != 1);
		}
	}

	value += incr;
	o = createObject(REDIS_STRING, sdscatprintf(sdsempty(), "%lld", value));
	tryObjectEncoding(o);
	retval = dictAdd(c->db->dict, c->argv[1], o);
	if (retval == DICT_ERR) {
		dictReplace(c->db->dict, c->argv[1], o);
		removeExpire(c->db, c->argv[1]);
	} else {
		incrRefCount(c->argv[1]);
	}
	server.dirty++;
	addReply(c, shared.colon);
	addReply(c, o);
	addReply(c, shared.crlf);
}

static void incrCommand(redisClient *c) {
	incrDecrCommand(c, 1);
}

static void decrCommand(redisClient *c) {
	incrDecrCommand(c, -1);
}

static void incrbyCommand(redisClient *c) {
	long long incr = strtoll(c->argv[2]->ptr, NULL, 10);
	incrDecrCommand(c, incr);
}

static void decrbyCommand(redisClient *c) {
	long long incr = strtoll(c->argv[2]->ptr, NULL, 10);
	incrDecrCommand(c, -incr);
}

/* ========================= Type agnostic commands ========================= */

// 删除一个键，返回删除的个数
static void delCommand(redisClient *c) {
	int deleted = 0, j;

	for (j = 1; j < c->argc; j++) {
		if (deleteKey(c->db, c->argv[j])) {
			server.dirty++;
			deleted++;
		}
	}
	// 这里不直接使用default里面的方式，为了节省创建字符串应答的时间，提高性能，
	// （删除多半是发生在删除一个键或者键不存在的情况）
	switch (deleted) {
	case 0:
		addReply(c, shared.czero);
		break;
	case 1:
		addReply(c, shared.cone);
		break;
	default:
		addReplySds(c, sdscatprintf(sdsempty(), ":%d\r\n", deleted));
		break;
	}
}

// 判断一个键是否存在，存在则返回1，不存在返回0
static void existsCommand(redisClient *c) {
	addReply(c, lookupKeyRead(c->db, c->argv[1]) ? shared.cone : shared.czero);
}

static void selectCommand(redisClient *c) {
	int id = atoi(c->argv[1]->ptr);

	if (selectDb(c, id) == REDIS_ERR) {
		addReplySds(c, sdsnew("-ERR invalid DB index\r\n"));
	} else {
		addReply(c, shared.ok);
	}
}

static void randomkeyCommand(redisClient *c) {
	dictEntry *de;

	while (1) {
		de = dictGetRandomKey(c->db->dict);
		if (!de || expireIfNeeded(c->db, dictGetEntryKey(de)) == 0) break;
	}
	if (de == NULL) {
		addReply(c, shared.plus);
		addReply(c, shared.crlf);
	} else {
		addReply(c, shared.plus);
		addReply(c, dictGetEntryKey(de));
		addReply(c, shared.crlf);
	}
}

static void keysCommand(redisClient *c) {
	dictIterator *di;
	dictEntry *de;
	sds pattern = c->argv[1]->ptr;
	int plen = sdslen(pattern);
	unsigned long numkeys = 0, keyslen = 0;
	robj *lenobj = createObject(REDIS_STRING, NULL);

	di = dictGetIterator(c->db->dict);
	addReply(c, lenobj);
	decrRefCount(lenobj);
	while ((de = dictNext(di)) != NULL) {
		robj *keyobj = dictGetEntryKey(de);

		sds key = keyobj->ptr;
		if ((pattern[0] == '*' && pattern[1] == '\0') ||
		        stringmatchlen(pattern, plen, key, sdslen(key), 0)) {
			if (expireIfNeeded(c->db, keyobj) == 0) {
				if (numkeys != 0)
					addReply(c, shared.space);
				addReply(c, keyobj);
				numkeys++;
				keyslen += sdslen(key);
			}
		}
	}
	dictReleaseIterator(di);
	lenobj->ptr = sdscatprintf(sdsempty(), "$%lu\r\n", keyslen + (numkeys ? (numkeys - 1) : 0));
	addReply(c, shared.crlf);
}

static void dbsizeCommand(redisClient *c) {
	addReplySds(c,
	            sdscatprintf(sdsempty(), ":%lu\r\n", dictSize(c->db->dict)));
}

// 获取最近一次RDB持久化成功的时间
static void lastsaveCommand(redisClient *c) {
	addReplySds(c,
	            sdscatprintf(sdsempty(), ":%lu\r\n", server.lastsave));
}

// 获取值的数据类型
static void typeCommand(redisClient *c) {
	robj *o;
	char *type;

	o = lookupKeyRead(c->db, c->argv[1]);
	if (o == NULL) {
		type = "+none";
	} else {
		switch (o->type) {
		case REDIS_STRING: type = "+string"; break;
		case REDIS_LIST: type = "+list"; break;
		case REDIS_SET: type = "+set"; break;
		case REDIS_ZSET: type = "+zset"; break;
		default: type = "unknown"; break;
		}
	}
	addReplySds(c, sdsnew(type));
	addReply(c, shared.crlf);
}

static void saveCommand(redisClient *c) {
	if (server.bgsavechildpid != -1) {
		addReplySds(c, sdsnew("-ERR background save in progress\r\n"));
		return;
	}
	if (rdbSave(server.dbfilename) == REDIS_OK) {
		addReply(c, shared.ok);
	} else {
		addReply(c, shared.err);
	}
}

static void bgsaveCommand(redisClient *c) {
	if (server.bgsavechildpid != -1) {
		addReplySds(c, sdsnew("-ERR background save already in progress\r\n"));
		return;
	}
	if (rdbSaveBackground(server.dbfilename) == REDIS_OK) {
		char *status = "+Background saving started\r\n";
		addReplySds(c, sdsnew(status));
	} else {
		addReply(c, shared.err);
	}
}

static void shutdownCommand(redisClient *c) {
	redisLog(REDIS_WARNING, "User requested shutdown, saving DB...");
	/* Kill the saving child if there is a background saving in progress.
	   We want to avoid race conditions, for instance our saving child may
	   overwrite the synchronous saving did by SHUTDOWN. */
	if (server.bgsavechildpid != -1) {
		redisLog(REDIS_WARNING, "There is a live saving child. Killing it!");
		kill(server.bgsavechildpid, SIGKILL);
		rdbRemoveTempFile(server.bgsavechildpid);
	}
	if (server.appendonly) {
		/* Append only file: fsync() the AOF and exit */
		fsync(server.appendfd);
		if (server.vm_enabled) unlink(server.vm_swap_file);
		exit(0);
	} else {
		/* Snapshotting. Perform a SYNC SAVE and exit */
		if (rdbSave(server.dbfilename) == REDIS_OK) {
			if (server.daemonize)
				unlink(server.pidfile);
			redisLog(REDIS_WARNING, "%zu bytes used at exit", zmalloc_used_memory());
			redisLog(REDIS_WARNING, "Server exit now, bye bye...");
			if (server.vm_enabled) unlink(server.vm_swap_file);
			exit(0);
		} else {
			/* Ooops.. error saving! The best we can do is to continue operating.
			 * Note that if there was a background saving process, in the next
			 * cron() Redis will be notified that the background saving aborted,
			 * handling special stuff like slaves pending for synchronization... */
			redisLog(REDIS_WARNING, "Error trying to save the DB, can't exit");
			addReplySds(c, sdsnew("-ERR can't quit, problems saving the DB\r\n"));
		}
	}
}

static void renameGenericCommand(redisClient *c, int nx) {
	robj *o;

	/* To use the same key as src and dst is probably an error */
	if (sdscmp(c->argv[1]->ptr, c->argv[2]->ptr) == 0) {
		addReply(c, shared.sameobjecterr);
		return;
	}

	o = lookupKeyWrite(c->db, c->argv[1]);
	if (o == NULL) {
		addReply(c, shared.nokeyerr);
		return;
	}
	incrRefCount(o);
	deleteIfVolatile(c->db, c->argv[2]);
	if (dictAdd(c->db->dict, c->argv[2], o) == DICT_ERR) {
		if (nx) {
			decrRefCount(o);
			addReply(c, shared.czero);
			return;
		}
		dictReplace(c->db->dict, c->argv[2], o);
	} else {
		incrRefCount(c->argv[2]);
	}
	deleteKey(c->db, c->argv[1]);
	server.dirty++;
	addReply(c, nx ? shared.cone : shared.ok);
}

static void renameCommand(redisClient *c) {
	renameGenericCommand(c, 0);
}

static void renamenxCommand(redisClient *c) {
	renameGenericCommand(c, 1);
}

static void moveCommand(redisClient *c) {
	robj *o;
	redisDb *src, *dst;
	int srcid;

	/* Obtain source and target DB pointers */
	src = c->db;
	srcid = c->db->id;
	if (selectDb(c, atoi(c->argv[2]->ptr)) == REDIS_ERR) {
		addReply(c, shared.outofrangeerr);
		return;
	}
	dst = c->db;
	selectDb(c, srcid); /* Back to the source DB */

	/* If the user is moving using as target the same
	 * DB as the source DB it is probably an error. */
	if (src == dst) {
		addReply(c, shared.sameobjecterr);
		return;
	}

	/* Check if the element exists and get a reference */
	o = lookupKeyWrite(c->db, c->argv[1]);
	if (!o) {
		addReply(c, shared.czero);
		return;
	}

	/* Try to add the element to the target DB */
	deleteIfVolatile(dst, c->argv[1]);
	if (dictAdd(dst->dict, c->argv[1], o) == DICT_ERR) {
		addReply(c, shared.czero);
		return;
	}
	incrRefCount(c->argv[1]);
	incrRefCount(o);

	/* OK! key moved, free the entry in the source DB */
	deleteKey(src, c->argv[1]);
	server.dirty++;
	addReply(c, shared.cone);
}

/* =================================== Lists ================================ */
// 向列表增加元素，其中where控制在左边还是右边
static void pushGenericCommand(redisClient *c, int where) {
	robj *lobj;
	list *list;

	// 查询写入键对应的值
	lobj = lookupKeyWrite(c->db, c->argv[1]);
	if (lobj == NULL) {
		// 处理阻塞等待的客户端
		if (handleClientsWaitingListPush(c, c->argv[1], c->argv[2])) {
			addReply(c, shared.ok);
			return;
		}
		// 创建List对象，并将数据放入List中
		lobj = createListObject();
		list = lobj->ptr;
		if (where == REDIS_HEAD) {
			listAddNodeHead(list, c->argv[2]);
		} else {
			listAddNodeTail(list, c->argv[2]);
		}
		dictAdd(c->db->dict, c->argv[1], lobj);
		incrRefCount(c->argv[1]);
		incrRefCount(c->argv[2]);
	} else {
		// 和上面代码差不多，只是多了类型检查以及少了对象值的创建和键的添加
		if (lobj->type != REDIS_LIST) {
			addReply(c, shared.wrongtypeerr);
			return;
		}
		if (handleClientsWaitingListPush(c, c->argv[1], c->argv[2])) {
			addReply(c, shared.ok);
			return;
		}
		list = lobj->ptr;
		if (where == REDIS_HEAD) {
			listAddNodeHead(list, c->argv[2]);
		} else {
			listAddNodeTail(list, c->argv[2]);
		}
		incrRefCount(c->argv[2]);
	}
	server.dirty++;
	addReply(c, shared.ok);
}

// 从列表左边增加元素
static void lpushCommand(redisClient *c) {
	pushGenericCommand(c, REDIS_HEAD);
}

// 从列表右边增加元素
static void rpushCommand(redisClient *c) {
	pushGenericCommand(c, REDIS_TAIL);
}

// 获取链表长度
static void llenCommand(redisClient *c) {
	robj *o;
	list *l;

	o = lookupKeyRead(c->db, c->argv[1]);
	if (o == NULL) {
		addReply(c, shared.czero);
		return;
	} else {
		if (o->type != REDIS_LIST) {
			addReply(c, shared.wrongtypeerr);
		} else {
			l = o->ptr;
			addReplySds(c, sdscatprintf(sdsempty(), ":%d\r\n", listLength(l)));
		}
	}
}

// 获取链表指定下标的元素值，从0开始，同理，也是根据index的正负来决定左右方向
static void lindexCommand(redisClient *c) {
	robj *o;
	int index = atoi(c->argv[2]->ptr);

	// 查找Key对应的value
	o = lookupKeyRead(c->db, c->argv[1]);
	if (o == NULL) {
		addReply(c, shared.nullbulk);
	} else {
		if (o->type != REDIS_LIST) {
			addReply(c, shared.wrongtypeerr);
		} else {
			list *list = o->ptr;
			listNode *ln;

			ln = listIndex(list, index);
			if (ln == NULL) {
				addReply(c, shared.nullbulk);
			} else {
				robj *ele = listNodeValue(ln);
				addReplyBulkLen(c, ele);
				addReply(c, ele);
				addReply(c, shared.crlf);
			}
		}
	}
}

// 设置List指定下标元素的值
static void lsetCommand(redisClient *c) {
	robj *o;
	int index = atoi(c->argv[2]->ptr);

	o = lookupKeyWrite(c->db, c->argv[1]);
	if (o == NULL) {
		addReply(c, shared.nokeyerr);
	} else {
		if (o->type != REDIS_LIST) {
			addReply(c, shared.wrongtypeerr);
		} else {
			list *list = o->ptr;
			listNode *ln;

			ln = listIndex(list, index);
			if (ln == NULL) {
				// 空，超出范围
				addReply(c, shared.outofrangeerr);
			} else {
				robj *ele = listNodeValue(ln);

				decrRefCount(ele);
				listNodeValue(ln) = c->argv[3];
				incrRefCount(c->argv[3]);
				addReply(c, shared.ok);
				server.dirty++;
			}
		}
	}
}

// 从链表弹出元素
static void popGenericCommand(redisClient *c, int where) {
	robj *o;

	o = lookupKeyWrite(c->db, c->argv[1]);
	if (o == NULL) {
		addReply(c, shared.nullbulk);
	} else {
		if (o->type != REDIS_LIST) {
			addReply(c, shared.wrongtypeerr);
		} else {
			list *list = o->ptr;
			listNode *ln;

			if (where == REDIS_HEAD)
				ln = listFirst(list);
			else
				ln = listLast(list);

			if (ln == NULL) {
				addReply(c, shared.nullbulk);
			} else {
				robj *ele = listNodeValue(ln);
				addReplyBulkLen(c, ele);
				addReply(c, ele);
				addReply(c, shared.crlf);
				listDelNode(list, ln);
				server.dirty++;
			}
		}
	}
}

// 从链表左边弹出元素
static void lpopCommand(redisClient *c) {
	popGenericCommand(c, REDIS_HEAD);
}

// 从链表右边弹出元素
static void rpopCommand(redisClient *c) {
	popGenericCommand(c, REDIS_TAIL);
}

// 获取链表指定范围的元素
static void lrangeCommand(redisClient *c) {
	robj *o;
	int start = atoi(c->argv[2]->ptr);
	int end = atoi(c->argv[3]->ptr);

	o = lookupKeyRead(c->db, c->argv[1]);
	if (o == NULL) {
		addReply(c, shared.nullmultibulk);
	} else {
		if (o->type != REDIS_LIST) {
			addReply(c, shared.wrongtypeerr);
		} else {
			list *list = o->ptr;
			listNode *ln;
			int llen = listLength(list);
			int rangelen, j;
			robj *ele;

			/* convert negative indexes */
			if (start < 0) start = llen + start;
			if (end < 0) end = llen + end;
			if (start < 0) start = 0;
			if (end < 0) end = 0;

			/* indexes sanity checks */
			if (start > end || start >= llen) {
				/* Out of range start or start > end result in empty list */
				addReply(c, shared.emptymultibulk);
				return;
			}
			if (end >= llen) end = llen - 1;
			rangelen = (end - start) + 1;

			/* Return the result in form of a multi-bulk reply */
			ln = listIndex(list, start);
			addReplySds(c, sdscatprintf(sdsempty(), "*%d\r\n", rangelen));
			for (j = 0; j < rangelen; j++) {
				ele = listNodeValue(ln);
				addReplyBulkLen(c, ele);
				addReply(c, ele);
				addReply(c, shared.crlf);
				ln = ln->next;
			}
		}
	}
}

// 保留List指定范围区间的元素
static void ltrimCommand(redisClient *c) {
	robj *o;
	int start = atoi(c->argv[2]->ptr);
	int end = atoi(c->argv[3]->ptr);

	o = lookupKeyWrite(c->db, c->argv[1]);
	if (o == NULL) {
		addReply(c, shared.ok);
	} else {
		if (o->type != REDIS_LIST) {
			addReply(c, shared.wrongtypeerr);
		} else {
			list *list = o->ptr;
			listNode *ln;
			int llen = listLength(list);
			int j, ltrim, rtrim;

			/* convert negative indexes */
			if (start < 0) start = llen + start;
			if (end < 0) end = llen + end;
			if (start < 0) start = 0;
			if (end < 0) end = 0;

			/* indexes sanity checks */
			if (start > end || start >= llen) {
				/* Out of range start or start > end result in empty list */
				ltrim = llen;
				rtrim = 0;
			} else {
				if (end >= llen) end = llen - 1;
				ltrim = start;
				rtrim = llen - end - 1;
			}

			/* Remove list elements to perform the trim */
			for (j = 0; j < ltrim; j++) {
				ln = listFirst(list);
				listDelNode(list, ln);
			}
			for (j = 0; j < rtrim; j++) {
				ln = listLast(list);
				listDelNode(list, ln);
			}
			server.dirty++;
			addReply(c, shared.ok);
		}
	}
}

// 删除链表中指定值的元素
// 其中第二个参数count大于0则从左边开始，小于0则从右边开始,删除|count|个
// 第三个参数为要删除的值
static void lremCommand(redisClient *c) {
	robj *o;

	o = lookupKeyWrite(c->db, c->argv[1]);
	if (o == NULL) {
		addReply(c, shared.czero);
	} else {
		if (o->type != REDIS_LIST) {
			addReply(c, shared.wrongtypeerr);
		} else {
			list *list = o->ptr;
			listNode *ln, *next;
			int toremove = atoi(c->argv[2]->ptr);
			int removed = 0;
			int fromtail = 0;

			if (toremove < 0) {
				toremove = -toremove;
				fromtail = 1;
			}
			ln = fromtail ? list->tail : list->head;
			while (ln) {
				robj *ele = listNodeValue(ln);

				next = fromtail ? ln->prev : ln->next;
				if (compareStringObjects(ele, c->argv[3]) == 0) {
					listDelNode(list, ln);
					server.dirty++;
					removed++;
					if (toremove && removed == toremove) break;
				}
				ln = next;
			}
			addReplySds(c, sdscatprintf(sdsempty(), ":%d\r\n", removed));
		}
	}
}

/* This is the semantic of this command:
 *  RPOPLPUSH srclist dstlist:
 *   IF LLEN(srclist) > 0
 *     element = RPOP srclist
 *     LPUSH dstlist element
 *     RETURN element
 *   ELSE
 *     RETURN nil
 *   END
 *  END
 *
 * The idea is to be able to get an element from a list in a reliable way
 * since the element is not just returned but pushed against another list
 * as well. This command was originally proposed by Ezra Zygmuntowicz.
 */
// 从链表的右边弹出，放到另一个链表的左边（也可以是同一个链表）
static void rpoplpushcommand(redisClient *c) {
	robj *sobj;


	sobj = lookupKeyWrite(c->db, c->argv[1]);
	if (sobj == NULL) {
		addReply(c, shared.nullbulk);
	} else {
		if (sobj->type != REDIS_LIST) {
			addReply(c, shared.wrongtypeerr);
		} else {
			list *srclist = sobj->ptr;
			// 从第一个链表Src右边取出数据
			listNode *ln = listLast(srclist);

			if (ln == NULL) {
				addReply(c, shared.nullbulk);
			} else {
				robj *dobj = lookupKeyWrite(c->db, c->argv[2]);
				robj *ele = listNodeValue(ln);
				list *dstlist;

				if (dobj && dobj->type != REDIS_LIST) {
					addReply(c, shared.wrongtypeerr);
					return;
				}

				/* Add the element to the target list (unless it's directly
				 * passed to some BLPOP-ing client */
				if (!handleClientsWaitingListPush(c, c->argv[2], ele)) {
					if (dobj == NULL) {
						/* Create the list if the key does not exist */
						dobj = createListObject();
						dictAdd(c->db->dict, c->argv[2], dobj);
						incrRefCount(c->argv[2]);
					}
					// 如果没有阻塞的客户端等待，才放入到链表dst左边
					dstlist = dobj->ptr;
					listAddNodeHead(dstlist, ele);
					incrRefCount(ele);
				}

				/* Send the element to the client as reply as well */
				addReplyBulkLen(c, ele);
				addReply(c, ele);
				addReply(c, shared.crlf);

				/* Finally remove the element from the source list */
				listDelNode(srclist, ln);
				server.dirty++;
			}
		}
	}
}


/* ==================================== Sets ================================ */

// 此版本的Set采用的是Dict哈希映射来实现的

// 添加集合元素
static void saddCommand(redisClient *c) {
	robj *set;

	set = lookupKeyWrite(c->db, c->argv[1]);
	if (set == NULL) {
		// 创建集合对象
		set = createSetObject();
		dictAdd(c->db->dict, c->argv[1], set);
		incrRefCount(c->argv[1]);
	} else {
		if (set->type != REDIS_SET) {
			addReply(c, shared.wrongtypeerr);
			return;
		}
	}
	if (dictAdd(set->ptr, c->argv[2], NULL) == DICT_OK) {
		incrRefCount(c->argv[2]);
		server.dirty++;
		// 成功则返回1
		addReply(c, shared.cone);
	} else {
		// 元素已经存在，返回0
		addReply(c, shared.czero);
	}
}

// 删除集合中的元素
static void sremCommand(redisClient *c) {
	robj *set;

	set = lookupKeyWrite(c->db, c->argv[1]);
	if (set == NULL) {
		addReply(c, shared.czero);
	} else {
		if (set->type != REDIS_SET) {
			addReply(c, shared.wrongtypeerr);
			return;
		}
		if (dictDelete(set->ptr, c->argv[2]) == DICT_OK) {
			server.dirty++;
			if (htNeedsResize(set->ptr)) dictResize(set->ptr);
			addReply(c, shared.cone);
		} else {
			// 元素不存在
			addReply(c, shared.czero);
		}
	}
}

static void smoveCommand(redisClient *c) {
	robj *srcset, *dstset;

	srcset = lookupKeyWrite(c->db, c->argv[1]);
	dstset = lookupKeyWrite(c->db, c->argv[2]);

	/* If the source key does not exist return 0, if it's of the wrong type
	 * raise an error */
	if (srcset == NULL || srcset->type != REDIS_SET) {
		addReply(c, srcset ? shared.wrongtypeerr : shared.czero);
		return;
	}
	/* Error if the destination key is not a set as well */
	if (dstset && dstset->type != REDIS_SET) {
		addReply(c, shared.wrongtypeerr);
		return;
	}
	/* Remove the element from the source set */
	if (dictDelete(srcset->ptr, c->argv[3]) == DICT_ERR) {
		/* Key not found in the src set! return zero */
		addReply(c, shared.czero);
		return;
	}
	server.dirty++;
	/* Add the element to the destination set */
	if (!dstset) {
		dstset = createSetObject();
		dictAdd(c->db->dict, c->argv[2], dstset);
		incrRefCount(c->argv[2]);
	}
	if (dictAdd(dstset->ptr, c->argv[3], NULL) == DICT_OK)
		incrRefCount(c->argv[3]);
	addReply(c, shared.cone);
}

// 判断集合元素是否存在
static void sismemberCommand(redisClient *c) {
	robj *set;

	set = lookupKeyRead(c->db, c->argv[1]);
	if (set == NULL) {
		addReply(c, shared.czero);
	} else {
		if (set->type != REDIS_SET) {
			addReply(c, shared.wrongtypeerr);
			return;
		}
		if (dictFind(set->ptr, c->argv[2]))
			addReply(c, shared.cone);
		else
			addReply(c, shared.czero);
	}
}

// 获取集合元素个数
static void scardCommand(redisClient *c) {
	robj *o;
	dict *s;

	o = lookupKeyRead(c->db, c->argv[1]);
	if (o == NULL) {
		addReply(c, shared.czero);
		return;
	} else {
		if (o->type != REDIS_SET) {
			addReply(c, shared.wrongtypeerr);
		} else {
			s = o->ptr;
			addReplySds(c, sdscatprintf(sdsempty(), ":%lu\r\n",
			                            dictSize(s)));
		}
	}
}

// 从集合中弹出一个元素（随机）
static void spopCommand(redisClient *c) {
	robj *set;
	dictEntry *de;

	set = lookupKeyWrite(c->db, c->argv[1]);
	if (set == NULL) {
		addReply(c, shared.nullbulk);
	} else {
		if (set->type != REDIS_SET) {
			addReply(c, shared.wrongtypeerr);
			return;
		}
		de = dictGetRandomKey(set->ptr);
		if (de == NULL) {
			addReply(c, shared.nullbulk);
		} else {
			robj *ele = dictGetEntryKey(de);

			addReplyBulkLen(c, ele);
			addReply(c, ele);
			addReply(c, shared.crlf);
			dictDelete(set->ptr, ele);
			if (htNeedsResize(set->ptr)) dictResize(set->ptr);
			server.dirty++;
		}
	}
}

// 和spopCommand实现类似，只是少了元素删除的操作
static void srandmemberCommand(redisClient *c) {
	robj *set;
	dictEntry *de;

	set = lookupKeyRead(c->db, c->argv[1]);
	if (set == NULL) {
		addReply(c, shared.nullbulk);
	} else {
		if (set->type != REDIS_SET) {
			addReply(c, shared.wrongtypeerr);
			return;
		}
		de = dictGetRandomKey(set->ptr);
		if (de == NULL) {
			addReply(c, shared.nullbulk);
		} else {
			robj *ele = dictGetEntryKey(de);

			addReplyBulkLen(c, ele);
			addReply(c, ele);
			addReply(c, shared.crlf);
		}
	}
}

static int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
	dict **d1 = (void*) s1, **d2 = (void*) s2;

	return dictSize(*d1) - dictSize(*d2);
}

static void sinterGenericCommand(redisClient *c, robj **setskeys, unsigned long setsnum, robj *dstkey) {
	dict **dv = zmalloc(sizeof(dict*)*setsnum);
	dictIterator *di;
	dictEntry *de;
	robj *lenobj = NULL, *dstset = NULL;
	unsigned long j, cardinality = 0;

	for (j = 0; j < setsnum; j++) {
		robj *setobj;

		setobj = dstkey ?
		         lookupKeyWrite(c->db, setskeys[j]) :
		         lookupKeyRead(c->db, setskeys[j]);
		if (!setobj) {
			zfree(dv);
			if (dstkey) {
				if (deleteKey(c->db, dstkey))
					server.dirty++;
				addReply(c, shared.czero);
			} else {
				addReply(c, shared.nullmultibulk);
			}
			return;
		}
		if (setobj->type != REDIS_SET) {
			zfree(dv);
			addReply(c, shared.wrongtypeerr);
			return;
		}
		dv[j] = setobj->ptr;
	}
	/* Sort sets from the smallest to largest, this will improve our
	 * algorithm's performace */
	qsort(dv, setsnum, sizeof(dict*), qsortCompareSetsByCardinality);

	/* The first thing we should output is the total number of elements...
	 * since this is a multi-bulk write, but at this stage we don't know
	 * the intersection set size, so we use a trick, append an empty object
	 * to the output list and save the pointer to later modify it with the
	 * right length */
	if (!dstkey) {
		lenobj = createObject(REDIS_STRING, NULL);
		addReply(c, lenobj);
		decrRefCount(lenobj);
	} else {
		/* If we have a target key where to store the resulting set
		 * create this key with an empty set inside */
		dstset = createSetObject();
	}

	/* Iterate all the elements of the first (smallest) set, and test
	 * the element against all the other sets, if at least one set does
	 * not include the element it is discarded */
	di = dictGetIterator(dv[0]);

	while ((de = dictNext(di)) != NULL) {
		robj *ele;

		for (j = 1; j < setsnum; j++)
			if (dictFind(dv[j], dictGetEntryKey(de)) == NULL) break;
		if (j != setsnum)
			continue; /* at least one set does not contain the member */
		ele = dictGetEntryKey(de);
		if (!dstkey) {
			addReplyBulkLen(c, ele);
			addReply(c, ele);
			addReply(c, shared.crlf);
			cardinality++;
		} else {
			dictAdd(dstset->ptr, ele, NULL);
			incrRefCount(ele);
		}
	}
	dictReleaseIterator(di);

	if (dstkey) {
		/* Store the resulting set into the target */
		deleteKey(c->db, dstkey);
		dictAdd(c->db->dict, dstkey, dstset);
		incrRefCount(dstkey);
	}

	if (!dstkey) {
		lenobj->ptr = sdscatprintf(sdsempty(), "*%lu\r\n", cardinality);
	} else {
		addReplySds(c, sdscatprintf(sdsempty(), ":%lu\r\n",
		                            dictSize((dict*)dstset->ptr)));
		server.dirty++;
	}
	zfree(dv);
}

static void sinterCommand(redisClient *c) {
	sinterGenericCommand(c, c->argv + 1, c->argc - 1, NULL);
}

static void sinterstoreCommand(redisClient *c) {
	sinterGenericCommand(c, c->argv + 2, c->argc - 2, c->argv[1]);
}

#define REDIS_OP_UNION 0
#define REDIS_OP_DIFF 1

// 集合操作
static void sunionDiffGenericCommand(redisClient *c, robj **setskeys, int setsnum, robj *dstkey, int op) {
	dict **dv = zmalloc(sizeof(dict*)*setsnum);
	dictIterator *di;
	dictEntry *de;
	robj *dstset = NULL;
	int j, cardinality = 0;

	// 查找出Key对应的集合，并校验类型
	for (j = 0; j < setsnum; j++) {
		robj *setobj;

		setobj = dstkey ?
		         lookupKeyWrite(c->db, setskeys[j]) :
		         lookupKeyRead(c->db, setskeys[j]);
		if (!setobj) {
			dv[j] = NULL;
			continue;
		}
		if (setobj->type != REDIS_SET) {
			zfree(dv);
			addReply(c, shared.wrongtypeerr);
			return;
		}
		dv[j] = setobj->ptr;
	}

	/* We need a temp set object to store our union. If the dstkey
	 * is not NULL (that is, we are inside an SUNIONSTORE operation) then
	 * this set object will be the resulting object to set into the target key*/
	// 用于临时存储集合操作的结果，如果dstkey不为空，则用此对象替换掉该key对应的值
	dstset = createSetObject();

	/* Iterate all the elements of all the sets, add every element a single
	 * time to the result set */
	for (j = 0; j < setsnum; j++) {
		if (op == REDIS_OP_DIFF && j == 0 && !dv[j]) break; /* result set is empty */
		if (!dv[j]) continue; /* non existing keys are like empty sets */

		di = dictGetIterator(dv[j]);

		while ((de = dictNext(di)) != NULL) {
			robj *ele;

			/* dictAdd will not add the same element multiple times */
			ele = dictGetEntryKey(de);
			if (op == REDIS_OP_UNION || j == 0) {
				// 集合并操作，将所有元素都添加到dstset中
				// 或者是集合差操作，但是第一个集合作为初始集合
				if (dictAdd(dstset->ptr, ele, NULL) == DICT_OK) {
					incrRefCount(ele);
					cardinality++;
				}
			} else if (op == REDIS_OP_DIFF) {
				// 集合差操作，当前集合所拥有的元素从初始集合中删除
				if (dictDelete(dstset->ptr, ele) == DICT_OK) {
					cardinality--;
				}
			}
		}
		dictReleaseIterator(di);

		if (op == REDIS_OP_DIFF && cardinality == 0) break; /* result set is empty */
	}

	/* Output the content of the resulting set, if not in STORE mode */
	if (!dstkey) {
		addReplySds(c, sdscatprintf(sdsempty(), "*%d\r\n", cardinality));
		di = dictGetIterator(dstset->ptr);
		while ((de = dictNext(di)) != NULL) {
			robj *ele;

			ele = dictGetEntryKey(de);
			addReplyBulkLen(c, ele);
			addReply(c, ele);
			addReply(c, shared.crlf);
		}
		dictReleaseIterator(di);
	} else {
		/* If we have a target key where to store the resulting set
		 * create this key with the result set inside */
		deleteKey(c->db, dstkey);
		dictAdd(c->db->dict, dstkey, dstset);
		incrRefCount(dstkey);
	}

	/* Cleanup */
	if (!dstkey) {
		decrRefCount(dstset);
	} else {
		addReplySds(c, sdscatprintf(sdsempty(), ":%lu\r\n",
		                            dictSize((dict*)dstset->ptr)));
		server.dirty++;
	}
	zfree(dv);
}

// 集合并
static void sunionCommand(redisClient *c) {
	sunionDiffGenericCommand(c, c->argv + 1, c->argc - 1, NULL, REDIS_OP_UNION);
}

// 集合并，然后将结果存储到指定的Key中
static void sunionstoreCommand(redisClient *c) {
	sunionDiffGenericCommand(c, c->argv + 2, c->argc - 2, c->argv[1], REDIS_OP_UNION);
}

// 集合差
static void sdiffCommand(redisClient *c) {
	sunionDiffGenericCommand(c, c->argv + 1, c->argc - 1, NULL, REDIS_OP_DIFF);
}

// 集合差，然后将结果存储到指定的Key中
static void sdiffstoreCommand(redisClient *c) {
	sunionDiffGenericCommand(c, c->argv + 2, c->argc - 2, c->argv[1], REDIS_OP_DIFF);
}

/* ==================================== ZSets =============================== */

/* ZSETs are ordered sets using two data structures to hold the same elements
 * in order to get O(log(N)) INSERT and REMOVE operations into a sorted
 * data structure.
 *
 * The elements are added to an hash table mapping Redis objects to scores.
 * At the same time the elements are added to a skip list mapping scores
 * to Redis objects (so objects are sorted by scores in this "view"). */

/* This skiplist implementation is almost a C translation of the original
 * algorithm described by William Pugh in "Skip Lists: A Probabilistic
 * Alternative to Balanced Trees", modified in three ways:
 * a) this implementation allows for repeated values.
 * b) the comparison is not just by key (our 'score') but by satellite data.
 * c) there is a back pointer, so it's a doubly linked list with the back
 * pointers being only at "level 1". This allows to traverse the list
 * from tail to head, useful for ZREVRANGE. */

static zskiplistNode *zslCreateNode(int level, double score, robj *obj) {
	zskiplistNode *zn = zmalloc(sizeof(*zn));

	zn->forward = zmalloc(sizeof(zskiplistNode*) * level);
	zn->score = score;
	zn->obj = obj;
	return zn;
}

// 创建跳跃表Skiplist
static zskiplist *zslCreate(void) {
	int j;
	zskiplist *zsl;

	zsl = zmalloc(sizeof(*zsl));
	zsl->level = 1;
	zsl->length = 0;
	// 头结点初始化创建MAX_LEVEL层，用于保存后续每一层的开始指针
	zsl->header = zslCreateNode(ZSKIPLIST_MAXLEVEL, 0, NULL);
	for (j = 0; j < ZSKIPLIST_MAXLEVEL; j++)
		zsl->header->forward[j] = NULL;
	zsl->header->backward = NULL;
	zsl->tail = NULL;
	return zsl;
}

// 释放节点
static void zslFreeNode(zskiplistNode *node) {
	decrRefCount(node->obj);
	zfree(node->forward);
	zfree(node);
}

// 释放Skiplist
static void zslFree(zskiplist *zsl) {
	zskiplistNode *node = zsl->header->forward[0], *next;

	zfree(zsl->header->forward);
	zfree(zsl->header);
	while (node) {
		// 和正常List一样，从底层逐个释放
		next = node->forward[0];
		zslFreeNode(node);
		node = next;
	}
	zfree(zsl);
}

// 随机层数，概率为1/4(空间复杂度为1-(1-p)约为1.33)
static int zslRandomLevel(void) {
	int level = 1;
	while ((random() & 0xFFFF) < (ZSKIPLIST_P * 0xFFFF))
		level += 1;
	return level;
}

// 插入节点
static void zslInsert(zskiplist *zsl, double score, robj *obj) {
	zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
	int i, level;

	x = zsl->header;
	// 查找每一层比score小的节点（即在所有比score小的节点中分数最大的）（查找每一层相邻的节点）
	// 插入的时候，如果要更新，则只需要更新每一层相邻的节点
	// 同时也是查找需要插入的位置
	for (i = zsl->level - 1; i >= 0; i--) {
		while (x->forward[i] &&
		        (x->forward[i]->score < score ||
		         (x->forward[i]->score == score &&
		          compareStringObjects(x->forward[i]->obj, obj) < 0)))
			x = x->forward[i];
		update[i] = x;
	}
	/* we assume the key is not already inside, since we allow duplicated
	 * scores, and the re-insertion of score and redis object should never
	 * happpen since the caller of zslInsert() should test in the hash table
	 * if the element is already inside or not. */
	level = zslRandomLevel();
	// 比当前最高层级大，要更新头节点信息
	if (level > zsl->level) {
		for (i = zsl->level; i < level; i++)
			update[i] = zsl->header;
		zsl->level = level;
	}
	// 创建节点
	x = zslCreateNode(level, score, obj);
	for (i = 0; i < level; i++) {
		// 保存所有相邻左边节点的forward节点指针（指向下一个节点）
		x->forward[i] = update[i]->forward[i];
		// 更新相邻左边节点的forward指针，指向当前x节点
		update[i]->forward[i] = x;
	}
	// 如果不是首节点，则更新backward回退指针
	x->backward = (update[0] == zsl->header) ? NULL : update[0];
	// 如果不是最后一个节点，则更新下一个节点的backward，指向当前节点
	if (x->forward[0])
		x->forward[0]->backward = x;
	else
		// 否则更新尾节点指针
		zsl->tail = x;
	// 链表长度+1
	zsl->length++;
}

/* Delete an element with matching score/object from the skiplist. */
// 删除指定节点，和插入类似，先查找相邻的节点，再更新节点信息和删除节点
static int zslDelete(zskiplist *zsl, double score, robj *obj) {
	zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
	int i;

	x = zsl->header;
	// 查找每一层相邻的节点
	for (i = zsl->level - 1; i >= 0; i--) {
		while (x->forward[i] &&
		        (x->forward[i]->score < score ||
		         (x->forward[i]->score == score &&
		          compareStringObjects(x->forward[i]->obj, obj) < 0)))
			x = x->forward[i];
		update[i] = x;
	}
	/* We may have multiple elements with the same score, what we need
	 * is to find the element with both the right score and object. */
	x = x->forward[0];
	// 判断x是否为需要删除的节点
	if (x && score == x->score && compareStringObjects(x->obj, obj) == 0) {
		// 更新所涉及的相邻节点信息（更新指向x的节点forward指针，使其指向x的后续节点）
		for (i = 0; i < zsl->level; i++) {
			// 直到x的最高层
			if (update[i]->forward[i] != x) break;
			update[i]->forward[i] = x->forward[i];
		}
		if (x->forward[0]) {
			// 有后续节点
			x->forward[0]->backward = (x->backward == zsl->header) ?
			                          NULL : x->backward;
		} else {
			// 尾节点
			zsl->tail = x->backward;
		}
		// 删除x节点
		zslFreeNode(x);
		// 删除的节点有可能是最高层的，所以需要处理层级降低的情况
		while (zsl->level > 1 && zsl->header->forward[zsl->level - 1] == NULL)
			zsl->level--;
		zsl->length--;
		return 1;
	} else {
		return 0; /* not found */
	}
	return 0; /* not found */
}

/* Delete all the elements with score between min and max from the skiplist.
 * Min and mx are inclusive, so a score >= min || score <= max is deleted.
 * Note that this function takes the reference to the hash table view of the
 * sorted set, in order to remove the elements from the hash table too. */
// 删除所有分数在min和max之间的节点
static unsigned long zslDeleteRange(zskiplist *zsl, double min, double max, dict *dict) {
	zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
	unsigned long removed = 0;
	int i;

	// 先查找出大于min的前一个节点
	x = zsl->header;
	for (i = zsl->level - 1; i >= 0; i--) {
		while (x->forward[i] && x->forward[i]->score < min)
			x = x->forward[i];
		update[i] = x;
	}
	/* We may have multiple elements with the same score, what we need
	 * is to find the element with both the right score and object. */
	x = x->forward[0];
	while (x && x->score <= max) {
		// 从大于min开始的节点，逐个删除小于max的节点
		zskiplistNode *next;

		// 和zslDelete删除单个节点类似
		for (i = 0; i < zsl->level; i++) {
			if (update[i]->forward[i] != x) break;
			update[i]->forward[i] = x->forward[i];
		}
		if (x->forward[0]) {
			x->forward[0]->backward = (x->backward == zsl->header) ?
			                          NULL : x->backward;
		} else {
			zsl->tail = x->backward;
		}
		next = x->forward[0];
		// 将对象从dict中删除（ZSet在dict中保存了对象到分数的映射）
		dictDelete(dict, x->obj);
		zslFreeNode(x);
		// 更新层级
		while (zsl->level > 1 && zsl->header->forward[zsl->level - 1] == NULL)
			zsl->level--;
		zsl->length--;
		removed++;
		x = next;
	}
	// 返回删除的元素个数
	return removed; /* not found */
}

/* Find the first node having a score equal or greater than the specified one.
 * Returns NULL if there is no match. */
// 查找第一个>=score的节点
static zskiplistNode *zslFirstWithScore(zskiplist *zsl, double score) {
	zskiplistNode *x;
	int i;

	x = zsl->header;
	for (i = zsl->level - 1; i >= 0; i--) {
		while (x->forward[i] && x->forward[i]->score < score)
			x = x->forward[i];
	}
	/* We may have multiple elements with the same score, what we need
	 * is to find the element with both the right score and object. */
	return x->forward[0];
}

/* The actual Z-commands implementations */

/* This generic command implements both ZADD and ZINCRBY.
 * scoreval is the score if the operation is a ZADD (doincrement == 0) or
 * the increment if the operation is a ZINCRBY (doincrement == 1). */
// 同时实现了ZADD和ZINCRBY命令
// 如果doincrement为0，则为ZADD，否则为ZINCRBY
static void zaddGenericCommand(redisClient *c, robj *key, robj *ele, double scoreval, int doincrement) {
	robj *zsetobj;
	zset *zs;
	double *score;

	// 先查找ZSet对象
	zsetobj = lookupKeyWrite(c->db, key);
	if (zsetobj == NULL) {
		// 不存在，则创建
		zsetobj = createZsetObject();
		dictAdd(c->db->dict, key, zsetobj);
		// 这里通过增加引用，就不用再次创建Key了
		incrRefCount(key);
	} else {
		if (zsetobj->type != REDIS_ZSET) {
			addReply(c, shared.wrongtypeerr);
			return;
		}
	}
	zs = zsetobj->ptr;

	/* Ok now since we implement both ZADD and ZINCRBY here the code
	 * needs to handle the two different conditions. It's all about setting
	 * '*score', that is, the new score to set, to the right value. */
	score = zmalloc(sizeof(double));
	if (doincrement) {
		dictEntry *de;

		// 执行ZINCRBY
		/* Read the old score. If the element was not present starts from 0 */
		de = dictFind(zs->dict, ele);
		if (de) {
			// 已有对应的元素，加上原来的值
			double *oldscore = dictGetEntryVal(de);
			*score = *oldscore + scoreval;
		} else {
			// 不存在，则和ZADD一样
			*score = scoreval;
		}
	} else {
		*score = scoreval;
	}

	/* What follows is a simple remove and re-insert operation that is common
	 * to both ZADD and ZINCRBY... */
	// 尝试添加到字典中，如果成功，则表示新的元素，否则已存在了
	if (dictAdd(zs->dict, ele, score) == DICT_OK) {
		/* case 1: New element */
		// 新的元素
		// 通过增加对象引用，避免对象重复创建
		incrRefCount(ele); /* added to hash */
		zslInsert(zs->zsl, *score, ele);
		incrRefCount(ele); /* added to skiplist */
		server.dirty++;
		if (doincrement)
			// ZINCRBY，返回最新的值
			addReplyDouble(c, *score);
		else
			// ZADD，返回1表示添加成功
			addReply(c, shared.cone);
	} else {
		// 添加失败，表示已经存在元素
		dictEntry *de;
		double *oldscore;

		/* case 2: Score update operation */
		de = dictFind(zs->dict, ele);
		redisAssert(de != NULL);
		oldscore = dictGetEntryVal(de);
		if (*score != *oldscore) {
			// 如果两个分数不一样，则需要先将旧的删除，再重新插入
			int deleted;

			/* Remove and insert the element in the skip list with new score */
			deleted = zslDelete(zs->zsl, *oldscore, ele);
			redisAssert(deleted != 0);
			zslInsert(zs->zsl, *score, ele);
			incrRefCount(ele);
			/* Update the score in the hash table */
			dictReplace(zs->dict, ele, score);
			server.dirty++;
		} else {
			zfree(score);
		}
		if (doincrement)
			addReplyDouble(c, *score);
		else
			addReply(c, shared.czero);
	}
}

// ZADD命令
static void zaddCommand(redisClient *c) {
	double scoreval;

	scoreval = strtod(c->argv[2]->ptr, NULL);
	zaddGenericCommand(c, c->argv[1], c->argv[3], scoreval, 0);
}

// ZINCRBY命令
static void zincrbyCommand(redisClient *c) {
	double scoreval;

	scoreval = strtod(c->argv[2]->ptr, NULL);
	zaddGenericCommand(c, c->argv[1], c->argv[3], scoreval, 1);
}

// ZREM命令，从集合中删除元素
static void zremCommand(redisClient *c) {
	robj *zsetobj;
	zset *zs;

	zsetobj = lookupKeyWrite(c->db, c->argv[1]);
	if (zsetobj == NULL) {
		addReply(c, shared.czero);
	} else {
		dictEntry *de;
		double *oldscore;
		int deleted;

		if (zsetobj->type != REDIS_ZSET) {
			addReply(c, shared.wrongtypeerr);
			return;
		}
		zs = zsetobj->ptr;
		de = dictFind(zs->dict, c->argv[2]);
		if (de == NULL) {
			addReply(c, shared.czero);
			return;
		}
		// 先从Skiplist中删除对应的节点，再从字典中删除
		/* Delete from the skiplist */
		oldscore = dictGetEntryVal(de);
		deleted = zslDelete(zs->zsl, *oldscore, c->argv[2]);
		redisAssert(deleted != 0);

		/* Delete from the hash table */
		dictDelete(zs->dict, c->argv[2]);
		if (htNeedsResize(zs->dict)) dictResize(zs->dict);
		server.dirty++;
		addReply(c, shared.cone);
	}
}

// 根据指定的分数范围来删除元素，调用zslDeleteRange来实现
static void zremrangebyscoreCommand(redisClient *c) {
	double min = strtod(c->argv[2]->ptr, NULL);
	double max = strtod(c->argv[3]->ptr, NULL);
	robj *zsetobj;
	zset *zs;

	zsetobj = lookupKeyWrite(c->db, c->argv[1]);
	if (zsetobj == NULL) {
		addReply(c, shared.czero);
	} else {
		long deleted;

		if (zsetobj->type != REDIS_ZSET) {
			addReply(c, shared.wrongtypeerr);
			return;
		}
		zs = zsetobj->ptr;
		deleted = zslDeleteRange(zs->zsl, min, max, zs->dict);
		if (htNeedsResize(zs->dict)) dictResize(zs->dict);
		server.dirty += deleted;
		addReplySds(c, sdscatprintf(sdsempty(), ":%lu\r\n", deleted));
	}
}

// 查找指定排名区间的元素（和普通的双向链表实现类似）
static void zrangeGenericCommand(redisClient *c, int reverse) {
	robj *o;
	int start = atoi(c->argv[2]->ptr);
	int end = atoi(c->argv[3]->ptr);
	int withscores = 0;

	if (c->argc == 5 && !strcasecmp(c->argv[4]->ptr, "withscores")) {
		withscores = 1;
	} else if (c->argc >= 5) {
		addReply(c, shared.syntaxerr);
		return;
	}

	o = lookupKeyRead(c->db, c->argv[1]);
	if (o == NULL) {
		addReply(c, shared.nullmultibulk);
	} else {
		if (o->type != REDIS_ZSET) {
			addReply(c, shared.wrongtypeerr);
		} else {
			zset *zsetobj = o->ptr;
			zskiplist *zsl = zsetobj->zsl;
			zskiplistNode *ln;

			int llen = zsl->length;
			int rangelen, j;
			robj *ele;

			/* convert negative indexes */
			if (start < 0) start = llen + start;
			if (end < 0) end = llen + end;
			if (start < 0) start = 0;
			if (end < 0) end = 0;

			/* indexes sanity checks */
			if (start > end || start >= llen) {
				/* Out of range start or start > end result in empty list */
				addReply(c, shared.emptymultibulk);
				return;
			}
			if (end >= llen) end = llen - 1;
			rangelen = (end - start) + 1;

			/* Return the result in form of a multi-bulk reply */
			if (reverse) {
				ln = zsl->tail;
				while (start--)
					ln = ln->backward;
			} else {
				ln = zsl->header->forward[0];
				while (start--)
					ln = ln->forward[0];
			}

			addReplySds(c, sdscatprintf(sdsempty(), "*%d\r\n",
			                            withscores ? (rangelen * 2) : rangelen));
			for (j = 0; j < rangelen; j++) {
				ele = ln->obj;
				addReplyBulkLen(c, ele);
				addReply(c, ele);
				addReply(c, shared.crlf);
				if (withscores)
					addReplyDouble(c, ln->score);
				ln = reverse ? ln->backward : ln->forward[0];
			}
		}
	}
}

static void zrangeCommand(redisClient *c) {
	zrangeGenericCommand(c, 0);
}

static void zrevrangeCommand(redisClient *c) {
	zrangeGenericCommand(c, 1);
}

// 根据指定分数范围来获取元素
static void zrangebyscoreCommand(redisClient *c) {
	robj *o;
	double min = strtod(c->argv[2]->ptr, NULL);
	double max = strtod(c->argv[3]->ptr, NULL);
	int offset = 0, limit = -1;

	if (c->argc != 4 && c->argc != 7) {
		addReplySds(c,
		            sdsnew("-ERR wrong number of arguments for ZRANGEBYSCORE\r\n"));
		return;
	} else if (c->argc == 7 && strcasecmp(c->argv[4]->ptr, "limit")) {
		addReply(c, shared.syntaxerr);
		return;
	} else if (c->argc == 7) {
		offset = atoi(c->argv[5]->ptr);
		limit = atoi(c->argv[6]->ptr);
		if (offset < 0) offset = 0;
	}

	o = lookupKeyRead(c->db, c->argv[1]);
	if (o == NULL) {
		addReply(c, shared.nullmultibulk);
	} else {
		if (o->type != REDIS_ZSET) {
			addReply(c, shared.wrongtypeerr);
		} else {
			// 执行查找
			zset *zsetobj = o->ptr;
			zskiplist *zsl = zsetobj->zsl;
			zskiplistNode *ln;
			robj *ele, *lenobj;
			unsigned int rangelen = 0;

			/* Get the first node with the score >= min */
			// 先找到第一个比min大的元素（logN）
			ln = zslFirstWithScore(zsl, min);
			if (ln == NULL) {
				/* No element matching the speciifed interval */
				addReply(c, shared.emptymultibulk);
				return;
			}

			/* We don't know in advance how many matching elements there
			 * are in the list, so we push this object that will represent
			 * the multi-bulk length in the output buffer, and will "fix"
			 * it later */
			lenobj = createObject(REDIS_STRING, NULL);
			addReply(c, lenobj);
			decrRefCount(lenobj);

			while (ln && ln->score <= max) {
				// 而后逐个遍历（总时间复杂度logN+M）
				if (offset) {
					// 偏移个数
					offset--;
					ln = ln->forward[0];
					continue;
				}
				if (limit == 0) break;
				ele = ln->obj;
				addReplyBulkLen(c, ele);
				addReply(c, ele);
				addReply(c, shared.crlf);
				ln = ln->forward[0];
				rangelen++;
				// limit为需要返回的个数
				if (limit > 0) limit--;
			}
			lenobj->ptr = sdscatprintf(sdsempty(), "*%d\r\n", rangelen);
		}
	}
}

// 获取ZSet集合元素个数
static void zcardCommand(redisClient *c) {
	robj *o;
	zset *zs;

	o = lookupKeyRead(c->db, c->argv[1]);
	if (o == NULL) {
		addReply(c, shared.czero);
		return;
	} else {
		if (o->type != REDIS_ZSET) {
			addReply(c, shared.wrongtypeerr);
		} else {
			zs = o->ptr;
			addReplySds(c, sdscatprintf(sdsempty(), ":%lu\r\n", zs->zsl->length));
		}
	}
}

// 获取指定元素的分数
static void zscoreCommand(redisClient *c) {
	robj *o;
	zset *zs;

	o = lookupKeyRead(c->db, c->argv[1]);
	if (o == NULL) {
		addReply(c, shared.nullbulk);
		return;
	} else {
		if (o->type != REDIS_ZSET) {
			addReply(c, shared.wrongtypeerr);
		} else {
			dictEntry *de;

			zs = o->ptr;
			// 直接在dict中根据对象查找
			de = dictFind(zs->dict, c->argv[2]);
			if (!de) {
				addReply(c, shared.nullbulk);
			} else {
				double *score = dictGetEntryVal(de);

				addReplyDouble(c, *score);
			}
		}
	}
}

/* ========================= Non type-specific commands  ==================== */

// 清空当前Redis内存的所有数据
static void flushdbCommand(redisClient *c) {
	server.dirty += dictSize(c->db->dict);
	dictEmpty(c->db->dict);
	dictEmpty(c->db->expires);
	addReply(c, shared.ok);
}

// 清空Redis内存的所有数据，包括RDB持久化的数据
static void flushallCommand(redisClient *c) {
	server.dirty += emptyDb();
	addReply(c, shared.ok);
	rdbSave(server.dbfilename);
	server.dirty++;
}

static redisSortOperation *createSortOperation(int type, robj *pattern) {
	redisSortOperation *so = zmalloc(sizeof(*so));
	so->type = type;
	so->pattern = pattern;
	return so;
}

/* Return the value associated to the key with a name obtained
 * substituting the first occurence of '*' in 'pattern' with 'subst' */
static robj *lookupKeyByPattern(redisDb *db, robj *pattern, robj *subst) {
	char *p;
	sds spat, ssub;
	robj keyobj;
	int prefixlen, sublen, postfixlen;
	/* Expoit the internal sds representation to create a sds string allocated on the stack in order to make this function faster */
	struct {
		long len;
		long free;
		char buf[REDIS_SORTKEY_MAX + 1];
	} keyname;

	/* If the pattern is "#" return the substitution object itself in order
	 * to implement the "SORT ... GET #" feature. */
	spat = pattern->ptr;
	if (spat[0] == '#' && spat[1] == '\0') {
		return subst;
	}

	/* The substitution object may be specially encoded. If so we create
	 * a decoded object on the fly. Otherwise getDecodedObject will just
	 * increment the ref count, that we'll decrement later. */
	subst = getDecodedObject(subst);

	ssub = subst->ptr;
	if (sdslen(spat) + sdslen(ssub) - 1 > REDIS_SORTKEY_MAX) return NULL;
	p = strchr(spat, '*');
	if (!p) {
		decrRefCount(subst);
		return NULL;
	}

	prefixlen = p - spat;
	sublen = sdslen(ssub);
	postfixlen = sdslen(spat) - (prefixlen + 1);
	memcpy(keyname.buf, spat, prefixlen);
	memcpy(keyname.buf + prefixlen, ssub, sublen);
	memcpy(keyname.buf + prefixlen + sublen, p + 1, postfixlen);
	keyname.buf[prefixlen + sublen + postfixlen] = '\0';
	keyname.len = prefixlen + sublen + postfixlen;

	initStaticStringObject(keyobj, ((char*)&keyname) + (sizeof(long) * 2))
	decrRefCount(subst);

	/* printf("lookup '%s' => %p\n", keyname.buf,de); */
	return lookupKeyRead(db, &keyobj);
}

/* sortCompare() is used by qsort in sortCommand(). Given that qsort_r with
 * the additional parameter is not standard but a BSD-specific we have to
 * pass sorting parameters via the global 'server' structure */
static int sortCompare(const void *s1, const void *s2) {
	const redisSortObject *so1 = s1, *so2 = s2;
	int cmp;

	if (!server.sort_alpha) {
		/* Numeric sorting. Here it's trivial as we precomputed scores */
		if (so1->u.score > so2->u.score) {
			cmp = 1;
		} else if (so1->u.score < so2->u.score) {
			cmp = -1;
		} else {
			cmp = 0;
		}
	} else {
		/* Alphanumeric sorting */
		if (server.sort_bypattern) {
			if (!so1->u.cmpobj || !so2->u.cmpobj) {
				/* At least one compare object is NULL */
				if (so1->u.cmpobj == so2->u.cmpobj)
					cmp = 0;
				else if (so1->u.cmpobj == NULL)
					cmp = -1;
				else
					cmp = 1;
			} else {
				/* We have both the objects, use strcoll */
				cmp = strcoll(so1->u.cmpobj->ptr, so2->u.cmpobj->ptr);
			}
		} else {
			/* Compare elements directly */
			robj *dec1, *dec2;

			dec1 = getDecodedObject(so1->obj);
			dec2 = getDecodedObject(so2->obj);
			cmp = strcoll(dec1->ptr, dec2->ptr);
			decrRefCount(dec1);
			decrRefCount(dec2);
		}
	}
	return server.sort_desc ? -cmp : cmp;
}

/* The SORT command is the most complex command in Redis. Warning: this code
 * is optimized for speed and a bit less for readability */
static void sortCommand(redisClient *c) {
	list *operations;
	int outputlen = 0;
	int desc = 0, alpha = 0;
	int limit_start = 0, limit_count = -1, start, end;
	int j, dontsort = 0, vectorlen;
	int getop = 0; /* GET operation counter */
	robj *sortval, *sortby = NULL, *storekey = NULL;
	redisSortObject *vector; /* Resulting vector to sort */

	/* Lookup the key to sort. It must be of the right types */
	sortval = lookupKeyRead(c->db, c->argv[1]);
	if (sortval == NULL) {
		addReply(c, shared.nullmultibulk);
		return;
	}
	if (sortval->type != REDIS_SET && sortval->type != REDIS_LIST &&
	        sortval->type != REDIS_ZSET)
	{
		addReply(c, shared.wrongtypeerr);
		return;
	}

	/* Create a list of operations to perform for every sorted element.
	 * Operations can be GET/DEL/INCR/DECR */
	operations = listCreate();
	listSetFreeMethod(operations, zfree);
	j = 2;

	/* Now we need to protect sortval incrementing its count, in the future
	 * SORT may have options able to overwrite/delete keys during the sorting
	 * and the sorted key itself may get destroied */
	incrRefCount(sortval);

	/* The SORT command has an SQL-alike syntax, parse it */
	while (j < c->argc) {
		int leftargs = c->argc - j - 1;
		if (!strcasecmp(c->argv[j]->ptr, "asc")) {
			desc = 0;
		} else if (!strcasecmp(c->argv[j]->ptr, "desc")) {
			desc = 1;
		} else if (!strcasecmp(c->argv[j]->ptr, "alpha")) {
			alpha = 1;
		} else if (!strcasecmp(c->argv[j]->ptr, "limit") && leftargs >= 2) {
			limit_start = atoi(c->argv[j + 1]->ptr);
			limit_count = atoi(c->argv[j + 2]->ptr);
			j += 2;
		} else if (!strcasecmp(c->argv[j]->ptr, "store") && leftargs >= 1) {
			storekey = c->argv[j + 1];
			j++;
		} else if (!strcasecmp(c->argv[j]->ptr, "by") && leftargs >= 1) {
			sortby = c->argv[j + 1];
			/* If the BY pattern does not contain '*', i.e. it is constant,
			 * we don't need to sort nor to lookup the weight keys. */
			if (strchr(c->argv[j + 1]->ptr, '*') == NULL) dontsort = 1;
			j++;
		} else if (!strcasecmp(c->argv[j]->ptr, "get") && leftargs >= 1) {
			listAddNodeTail(operations, createSortOperation(
			                    REDIS_SORT_GET, c->argv[j + 1]));
			getop++;
			j++;
		} else {
			decrRefCount(sortval);
			listRelease(operations);
			addReply(c, shared.syntaxerr);
			return;
		}
		j++;
	}

	/* Load the sorting vector with all the objects to sort */
	switch (sortval->type) {
	case REDIS_LIST: vectorlen = listLength((list*)sortval->ptr); break;
	case REDIS_SET: vectorlen =  dictSize((dict*)sortval->ptr); break;
	case REDIS_ZSET: vectorlen = dictSize(((zset*)sortval->ptr)->dict); break;
	default: vectorlen = 0; redisAssert(0); /* Avoid GCC warning */
	}
	vector = zmalloc(sizeof(redisSortObject) * vectorlen);
	j = 0;

	if (sortval->type == REDIS_LIST) {
		list *list = sortval->ptr;
		listNode *ln;
		listIter li;

		listRewind(list, &li);
		while ((ln = listNext(&li))) {
			robj *ele = ln->value;
			vector[j].obj = ele;
			vector[j].u.score = 0;
			vector[j].u.cmpobj = NULL;
			j++;
		}
	} else {
		dict *set;
		dictIterator *di;
		dictEntry *setele;

		if (sortval->type == REDIS_SET) {
			set = sortval->ptr;
		} else {
			zset *zs = sortval->ptr;
			set = zs->dict;
		}

		di = dictGetIterator(set);
		while ((setele = dictNext(di)) != NULL) {
			vector[j].obj = dictGetEntryKey(setele);
			vector[j].u.score = 0;
			vector[j].u.cmpobj = NULL;
			j++;
		}
		dictReleaseIterator(di);
	}
	redisAssert(j == vectorlen);

	/* Now it's time to load the right scores in the sorting vector */
	if (dontsort == 0) {
		for (j = 0; j < vectorlen; j++) {
			if (sortby) {
				robj *byval;

				byval = lookupKeyByPattern(c->db, sortby, vector[j].obj);
				if (!byval || byval->type != REDIS_STRING) continue;
				if (alpha) {
					vector[j].u.cmpobj = getDecodedObject(byval);
				} else {
					if (byval->encoding == REDIS_ENCODING_RAW) {
						vector[j].u.score = strtod(byval->ptr, NULL);
					} else {
						/* Don't need to decode the object if it's
						 * integer-encoded (the only encoding supported) so
						 * far. We can just cast it */
						if (byval->encoding == REDIS_ENCODING_INT) {
							vector[j].u.score = (long)byval->ptr;
						} else
							redisAssert(1 != 1);
					}
				}
			} else {
				if (!alpha) {
					if (vector[j].obj->encoding == REDIS_ENCODING_RAW)
						vector[j].u.score = strtod(vector[j].obj->ptr, NULL);
					else {
						if (vector[j].obj->encoding == REDIS_ENCODING_INT)
							vector[j].u.score = (long) vector[j].obj->ptr;
						else
							redisAssert(1 != 1);
					}
				}
			}
		}
	}

	/* We are ready to sort the vector... perform a bit of sanity check
	 * on the LIMIT option too. We'll use a partial version of quicksort. */
	start = (limit_start < 0) ? 0 : limit_start;
	end = (limit_count < 0) ? vectorlen - 1 : start + limit_count - 1;
	if (start >= vectorlen) {
		start = vectorlen - 1;
		end = vectorlen - 2;
	}
	if (end >= vectorlen) end = vectorlen - 1;

	if (dontsort == 0) {
		server.sort_desc = desc;
		server.sort_alpha = alpha;
		server.sort_bypattern = sortby ? 1 : 0;
		if (sortby && (start != 0 || end != vectorlen - 1))
			pqsort(vector, vectorlen, sizeof(redisSortObject), sortCompare, start, end);
		else
			qsort(vector, vectorlen, sizeof(redisSortObject), sortCompare);
	}

	/* Send command output to the output buffer, performing the specified
	 * GET/DEL/INCR/DECR operations if any. */
	outputlen = getop ? getop * (end - start + 1) : end - start + 1;
	if (storekey == NULL) {
		/* STORE option not specified, sent the sorting result to client */
		addReplySds(c, sdscatprintf(sdsempty(), "*%d\r\n", outputlen));
		for (j = start; j <= end; j++) {
			listNode *ln;
			listIter li;

			if (!getop) {
				addReplyBulkLen(c, vector[j].obj);
				addReply(c, vector[j].obj);
				addReply(c, shared.crlf);
			}
			listRewind(operations, &li);
			while ((ln = listNext(&li))) {
				redisSortOperation *sop = ln->value;
				robj *val = lookupKeyByPattern(c->db, sop->pattern,
				                               vector[j].obj);

				if (sop->type == REDIS_SORT_GET) {
					if (!val || val->type != REDIS_STRING) {
						addReply(c, shared.nullbulk);
					} else {
						addReplyBulkLen(c, val);
						addReply(c, val);
						addReply(c, shared.crlf);
					}
				} else {
					redisAssert(sop->type == REDIS_SORT_GET); /* always fails */
				}
			}
		}
	} else {
		robj *listObject = createListObject();
		list *listPtr = (list*) listObject->ptr;

		/* STORE option specified, set the sorting result as a List object */
		for (j = start; j <= end; j++) {
			listNode *ln;
			listIter li;

			if (!getop) {
				listAddNodeTail(listPtr, vector[j].obj);
				incrRefCount(vector[j].obj);
			}
			listRewind(operations, &li);
			while ((ln = listNext(&li))) {
				redisSortOperation *sop = ln->value;
				robj *val = lookupKeyByPattern(c->db, sop->pattern,
				                               vector[j].obj);

				if (sop->type == REDIS_SORT_GET) {
					if (!val || val->type != REDIS_STRING) {
						listAddNodeTail(listPtr, createStringObject("", 0));
					} else {
						listAddNodeTail(listPtr, val);
						incrRefCount(val);
					}
				} else {
					redisAssert(sop->type == REDIS_SORT_GET); /* always fails */
				}
			}
		}
		if (dictReplace(c->db->dict, storekey, listObject)) {
			incrRefCount(storekey);
		}
		/* Note: we add 1 because the DB is dirty anyway since even if the
		 * SORT result is empty a new key is set and maybe the old content
		 * replaced. */
		server.dirty += 1 + outputlen;
		addReplySds(c, sdscatprintf(sdsempty(), ":%d\r\n", outputlen));
	}

	/* Cleanup */
	decrRefCount(sortval);
	listRelease(operations);
	for (j = 0; j < vectorlen; j++) {
		if (sortby && alpha && vector[j].u.cmpobj)
			decrRefCount(vector[j].u.cmpobj);
	}
	zfree(vector);
}

/* Convert an amount of bytes into a human readable string in the form
 * of 100B, 2G, 100M, 4K, and so forth. */
static void bytesToHuman(char *s, unsigned long long n) {
	double d;

	if (n < 1024) {
		/* Bytes */
		sprintf(s, "%lluB", n);
		return;
	} else if (n < (1024 * 1024)) {
		d = (double)n / (1024);
		sprintf(s, "%.2fK", d);
	} else if (n < (1024LL * 1024 * 1024)) {
		d = (double)n / (1024 * 1024);
		sprintf(s, "%.2fM", d);
	} else if (n < (1024LL * 1024 * 1024 * 1024)) {
		d = (double)n / (1024LL * 1024 * 1024);
		sprintf(s, "%.2fG", d);
	}
}

/* Create the string returned by the INFO command. This is decoupled
 * by the INFO command itself as we need to report the same information
 * on memory corruption problems. */
static sds genRedisInfoString(void) {
	sds info;
	time_t uptime = time(NULL) - server.stat_starttime;
	int j;
	char hmem[64];

	bytesToHuman(hmem, zmalloc_used_memory());
	info = sdscatprintf(sdsempty(),
	                    "redis_version:%s\r\n"
	                    "arch_bits:%s\r\n"
	                    "multiplexing_api:%s\r\n"
	                    "process_id:%ld\r\n"
	                    "uptime_in_seconds:%ld\r\n"
	                    "uptime_in_days:%ld\r\n"
	                    "connected_clients:%d\r\n"
	                    "connected_slaves:%d\r\n"
	                    "blocked_clients:%d\r\n"
	                    "used_memory:%zu\r\n"
	                    "used_memory_human:%s\r\n"
	                    "changes_since_last_save:%lld\r\n"
	                    "bgsave_in_progress:%d\r\n"
	                    "last_save_time:%ld\r\n"
	                    "bgrewriteaof_in_progress:%d\r\n"
	                    "total_connections_received:%lld\r\n"
	                    "total_commands_processed:%lld\r\n"
	                    "vm_enabled:%d\r\n"
	                    "role:%s\r\n"
	                    , REDIS_VERSION,
	                    (sizeof(long) == 8) ? "64" : "32",
	                    aeGetApiName(),
	                    (long) getpid(),
	                    uptime,
	                    uptime / (3600 * 24),
	                    listLength(server.clients) - listLength(server.slaves),
	                    listLength(server.slaves),
	                    server.blockedclients,
	                    zmalloc_used_memory(),
	                    hmem,
	                    server.dirty,
	                    server.bgsavechildpid != -1,
	                    server.lastsave,
	                    server.bgrewritechildpid != -1,
	                    server.stat_numconnections,
	                    server.stat_numcommands,
	                    server.vm_enabled != 0,
	                    server.masterhost == NULL ? "master" : "slave"
	                   );
	if (server.masterhost) {
		info = sdscatprintf(info,
		                    "master_host:%s\r\n"
		                    "master_port:%d\r\n"
		                    "master_link_status:%s\r\n"
		                    "master_last_io_seconds_ago:%d\r\n"
		                    , server.masterhost,
		                    server.masterport,
		                    (server.replstate == REDIS_REPL_CONNECTED) ?
		                    "up" : "down",
		                    server.master ? ((int)(time(NULL) - server.master->lastinteraction)) : -1
		                   );
	}
	if (server.vm_enabled) {
		lockThreadedIO();
		info = sdscatprintf(info,
		                    "vm_conf_max_memory:%llu\r\n"
		                    "vm_conf_page_size:%llu\r\n"
		                    "vm_conf_pages:%llu\r\n"
		                    "vm_stats_used_pages:%llu\r\n"
		                    "vm_stats_swapped_objects:%llu\r\n"
		                    "vm_stats_swappin_count:%llu\r\n"
		                    "vm_stats_swappout_count:%llu\r\n"
		                    "vm_stats_io_newjobs_len:%lu\r\n"
		                    "vm_stats_io_processing_len:%lu\r\n"
		                    "vm_stats_io_processed_len:%lu\r\n"
		                    "vm_stats_io_waiting_clients:%lu\r\n"
		                    "vm_stats_io_active_threads:%lu\r\n"
		                    , (unsigned long long) server.vm_max_memory,
		                    (unsigned long long) server.vm_page_size,
		                    (unsigned long long) server.vm_pages,
		                    (unsigned long long) server.vm_stats_used_pages,
		                    (unsigned long long) server.vm_stats_swapped_objects,
		                    (unsigned long long) server.vm_stats_swapins,
		                    (unsigned long long) server.vm_stats_swapouts,
		                    (unsigned long) listLength(server.io_newjobs),
		                    (unsigned long) listLength(server.io_processing),
		                    (unsigned long) listLength(server.io_processed),
		                    (unsigned long) listLength(server.io_clients),
		                    (unsigned long) server.io_active_threads
		                   );
		unlockThreadedIO();
	}
	for (j = 0; j < server.dbnum; j++) {
		long long keys, vkeys;

		keys = dictSize(server.db[j].dict);
		vkeys = dictSize(server.db[j].expires);
		if (keys || vkeys) {
			info = sdscatprintf(info, "db%d:keys=%lld,expires=%lld\r\n",
			                    j, keys, vkeys);
		}
	}
	return info;
}

static void infoCommand(redisClient *c) {
	sds info = genRedisInfoString();
	addReplySds(c, sdscatprintf(sdsempty(), "$%lu\r\n",
	                            (unsigned long)sdslen(info)));
	addReplySds(c, info);
	addReply(c, shared.crlf);
}

static void monitorCommand(redisClient *c) {
	/* ignore MONITOR if aleady slave or in monitor mode */
	if (c->flags & REDIS_SLAVE) return;

	c->flags |= (REDIS_SLAVE | REDIS_MONITOR);
	c->slaveseldb = 0;
	listAddNodeTail(server.monitors, c);
	addReply(c, shared.ok);
}

/* ================================= Expire ================================= */
static int removeExpire(redisDb *db, robj *key) {
	if (dictDelete(db->expires, key) == DICT_OK) {
		return 1;
	} else {
		return 0;
	}
}

static int setExpire(redisDb *db, robj *key, time_t when) {
	if (dictAdd(db->expires, key, (void*)when) == DICT_ERR) {
		return 0;
	} else {
		incrRefCount(key);
		return 1;
	}
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
static time_t getExpire(redisDb *db, robj *key) {
	dictEntry *de;

	/* No expire? return ASAP */
	if (dictSize(db->expires) == 0 ||
	        (de = dictFind(db->expires, key)) == NULL) return -1;

	return (time_t) dictGetEntryVal(de);
}

// 删除过期的数据
static int expireIfNeeded(redisDb *db, robj *key) {
	time_t when;
	dictEntry *de;

	/* No expire? return ASAP */
	if (dictSize(db->expires) == 0 ||
	        (de = dictFind(db->expires, key)) == NULL) return 0;

	/* Lookup the expire */
	when = (time_t) dictGetEntryVal(de);
	if (time(NULL) <= when) return 0;

	/* Delete the key */
	dictDelete(db->expires, key);
	return dictDelete(db->dict, key) == DICT_OK;
}

// 如果键设置了过期时间，则直接删除
static int deleteIfVolatile(redisDb *db, robj *key) {
	dictEntry *de;

	/* No expire? return ASAP */
	if (dictSize(db->expires) == 0 ||
	        (de = dictFind(db->expires, key)) == NULL) return 0;

	/* Delete the key */
	server.dirty++;
	dictDelete(db->expires, key);
	return dictDelete(db->dict, key) == DICT_OK;
}

// 过期命令通用实现
static void expireGenericCommand(redisClient *c, robj *key, time_t seconds) {
	dictEntry *de;

	de = dictFind(c->db->dict, key);
	if (de == NULL) {
		addReply(c, shared.czero);
		return;
	}
	if (seconds < 0) {
		// 时间小于0，则直接淘汰
		if (deleteKey(c->db, key)) server.dirty++;
		addReply(c, shared.cone);
		return;
	} else {
		// 向过期Dict中加入<key,time>映射即可
		time_t when = time(NULL) + seconds;
		if (setExpire(c->db, key, when)) {
			addReply(c, shared.cone);
			server.dirty++;
		} else {
			addReply(c, shared.czero);
		}
		return;
	}
}

static void expireCommand(redisClient *c) {
	expireGenericCommand(c, c->argv[1], strtol(c->argv[2]->ptr, NULL, 10));
}

static void expireatCommand(redisClient *c) {
	expireGenericCommand(c, c->argv[1], strtol(c->argv[2]->ptr, NULL, 10) - time(NULL));
}

// 获取过期时间,-1表示没有过期时间（键不在过期Dict中）
static void ttlCommand(redisClient *c) {
	time_t expire;
	int ttl = -1;

	expire = getExpire(c->db, c->argv[1]);
	if (expire != -1) {
		ttl = (int) (expire - time(NULL));
		if (ttl < 0) ttl = -1;
	}
	addReplySds(c, sdscatprintf(sdsempty(), ":%d\r\n", ttl));
}

/* ================================ MULTI/EXEC ============================== */

/* Client state initialization for MULTI/EXEC */
// 初始化MULTI状态
static void initClientMultiState(redisClient *c) {
	c->mstate.commands = NULL;
	c->mstate.count = 0;
}

/* Release all the resources associated with MULTI/EXEC state */
// 释放MULTI缓存起来的所有命令
static void freeClientMultiState(redisClient *c) {
	int j;

	for (j = 0; j < c->mstate.count; j++) {
		int i;
		multiCmd *mc = c->mstate.commands + j;

		for (i = 0; i < mc->argc; i++)
			decrRefCount(mc->argv[i]);
		zfree(mc->argv);
	}
	zfree(c->mstate.commands);
}

/* Add a new command into the MULTI commands queue */
// 之前执行了MULTI，将命令缓存到起来
static void queueMultiCommand(redisClient *c, struct redisCommand *cmd) {
	multiCmd *mc;
	int j;

	c->mstate.commands = zrealloc(c->mstate.commands,
	                              sizeof(multiCmd) * (c->mstate.count + 1));
	mc = c->mstate.commands + c->mstate.count;
	mc->cmd = cmd;
	mc->argc = c->argc;
	mc->argv = zmalloc(sizeof(robj*)*c->argc);
	memcpy(mc->argv, c->argv, sizeof(robj*)*c->argc);
	for (j = 0; j < c->argc; j++)
		incrRefCount(mc->argv[j]);
	c->mstate.count++;
}

// 将客户端状态置为MULTI，用于后续判断该状态，将命令放入队列中缓存起来，直到EXEC执行或者DISCARD抛弃
static void multiCommand(redisClient *c) {
	c->flags |= REDIS_MULTI;
	addReply(c, shared.ok);
}

// 执行之前缓存起来的命令
static void execCommand(redisClient *c) {
	int j;
	robj **orig_argv;
	int orig_argc;

	if (!(c->flags & REDIS_MULTI)) {
		addReplySds(c, sdsnew("-ERR EXEC without MULTI\r\n"));
		return;
	}

	orig_argv = c->argv;
	orig_argc = c->argc;
	addReplySds(c, sdscatprintf(sdsempty(), "*%d\r\n", c->mstate.count));
	for (j = 0; j < c->mstate.count; j++) {
		c->argc = c->mstate.commands[j].argc;
		c->argv = c->mstate.commands[j].argv;
		call(c, c->mstate.commands[j].cmd);
	}
	c->argv = orig_argv;
	c->argc = orig_argc;
	freeClientMultiState(c);
	initClientMultiState(c);
	// 恢复状态（将之前的MULTI状态清除掉）
	c->flags &= (~REDIS_MULTI);
}

/* =========================== Blocking Operations  ========================= */

/* Currently Redis blocking operations support is limited to list POP ops,
 * so the current implementation is not fully generic, but it is also not
 * completely specific so it will not require a rewrite to support new
 * kind of blocking operations in the future.
 *
 * Still it's important to note that list blocking operations can be already
 * used as a notification mechanism in order to implement other blocking
 * operations at application level, so there must be a very strong evidence
 * of usefulness and generality before new blocking operations are implemented.
 *
 * This is how the current blocking POP works, we use BLPOP as example:
 * - If the user calls BLPOP and the key exists and contains a non empty list
 *   then LPOP is called instead. So BLPOP is semantically the same as LPOP
 *   if there is not to block.
 * - If instead BLPOP is called and the key does not exists or the list is
 *   empty we need to block. In order to do so we remove the notification for
 *   new data to read in the client socket (so that we'll not serve new
 *   requests if the blocking request is not served). Also we put the client
 *   in a dictionary (db->blockingkeys) mapping keys to a list of clients
 *   blocking for this keys.
 * - If a PUSH operation against a key with blocked clients waiting is
 *   performed, we serve the first in the list: basically instead to push
 *   the new element inside the list we return it to the (first / oldest)
 *   blocking client, unblock the client, and remove it form the list.
 *
 * The above comment and the source code should be enough in order to understand
 * the implementation and modify / fix it later.
 */

/* Set a client in blocking mode for the specified key, with the specified
 * timeout */
// 阻塞客户端，等待指定的Keys
static void blockForKeys(redisClient *c, robj **keys, int numkeys, time_t timeout) {
	dictEntry *de;
	list *l;
	int j;

	c->blockingkeys = zmalloc(sizeof(robj*)*numkeys);
	c->blockingkeysnum = numkeys;
	// 客户端等待超时时间
	c->blockingto = timeout;
	for (j = 0; j < numkeys; j++) {
		/* Add the key in the client structure, to map clients -> keys */
		c->blockingkeys[j] = keys[j];
		incrRefCount(keys[j]);

		/* And in the other "side", to map keys -> clients */
		// 查找是否已存在对应的Keys
		de = dictFind(c->db->blockingkeys, keys[j]);
		if (de == NULL) {
			int retval;

			/* For every key we take a list of clients blocked for it */
			// 没有客户端在等待该Key，则需先创建List，用于处理多个客户端等待同一个Key的情况
			l = listCreate();
			retval = dictAdd(c->db->blockingkeys, keys[j], l);
			incrRefCount(keys[j]);
			assert(retval == DICT_OK);
		} else {
			l = dictGetEntryVal(de);
		}
		// 放入队尾，FIFO策略
		listAddNodeTail(l, c);
	}
	/* Mark the client as a blocked client */
	// 置为BLOCKED以及删除读处理句柄（不再处理客户端请求）
	c->flags |= REDIS_BLOCKED;
	aeDeleteFileEvent(server.el, c->fd, AE_READABLE);
	server.blockedclients++;
}

/* Unblock a client that's waiting in a blocking operation such as BLPOP */
// 将客户端从阻塞列表中移除（一般是获取到了数据或者关闭客户端）
static void unblockClientWaitingData(redisClient *c) {
	dictEntry *de;
	list *l;
	int j;

	assert(c->blockingkeys != NULL);
	/* The client may wait for multiple keys, so unblock it for every key. */
	// 一个客户端可能在等待多个Key（如BLPOP key1 key2）
	for (j = 0; j < c->blockingkeysnum; j++) {
		/* Remove this client from the list of clients waiting for this key. */
		de = dictFind(c->db->blockingkeys, c->blockingkeys[j]);
		assert(de != NULL);
		l = dictGetEntryVal(de);
		listDelNode(l, listSearchKey(l, c));
		/* If the list is empty we need to remove it to avoid wasting memory */
		if (listLength(l) == 0)
			dictDelete(c->db->blockingkeys, c->blockingkeys[j]);
		decrRefCount(c->blockingkeys[j]);
	}
	/* Cleanup the client structure */
	zfree(c->blockingkeys);
	c->blockingkeys = NULL;
	c->flags &= (~REDIS_BLOCKED);
	server.blockedclients--;
	/* Ok now we are ready to get read events from socket, note that we
	 * can't trap errors here as it's possible that unblockClientWaitingDatas() is
	 * called from freeClient() itself, and the only thing we can do
	 * if we failed to register the READABLE event is to kill the client.
	 * Still the following function should never fail in the real world as
	 * we are sure the file descriptor is sane, and we exit on out of mem. */
	aeCreateFileEvent(server.el, c->fd, AE_READABLE, readQueryFromClient, c);
	/* As a final step we want to process data if there is some command waiting
	 * in the input buffer. Note that this is safe even if
	 * unblockClientWaitingData() gets called from freeClient() because
	 * freeClient() will be smart enough to call this function
	 * *after* c->querybuf was set to NULL. */
	if (c->querybuf && sdslen(c->querybuf) > 0) processInputBuffer(c);
}

/* This should be called from any function PUSHing into lists.
 * 'c' is the "pushing client", 'key' is the key it is pushing data against,
 * 'ele' is the element pushed.
 *
 * If the function returns 0 there was no client waiting for a list push
 * against this key.
 *
 * If the function returns 1 there was a client waiting for a list push
 * against this key, the element was passed to this client thus it's not
 * needed to actually add it to the list and the caller should return asap. */
// 处理BLPOP,BRPOP这类阻塞型命令，如果有客户端在等待，则直接放到客户端应答中，而不再放入List中
static int handleClientsWaitingListPush(redisClient *c, robj *key, robj *ele) {
	struct dictEntry *de;
	redisClient *receiver;
	list *l;
	listNode *ln;

	de = dictFind(c->db->blockingkeys, key);
	if (de == NULL) return 0;
	l = dictGetEntryVal(de);
	ln = listFirst(l);
	assert(ln != NULL);
	receiver = ln->value;

	addReplySds(receiver, sdsnew("*2\r\n"));
	addReplyBulkLen(receiver, key);
	addReply(receiver, key);
	addReply(receiver, shared.crlf);
	addReplyBulkLen(receiver, ele);
	addReply(receiver, ele);
	addReply(receiver, shared.crlf);
	unblockClientWaitingData(receiver);
	return 1;
}

/* Blocking RPOP/LPOP */
// 阻塞式获取链表的元素
static void blockingPopGenericCommand(redisClient *c, int where) {
	robj *o;
	time_t timeout;
	int j;

	for (j = 1; j < c->argc - 1; j++) {
		o = lookupKeyWrite(c->db, c->argv[j]);
		if (o != NULL) {
			// 链表中存在元素，无需阻塞
			if (o->type != REDIS_LIST) {
				addReply(c, shared.wrongtypeerr);
				return;
			} else {
				list *list = o->ptr;
				if (listLength(list) != 0) {
					/* If the list contains elements fall back to the usual
					 * non-blocking POP operation */
					robj *argv[2], **orig_argv;
					int orig_argc;

					/* We need to alter the command arguments before to call
					 * popGenericCommand() as the command takes a single key. */
					orig_argv = c->argv;
					orig_argc = c->argc;
					argv[1] = c->argv[j];
					c->argv = argv;
					c->argc = 2;

					/* Also the return value is different, we need to output
					 * the multi bulk reply header and the key name. The
					 * "real" command will add the last element (the value)
					 * for us. If this souds like an hack to you it's just
					 * because it is... */
					addReplySds(c, sdsnew("*2\r\n"));
					addReplyBulkLen(c, argv[1]);
					addReply(c, argv[1]);
					addReply(c, shared.crlf);
					popGenericCommand(c, where);

					/* Fix the client structure with the original stuff */
					c->argv = orig_argv;
					c->argc = orig_argc;
					return;
				}
			}
		}
	}
	/* If the list is empty or the key does not exists we must block */
	timeout = strtol(c->argv[c->argc - 1]->ptr, NULL, 10);
	if (timeout > 0) timeout += time(NULL);
	// 阻塞，直到有元素
	blockForKeys(c, c->argv + 1, c->argc - 2, timeout);
}

static void blpopCommand(redisClient *c) {
	blockingPopGenericCommand(c, REDIS_HEAD);
}

static void brpopCommand(redisClient *c) {
	blockingPopGenericCommand(c, REDIS_TAIL);
}

/* =============================== Replication  ============================= */

static int syncWrite(int fd, char *ptr, ssize_t size, int timeout) {
	ssize_t nwritten, ret = size;
	time_t start = time(NULL);

	timeout++;
	while (size) {
		if (aeWait(fd, AE_WRITABLE, 1000) & AE_WRITABLE) {
			nwritten = write(fd, ptr, size);
			if (nwritten == -1) return -1;
			ptr += nwritten;
			size -= nwritten;
		}
		if ((time(NULL) - start) > timeout) {
			errno = ETIMEDOUT;
			return -1;
		}
	}
	return ret;
}

static int syncRead(int fd, char *ptr, ssize_t size, int timeout) {
	ssize_t nread, totread = 0;
	time_t start = time(NULL);

	timeout++;
	while (size) {
		if (aeWait(fd, AE_READABLE, 1000) & AE_READABLE) {
			nread = read(fd, ptr, size);
			if (nread == -1) return -1;
			ptr += nread;
			size -= nread;
			totread += nread;
		}
		if ((time(NULL) - start) > timeout) {
			errno = ETIMEDOUT;
			return -1;
		}
	}
	return totread;
}

static int syncReadLine(int fd, char *ptr, ssize_t size, int timeout) {
	ssize_t nread = 0;

	size--;
	while (size) {
		char c;

		if (syncRead(fd, &c, 1, timeout) == -1) return -1;
		if (c == '\n') {
			*ptr = '\0';
			if (nread && *(ptr - 1) == '\r') *(ptr - 1) = '\0';
			return nread;
		} else {
			*ptr++ = c;
			*ptr = '\0';
			nread++;
		}
	}
	return nread;
}

static void syncCommand(redisClient *c) {
	/* ignore SYNC if aleady slave or in monitor mode */
	if (c->flags & REDIS_SLAVE) return;

	/* SYNC can't be issued when the server has pending data to send to
	 * the client about already issued commands. We need a fresh reply
	 * buffer registering the differences between the BGSAVE and the current
	 * dataset, so that we can copy to other slaves if needed. */
	if (listLength(c->reply) != 0) {
		addReplySds(c, sdsnew("-ERR SYNC is invalid with pending input\r\n"));
		return;
	}

	redisLog(REDIS_NOTICE, "Slave ask for synchronization");
	/* Here we need to check if there is a background saving operation
	 * in progress, or if it is required to start one */
	if (server.bgsavechildpid != -1) {
		/* Ok a background save is in progress. Let's check if it is a good
		 * one for replication, i.e. if there is another slave that is
		 * registering differences since the server forked to save */
		redisClient *slave;
		listNode *ln;
		listIter li;

		listRewind(server.slaves, &li);
		while ((ln = listNext(&li))) {
			slave = ln->value;
			if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) break;
		}
		if (ln) {
			/* Perfect, the server is already registering differences for
			 * another slave. Set the right state, and copy the buffer. */
			listRelease(c->reply);
			c->reply = listDup(slave->reply);
			c->replstate = REDIS_REPL_WAIT_BGSAVE_END;
			redisLog(REDIS_NOTICE, "Waiting for end of BGSAVE for SYNC");
		} else {
			/* No way, we need to wait for the next BGSAVE in order to
			 * register differences */
			c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
			redisLog(REDIS_NOTICE, "Waiting for next BGSAVE for SYNC");
		}
	} else {
		/* Ok we don't have a BGSAVE in progress, let's start one */
		redisLog(REDIS_NOTICE, "Starting BGSAVE for SYNC");
		if (rdbSaveBackground(server.dbfilename) != REDIS_OK) {
			redisLog(REDIS_NOTICE, "Replication failed, can't BGSAVE");
			addReplySds(c, sdsnew("-ERR Unalbe to perform background save\r\n"));
			return;
		}
		c->replstate = REDIS_REPL_WAIT_BGSAVE_END;
	}
	c->repldbfd = -1;
	c->flags |= REDIS_SLAVE;
	c->slaveseldb = 0;
	listAddNodeTail(server.slaves, c);
	return;
}

static void sendBulkToSlave(aeEventLoop *el, int fd, void *privdata, int mask) {
	redisClient *slave = privdata;
	REDIS_NOTUSED(el);
	REDIS_NOTUSED(mask);
	char buf[REDIS_IOBUF_LEN];
	ssize_t nwritten, buflen;

	if (slave->repldboff == 0) {
		/* Write the bulk write count before to transfer the DB. In theory here
		 * we don't know how much room there is in the output buffer of the
		 * socket, but in pratice SO_SNDLOWAT (the minimum count for output
		 * operations) will never be smaller than the few bytes we need. */
		sds bulkcount;

		bulkcount = sdscatprintf(sdsempty(), "$%lld\r\n", (unsigned long long)
		                         slave->repldbsize);
		if (write(fd, bulkcount, sdslen(bulkcount)) != (signed)sdslen(bulkcount))
		{
			sdsfree(bulkcount);
			freeClient(slave);
			return;
		}
		sdsfree(bulkcount);
	}
	lseek(slave->repldbfd, slave->repldboff, SEEK_SET);
	buflen = read(slave->repldbfd, buf, REDIS_IOBUF_LEN);
	if (buflen <= 0) {
		redisLog(REDIS_WARNING, "Read error sending DB to slave: %s",
		         (buflen == 0) ? "premature EOF" : strerror(errno));
		freeClient(slave);
		return;
	}
	if ((nwritten = write(fd, buf, buflen)) == -1) {
		redisLog(REDIS_VERBOSE, "Write error sending DB to slave: %s",
		         strerror(errno));
		freeClient(slave);
		return;
	}
	slave->repldboff += nwritten;
	if (slave->repldboff == slave->repldbsize) {
		close(slave->repldbfd);
		slave->repldbfd = -1;
		aeDeleteFileEvent(server.el, slave->fd, AE_WRITABLE);
		slave->replstate = REDIS_REPL_ONLINE;
		if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE,
		                      sendReplyToClient, slave) == AE_ERR) {
			freeClient(slave);
			return;
		}
		addReplySds(slave, sdsempty());
		redisLog(REDIS_NOTICE, "Synchronization with slave succeeded");
	}
}

/* This function is called at the end of every backgrond saving.
 * The argument bgsaveerr is REDIS_OK if the background saving succeeded
 * otherwise REDIS_ERR is passed to the function.
 *
 * The goal of this function is to handle slaves waiting for a successful
 * background saving in order to perform non-blocking synchronization. */
static void updateSlavesWaitingBgsave(int bgsaveerr) {
	listNode *ln;
	int startbgsave = 0;
	listIter li;

	listRewind(server.slaves, &li);
	while ((ln = listNext(&li))) {
		redisClient *slave = ln->value;

		if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) {
			startbgsave = 1;
			slave->replstate = REDIS_REPL_WAIT_BGSAVE_END;
		} else if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) {
			struct redis_stat buf;

			if (bgsaveerr != REDIS_OK) {
				freeClient(slave);
				redisLog(REDIS_WARNING, "SYNC failed. BGSAVE child returned an error");
				continue;
			}
			if ((slave->repldbfd = open(server.dbfilename, O_RDONLY)) == -1 ||
			        redis_fstat(slave->repldbfd, &buf) == -1) {
				freeClient(slave);
				redisLog(REDIS_WARNING, "SYNC failed. Can't open/stat DB after BGSAVE: %s", strerror(errno));
				continue;
			}
			slave->repldboff = 0;
			slave->repldbsize = buf.st_size;
			slave->replstate = REDIS_REPL_SEND_BULK;
			aeDeleteFileEvent(server.el, slave->fd, AE_WRITABLE);
			if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE, sendBulkToSlave, slave) == AE_ERR) {
				freeClient(slave);
				continue;
			}
		}
	}
	if (startbgsave) {
		if (rdbSaveBackground(server.dbfilename) != REDIS_OK) {
			listIter li;

			listRewind(server.slaves, &li);
			redisLog(REDIS_WARNING, "SYNC failed. BGSAVE failed");
			while ((ln = listNext(&li))) {
				redisClient *slave = ln->value;

				if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START)
					freeClient(slave);
			}
		}
	}
}

static int syncWithMaster(void) {
	char buf[1024], tmpfile[256], authcmd[1024];
	int dumpsize;
	int fd = anetTcpConnect(NULL, server.masterhost, server.masterport);
	int dfd;

	if (fd == -1) {
		redisLog(REDIS_WARNING, "Unable to connect to MASTER: %s",
		         strerror(errno));
		return REDIS_ERR;
	}

	/* AUTH with the master if required. */
	if (server.masterauth) {
		snprintf(authcmd, 1024, "AUTH %s\r\n", server.masterauth);
		if (syncWrite(fd, authcmd, strlen(server.masterauth) + 7, 5) == -1) {
			close(fd);
			redisLog(REDIS_WARNING, "Unable to AUTH to MASTER: %s",
			         strerror(errno));
			return REDIS_ERR;
		}
		/* Read the AUTH result.  */
		if (syncReadLine(fd, buf, 1024, 3600) == -1) {
			close(fd);
			redisLog(REDIS_WARNING, "I/O error reading auth result from MASTER: %s",
			         strerror(errno));
			return REDIS_ERR;
		}
		if (buf[0] != '+') {
			close(fd);
			redisLog(REDIS_WARNING, "Cannot AUTH to MASTER, is the masterauth password correct?");
			return REDIS_ERR;
		}
	}

	/* Issue the SYNC command */
	if (syncWrite(fd, "SYNC \r\n", 7, 5) == -1) {
		close(fd);
		redisLog(REDIS_WARNING, "I/O error writing to MASTER: %s",
		         strerror(errno));
		return REDIS_ERR;
	}
	/* Read the bulk write count */
	if (syncReadLine(fd, buf, 1024, 3600) == -1) {
		close(fd);
		redisLog(REDIS_WARNING, "I/O error reading bulk count from MASTER: %s",
		         strerror(errno));
		return REDIS_ERR;
	}
	if (buf[0] != '$') {
		close(fd);
		redisLog(REDIS_WARNING, "Bad protocol from MASTER, the first byte is not '$', are you sure the host and port are right?");
		return REDIS_ERR;
	}
	dumpsize = atoi(buf + 1);
	redisLog(REDIS_NOTICE, "Receiving %d bytes data dump from MASTER", dumpsize);
	/* Read the bulk write data on a temp file */
	snprintf(tmpfile, 256, "temp-%d.%ld.rdb", (int)time(NULL), (long int)random());
	dfd = open(tmpfile, O_CREAT | O_WRONLY, 0644);
	if (dfd == -1) {
		close(fd);
		redisLog(REDIS_WARNING, "Opening the temp file needed for MASTER <-> SLAVE synchronization: %s", strerror(errno));
		return REDIS_ERR;
	}
	while (dumpsize) {
		int nread, nwritten;

		nread = read(fd, buf, (dumpsize < 1024) ? dumpsize : 1024);
		if (nread == -1) {
			redisLog(REDIS_WARNING, "I/O error trying to sync with MASTER: %s",
			         strerror(errno));
			close(fd);
			close(dfd);
			return REDIS_ERR;
		}
		nwritten = write(dfd, buf, nread);
		if (nwritten == -1) {
			redisLog(REDIS_WARNING, "Write error writing to the DB dump file needed for MASTER <-> SLAVE synchrnonization: %s", strerror(errno));
			close(fd);
			close(dfd);
			return REDIS_ERR;
		}
		dumpsize -= nread;
	}
	close(dfd);
	if (rename(tmpfile, server.dbfilename) == -1) {
		redisLog(REDIS_WARNING, "Failed trying to rename the temp DB into dump.rdb in MASTER <-> SLAVE synchronization: %s", strerror(errno));
		unlink(tmpfile);
		close(fd);
		return REDIS_ERR;
	}
	emptyDb();
	if (rdbLoad(server.dbfilename) != REDIS_OK) {
		redisLog(REDIS_WARNING, "Failed trying to load the MASTER synchronization DB from disk");
		close(fd);
		return REDIS_ERR;
	}
	server.master = createClient(fd);
	server.master->flags |= REDIS_MASTER;
	server.master->authenticated = 1;
	server.replstate = REDIS_REPL_CONNECTED;
	return REDIS_OK;
}

static void slaveofCommand(redisClient *c) {
	if (!strcasecmp(c->argv[1]->ptr, "no") &&
	        !strcasecmp(c->argv[2]->ptr, "one")) {
		if (server.masterhost) {
			sdsfree(server.masterhost);
			server.masterhost = NULL;
			if (server.master) freeClient(server.master);
			server.replstate = REDIS_REPL_NONE;
			redisLog(REDIS_NOTICE, "MASTER MODE enabled (user request)");
		}
	} else {
		sdsfree(server.masterhost);
		server.masterhost = sdsdup(c->argv[1]->ptr);
		server.masterport = atoi(c->argv[2]->ptr);
		if (server.master) freeClient(server.master);
		server.replstate = REDIS_REPL_CONNECT;
		redisLog(REDIS_NOTICE, "SLAVE OF %s:%d enabled (user request)",
		         server.masterhost, server.masterport);
	}
	addReply(c, shared.ok);
}

/* ============================ Maxmemory directive  ======================== */

/* Try to free one object form the pre-allocated objects free list.
 * This is useful under low mem conditions as by default we take 1 million
 * free objects allocated. On success REDIS_OK is returned, otherwise
 * REDIS_ERR. */
static int tryFreeOneObjectFromFreelist(void) {
	robj *o;

	if (server.vm_enabled) pthread_mutex_lock(&server.obj_freelist_mutex);
	if (listLength(server.objfreelist)) {
		listNode *head = listFirst(server.objfreelist);
		o = listNodeValue(head);
		listDelNode(server.objfreelist, head);
		if (server.vm_enabled) pthread_mutex_unlock(&server.obj_freelist_mutex);
		zfree(o);
		return REDIS_OK;
	} else {
		if (server.vm_enabled) pthread_mutex_unlock(&server.obj_freelist_mutex);
		return REDIS_ERR;
	}
}

/* This function gets called when 'maxmemory' is set on the config file to limit
 * the max memory used by the server, and we are out of memory.
 * This function will try to, in order:
 *
 * - Free objects from the free list
 * - Try to remove keys with an EXPIRE set
 *
 * It is not possible to free enough memory to reach used-memory < maxmemory
 * the server will start refusing commands that will enlarge even more the
 * memory usage.
 */
static void freeMemoryIfNeeded(void) {
	while (server.maxmemory && zmalloc_used_memory() > server.maxmemory) {
		int j, k, freed = 0;

		if (tryFreeOneObjectFromFreelist() == REDIS_OK) continue;
		for (j = 0; j < server.dbnum; j++) {
			int minttl = -1;
			robj *minkey = NULL;
			struct dictEntry *de;

			if (dictSize(server.db[j].expires)) {
				freed = 1;
				/* From a sample of three keys drop the one nearest to
				 * the natural expire */
				for (k = 0; k < 3; k++) {
					time_t t;

					de = dictGetRandomKey(server.db[j].expires);
					t = (time_t) dictGetEntryVal(de);
					if (minttl == -1 || t < minttl) {
						minkey = dictGetEntryKey(de);
						minttl = t;
					}
				}
				deleteKey(server.db + j, minkey);
			}
		}
		if (!freed) return; /* nothing to free... */
	}
}

/* ============================== Append Only file ========================== */

static void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc) {
	sds buf = sdsempty();
	int j;
	ssize_t nwritten;
	time_t now;
	robj *tmpargv[3];

	/* The DB this command was targetting is not the same as the last command
	 * we appendend. To issue a SELECT command is needed. */
	if (dictid != server.appendseldb) {
		char seldb[64];

		snprintf(seldb, sizeof(seldb), "%d", dictid);
		buf = sdscatprintf(buf, "*2\r\n$6\r\nSELECT\r\n$%lu\r\n%s\r\n",
		                   (unsigned long)strlen(seldb), seldb);
		server.appendseldb = dictid;
	}

	/* "Fix" the argv vector if the command is EXPIRE. We want to translate
	 * EXPIREs into EXPIREATs calls */
	if (cmd->proc == expireCommand) {
		long when;

		tmpargv[0] = createStringObject("EXPIREAT", 8);
		tmpargv[1] = argv[1];
		incrRefCount(argv[1]);
		when = time(NULL) + strtol(argv[2]->ptr, NULL, 10);
		tmpargv[2] = createObject(REDIS_STRING,
		                          sdscatprintf(sdsempty(), "%ld", when));
		argv = tmpargv;
	}

	/* Append the actual command */
	buf = sdscatprintf(buf, "*%d\r\n", argc);
	for (j = 0; j < argc; j++) {
		robj *o = argv[j];

		o = getDecodedObject(o);
		buf = sdscatprintf(buf, "$%lu\r\n", (unsigned long)sdslen(o->ptr));
		buf = sdscatlen(buf, o->ptr, sdslen(o->ptr));
		buf = sdscatlen(buf, "\r\n", 2);
		decrRefCount(o);
	}

	/* Free the objects from the modified argv for EXPIREAT */
	if (cmd->proc == expireCommand) {
		for (j = 0; j < 3; j++)
			decrRefCount(argv[j]);
	}

	/* We want to perform a single write. This should be guaranteed atomic
	 * at least if the filesystem we are writing is a real physical one.
	 * While this will save us against the server being killed I don't think
	 * there is much to do about the whole server stopping for power problems
	 * or alike */
	nwritten = write(server.appendfd, buf, sdslen(buf));
	if (nwritten != (signed)sdslen(buf)) {
		/* Ooops, we are in troubles. The best thing to do for now is
		 * to simply exit instead to give the illusion that everything is
		 * working as expected. */
		if (nwritten == -1) {
			redisLog(REDIS_WARNING, "Exiting on error writing to the append-only file: %s", strerror(errno));
		} else {
			redisLog(REDIS_WARNING, "Exiting on short write while writing to the append-only file: %s", strerror(errno));
		}
		exit(1);
	}
	/* If a background append only file rewriting is in progress we want to
	 * accumulate the differences between the child DB and the current one
	 * in a buffer, so that when the child process will do its work we
	 * can append the differences to the new append only file. */
	if (server.bgrewritechildpid != -1)
		server.bgrewritebuf = sdscatlen(server.bgrewritebuf, buf, sdslen(buf));

	sdsfree(buf);
	now = time(NULL);
	if (server.appendfsync == APPENDFSYNC_ALWAYS ||
	        (server.appendfsync == APPENDFSYNC_EVERYSEC &&
	         now - server.lastfsync > 1))
	{
		fsync(server.appendfd); /* Let's try to get this data on the disk */
		server.lastfsync = now;
	}
}

/* In Redis commands are always executed in the context of a client, so in
 * order to load the append only file we need to create a fake client. */
static struct redisClient *createFakeClient(void) {
	struct redisClient *c = zmalloc(sizeof(*c));

	selectDb(c, 0);
	c->fd = -1;
	c->querybuf = sdsempty();
	c->argc = 0;
	c->argv = NULL;
	c->flags = 0;
	/* We set the fake client as a slave waiting for the synchronization
	 * so that Redis will not try to send replies to this client. */
	c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
	c->reply = listCreate();
	listSetFreeMethod(c->reply, decrRefCount);
	listSetDupMethod(c->reply, dupClientReplyValue);
	return c;
}

static void freeFakeClient(struct redisClient *c) {
	sdsfree(c->querybuf);
	listRelease(c->reply);
	zfree(c);
}

/* Replay the append log file. On error REDIS_OK is returned. On non fatal
 * error (the append only file is zero-length) REDIS_ERR is returned. On
 * fatal error an error message is logged and the program exists. */
int loadAppendOnlyFile(char *filename) {
	struct redisClient *fakeClient;
	FILE *fp = fopen(filename, "r");
	struct redis_stat sb;
	unsigned long long loadedkeys = 0;

	if (redis_fstat(fileno(fp), &sb) != -1 && sb.st_size == 0)
		return REDIS_ERR;

	if (fp == NULL) {
		redisLog(REDIS_WARNING, "Fatal error: can't open the append log file for reading: %s", strerror(errno));
		exit(1);
	}

	fakeClient = createFakeClient();
	while (1) {
		int argc, j;
		unsigned long len;
		robj **argv;
		char buf[128];
		sds argsds;
		struct redisCommand *cmd;

		if (fgets(buf, sizeof(buf), fp) == NULL) {
			if (feof(fp))
				break;
			else
				goto readerr;
		}
		if (buf[0] != '*') goto fmterr;
		argc = atoi(buf + 1);
		argv = zmalloc(sizeof(robj*)*argc);
		for (j = 0; j < argc; j++) {
			if (fgets(buf, sizeof(buf), fp) == NULL) goto readerr;
			if (buf[0] != '$') goto fmterr;
			len = strtol(buf + 1, NULL, 10);
			argsds = sdsnewlen(NULL, len);
			if (len && fread(argsds, len, 1, fp) == 0) goto fmterr;
			argv[j] = createObject(REDIS_STRING, argsds);
			if (fread(buf, 2, 1, fp) == 0) goto fmterr; /* discard CRLF */
		}

		/* Command lookup */
		cmd = lookupCommand(argv[0]->ptr);
		if (!cmd) {
			redisLog(REDIS_WARNING, "Unknown command '%s' reading the append only file", argv[0]->ptr);
			exit(1);
		}
		/* Try object sharing and encoding */
		if (server.shareobjects) {
			int j;
			for (j = 1; j < argc; j++)
				argv[j] = tryObjectSharing(argv[j]);
		}
		if (cmd->flags & REDIS_CMD_BULK)
			tryObjectEncoding(argv[argc - 1]);
		/* Run the command in the context of a fake client */
		fakeClient->argc = argc;
		fakeClient->argv = argv;
		cmd->proc(fakeClient);
		/* Discard the reply objects list from the fake client */
		while (listLength(fakeClient->reply))
			listDelNode(fakeClient->reply, listFirst(fakeClient->reply));
		/* Clean up, ready for the next command */
		for (j = 0; j < argc; j++) decrRefCount(argv[j]);
		zfree(argv);
		/* Handle swapping while loading big datasets when VM is on */
		loadedkeys++;
		if (server.vm_enabled && (loadedkeys % 5000) == 0) {
			while (zmalloc_used_memory() > server.vm_max_memory) {
				if (vmSwapOneObjectBlocking() == REDIS_ERR) break;
			}
		}
	}
	fclose(fp);
	freeFakeClient(fakeClient);
	return REDIS_OK;

readerr:
	if (feof(fp)) {
		redisLog(REDIS_WARNING, "Unexpected end of file reading the append only file");
	} else {
		redisLog(REDIS_WARNING, "Unrecoverable error reading the append only file: %s", strerror(errno));
	}
	exit(1);
fmterr:
	redisLog(REDIS_WARNING, "Bad file format reading the append only file");
	exit(1);
}

/* Write an object into a file in the bulk format $<count>\r\n<payload>\r\n */
static int fwriteBulk(FILE *fp, robj *obj) {
	char buf[128];
	int decrrc = 0;

	/* Avoid the incr/decr ref count business if possible to help
	 * copy-on-write (we are often in a child process when this function
	 * is called).
	 * Also makes sure that key objects don't get incrRefCount-ed when VM
	 * is enabled */
	if (obj->encoding != REDIS_ENCODING_RAW) {
		obj = getDecodedObject(obj);
		decrrc = 1;
	}
	snprintf(buf, sizeof(buf), "$%ld\r\n", (long)sdslen(obj->ptr));
	if (fwrite(buf, strlen(buf), 1, fp) == 0) goto err;
	if (sdslen(obj->ptr) && fwrite(obj->ptr, sdslen(obj->ptr), 1, fp) == 0)
		goto err;
	if (fwrite("\r\n", 2, 1, fp) == 0) goto err;
	if (decrrc) decrRefCount(obj);
	return 1;
err:
	if (decrrc) decrRefCount(obj);
	return 0;
}

/* Write a double value in bulk format $<count>\r\n<payload>\r\n */
static int fwriteBulkDouble(FILE *fp, double d) {
	char buf[128], dbuf[128];

	snprintf(dbuf, sizeof(dbuf), "%.17g\r\n", d);
	snprintf(buf, sizeof(buf), "$%lu\r\n", (unsigned long)strlen(dbuf) - 2);
	if (fwrite(buf, strlen(buf), 1, fp) == 0) return 0;
	if (fwrite(dbuf, strlen(dbuf), 1, fp) == 0) return 0;
	return 1;
}

/* Write a long value in bulk format $<count>\r\n<payload>\r\n */
static int fwriteBulkLong(FILE *fp, long l) {
	char buf[128], lbuf[128];

	snprintf(lbuf, sizeof(lbuf), "%ld\r\n", l);
	snprintf(buf, sizeof(buf), "$%lu\r\n", (unsigned long)strlen(lbuf) - 2);
	if (fwrite(buf, strlen(buf), 1, fp) == 0) return 0;
	if (fwrite(lbuf, strlen(lbuf), 1, fp) == 0) return 0;
	return 1;
}

/* Write a sequence of commands able to fully rebuild the dataset into
 * "filename". Used both by REWRITEAOF and BGREWRITEAOF. */
static int rewriteAppendOnlyFile(char *filename) {
	dictIterator *di = NULL;
	dictEntry *de;
	FILE *fp;
	char tmpfile[256];
	int j;
	time_t now = time(NULL);

	/* Note that we have to use a different temp name here compared to the
	 * one used by rewriteAppendOnlyFileBackground() function. */
	snprintf(tmpfile, 256, "temp-rewriteaof-%d.aof", (int) getpid());
	fp = fopen(tmpfile, "w");
	if (!fp) {
		redisLog(REDIS_WARNING, "Failed rewriting the append only file: %s", strerror(errno));
		return REDIS_ERR;
	}
	for (j = 0; j < server.dbnum; j++) {
		char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
		redisDb *db = server.db + j;
		dict *d = db->dict;
		if (dictSize(d) == 0) continue;
		di = dictGetIterator(d);
		if (!di) {
			fclose(fp);
			return REDIS_ERR;
		}

		/* SELECT the new DB */
		if (fwrite(selectcmd, sizeof(selectcmd) - 1, 1, fp) == 0) goto werr;
		if (fwriteBulkLong(fp, j) == 0) goto werr;

		/* Iterate this DB writing every entry */
		while ((de = dictNext(di)) != NULL) {
			robj *key, *o;
			time_t expiretime;
			int swapped;

			key = dictGetEntryKey(de);
			/* If the value for this key is swapped, load a preview in memory.
			 * We use a "swapped" flag to remember if we need to free the
			 * value object instead to just increment the ref count anyway
			 * in order to avoid copy-on-write of pages if we are forked() */
			if (!server.vm_enabled || key->storage == REDIS_VM_MEMORY ||
			        key->storage == REDIS_VM_SWAPPING) {
				o = dictGetEntryVal(de);
				swapped = 0;
			} else {
				o = vmPreviewObject(key);
				swapped = 1;
			}
			expiretime = getExpire(db, key);

			/* Save the key and associated value */
			if (o->type == REDIS_STRING) {
				/* Emit a SET command */
				char cmd[] = "*3\r\n$3\r\nSET\r\n";
				if (fwrite(cmd, sizeof(cmd) - 1, 1, fp) == 0) goto werr;
				/* Key and value */
				if (fwriteBulk(fp, key) == 0) goto werr;
				if (fwriteBulk(fp, o) == 0) goto werr;
			} else if (o->type == REDIS_LIST) {
				/* Emit the RPUSHes needed to rebuild the list */
				list *list = o->ptr;
				listNode *ln;
				listIter li;

				listRewind(list, &li);
				while ((ln = listNext(&li))) {
					char cmd[] = "*3\r\n$5\r\nRPUSH\r\n";
					robj *eleobj = listNodeValue(ln);

					if (fwrite(cmd, sizeof(cmd) - 1, 1, fp) == 0) goto werr;
					if (fwriteBulk(fp, key) == 0) goto werr;
					if (fwriteBulk(fp, eleobj) == 0) goto werr;
				}
			} else if (o->type == REDIS_SET) {
				/* Emit the SADDs needed to rebuild the set */
				dict *set = o->ptr;
				dictIterator *di = dictGetIterator(set);
				dictEntry *de;

				while ((de = dictNext(di)) != NULL) {
					char cmd[] = "*3\r\n$4\r\nSADD\r\n";
					robj *eleobj = dictGetEntryKey(de);

					if (fwrite(cmd, sizeof(cmd) - 1, 1, fp) == 0) goto werr;
					if (fwriteBulk(fp, key) == 0) goto werr;
					if (fwriteBulk(fp, eleobj) == 0) goto werr;
				}
				dictReleaseIterator(di);
			} else if (o->type == REDIS_ZSET) {
				/* Emit the ZADDs needed to rebuild the sorted set */
				zset *zs = o->ptr;
				dictIterator *di = dictGetIterator(zs->dict);
				dictEntry *de;

				while ((de = dictNext(di)) != NULL) {
					char cmd[] = "*4\r\n$4\r\nZADD\r\n";
					robj *eleobj = dictGetEntryKey(de);
					double *score = dictGetEntryVal(de);

					if (fwrite(cmd, sizeof(cmd) - 1, 1, fp) == 0) goto werr;
					if (fwriteBulk(fp, key) == 0) goto werr;
					if (fwriteBulkDouble(fp, *score) == 0) goto werr;
					if (fwriteBulk(fp, eleobj) == 0) goto werr;
				}
				dictReleaseIterator(di);
			} else {
				redisAssert(0 != 0);
			}
			/* Save the expire time */
			if (expiretime != -1) {
				char cmd[] = "*3\r\n$8\r\nEXPIREAT\r\n";
				/* If this key is already expired skip it */
				if (expiretime < now) continue;
				if (fwrite(cmd, sizeof(cmd) - 1, 1, fp) == 0) goto werr;
				if (fwriteBulk(fp, key) == 0) goto werr;
				if (fwriteBulkLong(fp, expiretime) == 0) goto werr;
			}
			if (swapped) decrRefCount(o);
		}
		dictReleaseIterator(di);
	}

	/* Make sure data will not remain on the OS's output buffers */
	fflush(fp);
	fsync(fileno(fp));
	fclose(fp);

	/* Use RENAME to make sure the DB file is changed atomically only
	 * if the generate DB file is ok. */
	if (rename(tmpfile, filename) == -1) {
		redisLog(REDIS_WARNING, "Error moving temp append only file on the final destination: %s", strerror(errno));
		unlink(tmpfile);
		return REDIS_ERR;
	}
	redisLog(REDIS_NOTICE, "SYNC append only file rewrite performed");
	return REDIS_OK;

werr:
	fclose(fp);
	unlink(tmpfile);
	redisLog(REDIS_WARNING, "Write error writing append only file on disk: %s", strerror(errno));
	if (di) dictReleaseIterator(di);
	return REDIS_ERR;
}

/* This is how rewriting of the append only file in background works:
 *
 * 1) The user calls BGREWRITEAOF
 * 2) Redis calls this function, that forks():
 *    2a) the child rewrite the append only file in a temp file.
 *    2b) the parent accumulates differences in server.bgrewritebuf.
 * 3) When the child finished '2a' exists.
 * 4) The parent will trap the exit code, if it's OK, will append the
 *    data accumulated into server.bgrewritebuf into the temp file, and
 *    finally will rename(2) the temp file in the actual file name.
 *    The the new file is reopened as the new append only file. Profit!
 */
static int rewriteAppendOnlyFileBackground(void) {
	pid_t childpid;

	if (server.bgrewritechildpid != -1) return REDIS_ERR;
	if (server.vm_enabled) waitEmptyIOJobsQueue();
	if ((childpid = fork()) == 0) {
		/* Child */
		char tmpfile[256];

		if (server.vm_enabled) vmReopenSwapFile();
		close(server.fd);
		snprintf(tmpfile, 256, "temp-rewriteaof-bg-%d.aof", (int) getpid());
		if (rewriteAppendOnlyFile(tmpfile) == REDIS_OK) {
			exit(0);
		} else {
			exit(1);
		}
	} else {
		/* Parent */
		if (childpid == -1) {
			redisLog(REDIS_WARNING,
			         "Can't rewrite append only file in background: fork: %s",
			         strerror(errno));
			return REDIS_ERR;
		}
		redisLog(REDIS_NOTICE,
		         "Background append only file rewriting started by pid %d", childpid);
		server.bgrewritechildpid = childpid;
		/* We set appendseldb to -1 in order to force the next call to the
		 * feedAppendOnlyFile() to issue a SELECT command, so the differences
		 * accumulated by the parent into server.bgrewritebuf will start
		 * with a SELECT statement and it will be safe to merge. */
		server.appendseldb = -1;
		return REDIS_OK;
	}
	return REDIS_OK; /* unreached */
}

static void bgrewriteaofCommand(redisClient *c) {
	if (server.bgrewritechildpid != -1) {
		addReplySds(c, sdsnew("-ERR background append only file rewriting already in progress\r\n"));
		return;
	}
	if (rewriteAppendOnlyFileBackground() == REDIS_OK) {
		char *status = "+Background append only file rewriting started\r\n";
		addReplySds(c, sdsnew(status));
	} else {
		addReply(c, shared.err);
	}
}

static void aofRemoveTempFile(pid_t childpid) {
	char tmpfile[256];

	snprintf(tmpfile, 256, "temp-rewriteaof-bg-%d.aof", (int) childpid);
	unlink(tmpfile);
}

/* Virtual Memory is composed mainly of two subsystems:
 * - Blocking Virutal Memory
 * - Threaded Virtual Memory I/O
 * The two parts are not fully decoupled, but functions are split among two
 * different sections of the source code (delimited by comments) in order to
 * make more clear what functionality is about the blocking VM and what about
 * the threaded (not blocking) VM.
 *
 * Redis VM design:
 *
 * Redis VM is a blocking VM (one that blocks reading swapped values from
 * disk into memory when a value swapped out is needed in memory) that is made
 * unblocking by trying to examine the command argument vector in order to
 * load in background values that will likely be needed in order to exec
 * the command. The command is executed only once all the relevant keys
 * are loaded into memory.
 *
 * This basically is almost as simple of a blocking VM, but almost as parallel
 * as a fully non-blocking VM.
 */

/* =================== Virtual Memory - Blocking Side  ====================== */

/* substitute the first occurrence of '%p' with the process pid in the
 * swap file name. */
static void expandVmSwapFilename(void) {
	char *p = strstr(server.vm_swap_file, "%p");
	sds new;

	if (!p) return;
	new = sdsempty();
	*p = '\0';
	new = sdscat(new, server.vm_swap_file);
	new = sdscatprintf(new, "%ld", (long) getpid());
	new = sdscat(new, p + 2);
	zfree(server.vm_swap_file);
	server.vm_swap_file = new;
}

static void vmInit(void) {
	off_t totsize;
	int pipefds[2];
	size_t stacksize;

	if (server.vm_max_threads != 0)
		zmalloc_enable_thread_safeness(); /* we need thread safe zmalloc() */

	expandVmSwapFilename();
	redisLog(REDIS_NOTICE, "Using '%s' as swap file", server.vm_swap_file);
	if ((server.vm_fp = fopen(server.vm_swap_file, "r+b")) == NULL) {
		server.vm_fp = fopen(server.vm_swap_file, "w+b");
	}
	if (server.vm_fp == NULL) {
		redisLog(REDIS_WARNING,
		         "Impossible to open the swap file: %s. Exiting.",
		         strerror(errno));
		exit(1);
	}
	server.vm_fd = fileno(server.vm_fp);
	server.vm_next_page = 0;
	server.vm_near_pages = 0;
	server.vm_stats_used_pages = 0;
	server.vm_stats_swapped_objects = 0;
	server.vm_stats_swapouts = 0;
	server.vm_stats_swapins = 0;
	totsize = server.vm_pages * server.vm_page_size;
	redisLog(REDIS_NOTICE, "Allocating %lld bytes of swap file", totsize);
	if (ftruncate(server.vm_fd, totsize) == -1) {
		redisLog(REDIS_WARNING, "Can't ftruncate swap file: %s. Exiting.",
		         strerror(errno));
		exit(1);
	} else {
		redisLog(REDIS_NOTICE, "Swap file allocated with success");
	}
	server.vm_bitmap = zmalloc((server.vm_pages + 7) / 8);
	redisLog(REDIS_VERBOSE, "Allocated %lld bytes page table for %lld pages",
	         (long long) (server.vm_pages + 7) / 8, server.vm_pages);
	memset(server.vm_bitmap, 0, (server.vm_pages + 7) / 8);

	/* Initialize threaded I/O (used by Virtual Memory) */
	server.io_newjobs = listCreate();
	server.io_processing = listCreate();
	server.io_processed = listCreate();
	server.io_clients = listCreate();
	pthread_mutex_init(&server.io_mutex, NULL);
	pthread_mutex_init(&server.obj_freelist_mutex, NULL);
	pthread_mutex_init(&server.io_swapfile_mutex, NULL);
	server.io_active_threads = 0;
	if (pipe(pipefds) == -1) {
		redisLog(REDIS_WARNING, "Unable to intialized VM: pipe(2): %s. Exiting."
		         , strerror(errno));
		exit(1);
	}
	server.io_ready_pipe_read = pipefds[0];
	server.io_ready_pipe_write = pipefds[1];
	redisAssert(anetNonBlock(NULL, server.io_ready_pipe_read) != ANET_ERR);
	/* LZF requires a lot of stack */
	pthread_attr_init(&server.io_threads_attr);
	pthread_attr_getstacksize(&server.io_threads_attr, &stacksize);
	while (stacksize < REDIS_THREAD_STACK_SIZE) stacksize *= 2;
	pthread_attr_setstacksize(&server.io_threads_attr, stacksize);
	/* Listen for events in the threaded I/O pipe */
	if (aeCreateFileEvent(server.el, server.io_ready_pipe_read, AE_READABLE,
	                      vmThreadedIOCompletedJob, NULL) == AE_ERR)
		oom("creating file event");
}

/* Mark the page as used */
static void vmMarkPageUsed(off_t page) {
	off_t byte = page / 8;
	int bit = page & 7;
	redisAssert(vmFreePage(page) == 1);
	server.vm_bitmap[byte] |= 1 << bit;
	redisLog(REDIS_DEBUG, "Mark used: %lld (byte:%lld bit:%d)\n",
	         (long long)page, (long long)byte, bit);
}

/* Mark N contiguous pages as used, with 'page' being the first. */
static void vmMarkPagesUsed(off_t page, off_t count) {
	off_t j;

	for (j = 0; j < count; j++)
		vmMarkPageUsed(page + j);
	server.vm_stats_used_pages += count;
}

/* Mark the page as free */
static void vmMarkPageFree(off_t page) {
	off_t byte = page / 8;
	int bit = page & 7;
	redisAssert(vmFreePage(page) == 0);
	server.vm_bitmap[byte] &= ~(1 << bit);
	redisLog(REDIS_DEBUG, "Mark free: %lld (byte:%lld bit:%d)\n",
	         (long long)page, (long long)byte, bit);
}

/* Mark N contiguous pages as free, with 'page' being the first. */
static void vmMarkPagesFree(off_t page, off_t count) {
	off_t j;

	for (j = 0; j < count; j++)
		vmMarkPageFree(page + j);
	server.vm_stats_used_pages -= count;
	if (server.vm_stats_used_pages > 100000000) {
		*((char*) - 1) = 'x';
	}
}

/* Test if the page is free */
static int vmFreePage(off_t page) {
	off_t byte = page / 8;
	int bit = page & 7;
	return (server.vm_bitmap[byte] & (1 << bit)) == 0;
}

/* Find N contiguous free pages storing the first page of the cluster in *first.
 * Returns REDIS_OK if it was able to find N contiguous pages, otherwise
 * REDIS_ERR is returned.
 *
 * This function uses a simple algorithm: we try to allocate
 * REDIS_VM_MAX_NEAR_PAGES sequentially, when we reach this limit we start
 * again from the start of the swap file searching for free spaces.
 *
 * If it looks pretty clear that there are no free pages near our offset
 * we try to find less populated places doing a forward jump of
 * REDIS_VM_MAX_RANDOM_JUMP, then we start scanning again a few pages
 * without hurry, and then we jump again and so forth...
 *
 * This function can be improved using a free list to avoid to guess
 * too much, since we could collect data about freed pages.
 *
 * note: I implemented this function just after watching an episode of
 * Battlestar Galactica, where the hybrid was continuing to say "JUMP!"
 */
static int vmFindContiguousPages(off_t *first, off_t n) {
	off_t base, offset = 0, since_jump = 0, numfree = 0;

	if (server.vm_near_pages == REDIS_VM_MAX_NEAR_PAGES) {
		server.vm_near_pages = 0;
		server.vm_next_page = 0;
	}
	server.vm_near_pages++; /* Yet another try for pages near to the old ones */
	base = server.vm_next_page;

	while (offset < server.vm_pages) {
		off_t this = base + offset;

		/* If we overflow, restart from page zero */
		if (this >= server.vm_pages) {
			this -= server.vm_pages;
			if (this == 0) {
				/* Just overflowed, what we found on tail is no longer
				 * interesting, as it's no longer contiguous. */
				numfree = 0;
			}
		}
		redisLog(REDIS_DEBUG, "THIS: %lld (%c)\n", (long long) this, vmFreePage(this) ? 'F' : 'X');
		if (vmFreePage(this)) {
			/* This is a free page */
			numfree++;
			/* Already got N free pages? Return to the caller, with success */
			if (numfree == n) {
				*first = this - (n - 1);
				server.vm_next_page = this + 1;
				return REDIS_OK;
			}
		} else {
			/* The current one is not a free page */
			numfree = 0;
		}

		/* Fast-forward if the current page is not free and we already
		 * searched enough near this place. */
		since_jump++;
		if (!numfree && since_jump >= REDIS_VM_MAX_RANDOM_JUMP / 4) {
			offset += random() % REDIS_VM_MAX_RANDOM_JUMP;
			since_jump = 0;
			/* Note that even if we rewind after the jump, we are don't need
			 * to make sure numfree is set to zero as we only jump *if* it
			 * is set to zero. */
		} else {
			/* Otherwise just check the next page */
			offset++;
		}
	}
	return REDIS_ERR;
}

/* Write the specified object at the specified page of the swap file */
static int vmWriteObjectOnSwap(robj *o, off_t page) {
	if (server.vm_enabled) pthread_mutex_lock(&server.io_swapfile_mutex);
	if (fseeko(server.vm_fp, page * server.vm_page_size, SEEK_SET) == -1) {
		if (server.vm_enabled) pthread_mutex_unlock(&server.io_swapfile_mutex);
		redisLog(REDIS_WARNING,
		         "Critical VM problem in vmSwapObjectBlocking(): can't seek: %s",
		         strerror(errno));
		return REDIS_ERR;
	}
	rdbSaveObject(server.vm_fp, o);
	if (server.vm_enabled) pthread_mutex_unlock(&server.io_swapfile_mutex);
	return REDIS_OK;
}

/* Swap the 'val' object relative to 'key' into disk. Store all the information
 * needed to later retrieve the object into the key object.
 * If we can't find enough contiguous empty pages to swap the object on disk
 * REDIS_ERR is returned. */
static int vmSwapObjectBlocking(robj *key, robj *val) {
	off_t pages = rdbSavedObjectPages(val, NULL);
	off_t page;

	assert(key->storage == REDIS_VM_MEMORY);
	assert(key->refcount == 1);
	if (vmFindContiguousPages(&page, pages) == REDIS_ERR) return REDIS_ERR;
	if (vmWriteObjectOnSwap(val, page) == REDIS_ERR) return REDIS_ERR;
	key->vm.page = page;
	key->vm.usedpages = pages;
	key->storage = REDIS_VM_SWAPPED;
	key->vtype = val->type;
	decrRefCount(val); /* Deallocate the object from memory. */
	vmMarkPagesUsed(page, pages);
	redisLog(REDIS_DEBUG, "VM: object %s swapped out at %lld (%lld pages)",
	         (unsigned char*) key->ptr,
	         (unsigned long long) page, (unsigned long long) pages);
	server.vm_stats_swapped_objects++;
	server.vm_stats_swapouts++;
	fflush(server.vm_fp);
	return REDIS_OK;
}

static robj *vmReadObjectFromSwap(off_t page, int type) {
	robj *o;

	if (server.vm_enabled) pthread_mutex_lock(&server.io_swapfile_mutex);
	if (fseeko(server.vm_fp, page * server.vm_page_size, SEEK_SET) == -1) {
		redisLog(REDIS_WARNING,
		         "Unrecoverable VM problem in vmLoadObject(): can't seek: %s",
		         strerror(errno));
		exit(1);
	}
	o = rdbLoadObject(type, server.vm_fp);
	if (o == NULL) {
		redisLog(REDIS_WARNING, "Unrecoverable VM problem in vmLoadObject(): can't load object from swap file: %s", strerror(errno));
		exit(1);
	}
	if (server.vm_enabled) pthread_mutex_unlock(&server.io_swapfile_mutex);
	return o;
}

/* Load the value object relative to the 'key' object from swap to memory.
 * The newly allocated object is returned.
 *
 * If preview is true the unserialized object is returned to the caller but
 * no changes are made to the key object, nor the pages are marked as freed */
static robj *vmGenericLoadObject(robj *key, int preview) {
	robj *val;

	redisAssert(key->storage == REDIS_VM_SWAPPED);
	val = vmReadObjectFromSwap(key->vm.page, key->vtype);
	if (!preview) {
		key->storage = REDIS_VM_MEMORY;
		key->vm.atime = server.unixtime;
		vmMarkPagesFree(key->vm.page, key->vm.usedpages);
		redisLog(REDIS_DEBUG, "VM: object %s loaded from disk",
		         (unsigned char*) key->ptr);
		server.vm_stats_swapped_objects--;
	} else {
		redisLog(REDIS_DEBUG, "VM: object %s previewed from disk",
		         (unsigned char*) key->ptr);
	}
	server.vm_stats_swapins++;
	return val;
}

/* Plain object loading, from swap to memory */
static robj *vmLoadObject(robj *key) {
	/* If we are loading the object in background, stop it, we
	 * need to load this object synchronously ASAP. */
	if (key->storage == REDIS_VM_LOADING)
		vmCancelThreadedIOJob(key);
	return vmGenericLoadObject(key, 0);
}

/* Just load the value on disk, without to modify the key.
 * This is useful when we want to perform some operation on the value
 * without to really bring it from swap to memory, like while saving the
 * dataset or rewriting the append only log. */
static robj *vmPreviewObject(robj *key) {
	return vmGenericLoadObject(key, 1);
}

/* How a good candidate is this object for swapping?
 * The better candidate it is, the greater the returned value.
 *
 * Currently we try to perform a fast estimation of the object size in
 * memory, and combine it with aging informations.
 *
 * Basically swappability = idle-time * log(estimated size)
 *
 * Bigger objects are preferred over smaller objects, but not
 * proportionally, this is why we use the logarithm. This algorithm is
 * just a first try and will probably be tuned later. */
static double computeObjectSwappability(robj *o) {
	time_t age = server.unixtime - o->vm.atime;
	long asize = 0;
	list *l;
	dict *d;
	struct dictEntry *de;
	int z;

	if (age <= 0) return 0;
	switch (o->type) {
	case REDIS_STRING:
		if (o->encoding != REDIS_ENCODING_RAW) {
			asize = sizeof(*o);
		} else {
			asize = sdslen(o->ptr) + sizeof(*o) + sizeof(long) * 2;
		}
		break;
	case REDIS_LIST:
		l = o->ptr;
		listNode *ln = listFirst(l);

		asize = sizeof(list);
		if (ln) {
			robj *ele = ln->value;
			long elesize;

			elesize = (ele->encoding == REDIS_ENCODING_RAW) ?
			          (sizeof(*o) + sdslen(ele->ptr)) :
			          sizeof(*o);
			asize += (sizeof(listNode) + elesize) * listLength(l);
		}
		break;
	case REDIS_SET:
	case REDIS_ZSET:
		z = (o->type == REDIS_ZSET);
		d = z ? ((zset*)o->ptr)->dict : o->ptr;

		asize = sizeof(dict) + (sizeof(struct dictEntry*)*dictSlots(d));
		if (z) asize += sizeof(zset) - sizeof(dict);
		if (dictSize(d)) {
			long elesize;
			robj *ele;

			de = dictGetRandomKey(d);
			ele = dictGetEntryKey(de);
			elesize = (ele->encoding == REDIS_ENCODING_RAW) ?
			          (sizeof(*o) + sdslen(ele->ptr)) :
			          sizeof(*o);
			asize += (sizeof(struct dictEntry) + elesize) * dictSize(d);
			if (z) asize += sizeof(zskiplistNode) * dictSize(d);
		}
		break;
	}
	return (double)asize * log(1 + asize);
}

/* Try to swap an object that's a good candidate for swapping.
 * Returns REDIS_OK if the object was swapped, REDIS_ERR if it's not possible
 * to swap any object at all.
 *
 * If 'usethreaded' is true, Redis will try to swap the object in background
 * using I/O threads. */
static int vmSwapOneObject(int usethreads) {
	int j, i;
	struct dictEntry *best = NULL;
	double best_swappability = 0;
	redisDb *best_db = NULL;
	robj *key, *val;

	for (j = 0; j < server.dbnum; j++) {
		redisDb *db = server.db + j;
		/* Why maxtries is set to 100?
		 * Because this way (usually) we'll find 1 object even if just 1% - 2%
		 * are swappable objects */
		int maxtries = 100;

		if (dictSize(db->dict) == 0) continue;
		for (i = 0; i < 5; i++) {
			dictEntry *de;
			double swappability;

			if (maxtries) maxtries--;
			de = dictGetRandomKey(db->dict);
			key = dictGetEntryKey(de);
			val = dictGetEntryVal(de);
			/* Only swap objects that are currently in memory.
			 *
			 * Also don't swap shared objects if threaded VM is on, as we
			 * try to ensure that the main thread does not touch the
			 * object while the I/O thread is using it, but we can't
			 * control other keys without adding additional mutex. */
			if (key->storage != REDIS_VM_MEMORY ||
			        (server.vm_max_threads != 0 && val->refcount != 1)) {
				if (maxtries) i--; /* don't count this try */
				continue;
			}
			swappability = computeObjectSwappability(val);
			if (!best || swappability > best_swappability) {
				best = de;
				best_swappability = swappability;
				best_db = db;
			}
		}
	}
	if (best == NULL) {
		redisLog(REDIS_DEBUG, "No swappable key found!");
		return REDIS_ERR;
	}
	key = dictGetEntryKey(best);
	val = dictGetEntryVal(best);

	redisLog(REDIS_DEBUG, "Key with best swappability: %s, %f",
	         key->ptr, best_swappability);

	/* Unshare the key if needed */
	if (key->refcount > 1) {
		robj *newkey = dupStringObject(key);
		decrRefCount(key);
		key = dictGetEntryKey(best) = newkey;
	}
	/* Swap it */
	if (usethreads) {
		vmSwapObjectThreaded(key, val, best_db);
		return REDIS_OK;
	} else {
		if (vmSwapObjectBlocking(key, val) == REDIS_OK) {
			dictGetEntryVal(best) = NULL;
			return REDIS_OK;
		} else {
			return REDIS_ERR;
		}
	}
}

static int vmSwapOneObjectBlocking() {
	return vmSwapOneObject(0);
}

static int vmSwapOneObjectThreaded() {
	return vmSwapOneObject(1);
}

/* Return true if it's safe to swap out objects in a given moment.
 * Basically we don't want to swap objects out while there is a BGSAVE
 * or a BGAEOREWRITE running in backgroud. */
static int vmCanSwapOut(void) {
	return (server.bgsavechildpid == -1 && server.bgrewritechildpid == -1);
}

/* Delete a key if swapped. Returns 1 if the key was found, was swapped
 * and was deleted. Otherwise 0 is returned. */
static int deleteIfSwapped(redisDb *db, robj *key) {
	dictEntry *de;
	robj *foundkey;

	if ((de = dictFind(db->dict, key)) == NULL) return 0;
	foundkey = dictGetEntryKey(de);
	if (foundkey->storage == REDIS_VM_MEMORY) return 0;
	deleteKey(db, key);
	return 1;
}

/* =================== Virtual Memory - Threaded I/O  ======================= */

static void freeIOJob(iojob *j) {
	if (j->type == REDIS_IOJOB_PREPARE_SWAP ||
	        j->type == REDIS_IOJOB_DO_SWAP)
		decrRefCount(j->val);
	decrRefCount(j->key);
	zfree(j);
}

/* Every time a thread finished a Job, it writes a byte into the write side
 * of an unix pipe in order to "awake" the main thread, and this function
 * is called. */
static void vmThreadedIOCompletedJob(aeEventLoop *el, int fd, void *privdata,
                                     int mask)
{
	char buf[1];
	int retval, processed = 0, toprocess = -1, trytoswap = 1;
	REDIS_NOTUSED(el);
	REDIS_NOTUSED(mask);
	REDIS_NOTUSED(privdata);

	/* For every byte we read in the read side of the pipe, there is one
	 * I/O job completed to process. */
	while ((retval = read(fd, buf, 1)) == 1) {
		iojob *j;
		listNode *ln;
		robj *key;
		struct dictEntry *de;

		redisLog(REDIS_DEBUG, "Processing I/O completed job");

		/* Get the processed element (the oldest one) */
		lockThreadedIO();
		assert(listLength(server.io_processed) != 0);
		if (toprocess == -1) {
			toprocess = (listLength(server.io_processed) * REDIS_MAX_COMPLETED_JOBS_PROCESSED) / 100;
			if (toprocess <= 0) toprocess = 1;
		}
		ln = listFirst(server.io_processed);
		j = ln->value;
		listDelNode(server.io_processed, ln);
		unlockThreadedIO();
		/* If this job is marked as canceled, just ignore it */
		if (j->canceled) {
			freeIOJob(j);
			continue;
		}
		/* Post process it in the main thread, as there are things we
		 * can do just here to avoid race conditions and/or invasive locks */
		redisLog(REDIS_DEBUG, "Job %p type: %d, key at %p (%s) refcount: %d\n", (void*) j, j->type, (void*)j->key, (char*)j->key->ptr, j->key->refcount);
		de = dictFind(j->db->dict, j->key);
		assert(de != NULL);
		key = dictGetEntryKey(de);
		if (j->type == REDIS_IOJOB_LOAD) {
			/* Key loaded, bring it at home */
			key->storage = REDIS_VM_MEMORY;
			key->vm.atime = server.unixtime;
			vmMarkPagesFree(key->vm.page, key->vm.usedpages);
			redisLog(REDIS_DEBUG, "VM: object %s loaded from disk (threaded)",
			         (unsigned char*) key->ptr);
			server.vm_stats_swapped_objects--;
			server.vm_stats_swapins++;
			freeIOJob(j);
		} else if (j->type == REDIS_IOJOB_PREPARE_SWAP) {
			/* Now we know the amount of pages required to swap this object.
			 * Let's find some space for it, and queue this task again
			 * rebranded as REDIS_IOJOB_DO_SWAP. */
			if (!vmCanSwapOut() ||
			        vmFindContiguousPages(&j->page, j->pages) == REDIS_ERR)
			{
				/* Ooops... no space or we can't swap as there is
				 * a fork()ed Redis trying to save stuff on disk. */
				freeIOJob(j);
				key->storage = REDIS_VM_MEMORY; /* undo operation */
			} else {
				/* Note that we need to mark this pages as used now,
				 * if the job will be canceled, we'll mark them as freed
				 * again. */
				vmMarkPagesUsed(j->page, j->pages);
				j->type = REDIS_IOJOB_DO_SWAP;
				lockThreadedIO();
				queueIOJob(j);
				unlockThreadedIO();
			}
		} else if (j->type == REDIS_IOJOB_DO_SWAP) {
			robj *val;

			/* Key swapped. We can finally free some memory. */
			if (key->storage != REDIS_VM_SWAPPING) {
				printf("key->storage: %d\n", key->storage);
				printf("key->name: %s\n", (char*)key->ptr);
				printf("key->refcount: %d\n", key->refcount);
				printf("val: %p\n", (void*)j->val);
				printf("val->type: %d\n", j->val->type);
				printf("val->ptr: %s\n", (char*)j->val->ptr);
			}
			redisAssert(key->storage == REDIS_VM_SWAPPING);
			val = dictGetEntryVal(de);
			key->vm.page = j->page;
			key->vm.usedpages = j->pages;
			key->storage = REDIS_VM_SWAPPED;
			key->vtype = j->val->type;
			decrRefCount(val); /* Deallocate the object from memory. */
			dictGetEntryVal(de) = NULL;
			redisLog(REDIS_DEBUG,
			         "VM: object %s swapped out at %lld (%lld pages) (threaded)",
			         (unsigned char*) key->ptr,
			         (unsigned long long) j->page, (unsigned long long) j->pages);
			server.vm_stats_swapped_objects++;
			server.vm_stats_swapouts++;
			freeIOJob(j);
			/* Put a few more swap requests in queue if we are still
			 * out of memory */
			if (trytoswap && vmCanSwapOut() &&
			        zmalloc_used_memory() > server.vm_max_memory)
			{
				int more = 1;
				while (more) {
					lockThreadedIO();
					more = listLength(server.io_newjobs) <
					       (unsigned) server.vm_max_threads;
					unlockThreadedIO();
					/* Don't waste CPU time if swappable objects are rare. */
					if (vmSwapOneObjectThreaded() == REDIS_ERR) {
						trytoswap = 0;
						break;
					}
				}
			}
		}
		processed++;
		if (processed == toprocess) return;
	}
	if (retval < 0 && errno != EAGAIN) {
		redisLog(REDIS_WARNING,
		         "WARNING: read(2) error in vmThreadedIOCompletedJob() %s",
		         strerror(errno));
	}
}

static void lockThreadedIO(void) {
	pthread_mutex_lock(&server.io_mutex);
}

static void unlockThreadedIO(void) {
	pthread_mutex_unlock(&server.io_mutex);
}

/* Remove the specified object from the threaded I/O queue if still not
 * processed, otherwise make sure to flag it as canceled. */
static void vmCancelThreadedIOJob(robj *o) {
	list *lists[3] = {
		server.io_newjobs,      /* 0 */
		server.io_processing,   /* 1 */
		server.io_processed     /* 2 */
	};
	int i;

	assert(o->storage == REDIS_VM_LOADING || o->storage == REDIS_VM_SWAPPING);
again:
	lockThreadedIO();
	/* Search for a matching key in one of the queues */
	for (i = 0; i < 3; i++) {
		listNode *ln;
		listIter li;

		listRewind(lists[i], &li);
		while ((ln = listNext(&li)) != NULL) {
			iojob *job = ln->value;

			if (job->canceled) continue; /* Skip this, already canceled. */
			if (compareStringObjects(job->key, o) == 0) {
				redisLog(REDIS_DEBUG, "*** CANCELED %p (%s) (type %d) (LIST ID %d)\n",
				         (void*)job, (char*)o->ptr, job->type, i);
				/* Mark the pages as free since the swap didn't happened
				 * or happened but is now discarded. */
				if (i != 1 && job->type == REDIS_IOJOB_DO_SWAP)
					vmMarkPagesFree(job->page, job->pages);
				/* Cancel the job. It depends on the list the job is
				 * living in. */
				switch (i) {
				case 0: /* io_newjobs */
					/* If the job was yet not processed the best thing to do
					 * is to remove it from the queue at all */
					freeIOJob(job);
					listDelNode(lists[i], ln);
					break;
				case 1: /* io_processing */
					/* Oh Shi- the thread is messing with the Job, and
					 * probably with the object if this is a
					 * PREPARE_SWAP or DO_SWAP job. Better to wait for the
					 * job to move into the next queue... */
					if (job->type != REDIS_IOJOB_LOAD) {
						/* Yes, we try again and again until the job
						 * is completed. */
						unlockThreadedIO();
						/* But let's wait some time for the I/O thread
						 * to finish with this job. After all this condition
						 * should be very rare. */
						usleep(1);
						goto again;
					} else {
						job->canceled = 1;
						break;
					}
				case 2: /* io_processed */
					/* The job was already processed, that's easy...
					 * just mark it as canceled so that we'll ignore it
					 * when processing completed jobs. */
					job->canceled = 1;
					break;
				}
				/* Finally we have to adjust the storage type of the object
				 * in order to "UNDO" the operaiton. */
				if (o->storage == REDIS_VM_LOADING)
					o->storage = REDIS_VM_SWAPPED;
				else if (o->storage == REDIS_VM_SWAPPING)
					o->storage = REDIS_VM_MEMORY;
				unlockThreadedIO();
				return;
			}
		}
	}
	unlockThreadedIO();
	assert(1 != 1); /* We should never reach this */
}

static void *IOThreadEntryPoint(void *arg) {
	iojob *j;
	listNode *ln;
	REDIS_NOTUSED(arg);

	pthread_detach(pthread_self());
	while (1) {
		/* Get a new job to process */
		lockThreadedIO();
		if (listLength(server.io_newjobs) == 0) {
			/* No new jobs in queue, exit. */
			redisLog(REDIS_DEBUG, "Thread %lld exiting, nothing to do",
			         (long long) pthread_self());
			server.io_active_threads--;
			unlockThreadedIO();
			return NULL;
		}
		ln = listFirst(server.io_newjobs);
		j = ln->value;
		listDelNode(server.io_newjobs, ln);
		/* Add the job in the processing queue */
		j->thread = pthread_self();
		listAddNodeTail(server.io_processing, j);
		ln = listLast(server.io_processing); /* We use ln later to remove it */
		unlockThreadedIO();
		redisLog(REDIS_DEBUG, "Thread %lld got a new job (type %d): %p about key '%s'",
		         (long long) pthread_self(), j->type, (void*)j, (char*)j->key->ptr);

		/* Process the Job */
		if (j->type == REDIS_IOJOB_LOAD) {
		} else if (j->type == REDIS_IOJOB_PREPARE_SWAP) {
			FILE *fp = fopen("/dev/null", "w+");
			j->pages = rdbSavedObjectPages(j->val, fp);
			fclose(fp);
		} else if (j->type == REDIS_IOJOB_DO_SWAP) {
			if (vmWriteObjectOnSwap(j->val, j->page) == REDIS_ERR)
				j->canceled = 1;
		}

		/* Done: insert the job into the processed queue */
		redisLog(REDIS_DEBUG, "Thread %lld completed the job: %p (key %s)",
		         (long long) pthread_self(), (void*)j, (char*)j->key->ptr);
		lockThreadedIO();
		listDelNode(server.io_processing, ln);
		listAddNodeTail(server.io_processed, j);
		unlockThreadedIO();

		/* Signal the main thread there is new stuff to process */
		assert(write(server.io_ready_pipe_write, "x", 1) == 1);
	}
	return NULL; /* never reached */
}

static void spawnIOThread(void) {
	pthread_t thread;

	pthread_create(&thread, &server.io_threads_attr, IOThreadEntryPoint, NULL);
	server.io_active_threads++;
}

/* We need to wait for the last thread to exit before we are able to
 * fork() in order to BGSAVE or BGREWRITEAOF. */
static void waitEmptyIOJobsQueue(void) {
	while (1) {
		int io_processed_len;

		lockThreadedIO();
		if (listLength(server.io_newjobs) == 0 &&
		        listLength(server.io_processing) == 0 &&
		        server.io_active_threads == 0)
		{
			unlockThreadedIO();
			return;
		}
		/* While waiting for empty jobs queue condition we post-process some
		 * finshed job, as I/O threads may be hanging trying to write against
		 * the io_ready_pipe_write FD but there are so much pending jobs that
		 * it's blocking. */
		io_processed_len = listLength(server.io_processed);
		unlockThreadedIO();
		if (io_processed_len) {
			vmThreadedIOCompletedJob(NULL, server.io_ready_pipe_read, NULL, 0);
			usleep(1000); /* 1 millisecond */
		} else {
			usleep(10000); /* 10 milliseconds */
		}
	}
}

static void vmReopenSwapFile(void) {
	fclose(server.vm_fp);
	server.vm_fp = fopen(server.vm_swap_file, "r+b");
	if (server.vm_fp == NULL) {
		redisLog(REDIS_WARNING, "Can't re-open the VM swap file: %s. Exiting.",
		         server.vm_swap_file);
		exit(1);
	}
	server.vm_fd = fileno(server.vm_fp);
}

/* This function must be called while with threaded IO locked */
static void queueIOJob(iojob *j) {
	redisLog(REDIS_DEBUG, "Queued IO Job %p type %d about key '%s'\n",
	         (void*)j, j->type, (char*)j->key->ptr);
	listAddNodeTail(server.io_newjobs, j);
	if (server.io_active_threads < server.vm_max_threads)
		spawnIOThread();
}

static int vmSwapObjectThreaded(robj *key, robj *val, redisDb *db) {
	iojob *j;

	assert(key->storage == REDIS_VM_MEMORY);
	assert(key->refcount == 1);

	j = zmalloc(sizeof(*j));
	j->type = REDIS_IOJOB_PREPARE_SWAP;
	j->db = db;
	j->key = dupStringObject(key);
	j->val = val;
	incrRefCount(val);
	j->canceled = 0;
	j->thread = (pthread_t) - 1;
	key->storage = REDIS_VM_SWAPPING;

	lockThreadedIO();
	queueIOJob(j);
	unlockThreadedIO();
	return REDIS_OK;
}

/* ============ Virtual Memory - Blocking clients on missing keys =========== */

/* Is this client attempting to run a command against swapped keys?
 * If so, block it ASAP, load the keys in background, then resume it.4
 *
 * The improtat thing about this function is that it can fail! If keys will
 * still be swapped when the client is resumed, a few of key lookups will
 * just block loading keys from disk. */
#if 0
static void blockClientOnSwappedKeys(redisClient *c) {
}
#endif

/* ================================= Debugging ============================== */

static void debugCommand(redisClient *c) {
	if (!strcasecmp(c->argv[1]->ptr, "segfault")) {
		*((char*) - 1) = 'x';
	} else if (!strcasecmp(c->argv[1]->ptr, "reload")) {
		if (rdbSave(server.dbfilename) != REDIS_OK) {
			addReply(c, shared.err);
			return;
		}
		emptyDb();
		if (rdbLoad(server.dbfilename) != REDIS_OK) {
			addReply(c, shared.err);
			return;
		}
		redisLog(REDIS_WARNING, "DB reloaded by DEBUG RELOAD");
		addReply(c, shared.ok);
	} else if (!strcasecmp(c->argv[1]->ptr, "loadaof")) {
		emptyDb();
		if (loadAppendOnlyFile(server.appendfilename) != REDIS_OK) {
			addReply(c, shared.err);
			return;
		}
		redisLog(REDIS_WARNING, "Append Only File loaded by DEBUG LOADAOF");
		addReply(c, shared.ok);
	} else if (!strcasecmp(c->argv[1]->ptr, "object") && c->argc == 3) {
		dictEntry *de = dictFind(c->db->dict, c->argv[2]);
		robj *key, *val;

		if (!de) {
			addReply(c, shared.nokeyerr);
			return;
		}
		key = dictGetEntryKey(de);
		val = dictGetEntryVal(de);
		if (server.vm_enabled && (key->storage == REDIS_VM_MEMORY ||
		                          key->storage == REDIS_VM_SWAPPING)) {
			addReplySds(c, sdscatprintf(sdsempty(),
			                            "+Key at:%p refcount:%d, value at:%p refcount:%d "
			                            "encoding:%d serializedlength:%lld\r\n",
			                            (void*)key, key->refcount, (void*)val, val->refcount,
			                            val->encoding, (long long) rdbSavedObjectLen(val, NULL)));
		} else {
			addReplySds(c, sdscatprintf(sdsempty(),
			                            "+Key at:%p refcount:%d, value swapped at: page %llu "
			                            "using %llu pages\r\n",
			                            (void*)key, key->refcount, (unsigned long long) key->vm.page,
			                            (unsigned long long) key->vm.usedpages));
		}
	} else if (!strcasecmp(c->argv[1]->ptr, "swapout") && c->argc == 3) {
		dictEntry *de = dictFind(c->db->dict, c->argv[2]);
		robj *key, *val;

		if (!server.vm_enabled) {
			addReplySds(c, sdsnew("-ERR Virtual Memory is disabled\r\n"));
			return;
		}
		if (!de) {
			addReply(c, shared.nokeyerr);
			return;
		}
		key = dictGetEntryKey(de);
		val = dictGetEntryVal(de);
		/* If the key is shared we want to create a copy */
		if (key->refcount > 1) {
			robj *newkey = dupStringObject(key);
			decrRefCount(key);
			key = dictGetEntryKey(de) = newkey;
		}
		/* Swap it */
		if (key->storage != REDIS_VM_MEMORY) {
			addReplySds(c, sdsnew("-ERR This key is not in memory\r\n"));
		} else if (vmSwapObjectBlocking(key, val) == REDIS_OK) {
			dictGetEntryVal(de) = NULL;
			addReply(c, shared.ok);
		} else {
			addReply(c, shared.err);
		}
	} else {
		addReplySds(c, sdsnew(
		                "-ERR Syntax error, try DEBUG [SEGFAULT|OBJECT <key>|SWAPOUT <key>|RELOAD]\r\n"));
	}
}

static void _redisAssert(char *estr, char *file, int line) {
	redisLog(REDIS_WARNING, "=== ASSERTION FAILED ===");
	redisLog(REDIS_WARNING, "==> %s:%d '%s' is not true\n", file, line, estr);
#ifdef HAVE_BACKTRACE
	redisLog(REDIS_WARNING, "(forcing SIGSEGV in order to print the stack trace)");
	*((char*) - 1) = 'x';
#endif
}

/* =================================== Main! ================================ */

#ifdef __linux__
int linuxOvercommitMemoryValue(void) {
	FILE *fp = fopen("/proc/sys/vm/overcommit_memory", "r");
	char buf[64];

	if (!fp) return -1;
	if (fgets(buf, 64, fp) == NULL) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	return atoi(buf);
}

void linuxOvercommitMemoryWarning(void) {
	if (linuxOvercommitMemoryValue() == 0) {
		redisLog(REDIS_WARNING, "WARNING overcommit_memory is set to 0! Background save may fail under low condition memory. To fix this issue add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and then reboot or run the command 'sysctl vm.overcommit_memory=1' for this to take effect.");
	}
}
#endif /* __linux__ */

static void daemonize(void) {
	int fd;
	FILE *fp;

	if (fork() != 0) exit(0); /* parent exits */
	setsid(); /* create a new session */

	/* Every output goes to /dev/null. If Redis is daemonized but
	 * the 'logfile' is set to 'stdout' in the configuration file
	 * it will not log at all. */
	if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		if (fd > STDERR_FILENO) close(fd);
	}
	/* Try to write the pid file */
	fp = fopen(server.pidfile, "w");
	if (fp) {
		fprintf(fp, "%d\n", getpid());
		fclose(fp);
	}
}

int main(int argc, char **argv) {
	initServerConfig();
	if (argc == 2) {
		resetServerSaveParams();
		loadServerConfig(argv[1]);
	} else if (argc > 2) {
		fprintf(stderr, "Usage: ./redis-server [/path/to/redis.conf]\n");
		exit(1);
	} else {
		redisLog(REDIS_WARNING, "Warning: no config file specified, using the default config. In order to specify a config file use 'redis-server /path/to/redis.conf'");
	}
	if (server.daemonize) daemonize();
	initServer();
	redisLog(REDIS_NOTICE, "Server started, Redis version " REDIS_VERSION);
#ifdef __linux__
	linuxOvercommitMemoryWarning();
#endif
	if (server.appendonly) {
		if (loadAppendOnlyFile(server.appendfilename) == REDIS_OK)
			redisLog(REDIS_NOTICE, "DB loaded from append only file");
	} else {
		if (rdbLoad(server.dbfilename) == REDIS_OK)
			redisLog(REDIS_NOTICE, "DB loaded from disk");
	}
	redisLog(REDIS_NOTICE, "The server is now ready to accept connections on port %d", server.port);
	aeMain(server.el);
	aeDeleteEventLoop(server.el);
	return 0;
}

/* ============================= Backtrace support ========================= */

#ifdef HAVE_BACKTRACE
static char *findFuncName(void *pointer, unsigned long *offset);

static void *getMcontextEip(ucontext_t *uc) {
#if defined(__FreeBSD__)
	return (void*) uc->uc_mcontext.mc_eip;
#elif defined(__dietlibc__)
	return (void*) uc->uc_mcontext.eip;
#elif defined(__APPLE__) && !defined(MAC_OS_X_VERSION_10_6)
#if __x86_64__
	return (void*) uc->uc_mcontext->__ss.__rip;
#else
	return (void*) uc->uc_mcontext->__ss.__eip;
#endif
#elif defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)
#if defined(_STRUCT_X86_THREAD_STATE64) && !defined(__i386__)
	return (void*) uc->uc_mcontext->__ss.__rip;
#else
	return (void*) uc->uc_mcontext->__ss.__eip;
#endif
#elif defined(__i386__) || defined(__X86_64__)  || defined(__x86_64__)
	return (void*) uc->uc_mcontext.gregs[REG_EIP]; /* Linux 32/64 bit */
#elif defined(__ia64__) /* Linux IA64 */
	return (void*) uc->uc_mcontext.sc_ip;
#else
	return NULL;
#endif
}

static void segvHandler(int sig, siginfo_t *info, void *secret) {
	void *trace[100];
	char **messages = NULL;
	int i, trace_size = 0;
	unsigned long offset = 0;
	ucontext_t *uc = (ucontext_t*) secret;
	sds infostring;
	REDIS_NOTUSED(info);

	redisLog(REDIS_WARNING,
	         "======= Ooops! Redis %s got signal: -%d- =======", REDIS_VERSION, sig);
	infostring = genRedisInfoString();
	redisLog(REDIS_WARNING, "%s", infostring);
	/* It's not safe to sdsfree() the returned string under memory
	 * corruption conditions. Let it leak as we are going to abort */
	// 返回调用堆栈信息
	trace_size = backtrace(trace, 100);
	/* overwrite sigaction with caller's address */
	if (getMcontextEip(uc) != NULL) {
		trace[1] = getMcontextEip(uc);
	}
	// 将调用堆栈地址转换为可读字符串
	messages = backtrace_symbols(trace, trace_size);

	for (i = 1; i < trace_size; ++i) {
		// 根据地址查找到调用的函数名称（预先在staticsymbols.h头文件中定义好了）
		char *fn = findFuncName(trace[i], &offset), *p;

		p = strchr(messages[i], '+');
		if (!fn || (p && ((unsigned long)strtol(p + 1, NULL, 10)) < offset)) {
			redisLog(REDIS_WARNING, "%s", messages[i]);
		} else {
			redisLog(REDIS_WARNING, "%d redis-server %p %s + %d", i, trace[i], fn, (unsigned int)offset);
		}
	}
	/* free(messages); Don't call free() with possibly corrupted memory. */
	exit(0);
}

// 设置信号处理句柄，用于捕获硬件错误产生的信号，输出上下文，方便定位问题
static void setupSigSegvAction(void) {
	struct sigaction act;

	sigemptyset (&act.sa_mask);
	/* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction
	 * is used. Otherwise, sa_handler is used */
	act.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND | SA_SIGINFO;
	act.sa_sigaction = segvHandler;
	// 无效内存引用
	sigaction (SIGSEGV, &act, NULL);
	// 硬件故障，如内存故障
	sigaction (SIGBUS, &act, NULL);
	// 算术运算异常，如除0
	sigaction (SIGFPE, &act, NULL);
	// 执行了非法硬件指令
	sigaction (SIGILL, &act, NULL);
	sigaction (SIGBUS, &act, NULL);
	return;
}

#include "staticsymbols.h"
/* This function try to convert a pointer into a function name. It's used in
 * oreder to provide a backtrace under segmentation fault that's able to
 * display functions declared as static (otherwise the backtrace is useless). */
static char *findFuncName(void *pointer, unsigned long *offset) {
	int i, ret = -1;
	unsigned long off, minoff = 0;

	/* Try to match against the Symbol with the smallest offset */
	for (i = 0; symsTable[i].pointer; i++) {
		unsigned long lp = (unsigned long) pointer;

		if (lp != (unsigned long) - 1 && lp >= symsTable[i].pointer) {
			off = lp - symsTable[i].pointer;
			if (ret < 0 || off < minoff) {
				minoff = off;
				ret = i;
			}
		}
	}
	if (ret == -1) return NULL;
	*offset = minoff;
	return symsTable[ret].name;
}
#else /* HAVE_BACKTRACE */
static void setupSigSegvAction(void) {
}
#endif /* HAVE_BACKTRACE */



/* The End */



