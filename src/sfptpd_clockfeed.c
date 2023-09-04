/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2023 Advanced Micro Devices, Inc. */

/**
 * @file   sfptpd_clockfeed.c
 * @brief  Feed of clock differences/timestamps
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>

#include "sfptpd_app.h"
#include "sfptpd_logging.h"
#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_constants.h"
#include "sfptpd_clock.h"
#include "sfptpd_thread.h"
#include "sfptpd_interface.h"
#include "sfptpd_statistics.h"
#include "sfptpd_time.h"
#include "sfptpd_engine.h"

#include "sfptpd_clockfeed.h"


/****************************************************************************
 * Constants
 ****************************************************************************/

#define CLOCKFEED_MODULE_MAGIC     0xC10CFEED0030D01EULL
#define CLOCKFEED_SOURCE_MAGIC     0xC10CFEED00005005ULL
#define CLOCKFEED_SHM_MAGIC        0xC10CFEED00005443ULL
#define CLOCKFEED_SUBSCRIBER_MAGIC 0xC10CFEED50B5C1BEULL
#define CLOCKFEED_DELETED_MAGIC    0xD0D00EC5C10CFEEDULL

#define CLOCK_POLL_TIMER_ID (0)

#define MAX_CLOCK_SAMPLES_LOG2 (4)
#define MAX_CLOCK_SAMPLES      (1 << MAX_CLOCK_SAMPLES_LOG2)

#define MAX_EVENT_SUBSCRIBERS (4)

/****************************************************************************
 * Clock feed messages
 ****************************************************************************/

/* Macro used to define message ID values for clock feed messages */
#define CLOCKFEED_MSG(x) SFPTPD_CLOCKFEED_MSG(x)

/* Add a clock source.
 * It is an synchronous message.
 */
#define CLOCKFEED_MSG_ADD_CLOCK   CLOCKFEED_MSG(1)
struct clockfeed_add_clock {
	struct sfptpd_clock *clock;
	int poll_period_log2;
};

/* Remove a clock source.
 * It is an synchronous message.
 */
#define CLOCKFEED_MSG_REMOVE_CLOCK   CLOCKFEED_MSG(2)
struct clockfeed_remove_clock {
	struct sfptpd_clock *clock;
};

/* Subscribe to a clock source.
 * It is a synchronous message with a reply.
 */
#define CLOCKFEED_MSG_SUBSCRIBE   CLOCKFEED_MSG(3)
struct clockfeed_subscribe_req {
	struct sfptpd_clock *clock;
};
struct clockfeed_subscribe_resp {
	struct sfptpd_clockfeed_sub *sub;
};

/* Unsubscribe from a clock source.
 * It is a synchronous message.
 */
#define CLOCKFEED_MSG_UNSUBSCRIBE   CLOCKFEED_MSG(4)
struct clockfeed_unsubscribe {
	struct sfptpd_clockfeed_sub *sub;
};

/* Notification that a cycle of processing all ready clock feeds has
 * been completed. This value is defined in the public header file.
 * It is an asynchronous message with no reply.
 */
#define CLOCKFEED_MSG_SYNC_EVENT   SFPTPD_CLOCKFEED_MSG_SYNC_EVENT

/* Subscribe to clock feed events
 * It is a synchronous message.
 */
#define CLOCKFEED_MSG_SUBSCRIBE_EVENTS  CLOCKFEED_MSG(6)
struct clockfeed_subscribe_events {
	struct sfptpd_thread *thread;
};

/* Unsubscribe to clock feed events
 * It is an synchronous message.
 */
#define CLOCKFEED_MSG_UNSUBSCRIBE_EVENTS  CLOCKFEED_MSG(7)
struct clockfeed_unsubscribe_events {
	struct sfptpd_thread *thread;
};

/* Union of all clock feed messages
 * @hdr Standard message header
 * @u Union of message payloads
 */
struct clockfeed_msg {
	sfptpd_msg_hdr_t hdr;
	union {
		struct clockfeed_add_clock add_clock;
		struct clockfeed_remove_clock remove_clock;
		struct clockfeed_subscribe_req subscribe_req;
		struct clockfeed_subscribe_resp subscribe_resp;
		struct clockfeed_unsubscribe unsubscribe;
		struct clockfeed_subscribe_events subscribe_events;
		struct clockfeed_unsubscribe_events unsubscribe_events;
	} u;
};

STATIC_ASSERT(sizeof(struct clockfeed_msg) < SFPTPD_SIZE_GLOBAL_MSGS);


/****************************************************************************
 * Types
 ****************************************************************************/

struct clockfeed_source;

struct clockfeed_shm {
	struct sfptpd_clockfeed_sample samples[MAX_CLOCK_SAMPLES];
	uint64_t magic;
	uint64_t write_counter;
};

struct sfptpd_clockfeed_sub {
	uint64_t magic;

	/* Read-only reference to source info and SHM */
	struct clockfeed_source *source;

	/* Sample counter for last read sample */
	int64_t read_counter;

	/* Minimum counter for next read sample */
	int64_t min_counter;

	/* Flags */
	bool have_max_age:1;
	bool have_max_age_diff:1;

	/* Maximum age of sample */
	struct timespec max_age;

	/* Maximum age difference of samples */
	struct timespec max_age_diff;

	/* Linked list of subscribers to source */
	struct sfptpd_clockfeed_sub *next;
};

struct clockfeed_source {
	uint64_t magic;

	/* Pointer to clock source */
	struct sfptpd_clock *clock;

	/* Log2 of the period to poll this source */
	int poll_period_log2;

	/* Counters */
	uint64_t cycles;

	/* Samples */
	struct clockfeed_shm shm;

	/* Subscribers */
	struct sfptpd_clockfeed_sub *subscribers;

	/* Next source in list */
	struct clockfeed_source *next;

	/* Is inactive */
	bool inactive;
};

struct sfptpd_clockfeed {
	uint64_t magic;

	/* Pointer to sync-engine */
	struct sfptpd_engine *engine;

	/* Clock feed Thread */
	struct sfptpd_thread *thread;

	/* Log2 of the period to poll overall */
	int poll_period_log2;

	/* Whether we have entered the RUNning phase */
	bool running_phase;

	/* Linked list of live clock sources */
	struct clockfeed_source *active;

	/* Linked list of removed (zombie) clock sources */
	struct clockfeed_source *inactive;

	/* Event subscribers */
	struct sfptpd_thread *event_subscribers[MAX_EVENT_SUBSCRIBERS];
};


/****************************************************************************
 * Global variables
 ****************************************************************************/

const static struct sfptpd_clockfeed *sfptpd_clockfeed = NULL;


/****************************************************************************
 * Function prototypes
 ****************************************************************************/



/****************************************************************************
 * Internal Functions
 ****************************************************************************/

void clockfeed_dump_state(struct sfptpd_clockfeed *clockfeed)
{
	struct sfptpd_clockfeed_sub *subscriber;
	struct clockfeed_source *source;
	int i;

	TRACE_L5("clockfeed: dumping state:\n");
	TRACE_L5("clockfeed:  event subscribers:\n");
	for (i = 0; i < MAX_EVENT_SUBSCRIBERS; i++)
		if (clockfeed->event_subscribers[i])
			TRACE_L5("clockfeed:   - thread %p\n", clockfeed->event_subscribers[i]);

	for (i = 0; i < 2; i ++) {
		const char *which[] = { "active", "inactive" };
		TRACE_L5("clockfeed:  %s sources:\n", which[i]);
		for (source = (i == 0 ? clockfeed->active : clockfeed->inactive); source; source = source->next) {
			TRACE_L5("clockfeed:   - clock %s\n", sfptpd_clock_get_short_name(source->clock));
			TRACE_L5("clockfeed:      write_counter %d\n", source->shm.write_counter);
			TRACE_L5("clockfeed:      subscribers:\n");
			for (subscriber = source->subscribers; subscriber; subscriber = subscriber->next) {
				TRACE_L5("clockfeed:     - subscriber %p\n", subscriber);
				TRACE_L5("clockfeed:        read_counter %d\n", subscriber->read_counter);
				TRACE_L5("clockfeed:        min_counter %d\n", subscriber->min_counter);
			}
		}
	}
}

static void clockfeed_send_sync_event(struct sfptpd_clockfeed *clockfeed)
{
	struct clockfeed_msg *msg;
	int i;

	assert(clockfeed != NULL);
	assert(clockfeed->magic == CLOCKFEED_MODULE_MAGIC);

	for (i = 0; i < MAX_EVENT_SUBSCRIBERS; i++) {
		if (clockfeed->event_subscribers[i]) {

			msg = (struct clockfeed_msg *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_LOCAL, false);
			if (msg == NULL) {
				SFPTPD_MSG_LOG_ALLOC_FAILED("local");
				/* Sit out this event if there is back-pressure */
			} else {
				SFPTPD_MSG_SEND(msg, clockfeed->event_subscribers[i],
						SFPTPD_CLOCKFEED_MSG_SYNC_EVENT, false);
			}
		}
	}
}

static void clockfeed_reap_zombies(struct sfptpd_clockfeed *module,
				   struct clockfeed_source *source)
{
	assert(module);
	assert(source);
	assert(module->magic == CLOCKFEED_MODULE_MAGIC);
	assert(source->magic == CLOCKFEED_SOURCE_MAGIC);

	if (source->inactive && source->subscribers == NULL) {
		struct clockfeed_source **nextp;

		TRACE_L6("clockfeed: removing source %s\n",
			 sfptpd_clock_get_short_name(source->clock));

		for (nextp = &module->inactive;
		     *nextp && (*nextp != source);
		     nextp = &(*nextp)->next)
			assert((*nextp)->magic == CLOCKFEED_SOURCE_MAGIC);

		assert(*nextp == source);

		*nextp = source->next;
		source->magic = CLOCKFEED_DELETED_MAGIC;
		free(source);
	}
}

static void clockfeed_on_timer(void *user_context, unsigned int id)
{
	struct sfptpd_clockfeed *clockfeed = (struct sfptpd_clockfeed *)user_context;
	struct clockfeed_source *source;
	const uint32_t index_mask = (1 << MAX_CLOCK_SAMPLES_LOG2) - 1;

	assert(clockfeed != NULL);
	assert(clockfeed->magic == CLOCKFEED_MODULE_MAGIC);

	for (source = clockfeed->active; source; source = source->next) {
		const int cadence = source->poll_period_log2 - clockfeed->poll_period_log2;
		const uint64_t cadence_mask = (1 << cadence) - 1;

		if ((source->cycles & cadence_mask) == 0) {
			uint32_t index = source->shm.write_counter & index_mask;
			struct sfptpd_clockfeed_sample *record = &source->shm.samples[index];
			struct timespec diff;

			record->seq = source->shm.write_counter;
			record->rc = sfptpd_clock_compare(source->clock,
						  sfptpd_clock_get_system_clock(),
						  &diff);

			clock_gettime(CLOCK_MONOTONIC, &record->mono);
			clock_gettime(CLOCK_REALTIME, &record->system);

			if (record->rc == 0) {
				sfptpd_time_add(&record->snapshot,
						&record->system,
						&diff);
			} else {
				record->snapshot.tv_sec = 0UL;
				record->snapshot.tv_nsec = 0UL;
			}

			TRACE_L6("clockfeed %s: %llu: %llu: %d: %llu.%09llu %llu.%09llu\n",
				 sfptpd_clock_get_short_name(source->clock),
				 source->cycles, source->shm.write_counter, record->rc,
				 record->system.tv_sec, record->system.tv_nsec,
				 record->snapshot.tv_sec, record->snapshot.tv_nsec);

			source->shm.write_counter++;
		}
		source->cycles++;
	}

	clockfeed_send_sync_event(clockfeed);
}

static int clockfeed_on_startup(void *context)
{
	struct sfptpd_clockfeed *module = (struct sfptpd_clockfeed *)context;
	struct timespec interval;
	uint64_t secs_fp32;
	int rc;

	assert(module != NULL);

	/* Create a message pool for sending end-of-scan sync messages */
	rc = sfptpd_thread_alloc_msg_pool(SFPTPD_MSG_POOL_LOCAL,
					  MAX_EVENT_SUBSCRIBERS,
					  sizeof(struct clockfeed_msg));
	if (rc != 0)
		return rc;

	rc = sfptpd_thread_timer_create(CLOCK_POLL_TIMER_ID, CLOCK_MONOTONIC,
					clockfeed_on_timer, module);
	if (rc != 0)
		return rc;

	secs_fp32 = 0x8000000000000000ULL >> (31 - module->poll_period_log2);

	interval.tv_sec = secs_fp32 >> 32;
	interval.tv_nsec = ((secs_fp32 & 0xFFFFFFFFUL) * 1000000000UL) >> 32;

	TRACE_L3("clockfeed: set poll interval to %d.%09llds\n",
		 interval.tv_sec, interval.tv_nsec);

	rc = sfptpd_thread_timer_start(CLOCK_POLL_TIMER_ID,
				       true, false, &interval);
	if (rc != 0)
		return rc;

	return 0;
}

static void clockfeed_on_run(void *context)
{
	struct sfptpd_clockfeed *module = (struct sfptpd_clockfeed *)context;

	assert(module != NULL);

	module->running_phase = true;
}

static void clockfeed_on_add_clock(struct sfptpd_clockfeed *module,
				   struct clockfeed_msg *msg)
{
	struct clockfeed_source *source;

	assert(module != NULL);
	assert(module->magic == CLOCKFEED_MODULE_MAGIC);
	assert(msg != NULL);

	TRACE_L6("clockfeed: received add_clock message\n");

	source = calloc(1, sizeof *source);
	assert(source);

	/* Populate source */
	source->magic = CLOCKFEED_SOURCE_MAGIC;
	source->shm.magic = CLOCKFEED_SHM_MAGIC;
	source->clock = msg->u.add_clock.clock;
	source->poll_period_log2 = msg->u.add_clock.poll_period_log2;

	if (source->poll_period_log2 < module->poll_period_log2) {
		ERROR("clockfeed: requested poll rate for %s (%d) exceeds "
		      "global limit of %d\n",
		      sfptpd_clock_get_short_name(source->clock),
		      source->poll_period_log2,
		      module->poll_period_log2);
		source->poll_period_log2 = module->poll_period_log2;
	}

	/* Add to linked list */
	source->next = module->active;
	module->active = source;

	TRACE_L3("clockfeed: added source %s with log2 sync interval %d\n",
		 sfptpd_clock_get_short_name(source->clock),
		 source->poll_period_log2);

	SFPTPD_MSG_REPLY(msg);
}

static void clockfeed_on_remove_clock(struct sfptpd_clockfeed *module,
				      struct clockfeed_msg *msg)
{
	struct clockfeed_source **source;

	assert(module != NULL);
	assert(module->magic == CLOCKFEED_MODULE_MAGIC);
	assert(msg != NULL);
	assert(msg->u.remove_clock.clock != NULL);

	TRACE_L6("clockfeed: received remove_clock message\n");

	for (source = &module->active;
	     *source && (*source)->clock != msg->u.remove_clock.clock;
	     source = &(*source)->next)
		assert((*source)->magic == CLOCKFEED_SOURCE_MAGIC);

	if (*source == NULL) {
		ERROR("clockfeed: cannot remove inactive clock %s\n",
		      sfptpd_clock_get_short_name(msg->u.remove_clock.clock));
	} else {
		struct clockfeed_source *s = *source;

		*source = s->next;
		s->next = module->inactive;
		s->inactive = true;
		module->inactive = s;

		TRACE_L6("clockfeed: marked source inactive: %s\n",
			 sfptpd_clock_get_short_name(s->clock));

		clockfeed_reap_zombies(module, s);
	}

	SFPTPD_MSG_REPLY(msg);
}

static void clockfeed_on_subscribe(struct sfptpd_clockfeed *module,
				   struct clockfeed_msg *msg)
{
	struct sfptpd_clockfeed_sub *subscriber;
	struct clockfeed_source *source;

	assert(module != NULL);
	assert(msg != NULL);
	assert(msg->u.subscribe_req.clock != NULL);

	TRACE_L6("clockfeed: received subscribe message\n");

	for (source = module->active;
	     source && source->clock != msg->u.subscribe_req.clock;
	     source = source->next)
		assert(source->magic == CLOCKFEED_SOURCE_MAGIC);

	if (source == NULL)
		for (source = module->inactive;
		     source && source->clock != msg->u.subscribe_req.clock;
		     source = source->next)
			assert(source->magic == CLOCKFEED_SOURCE_MAGIC);

	if (source == NULL) {
		ERROR("clockfeed: non-existent clock subscribed to: %s\n",
		      sfptpd_clock_get_short_name(msg->u.subscribe_req.clock));
		msg->u.subscribe_resp.sub = NULL;
	} else {
		subscriber = calloc(1, sizeof *subscriber);
		assert(subscriber);

		if (source->inactive)
			WARNING("clockfeed: subscribed to inactive source\n");

		subscriber->magic = CLOCKFEED_SUBSCRIBER_MAGIC;
		subscriber->source = source;
		subscriber->read_counter = -1;
		subscriber->min_counter = -1;
		subscriber->next = source->subscribers;
		source->subscribers = subscriber;

		msg->u.subscribe_resp.sub = subscriber;
	}

	SFPTPD_MSG_REPLY(msg);
}

static void clockfeed_on_unsubscribe(struct sfptpd_clockfeed *module,
				     struct clockfeed_msg *msg)
{
	struct sfptpd_clockfeed_sub **nextp, *s, *sub;

	assert(module != NULL);
	assert(msg != NULL);
	assert(msg->u.unsubscribe.sub != NULL);

	TRACE_L6("clockfeed: received unsubscribe message\n");

	sub = msg->u.unsubscribe.sub;

	assert(sub->magic == CLOCKFEED_SUBSCRIBER_MAGIC);

	for (nextp = &sub->source->subscribers;
	     (s = *nextp) != sub;
	     nextp = &(s->next));

	if (s == NULL) {
		ERROR("clockfeed: non-existent clock subscription\n");
	} else {
		*nextp = s->next;
	}

	clockfeed_reap_zombies(module, sub->source);
	sub->magic = CLOCKFEED_DELETED_MAGIC;
	free(sub);

	SFPTPD_MSG_REPLY(msg);
}

static void clockfeed_on_subscribe_events(struct sfptpd_clockfeed *module,
					  struct clockfeed_msg *msg)
{
	int i;

	assert(module != NULL);
	assert(msg != NULL);
	assert(msg->u.subscribe_events.thread != NULL);

	TRACE_L6("clockfeed: received subscribe_events message\n");

	for (i = 0; i < MAX_EVENT_SUBSCRIBERS; i++) {
		if (module->event_subscribers[i] == NULL) {
			module->event_subscribers[i] = msg->u.subscribe_events.thread;
			SFPTPD_MSG_REPLY(msg);
			return;
		}
	}

	sfptpd_thread_exit(ENOSPC);
}

static void clockfeed_on_unsubscribe_events(struct sfptpd_clockfeed *module,
					    struct clockfeed_msg *msg)
{
	int i;

	assert(module != NULL);
	assert(msg != NULL);
	assert(msg->u.unsubscribe_events.thread != NULL);

	TRACE_L6("clockfeed: received unsubscribe_events message\n");

	for (i = 0; i < MAX_EVENT_SUBSCRIBERS; i++) {
		if (module->event_subscribers[i] == msg->u.unsubscribe_events.thread)
			module->event_subscribers[i] = NULL;
	}

	if (i == MAX_EVENT_SUBSCRIBERS)
		TRACE_L6("clockfeed: non-subscriber event unsubscription request ignored\n");

	SFPTPD_MSG_REPLY(msg);
}

static void clockfeed_on_shutdown(void *context)
{
	struct sfptpd_clockfeed *module = (struct sfptpd_clockfeed *)context;
	struct clockfeed_source **source;
	struct clockfeed_source *s;
	int count;

	assert(module != NULL);
	assert(sfptpd_clockfeed == module);
	assert(module->magic == CLOCKFEED_MODULE_MAGIC);

	INFO("clockfeed: shutting down\n");

	clockfeed_dump_state(module);

	/* Mark all sources inactive */
	count = 0;
	for (source = &module->active; *source; source = &(*source)->next) {
		s = *source;
		assert(s->magic == CLOCKFEED_SOURCE_MAGIC);
		assert(!s->inactive);
		s->inactive = true;
		count++;
	}

	/* Move active list onto inactive list */
	*source = module->inactive;
	module->inactive = module->active;
	module->active = NULL;
	TRACE_L5("clockfeed: inactivated all %d active sources\n", count);

	/* Reap zombies */
	for (s = module->inactive; s; s = s->next)
		clockfeed_reap_zombies(module, s);

	clockfeed_dump_state(module);

	if (module->inactive)
		WARNING("clockfeed: clock source subscribers remaining on shutdown\n");

	module->magic = CLOCKFEED_DELETED_MAGIC;
	free(module);
	sfptpd_clockfeed = NULL;
}


static void clockfeed_on_message(void *context, struct sfptpd_msg_hdr *hdr)
{
	struct sfptpd_clockfeed *module = (struct sfptpd_clockfeed *)context;
	struct clockfeed_msg *msg = (struct clockfeed_msg *)hdr;

	assert(module != NULL);
	assert(module->magic == CLOCKFEED_MODULE_MAGIC);
	assert(msg != NULL);

	switch (SFPTPD_MSG_GET_ID(msg)) {
	case SFPTPD_APP_MSG_RUN:
		clockfeed_on_run(module);
		SFPTPD_MSG_FREE(msg);
		break;

	case CLOCKFEED_MSG_ADD_CLOCK:
		clockfeed_on_add_clock(module, msg);
		break;

	case CLOCKFEED_MSG_REMOVE_CLOCK:
		clockfeed_on_remove_clock(module, msg);
		break;

	case CLOCKFEED_MSG_SUBSCRIBE:
		clockfeed_on_subscribe(module, msg);
		break;

	case CLOCKFEED_MSG_UNSUBSCRIBE:
		clockfeed_on_unsubscribe(module, msg);
		break;

	case CLOCKFEED_MSG_SUBSCRIBE_EVENTS:
		clockfeed_on_subscribe_events(module, msg);
		break;

	case CLOCKFEED_MSG_UNSUBSCRIBE_EVENTS:
		clockfeed_on_unsubscribe_events(module, msg);
		break;

	default:
		WARNING("clockfeed: received unexpected message, id %d\n",
			sfptpd_msg_get_id(hdr));
		SFPTPD_MSG_FREE(msg);
	}
}


static void clockfeed_on_user_fds(void *context, unsigned int num_fds, int fds[])
{
	struct sfptpd_clockfeed *module = (struct sfptpd_clockfeed *) context;

	assert(module != NULL);
	assert(module->magic == CLOCKFEED_MODULE_MAGIC);
}


static const struct sfptpd_thread_ops clockfeed_thread_ops =
{
	clockfeed_on_startup,
	clockfeed_on_shutdown,
	clockfeed_on_message,
	clockfeed_on_user_fds
};


/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct sfptpd_clockfeed *sfptpd_clockfeed_create(struct sfptpd_thread **threadret,
						 int min_poll_period_log2)
{
	struct sfptpd_clockfeed *clockfeed;
	int rc;

	assert(threadret);
	assert(!sfptpd_clockfeed);

	TRACE_L3("clockfeed: creating service\n");

	*threadret = NULL;
	clockfeed = (struct sfptpd_clockfeed *) calloc(1, sizeof(*clockfeed));
	if (clockfeed == NULL) {
		CRITICAL("clockfeed: failed to allocate module memory\n");
		return NULL;
	}

	clockfeed->poll_period_log2 = min_poll_period_log2;

	/* Create the service thread- the thread start up routine will
	 * carry out the rest of the initialisation. */
	rc = sfptpd_thread_create("clocks", &clockfeed_thread_ops, clockfeed, threadret);
	if (rc != 0) {
		free(clockfeed);
		errno = rc;
		return NULL;
	}

	clockfeed->magic = CLOCKFEED_MODULE_MAGIC;
	clockfeed->thread = *threadret;
	sfptpd_clockfeed = clockfeed;
	return clockfeed;
}

void sfptpd_clockfeed_add_clock(struct sfptpd_clockfeed *clockfeed,
				struct sfptpd_clock *clock,
				int poll_period_log2)
{
	struct clockfeed_msg *msg;

	assert(clockfeed);
	assert(clockfeed->magic == CLOCKFEED_MODULE_MAGIC);

	msg = (struct clockfeed_msg *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	memset(&msg->u.add_clock, 0, sizeof(msg->u.add_clock));

	msg->u.add_clock.clock = clock;
	msg->u.add_clock.poll_period_log2 = poll_period_log2;

	SFPTPD_MSG_SEND_WAIT(msg, clockfeed->thread,
			     CLOCKFEED_MSG_ADD_CLOCK);
}

void sfptpd_clockfeed_remove_clock(struct sfptpd_clockfeed *clockfeed,
				   struct sfptpd_clock *clock)
{
	struct clockfeed_msg *msg;

	assert(clockfeed);
	assert(clockfeed->magic == CLOCKFEED_MODULE_MAGIC);

	msg = (struct clockfeed_msg *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	memset(&msg->u.remove_clock, 0, sizeof(msg->u.remove_clock));

	msg->u.remove_clock.clock = clock;

	SFPTPD_MSG_SEND_WAIT(msg, clockfeed->thread,
			     CLOCKFEED_MSG_REMOVE_CLOCK);
}

int sfptpd_clockfeed_subscribe(struct sfptpd_clock *clock,
			       struct sfptpd_clockfeed_sub **sub)
{
	struct clockfeed_msg *msg;
	int rc;

	assert(sfptpd_clockfeed);
	assert(sfptpd_clockfeed->magic == CLOCKFEED_MODULE_MAGIC);
	assert(clock);
	assert(sub);

	/* The calling code has an easier life if it can treat a system
	 * clock, i.e. NULL feed, the same as a real one. */
	if (sfptpd_clock_is_system(clock)) {
		*sub = NULL;
		return 0;
	}

	msg = (struct clockfeed_msg *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return ENOMEM;
	}

	memset(&msg->u.subscribe_req, 0, sizeof(msg->u.subscribe_req));

	msg->u.subscribe_req.clock = clock;

	rc = SFPTPD_MSG_SEND_WAIT(msg, sfptpd_clockfeed->thread,
				  CLOCKFEED_MSG_SUBSCRIBE);
	if (rc == 0) {
		assert(msg->u.subscribe_resp.sub);
		assert(msg->u.subscribe_resp.sub->magic == CLOCKFEED_SUBSCRIBER_MAGIC);
		*sub = msg->u.subscribe_resp.sub;
	}

	return rc;
}

void sfptpd_clockfeed_unsubscribe(struct sfptpd_clockfeed_sub *subscriber)
{
	struct clockfeed_msg *msg;

	assert(sfptpd_clockfeed);
	assert(sfptpd_clockfeed->magic == CLOCKFEED_MODULE_MAGIC);

	if (subscriber == NULL)
		return;

	assert(subscriber->magic == CLOCKFEED_SUBSCRIBER_MAGIC);

	msg = (struct clockfeed_msg *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	memset(&msg->u.unsubscribe, 0, sizeof(msg->u.unsubscribe));

	msg->u.unsubscribe.sub = subscriber;

	SFPTPD_MSG_SEND_WAIT(msg, sfptpd_clockfeed->thread,
			     CLOCKFEED_MSG_UNSUBSCRIBE);
}

static int clockfeed_compare_to_sys(struct sfptpd_clockfeed_sub *sub,
				    struct timespec *diff,
				    struct timespec *t1,
				    struct timespec *t2,
				    struct timespec *mono_time)
{
	const struct clockfeed_shm *shm = &sub->source->shm;
	const uint32_t index_mask = (1 << MAX_CLOCK_SAMPLES_LOG2) - 1;
	const struct sfptpd_clockfeed_sample *sample;
	struct sfptpd_clock *clock;
	struct timespec now_mono;
	struct timespec age;
	int writer1;
	int writer2;
	int rc = 0;

	diff->tv_sec = 0;
	diff->tv_nsec = 0;

	TRACE_L5("clockfeed: comparing %s (%p shm) to sys\n",
		 sfptpd_clock_get_short_name(sub->source->clock), shm);

	clock = sub->source->clock;
	writer1 = shm->write_counter;

	if (sub->source->inactive)
		return EOWNERDEAD;

	if (!sfptpd_clock_is_active(clock))
		return ENOENT;

	if (writer1 == 0) {
		ERROR("clockfeed: no samples yet obtained from %s\n",
		      sfptpd_clock_get_short_name(clock));
		return EAGAIN;
	}

	sample = &shm->samples[(writer1 - 1) & index_mask];

	if (sample->rc != 0)
		return rc;

	sfptpd_time_subtract(diff, &sample->snapshot, &sample->system);

	/* Check for overrun */
	writer2 = shm->write_counter;
	if (writer2 >= writer1 + MAX_CLOCK_SAMPLES - 1) {
		WARNING("clockfeed %s: last sample lost while reading - reader too slow? %lld > %lld + %d\n",
		        sfptpd_clock_get_short_name(clock), writer2, writer1, MAX_CLOCK_SAMPLES - 1);
		return ENODATA;
	}

	/* Check for old sample when new one requested */
	if (writer1 < sub->min_counter) {
		WARNING("clockfeed %s: old sample (%d) when fresh one (%d) requested\n",
		        sfptpd_clock_get_short_name(clock), writer1, sub->min_counter);
		return ESTALE;
	}
	if (sub->have_max_age) {
		rc = clock_gettime(CLOCK_MONOTONIC, &now_mono);
		if (rc != 0)
			return EAGAIN;
		sfptpd_time_subtract(&age, &now_mono, &sample->mono);
		if (sfptpd_time_cmp(&age, &sub->max_age) > 0) {
			WARNING("clockfeed %s: sample too old\n",
				sfptpd_clock_get_short_name(clock));
			return ESTALE;
		}
	}
	if (t1) {
		t1->tv_sec = sample->snapshot.tv_sec;
		t1->tv_nsec = sample->snapshot.tv_nsec;
	}
	if (t2) {
		t2->tv_sec = sample->system.tv_sec;
		t2->tv_nsec = sample->system.tv_nsec;
	}
	if (mono_time) {
		mono_time->tv_sec = sample->mono.tv_sec;
		mono_time->tv_nsec = sample->mono.tv_nsec;
	}
	if (rc == 0) {
		sub->read_counter = writer1;
	}

	return rc;
}

int sfptpd_clockfeed_compare(struct sfptpd_clockfeed_sub *sub1,
			     struct sfptpd_clockfeed_sub *sub2,
			     struct timespec *diff,
			     struct timespec *t1,
			     struct timespec *t2,
			     struct timespec *mono)
{
	const struct clockfeed_source *feed1 = sub1 ? sub1->source : NULL;
	const struct clockfeed_source *feed2 = sub2 ? sub2->source : NULL;
	const struct clockfeed_shm *shm1 = sub1 ? &feed1->shm : NULL;
	const struct clockfeed_shm *shm2 = sub2 ? &feed2->shm : NULL;
	const struct timespec *max_age_diff = NULL;
	struct timespec diff2;
	struct timespec mono1;
	struct timespec mono2;
	int rc = 0;

	diff->tv_sec = 0;
	diff->tv_nsec = 0;

	if (sub1 && sub2) {
		if (sub1->have_max_age_diff)
			max_age_diff = &sub1->max_age_diff;
		if (sub2->have_max_age_diff && (!max_age_diff || sfptpd_time_is_greater_or_equal(max_age_diff, &sub2->max_age_diff)))
			max_age_diff = &sub2->max_age_diff;
		if (!mono && max_age_diff)
			mono = &mono1;
	}

	TRACE_L5("clockfeed: comparing %s (%p shm) %s (%p shm)\n",
		 shm1 ? sfptpd_clock_get_short_name(feed1->clock) : "<sys>", shm1,
		 shm2 ? sfptpd_clock_get_short_name(feed2->clock) : "<sys>", shm2);

	if (sub1) {
		rc = clockfeed_compare_to_sys(sub1, diff, t1, sub2 ? NULL : t2, mono);
	}
	if (rc == 0 && sub2) {
		rc = clockfeed_compare_to_sys(sub2, &diff2, t2, sub1 ? NULL : t1, mono ? &mono2 : NULL);
		if (rc == 0) {
			sfptpd_time_subtract(diff, diff, &diff2);
			if (mono &&
			    (mono2.tv_sec < mono->tv_sec ||
			     (mono2.tv_sec == mono->tv_nsec && mono2.tv_nsec < mono->tv_nsec)))
				*mono = mono2;
		}
	}

	if (rc == 0 && max_age_diff) {
		struct timespec age_diff;

		if (sfptpd_time_is_greater_or_equal(&mono2, mono))
			sfptpd_time_subtract(&age_diff, &mono2, mono);
		else
			sfptpd_time_subtract(&age_diff, mono, &mono2);

		if (sfptpd_time_is_greater_or_equal(&age_diff, max_age_diff)) {
			WARNING("clockfeed %s-%s: to big an age difference between samples\n",
				sfptpd_clock_get_short_name(feed1->clock),
				sfptpd_clock_get_short_name(feed2->clock));
			return ESTALE;
		}
	}

	return rc;
}

void sfptpd_clockfeed_require_fresh(struct sfptpd_clockfeed_sub *sub)
{
	if (!sub)
		return;

	assert(sub->magic == CLOCKFEED_SUBSCRIBER_MAGIC);

	TRACE_L6("clockfeed %s: updating minimum read counter from %d to %d\n",
		 sfptpd_clock_get_short_name(sub->source->clock),
		 sub->min_counter, sub->read_counter + 1);

	sub->min_counter = sub->read_counter + 1;
}

void sfptpd_clockfeed_set_max_age(struct sfptpd_clockfeed_sub *sub,
				  const struct timespec *max_age) {
	if (!sub)
		return;

	assert(sub->magic == CLOCKFEED_SUBSCRIBER_MAGIC);

	sub->have_max_age = true;
	sub->max_age = *max_age;
}

void sfptpd_clockfeed_set_max_age_diff(struct sfptpd_clockfeed_sub *sub,
				       const struct timespec *max_age_diff) {
	if (!sub)
		return;

	assert(sub->magic == CLOCKFEED_SUBSCRIBER_MAGIC);

	sub->have_max_age_diff = true;
	sub->max_age_diff = *max_age_diff;
}

void sfptpd_clockfeed_subscribe_events(void)
{
	struct clockfeed_msg *msg;

	assert(sfptpd_clockfeed);
	assert(sfptpd_clockfeed->magic == CLOCKFEED_MODULE_MAGIC);

	msg = (struct clockfeed_msg *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		sfptpd_thread_exit(ENOMEM);
		return;
	}

	memset(&msg->u.subscribe_events, 0, sizeof(msg->u.subscribe_events));

	msg->u.subscribe_events.thread = sfptpd_thread_self();

	SFPTPD_MSG_SEND_WAIT(msg, sfptpd_clockfeed->thread,
			     CLOCKFEED_MSG_SUBSCRIBE_EVENTS);
}

void sfptpd_clockfeed_unsubscribe_events(void)
{
	struct clockfeed_msg *msg;

	assert(sfptpd_clockfeed);
	assert(sfptpd_clockfeed->magic == CLOCKFEED_MODULE_MAGIC);

	msg = (struct clockfeed_msg *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		sfptpd_thread_exit(ENOMEM);
		return;
	}

	memset(&msg->u.unsubscribe_events, 0, sizeof(msg->u.unsubscribe_events));

	msg->u.unsubscribe_events.thread = sfptpd_thread_self();

	SFPTPD_MSG_SEND_WAIT(msg, sfptpd_clockfeed->thread,
			     CLOCKFEED_MSG_UNSUBSCRIBE_EVENTS);
}
