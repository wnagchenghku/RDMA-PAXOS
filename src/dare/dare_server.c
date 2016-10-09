/**
 * DARE (Direct Access REplication)
 * 
 * Implementation of a DARE server
 *
 * Copyright (c) 2014-2015 ETH-Zurich. All rights reserved.
 * 
 * Author(s): Marius Poke <marius.poke@inf.ethz.ch>
 * 
 */
 
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <sys/types.h>
#include <sys/time.h>
#include <signal.h>


#include "../include/dare/dare_ibv.h"
#include "../include/dare/dare_server.h"
#include "../include/dare/dare_sm.h"
#include "../include/dare/dare_ep_db.h"
#include "../include/dare/timer.h"

/* 
 * HB period (seconds)
 * election timeout range (microseconds)
 * retransmission period (seconds)
 * period of checking for new connections (seconds)
 * log pruning period (seconds)
*/
#ifdef DEBUG
const double hb_period = 0.01;
const uint64_t elec_timeout_low = 100000;
const uint64_t elec_timeout_high = 300000;
const double retransmit_period = 0.04;
const double rc_info_period = 0.05;
const double log_pruning_period = 0.05;
#else
const double hb_period = 0.001;
const uint64_t elec_timeout_low = 10000;
const uint64_t elec_timeout_high = 30000;
const double rc_info_period = 0.01;
const double retransmit_period = 0.02;
const double log_pruning_period = 0.03;
#endif
/* Retry period before failures (seconds) */
const double retry_exec_period = 0.355;

/* Vars for timeout adjustment */
double recomputed_hb_timeout;
int leader_failed;
int hb_timeout_flag;
uint64_t latest_hb_received;

unsigned long long g_timerfreq;

#define IS_NONE \
    ( (SID_GET_IDX(data.ctrl_data->sid) == data.config.idx) && \
      (!SID_GET_L(data.ctrl_data->sid)) && \
      (SID_GET_TERM(data.ctrl_data->sid) == 0) )
#define IS_LEADER \
    ( !IS_NONE && (SID_GET_IDX(data.ctrl_data->sid) == data.config.idx) && \
      (SID_GET_L(data.ctrl_data->sid)) )
#define IS_CANDIDATE \
    ( !IS_NONE && (SID_GET_IDX(data.ctrl_data->sid) == data.config.idx) && \
      (!SID_GET_L(data.ctrl_data->sid)) )
#define IS_FOLLOWER (!IS_NONE && !IS_LEADER && !IS_CANDIDATE)
#define IS_SID_DIRTY (data.ctrl_data->sid != data.ctrl_data->sid)

/* DARE server state */
#define TERMINATE       0x1
#define INIT            0x2
#define JOINED          0x4
#define RC_ESTABLISHED  0x8
#define SM_RECOVERED    0x10
#define LOG_RECOVERED   0x20
#define SNAPSHOT        0x40
#define DIE_AF_COMMIT   0x80
uint64_t dare_state;

FILE *log_fp;

/* server data */
dare_server_data_t data;

int prev_log_entry_head = 0;

dare_log_entry_det_t last_applied_entry;

/* ================================================================== */
/* libEV events */

/* An idle event that polls for different stuff... */
ev_idle poll_event;

/* A timer event used for initialization and other stuff ... */
ev_timer timer_event;

/* A timer event for heartbeat mechanism */
ev_timer hb_event;

/* A timer event for adjusting the timeout period */
ev_timer to_adjust_event;

/* A timer event for log pruning */
ev_timer prune_event;

/* ================================================================== */
/* local function - prototypes */

static int
init_server_data();
static void
free_server_data();

static void 
poll_sm_reply();
static void
poll_sm_requests();
static void 
poll_config_entries();
static void 
commit_new_entries();
static void 
apply_committed_entries();

static double
random_election_timeout();
static double
hb_timeout();
static void
start_election();
static void 
poll_vote_count();

static void
polling();
static void 
poll_ud();
static void
poll_vote_requests();
static void 
check_failure_count();
static int
log_pruning();
static void
force_log_pruning();
static int 
update_cid( dare_cid_t cid );

static void int_handler(int);

/* ================================================================== */
/* Callbacks for libEV */

static void
init_network_cb( EV_P_ ev_timer *w, int revents );
static void
join_cluster_cb( EV_P_ ev_timer *w, int revents );
static void
exchange_rc_info_cb( EV_P_ ev_timer *w, int revents );
static void
get_replicated_vote_cb( EV_P_ ev_timer *w, int revents );
static void
send_sm_request_cb( EV_P_ ev_timer *w, int revents );
static void
recover_log_cb( EV_P_ ev_timer *w, int revents );
static void
update_rc_info_cb( EV_P_ ev_timer *w, int revents );
static void
prune_log_cb( EV_P_ ev_timer *w, int revents );
static void
hb_receive_cb( EV_P_ ev_timer *w, int revents );
static void
hb_send_cb( EV_P_ ev_timer *w, int revents );
static void
to_adjust_cb( EV_P_ ev_timer *w, int revents );
static void
poll_cb( EV_P_ ev_idle *w, int revents );

/* ================================================================== */
/* Init and cleaning up */
#if 1
ev_tstamp start_ts;

void *dare_server_init(void *arg)
{   
    int rc;
    dare_server_input_t *input = (dare_server_input_t *)arg;
    
    /* Initialize data fields to zero */
    memset(&data, 0, sizeof(dare_server_data_t));
    
    /* Store input into server's data structure */
    data.input = input;

    /* Set log file handler */
    log_fp = input->log;
    
    /* Set handler for SIGINT */
    signal(SIGINT, int_handler);
    
    /* Initialize timer */
    if (SRV_TYPE_LOGGP == input->srv_type) {
        HRT_INIT(g_timerfreq);
    }
#ifdef DEBUG    
    else {
        HRT_INIT(g_timerfreq);
    }
#endif
    //HRT_INIT(g_timerfreq);

    /* Init server data */    
    rc = init_server_data();
    if (0 != rc) {
        free_server_data();
        error_return(1, log_fp, "Cannot init server data\n");
    }

    free(input);
        
    /* Init EV loop */
    data.loop = EV_DEFAULT;
    recomputed_hb_timeout = 2*hb_period;
    
    start_ts = ev_now(data.loop);
    
    /* Schedule timer event */
    ev_timer_init(&timer_event, init_network_cb, 0., NOW);
    ev_set_priority(&timer_event, EV_MAXPRI-1);
    ev_timer_again(data.loop, &timer_event);
    
    /* Init prune event */
    ev_timer_init(&prune_event, prune_log_cb, 0., 0.);
    ev_set_priority(&prune_event, EV_MAXPRI-1);
    
    /* Init the poll event */
    ev_idle_init(&poll_event, poll_cb);
    ev_set_priority(&poll_event, EV_MAXPRI);
    
    /* Init HB event */
    ev_timer_init(&hb_event, hb_receive_cb, 0., 0.);
    ev_set_priority(&hb_event, EV_MAXPRI-1);
    
    /* Init timeout adjust event */
    ev_timer_init(&to_adjust_event, to_adjust_cb, 0., 0.);
    ev_set_priority(&to_adjust_event, EV_MAXPRI-1);

    /* Now wait for events to arrive */
    ev_run(data.loop, 0);

    return 0;
}

void dare_server_shutdown()
{
    ev_timer_stop(data.loop, &timer_event);
    ev_timer_stop(data.loop, &prune_event);
    ev_timer_stop(data.loop, &hb_event);
    ev_timer_stop(data.loop, &to_adjust_event);
    ev_break(data.loop, EVBREAK_ALL);
    
    dare_ib_srv_shutdown();
    free_server_data();
    fclose(log_fp);
    exit(1);
}

static int
init_server_data()
{
    int rc;
    uint8_t i;

    if (SRV_TYPE_LOGGP != data.input->srv_type) {
        /* Create local SM */
        switch(data.input->sm_type) {
            case SM_NULL:
                break;
            case SM_FS:
                break;
        }
    }

    /* Set up the configuration */
    data.config.idx = data.input->server_idx;
    data.apply_cmd = data.input->ucb;
    data.up_para = data.input->up_para;
    data.config.len = MAX_SERVER_COUNT;
    if (data.config.len < data.input->group_size) {
        error_return(1, log_fp, "Cannot have more than %d servers\n", 
                    MAX_SERVER_COUNT);
    }
    data.config.cid.size[0] = data.input->group_size;
    data.config.cid.state   = CID_STABLE;
    for (i = 0; i < data.input->group_size; i++) {
        CID_SERVER_ADD(data.config.cid, i);
    }    
    data.config.servers = (server_t*)malloc(data.config.len * sizeof(server_t));
    if (NULL == data.config.servers) {
        error_return(1, log_fp, "Cannot allocate configuration array\n");
    }
    memset(data.config.servers, 0, data.config.len * sizeof(server_t));
    for (i = 0; i < MAX_SERVER_COUNT; i++) {
        data.config.servers[i].next_lr_step = LR_GET_WRITE;
        data.config.servers[i].send_flag = 1;
    }
 
    /* Allocate ctrl_data - needs to be 8 bytes aligned for CAS operations */
    rc = posix_memalign((void**)&data.ctrl_data, sizeof(uint64_t), sizeof(ctrl_data_t));
    if (0!= rc) {
        error_return(1, log_fp, "Cannot allocate control data\n");
    }
    memset(data.ctrl_data, 0, sizeof(ctrl_data_t));
    data.ctrl_data->sid = SID_NULL;
 
    /* Set up log */
    data.log = log_new();
    if (NULL == data.log) {
        error_return(1, log_fp, "Cannot allocate log\n");
    }
    
    /* Allocate preregister snapshot */
    rc = posix_memalign((void**)&data.prereg_snapshot, sizeof(uint64_t), 
                    sizeof(snapshot_t) + PREREG_SNAPSHOT_SIZE);
    if (0!= rc) {
        error_return(1, log_fp, "Cannot allocate prereg snapshot\n");
    }

    pthread_spin_init(&data.spinlock, PTHREAD_PROCESS_PRIVATE);
    data.last_csm_idx = 0;
    data.last_cmt_csm_idx = 0;
    data.hash_map = NULL;

    data.endpoints = RB_ROOT;
    
    return 0;
}

static void
free_server_data()
{
    ep_db_free(&data.endpoints);
    
    if (NULL != data.snapshot) {
        free(data.snapshot);
        data.snapshot = NULL;
    }
    
    if (NULL != data.prereg_snapshot) {
        free(data.prereg_snapshot);
        data.prereg_snapshot = NULL;
    }
    
    /* Free log */
    log_free(data.log);

    /* Free control data */
    if (NULL != data.ctrl_data) {
        free(data.ctrl_data);
        data.ctrl_data = NULL;
    } 
    
    /* Free servers */
    if (NULL != data.config.servers) {
        free(data.config.servers);
        data.config.servers = NULL;
    }
}

#endif

/* ================================================================== */
/* Starting up */
#if 1
/**
 * Initialize networking data 
 *  IB device; UD data; RC data; and start UD
 */
static void
init_network_cb( EV_P_ ev_timer *w, int revents )
{
    int rc; 
    
    /* Init IB device */
    rc = dare_init_ib_device(MAX_CLIENT_COUNT);
    if (0 != rc) {
        error(log_fp, "Cannot init IB device\n");
        goto shutdown;
    }
    
    /* Init some IB data for the server */
    rc = dare_init_ib_srv_data(&data);
    if (0 != rc) {
        error(log_fp, "Cannot init IB SRV data\n");
        goto shutdown;
    }
    
    /* Init IB RC */
    rc = dare_init_ib_rc();
        if (0 != rc) {
        error(log_fp, "Cannot init IB RC\n");
        goto shutdown;
    }
    
    /* Start IB UD */
    rc = dare_start_ib_ud();
    if (0 != rc) {
        error(log_fp, "Cannot start IB UD\n");
        goto shutdown;
    }
    
    dare_state |= INIT;
    
    /* Start poll event */   
    ev_idle_start(EV_A_ &poll_event);
    
    if (SRV_TYPE_JOIN == data.input->srv_type) {
        /* Server joining the cluster */
        ev_set_cb(w, join_cluster_cb);
        w->repeat = NOW;
        ev_timer_again(EV_A_ w);
    }
    else if (1 == data.config.cid.size[0]) { 
        /* I'm the only one; I am the leader */
        info(log_fp, "Starting with size=1\n");
        SID_SET_TERM(data.ctrl_data->sid, 1);
        SID_SET_L(data.ctrl_data->sid);
        SID_SET_IDX(data.ctrl_data->sid, 0);
        w->repeat = 0.;
        ev_timer_again(EV_A_ w);
    }
    else {
        /* I already know the cluster size: start exchanging RC info */
        ev_set_cb(w, exchange_rc_info_cb);
        w->repeat = NOW;
        ev_timer_again(EV_A_ w);
    }
    
    return;

shutdown:
    w->repeat = 0;
    ev_timer_again(EV_A_ w);
    dare_server_shutdown();
}

/**
 * Send join requests to the cluster
 * Note: the timer is stopped when receiving a JOIN reply (poll_ud)
 */
static void
join_cluster_cb( EV_P_ ev_timer *w, int revents )
{
    int rc;
    
    rc = dare_ib_join_cluster();
    if (0 != rc) {
        error(log_fp, "Cannot join cluster\n");
        goto shutdown;
    }
    
    /* Retransmit after retransmission period */
    w->repeat = retransmit_period;
    ev_timer_again(EV_A_ w);
    
    return;

shutdown:
    w->repeat = 0;
    ev_timer_again(EV_A_ w);
    dare_server_shutdown();    
}

/**
 * Exchange RC info
 * Note: the timer is stopped when establishing connections with 
 * at least half of the servers  
 */
static void
exchange_rc_info_cb( EV_P_ ev_timer *w, int revents )
{
    int rc;
    
    rc = dare_ib_exchange_rc_info();
    if (0 != rc) {
        error(log_fp, "Exchanging RC info failed\n");
        goto shutdown;
    }
    
    /* Retransmit after retransmission period */
    w->repeat = retransmit_period;
    ev_timer_again(EV_A_ w);
    
    return;

shutdown:
    w->repeat = 0;
    ev_timer_again(EV_A_ w);
    dare_server_shutdown();
}

/**
 * Update RC info
 * Note: this event is repeated periodically to check for not 
 * established connections
 */
static void
update_rc_info_cb( EV_P_ ev_timer *w, int revents )
{
    int rc;
    rc = dare_ib_update_rc_info();
    if (0 != rc) {
        error(log_fp, "Cannot get vote SID\n");
        goto shutdown;
    }
    
    w->repeat = rc_info_period;
    ev_timer_again(EV_A_ w);
    return;
    
shutdown:
    w->repeat = 0;
    ev_timer_again(EV_A_ w);
    dare_server_shutdown();    
}

/**
 * Get the last vote of a server in the same position in the group
 */
static void
get_replicated_vote_cb( EV_P_ ev_timer *w, int revents )
{
    int rc;
    
    /* Set up starting SID: leader of term 0 (not existent) */
    SID_SET_TERM(data.ctrl_data->sid, 0);
    SID_SET_IDX(data.ctrl_data->sid, data.config.idx);    
    SID_SET_L(data.ctrl_data->sid);
    
    if (2 == data.config.cid.size[0]) {
        info(log_fp, "Warning: DARE cannot guarantee safety " 
        "with only 2 servers (replicated vote cannot be retrieved)\n");
        goto next;
    }
        
    text(log_fp, "\n>> GET REPLICATED VOTE  <<\n");
    rc = dare_ib_get_replicated_vote();
    if (rc > 1) {
        error(log_fp, "Cannot get vote SID\n");
        goto shutdown;
    }
    if (rc != 0) {
        /* Insuccess - try again later */  
        w->repeat = retransmit_period;
        ev_timer_again(EV_A_ w);
        return;
    }
    
next:
    /* Got replicated vote */
    info_wtime(log_fp, "Latest vote successfully retrieved\n");
    //memset(data.ctrl_data->sm_rep, 0, MAX_SERVER_COUNT * sizeof(sm_rep_t));
    /* Go to next recovery step */
    return;

shutdown:
    w->repeat = 0;
    ev_timer_again(EV_A_ w);
    dare_server_shutdown();
}

/**
 * Recover the log
 */
static void
recover_log_cb( EV_P_ ev_timer *w, int revents )
{
    int rc;
    
    text(log_fp, "\n>> RECOVER LOG <<\n");
    rc = dare_ib_recover_log();
    if (rc > 1) {
        error(log_fp, "Cannot recover log\n");
        goto shutdown;
    }
    if (rc != 0) {
        /* Insuccess - try again later */  
        w->repeat = retransmit_period;
        ev_timer_again(EV_A_ w);
        return;
    }
    
    /* Log recovered successfully */
    dare_state |= LOG_RECOVERED;
    info(log_fp, "Server recovered: ");
    INFO_PRINT_LOG(log_fp, data.log);
    
    /* Set a periodic timer that update the RC info;
    TODO should this be activated during recovery? */
    ev_set_cb(w, update_rc_info_cb);
    w->repeat = rc_info_period;
    ev_timer_again(EV_A_ w);
    
    if (0 == SID_GET_TERM(data.ctrl_data->sid)) {
        /* Still in term 0 - start election */
        start_election();
    }
    else {
        /* I have a proper SID */
        server_to_follower();
    }
    return;

shutdown:
    w->repeat = 0;
    ev_timer_again(EV_A_ w);
    dare_server_shutdown();
}

#endif

/* ================================================================== */
/* HB mechanism */
#if 1

/**
 * Periodically adjust the HB timeout
 */
static void
to_adjust_cb( EV_P_ ev_timer *w, int revents )
{
    static uint64_t total_count = 0;
    static uint64_t fp_count = 0;

    uint64_t hb;
    uint8_t leader = SID_GET_IDX(data.ctrl_data->sid);
    
    if (hb_timeout_flag) {
        w->repeat = 0;
        ev_timer_again(EV_A_ w);
        return;
    }

    /* Total number of trials */
    total_count++;
    
    /* Read HB and reset it - atomic operation */
    hb = __sync_fetch_and_and(&data.ctrl_data->hb[leader], 0);
    if (0 != hb) {
        /* HB received */
        latest_hb_received = hb;
        if (leader_failed) {
            /* False possitive */
            fp_count++;
            leader_failed = 0;
            /* Increament timer with 0.1 of HB period */
            recomputed_hb_timeout += hb_period * 1;
            info_wtime(log_fp, "false possitive => increase recomputed timeout: %lf\n", recomputed_hb_timeout);
        }
    }
    else {
        /* No HB */
        leader_failed = 1;
    }

    if (!hb_timeout_flag) {
        if ( (total_count > 100000) && ((double)fp_count/total_count < 0.0001) ) {
            /* Less than 0.01% false possitives */
            info_wtime(log_fp, "New timeout: %lf (old one was %lf)\n", 
                        recomputed_hb_timeout, hb_timeout());
            info(log_fp, "   # %"PRIu64" fp out of %"PRIu64"\n", fp_count, total_count);
            hb_timeout_flag = 1;
            /* From now on the timeout period is adjusted during the HB */ 
            w->repeat = 0;
            ev_timer_again(EV_A_ w);
            return;
        }
    }
    
    /* Reset timer */
    w->repeat = recomputed_hb_timeout;
    ev_timer_again(EV_A_ w);
}

/**
 * Periodically check for the HB flag
 */
static void
hb_receive_cb( EV_P_ ev_timer *w, int revents )
{   
    int rc;
    int timeout = 1;
    uint64_t hb;
    uint64_t new_sid;
    uint8_t i, size;
    
    /* Cannot receive HBs from servers in the extended config */
    size = get_group_size(data.config);
    
    if (data.config.idx >= size) {
        /* I'm not YET part of the group */
        w->repeat = hb_timeout();
        ev_timer_again(EV_A_ w);
        return;
    }
    
    /* Be sure that the tail does not remain set from a previous leadership */
    data.log->tail = data.log->len;

    uint8_t leader = SID_GET_IDX(data.ctrl_data->sid);
    new_sid = data.ctrl_data->sid;
    for (i = 0; i < size; i++) {
        if ( (i == data.config.idx) || !CID_IS_SERVER_ON(data.config.cid, i) )
            continue;
        if (i == leader) {
            if (!latest_hb_received) {
                latest_hb_received = __sync_fetch_and_and(&data.ctrl_data->hb[leader], 0);
            }
            /* HBs from leader are checked while adjusting the timeout */
            hb = latest_hb_received;
            latest_hb_received = 0;
        }
        else {
            /* Read HB and then reset it */
            hb = __sync_fetch_and_and(&data.ctrl_data->hb[i], 0);
        }
        if (0 == hb) {
            /* No heartbeat */
            continue;
        }
        if (hb < new_sid) {
            if (SID_GET_L(hb)) {
                /* Received HB from outdated leader */
                info_wtime(log_fp, "Received outdated HB from p%"PRIu8"\n", i);
                info(log_fp, "   # HB: [%010"PRIu64"|%d|%03"PRIu8"]; "
                    "local SID: [%010"PRIu64"|%d|%03"PRIu8"]\n",
                    SID_GET_TERM(hb), (SID_GET_L(hb) ? 1 : 0),
                    SID_GET_IDX(hb), SID_GET_TERM(new_sid),
                    (SID_GET_L(new_sid) ? 1 : 0), SID_GET_IDX(new_sid) );
                dare_ib_send_hb_reply(i);
                /* Increament timer with 0.1 of HB period */
               recomputed_hb_timeout += hb_period * 0.1;
            }
            continue;
        }

        /* Somebody sent me an up-to-date HB */
        timeout = 0;    
        /* Check if it is from a leader */
        if (SID_GET_L(hb)) {
            /* The HB was from a leader */
            new_sid = hb;
        }
    }
//text(log_fp, "\n");    
    
    if (timeout) {
        w->repeat = 0.;
        ev_timer_again(EV_A_ w);
        start_election(); 
        return;
    }
 
    if (new_sid != data.ctrl_data->sid) {       
        /* This SID is better */
        uint64_t old_sid = data.ctrl_data->sid;
        rc = server_update_sid(new_sid, data.ctrl_data->sid);
        if (0 != rc) {
            /* Cannot update SID */
            return;
        }
        
        /* If the HB has the same term, then it is from the recently elected leader */
        if (SID_GET_TERM(old_sid) != SID_GET_TERM(data.ctrl_data->sid)) {
            /* New leader */
            info_wtime(log_fp, "[T%"PRIu64"] Follow p%"PRIu8"\n", SID_GET_TERM(data.ctrl_data->sid), SID_GET_IDX(data.ctrl_data->sid));
            server_to_follower();
        }
        return;
    }
    
rearm:    
    /* Rearm HB event */
    //w->repeat = random_election_timeout();
    w->repeat = hb_timeout();
    //info_wtime(log_fp, "RECV HB (next=%lf sec)\n", w->repeat);
    ev_timer_again(EV_A_ w);
}

/**
 * Periodically update HB flag
 */
static void
hb_send_cb( EV_P_ ev_timer *w, int revents )
{
    int rc;
    static ev_tstamp last_hb = 0;
    
    /* Check if any server sent an HB reply; if that's the case, we need 
     * to incorporate that server into the active servers; restart election */
    uint64_t new_sid = data.ctrl_data->sid;
    uint64_t hb;
    uint8_t i, size;
    
    /* No need to send HBs to servers in the extended config */
    size = get_group_size(data.config);
    
    for (i = 0; i < size; i++) {
        if ( (i == data.config.idx) || !CID_IS_SERVER_ON(data.config.cid, i) )
            continue;

        /* Read HB and then reset it */
        hb = __sync_fetch_and_and(&data.ctrl_data->hb[i], 0);
        if (hb < new_sid) continue;

        /* Somebody sent me an HB reply with a higher term */
        info_wtime(log_fp, "Received HB from p%"PRIu8" with higher term %"PRIu64"\n", 
                    i, SID_GET_TERM(hb));
        rc = server_update_sid(new_sid, data.ctrl_data->sid);
        if (0 != rc) {
            /* Cannot update SID */
            return;
        }
        server_to_follower();
        return;
    }

    /* Send HB to all servers */
    rc = dare_ib_send_hb();
    if (0 != rc) {
        error(log_fp, "Cannot send heartbeats\n");
        goto shutdown;
    }
    ev_tstamp err = 0;
    if (last_hb != 0) {
        err = ev_now(EV_A) - (last_hb + hb_period);
    }
    last_hb = ev_now(EV_A);
    static uint64_t errs = 0;
    static uint64_t total = 0; total++;
    //info_wtime(log_fp, "SEND HB (err=%lf) (last_hb=%lf)\n", err, last_hb);
    if (err > hb_period) {
        errs++;
        info_wtime(log_fp, "SEND HB (err=%lf)\n", err);
        //info_wtime(log_fp, "TIME ERROR %"PRIu64" out of %"PRIu64"\n", errs, total);
        //dare_server_shutdown();
    }
    
    /* Rearm timer */
    w->repeat = hb_period;
    ev_timer_again(EV_A_ w);
    
    return;

shutdown:
    w->repeat = 0;
    ev_timer_again(EV_A_ w);
    dare_server_shutdown();
}

#endif

/* ================================================================== */
/* Polling */
#if 1
/**
 * Polling callback
 */
static void
poll_cb( EV_P_ ev_idle *w, int revents )
{
    polling();  
}

/**
 * Polling for different events
 */
static void
polling()
{
    /* Check if the termination flag was set */
    if (dare_state & TERMINATE) {
        dare_server_shutdown();
    }
    /* Poll UD connection for incoming messages */
    poll_ud();

    /* Stop here if not recovered yet */
    if (!(dare_state & LOG_RECOVERED))
        return;

/* Some code used for debugging and checking various IB properties */
#if 0
    while(1) {
        if (data.config.idx == 0) {
            SID_SET_TERM(data.ctrl_data->sid, 20);
            dare_ib_revoke_log_access();
            dare_ib_restore_log_access();
            int i;
            for (i = 21; i < 40; i++) {
                info_wtime(log_fp, "RDMA write (PSN=%d)...\n", i);
                dare_ib_send_msg();
            }
            info_wtime(log_fp, "DONE\n");
            sleep(4);
            for (i = 21; i < 40; i++) {
                info_wtime(log_fp, "RDMA write (PSN=%d)...\n", i);
                dare_ib_send_msg();
            }
            info_wtime(log_fp, "DONE\n");
        }
        else if (data.config.idx == 1) {
            SID_SET_TERM(data.ctrl_data->sid, 20);
            dare_ib_revoke_log_access();
            dare_ib_restore_log_access();
            sleep(2);
            SID_SET_TERM(data.ctrl_data->sid, 40);
            dare_ib_revoke_log_access();
            dare_ib_restore_log_access();
            while(1) {
                dare_ib_send_msg();
                if (dare_state & TERMINATE) {
                    dare_server_shutdown();
                }
            }
        }
        else if (data.config.idx == 2) {
            SID_SET_TERM(data.ctrl_data->sid, 40);
            dare_ib_revoke_log_access();
            dare_ib_restore_log_access();
            sleep(3);
            int i;
            for (i = 0; i < 5; i++) {
                info_wtime(log_fp, "RDMA write...\n");
                //dare_ib_send_msg();
            }
        }
        dare_server_shutdown();
        
        if (dare_state & TERMINATE) {
            dare_server_shutdown();
        }
    }
#endif
    
    /* Check the number of failed attempts to access a server 
    through the CTRL QP */
    check_failure_count();

    if (IS_LEADER) {
        /* Try to commit new log entries */
        commit_new_entries();
    }
    else {
        /* Poll for non SM log entries (CONFIG, HEAD...) */
        poll_config_entries();
    }

    /* Apply new committed entries */
    apply_committed_entries();

    if (IS_CANDIDATE) {
        /* Check the number of votes */
        poll_vote_count();
    
    }
    
    /* Poll for vote requests; 
    Note: if the SID of the candidate has a larger term, even a leader 
    or a candidate can vote */
    poll_vote_requests();
    
    if (IS_LEADER) {
        /* Check if log pruning is required */
        force_log_pruning();
    }
}

/**
 * Poll for UD messages
 */
static void
poll_ud()
{
    uint8_t type = dare_ib_poll_ud_queue();
    if (MSG_ERROR == type) {
        error(log_fp, "Cannot get UD message\n");
        dare_server_shutdown();
    }
    switch(type) {
        case CFG_REPLY:
        {
            dare_state |= JOINED;
            info(log_fp, "I got accepted into the cluster: idx=%"PRIu8"\n", 
                data.config.idx); PRINT_CID_(data.config.cid);
            
            /* Start RC discovery */
            ev_set_cb(&timer_event, exchange_rc_info_cb);
            timer_event.repeat = NOW;
            ev_timer_again(data.loop, &timer_event);
            break;
        }
        case RC_SYNACK:
        case RC_ACK:
        {
            /* Note: we can get here more than once; every time the server 
             * gets either a new RC_SYN or a new RC_ACK after a majority */
            if (!(dare_state & RC_ESTABLISHED)) {
                dare_state |= RC_ESTABLISHED;
                info(log_fp, "\n>> Connection establish <<\n");
                text(log_fp, "My SID is: "); PRINT_SID_(data.ctrl_data->sid);
                text(log_fp, "My CID is: "); PRINT_CID_(data.config.cid);
                print_rc_info();

                /* For initial start-up, skip recovery */
                if (!(dare_state & JOINED)) {
                    /* Set a periodic timer that update the RC info */
                    ev_set_cb(&timer_event, update_rc_info_cb);
                    timer_event.repeat = rc_info_period;
                    ev_timer_again(data.loop, &timer_event);
                    dare_state |= LOG_RECOVERED | SM_RECOVERED;
                    start_election();
                }
                else {
                    /* Start recovery */
                    ev_set_cb(&timer_event, get_replicated_vote_cb);
                    timer_event.repeat = NOW;
                    ev_timer_again(data.loop, &timer_event);
                }
            }
            break;
        }
    }
}

/** 
 * Check the number of failed attempts to access a server 
 * through the ctrl QP 
 */
static void 
check_failure_count()
{
    uint8_t i, connections = 0, 
            size = get_group_size(data.config);

    dare_cid_t cid = data.config.cid;
    for (i = 0; i < size; i++) {
        if (!CID_IS_SERVER_ON(cid, i)) continue;
        if (data.config.servers[i].fail_count >= PERMANENT_FAILURE) {
            //debug(log_fp, "Suspecting server %"PRIu8" to have failed\n", i);
            /* In stable configuration, the leader can remove 
            unresponsive servers (on the CTRL QP) */ 
            if ( (IS_LEADER) && (CID_STABLE == data.config.cid.state) ) {
            //log_append_entry(data.log, SID_GET_TERM(data.ctrl_data->sid), 0, 0, 
              //  NOOP, NULL);
                dare_ib_disconnect_server(i);
                CID_SERVER_RM(cid, i);
                info_wtime(log_fp, "REMOVE SERVER p%"PRIu8"\n", i);
                //info(log_fp, "   # Time till removal: %lf\n", ev_now(data.loop) - start_ts); 
            }
        }
        else {
            connections++;
        }
    }
    if (connections <= size / 2) {
        // TODO fix this; correctness should not depend on a majority :(
        info(log_fp, "Not enough connections... bye bye\n");
        dare_server_shutdown();
    }
    if ( (IS_LEADER) && !equal_cid(cid, data.config.cid) ) {
        /* Append CONFIG entry indicating removed servers */
        PRINT_CONF_TRANSIT(data.config.cid, cid);
        data.config.cid = cid;
        data.config.req_id = 0;
        data.config.clt_id = 0;
        log_append_entry(data.log, SID_GET_TERM(data.ctrl_data->sid), 0, 0, 
                CONFIG, &data.config.cid);
    }
}

#endif

/* ================================================================== */
/* Leader election */
#if 1
/**
 * Generate random election timeout in seconds
 */
static double
random_election_timeout()
{
    /* Generate time in microseconds in given interval */
    struct timeval tv;
    gettimeofday(&tv,NULL);
    uint64_t seed = data.config.idx*((tv.tv_sec%100)*1e6+tv.tv_usec);
    srand48(seed);
    uint64_t timeout = (lrand48() % (elec_timeout_high-elec_timeout_low)) 
                        + elec_timeout_low;
//info(log_fp, "election to in sec: %lf\n", (double)timeout * 1e-6);
    /* Return time in seconds */
    return (double)timeout * 1e-6;
}

static double
hb_timeout()
{
    if (hb_timeout_flag) {
        return recomputed_hb_timeout;
    }
    return 10 * hb_period;
}

/**
 * Start election
 */
static void
start_election()
{
    int rc, i;
    
    /* Get the latest SID */
    uint64_t new_sid = 0;    
    
    /* Set SID to [t+1|0|own_idx] */
    SID_SET_TERM(new_sid, SID_GET_TERM(data.ctrl_data->sid) + 1);
    SID_UNSET_L(new_sid);                   // no leader :(
    SID_SET_IDX(new_sid, data.config.idx);  // I can be the leader :)
    rc = server_update_sid(new_sid, data.ctrl_data->sid);
    if (0 != rc) {
        return;
    }
    
    info(log_fp, "\n");
    info_wtime(log_fp, "[T%"PRIu64"] Start election\n", 
            SID_GET_TERM(data.ctrl_data->sid));
    PRINT_CID(data.config.cid); PRINT_SID_(data.ctrl_data->sid);
    INFO_PRINT_LOG(log_fp, data.log);
    
    TEXT_PRINT_LOG(log_fp, data.log);
 
    /* Revoke access to the local log; we need exclusive access */
    rc = dare_ib_revoke_log_access();
    if (0 != rc) {
        /* This should never happen */
        error_exit(1, log_fp, "Cannot get exclusive access to local log\n");
    }
   
    /**
     *  Become a candidate 
     */
    uint8_t size = get_group_size(data.config);
    for (i = 0; i < size; i++) {
        /* Clear votes from a previous election */
        data.ctrl_data->vote_ack[i] = data.log->len;
        /* Set next step of the normal operation process */
        data.config.servers[i].next_lr_step = LR_GET_WRITE;
        /* Set send flag */
        data.config.servers[i].send_flag = 1;
    }

    /* Restart HB mechanism in receive mode; cannot wait forever for 
    followers to respond to me */
    ev_set_cb(&hb_event, hb_receive_cb);
    hb_event.repeat = random_election_timeout();
    //debug(log_fp, "Schedule HB event in %lf sec\n", hb_event.repeat);
    ev_timer_again(data.loop, &hb_event); 
    
    /* Send vote requests */
    rc = dare_ib_send_vote_request();
    if (0 != rc) {
        /* This should never happen */
        error_exit(1, log_fp, "Cannot send vote request\n");
    }
}

/**
 * Check the number of votes received
 */
static void 
poll_vote_count()
{
    int rc;
    uint8_t vote_count[2];
    vote_count[0] = 1;
    vote_count[1] = 1;
    uint8_t i, size = get_group_size(data.config);
    uint64_t remote_commit;
    
    //TIMER_INIT;
    
    //TIMER_START(log_fp, "Start counting votes...");
    for (i = 0; i < size; i++) {
        if (i == data.config.idx) continue;
        remote_commit = data.ctrl_data->vote_ack[i];
        if (data.log->len == remote_commit) {
            /* No reply from this server */
            continue;
        }
        /* Count votes */
        if (i < data.config.cid.size[0]) {
            vote_count[0]++;
        }
        if (i < data.config.cid.size[1]) {
            vote_count[1]++;
        }
        
        /* Store the received commit offset */
        data.ctrl_data->log_offsets[i].commit = remote_commit;
        /* No need to get the commit offset for this server */
        data.config.servers[i].next_lr_step = LR_GET_NCE_LEN;
        if (log_is_offset_larger(data.log, remote_commit, data.log->commit)) {
            /* Update local commit offset */
            data.log->commit = remote_commit;
        }
    }
    //TIMER_STOP(log_fp);
        
    if (vote_count[0] <  data.config.cid.size[0] / 2 + 1) {
        return;
    }
    if (CID_STABLE != data.config.cid.state) {
        if (vote_count[1] <  data.config.cid.size[1] / 2 + 1) {
            return;
        }
    }
    info(log_fp, "Votes:");
    for (i = 0; i < size; i++) {
        if (i == data.config.idx) continue;
        remote_commit = data.ctrl_data->vote_ack[i];
        if (data.log->len != remote_commit) {
            info(log_fp, " (p%"PRIu8")", i);
        }
    }
    info(log_fp, "\n");
    
    /**
     *  Won election: become leader 
     */
    //debug(log_fp, "vote count = %"PRIu8"\n", vote_count[0]);
    
    /* Update own SID to [t|1|own_idx] */
    uint64_t new_sid = data.ctrl_data->sid;
    SID_SET_L(new_sid);
    rc = server_update_sid(new_sid, data.ctrl_data->sid);
    if (0 != rc) {
        return;
    }    
    info_wtime(log_fp, "[T%"PRIu64"] LEADER\n", SID_GET_TERM(new_sid));
    INFO_PRINT_LOG(log_fp, data.log);
    info(log_fp, "CID: [%02"PRIu8"|%02"PRIu8"|%d|%03"PRIu32"]\n", 
            data.config.cid.size[0], data.config.cid.size[1], 
            data.config.cid.state, data.config.cid.bitmask);
    text(log_fp, "SID:"); PRINT_SID_(data.ctrl_data->sid);
    
    /* Go through the CONFIG entries => update configuration */
    poll_config_entries();
    
    /* Apply all committed entries => equal apply and commit offsets 
    Note: when applying an unstable CONFIG entry from the same epoch, 
    the new leader automatically adds the subsequent transition */
    apply_committed_entries();
    
    /* Check the state of the configuration */
    if (CID_STABLE == data.config.cid.state) {
        /* Append CONFIG entry to be able to commit previous entries; 
         also, the CONFIG entry removes (log adjustment) all other 
         invalid CONFIG entries; hence, a consistent configuration */
        debug(log_fp, "Adding BLANK entry (CONFIG)\n");
        data.config.req_id = 0;
        data.config.clt_id = 0;
        data.last_write_csm_idx = log_append_entry(data.log, 
            SID_GET_TERM(data.ctrl_data->sid), 0, 0, CONFIG, &data.config.cid);
        goto become_leader;
    }
    
    /* Unstable configuration */
    uint64_t offset = data.config.cid_offset;
    dare_log_entry_t *entry;
    while (log_offset_end_distance(data.log, offset)) {
        entry = log_get_entry(data.log, &offset);
        if (!log_fit_entry(data.log, offset, entry)) {
            /* Not enough place for an entry (with the command) */
            offset = 0;
            continue;
        }
        if ( (CONFIG == entry->type) && 
            (entry->idx > data.config.cid_idx) )
            break;
            
        /* Advance offset */
        offset += log_entry_len(entry);
    }
    if (log_offset_end_distance(data.log, offset)) {
        /* There is at least one CONFIG entry that is not applied
        => the last unstable CONFIG is also not applied; when the leader 
        applies it, the configuration state changes */
        data.last_write_csm_idx = log_append_entry(data.log, 
            SID_GET_TERM(data.ctrl_data->sid), 0, 0, NOOP, NULL);
        goto become_leader;
    }
    
    /* The unstable CONFIG entry is already committed and there is no 
    other CONFIG entry; then just add a CONFIG entry 
    NOTE: this covers the case when the previous leader failed after 
    committing a transitional/extended configuration, but before 
    applying it */
    dare_cid_t old_cid = data.config.cid;
    if (CID_EXTENDED == entry->data.cid.state) {
        /* Move configuration to a transitional state */
        data.config.cid.state = CID_TRANSIT;
    }
    else {
        /* Move configuration to a stable state */
        data.config.cid.state = CID_STABLE;
        /* Remove additional servers */
        uint8_t i;
        for (i = data.config.cid.size[0] - 1; 
            i > data.config.cid.size[1]; i--)
        {
            if (i == data.config.idx) {
                /* Let the leader finish the operation first
                Note: if the leader succeeds in committing 
                the CONFIG entry, the servers do not have to 
                be aware of the commit; a majority will have 
                the new entry => the new leader will have it */
                dare_state |= DIE_AF_COMMIT;
                CID_SERVER_RM(data.config.cid, i);
                continue;
            }
            if (!CID_IS_SERVER_ON(data.config.cid, i)) {
                continue;
            }
            CID_SERVER_RM(data.config.cid, i);
            dare_ib_disconnect_server(i);
        }
        data.config.cid.size[0] = data.config.cid.size[1];
        data.config.cid.size[1] = 0;
    }
    /* Append CONFIG entry as BLANK entry */
    PRINT_CONF_TRANSIT(old_cid, data.config.cid);
    data.last_write_csm_idx = log_append_entry(data.log, 
        SID_GET_TERM(data.ctrl_data->sid), data.config.req_id, 
        data.config.clt_id, CONFIG, &data.config.cid);

become_leader:
    ep_dp_reset_wait_idx(&data.endpoints);
    /* Start sending heartbeats */
    ev_set_cb(&hb_event, hb_send_cb);
    hb_event.repeat = NOW;
    ev_timer_again(data.loop, &hb_event);
    
    /* Suspend timeout adjusting mechanism */
    to_adjust_event.repeat = 0;
    ev_timer_again(data.loop, &to_adjust_event);
    
    /* Start log pruning */
    size = get_extended_group_size(data.config);
    for (i = 0; i < size; i++) {
        data.ctrl_data->apply_offsets[i] = data.log->head;
    }
    prune_event.repeat = NOW;
    ev_timer_again(data.loop, &prune_event);
    
    /* Gain log access */
    rc = dare_ib_restore_log_access();
    if (0 != rc) {
        /* This should never happen */
        error_exit(1, log_fp, "Cannot restore log access\n");
    }
}

/**
 * Poll for vote requests from other servers
 * the vote request is a tuple (sid, idx, term)
 *  - first, check if there is a decent sid 
 *  - then, revoke access to the log and do the up-to-date test
 */
static void
poll_vote_requests()
{
    int rc;
    uint8_t i, size = get_group_size(data.config);
    uint64_t new_sid;
    vote_req_t *request;
    
    /* To avoid crazy servers removing good leaders */
    if (SID_GET_L(data.ctrl_data->sid)) {
        /* Active leader known; just ignore vote requests 
        Note: a leader renounces its leadership when it receives 
        an HB reply from a server with a larger term */
        return;
    }
    
    /* No leader known; make sure about this, do not wait for the 
    timeout to check the HB array.
    !! This applies for nodes that voted but are not aware of the outcome */
    uint8_t possible_leader = SID_GET_IDX(data.ctrl_data->sid);
    uint64_t hb = data.ctrl_data->hb[possible_leader];
    if ( (0 != hb) && (SID_GET_TERM(hb) == SID_GET_TERM(data.ctrl_data->sid)) ) {
        /* My vote counts (democracy at its best)...  */
        server_update_sid(hb, data.ctrl_data->sid);
        return;
    }

    /* Okay, so there is no known leader;
    look for vote requests and find out the request with the best SID.
    
    Note: set the L flag to avoid voting twice in the same term:
          SID=[TERM|L|IDX] => [TERM|1|voted_idx] > [TERM|0|*] */
    uint64_t old_sid = data.ctrl_data->sid; SID_SET_L(old_sid);
    uint64_t best_sid = old_sid;
    for (i = 0; i < size; i++) {
        if (i == data.config.idx) continue;
        request = &(data.ctrl_data->vote_req[i]);
        if (request->sid != 0) {
            text(log_fp, "Vote request from:"); PRINT_SID_(request->sid); 
        }
        if (best_sid >= request->sid) {
            /* Candidate's SID is not good enough; drop request */
            request->sid = 0;
            continue;
        }
        /* Don't reset "request->sid" yet */
        best_sid = request->sid;
        /* Note: we iterate through the vote requests in idx order; thus, 
        other requests for the same term are also considered */
    }
    if (best_sid == old_sid) {
        /* No better SID */
        return;
    }
    
    /* I thought I saw a better candidate ... */
    uint64_t highest_term = SID_GET_TERM(best_sid);
    TIMER_INIT;
    TIMER_START(log_fp, "Vote request from a good candidate...");
    
    //~ info_wtime(log_fp, "Vote request from (p%"PRIu8", T%"PRIu64")\n", 
        //~ SID_GET_IDX(best_sid), SID_GET_TERM(best_sid));        
           
    /* Revoke remote access to local log; need to have exclusive 
     * access to the log to get the last entry */
    rc = dare_ib_revoke_log_access();
    if (0 != rc) {
        /* This should never happen */
        error_exit(1, log_fp, "Cannot lock local log\n");
    }
    
    /* Create not committed buffer & get best request */
    vote_req_t best_request;
    best_request.sid = old_sid;
    dare_nc_buf_t *nc_buf = &data.log->nc_buf[data.config.idx];
    log_entries_to_nc_buf(data.log, nc_buf);
    if (0 == nc_buf->len) {
        /* There are no not committed entries */
        uint64_t tail = log_get_tail(data.log);
        if (tail == data.log->len) {
            best_request.index = 0;
            best_request.term  = 0;
        }
        else {
            dare_log_entry_t* last_entry = 
                    log_get_entry(data.log, &tail);
            best_request.index = last_entry->idx;
            best_request.term  = last_entry->term;
        }
    }
    else {
        /* Get last entry from not committed buffer */
        best_request.index = nc_buf->entries[nc_buf->len-1].idx;
        best_request.term  = nc_buf->entries[nc_buf->len-1].term;
    }
    
//text(log_fp, "\n   Local [idx=%"PRIu64"; term=%"PRIu64"]\n", best_request.index, best_request.term);        
    info(log_fp, "   # Local [idx=%"PRIu64"; term=%"PRIu64"]\n", 
            best_request.index, best_request.term);        
    /* Choose the best candidate */
    for (i = 0; i < size; i++) {
        request = &(data.ctrl_data->vote_req[i]);
        if (best_request.sid > request->sid) {
            /* Candidate's SID is not good enough; drop request */
            request->sid = 0;
            continue;
        }
        if (highest_term < SID_GET_TERM(request->sid))
            highest_term = SID_GET_TERM(request->sid);
//text(log_fp, "   Remote(%"PRIu8") [idx=%"PRIu64"; term=%"PRIu64"]\n", i, request->index,request->term);
        info(log_fp, "   # Remote(p%"PRIu8") [idx=%"PRIu64"; term=%"PRIu64"]\n", 
                    i, request->index, request->term);
        if ( (best_request.term > request->term) || 
             ((best_request.term == request->term) && 
              (best_request.index > request->index)) )
        {
            /* Candidate's log is not good enough; drop request */
            request->sid = 0;
            continue;
        }
        /* I like this candidate */
        best_request.index = request->index;
        best_request.term = request->term;
        best_request.sid = request->sid;
        best_request.cid = request->cid;
        request->sid = 0;
    }
text(log_fp, "   Best [idx=%"PRIu64"; term=%"PRIu64"]\n", best_request.index, best_request.term);

    if (best_request.sid == old_sid) {
        /* Local log is better than remote logs; yet, local term is too low. 
        Increase TERM to increase chances to win election */
        new_sid = data.ctrl_data->sid;
        SID_SET_TERM(new_sid, highest_term);
        SID_SET_IDX(new_sid, data.config.idx);  // don't vote for anyone
        rc = server_update_sid(new_sid, data.ctrl_data->sid);
        if (0 != rc) {
            /* Could not update my SID; just return */
            return;
        }
        TIMER_STOP(log_fp); 
        return;
    }
    
    /* ... I did, I did saw a better candidate :) */
    info_wtime(log_fp, "[T%"PRIu64"] Vote for p%"PRIu8"\n", 
                SID_GET_TERM(best_request.sid), 
                SID_GET_IDX(best_request.sid));
//info(log_fp, "MY SIDS: "); PRINT_SID(data.ctrl_data->sid);PRINT_SID_(data.ctrl_data->sid);
//info(log_fp, "My log: "); INFO_PRINT_LOG(log_fp, data.log);
     
    /* Stop HB mechanism for the moment ... */
    hb_event.repeat = 0.;
    ev_timer_again(data.loop, &hb_event); 
    
    /* Update my local SID to show support */
    rc = server_update_sid(best_request.sid, data.ctrl_data->sid);
    if (0 != rc) {
        /* Could not update my SID; just return */
        return;
    }       

    /* Update configuration according to the candidate */
    update_cid(best_request.cid);
            
    /* Replicate this SID in case I crash */
    rc = dare_ib_replicate_vote();
    if (0 != rc) {
        /* This should never happen */
        error_exit(1, log_fp, "Cannot replicate votes\n");
    }
//debug(log_fp, "Vote replicated\n");
    
    // same as server_to_follower(), but no log_entries_to_nc_buf() 
    
    /* Stop log pruning */
    prune_event.repeat = 0.;
    ev_timer_again(data.loop, &prune_event);

    leader_failed = 0;
    if (!hb_timeout_flag) {
        /* Restart timeout adjusting mechanism */
        to_adjust_event.repeat = recomputed_hb_timeout;
        ev_timer_again(data.loop, &to_adjust_event);
    }
    
    /* Restore access to the log (for the supported candidate) */
    rc = dare_ib_restore_log_access();
    if (0 != rc) {
        /* This should never happen */
        error_exit(1, log_fp, "Cannot restore log access\n");
    }
    
    /* Send ACK to the candidate */
    rc = dare_ib_send_vote_ack();
    if (rc < 0) {
        /* Operation failed; start an election */
        start_election(); 
        return;
    }
    if (0 != rc) {
        /* This should never happen */
        error_exit(1, log_fp, "Cannot send vote ack\n");
    }
//debug(log_fp, "Send vote ACK\n");

    /* Restart HB mechanism in receive mode */
    ev_set_cb(&hb_event, hb_receive_cb);
    // TODO: this should be a different timeout
    //double tmp = random_election_timeout(); 
    double tmp = hb_timeout();
    //hb_event.repeat = random_election_timeout();
    hb_event.repeat = tmp;
    ev_timer_again(data.loop, &hb_event); 
    //info_wtime(log_fp, "Just voted; lets wait for %lf sec\n", tmp);
    
    TIMER_STOP(log_fp); 
}

#endif

/* ================================================================== */
/* Normal operation */
#if 1

static void 
commit_new_entries()
{
    int rc;
 
    if (log_offset_end_distance(data.log, data.log->commit)) {
        //info_wtime(log_fp, "TRY TO COMMIT NEW ENTRY\n");
        //INFO_PRINT_LOG(log_fp, data.log);
        rc = dare_ib_write_remote_logs(1);
        if (0 != rc) {
            error(log_fp, "Cannot write remote logs\n");
        }
    }
    else if (!is_log_empty(data.log)) {
        /* Check if any server is behind */
        uint8_t i, size = get_group_size(data.config);
        for (i = 0; i < size; i++) {
            if ( (i == data.config.idx) ||
                !CID_IS_SERVER_ON(data.config.cid, i) ||
                (data.config.servers[i].fail_count >= PERMANENT_FAILURE) )
            {
                continue;
            }
            if (data.ctrl_data->vote_ack[i] == data.log->len) {
                continue;
            }
            if ( (data.config.servers[i].next_lr_step != LR_UPDATE_LOG) ||
                (data.ctrl_data->log_offsets[i].end != data.log->end) )
            {
            //info(log_fp, "p%"PRIu8": log update -- step=%"PRIu8", end=%"PRIu64"\n", i, data.config.servers[i].next_lr_step, data.ctrl_data->log_offsets[i].end);
                /* Write remote logs to bring server up to date */
                rc = dare_ib_write_remote_logs(0);
                if (0 != rc) {
                    error(log_fp, "Cannot write remote logs\n");
                } 
                break;
            }
        }
    }
}

/**
 * Poll for new committed entries and apply them to the SM
 */
static void 
apply_committed_entries()
{
    int rc;
    int once = 0;
    
    uint64_t old_apply = data.log->apply;
    dare_log_entry_t *entry;
    while (log_is_offset_larger(data.log, 
                data.log->commit, data.log->apply))
    {
        if (!IS_LEADER) {
            //text_wtime(log_fp, "New committed entries ");
            //TEXT_PRINT_LOG(log_fp, data.log);
        }
        else {
            if (!once) {
                //info_wtime(log_fp, "New entries committed: %"PRIu64" -> %"PRIu64"\n", 
                //    data.log->apply, data.log->commit);
                once = 1;
            }
        }

        /* Get entry (cannot be NULL);
        Note: the apply offset is updated locally  */
        entry = log_get_entry(data.log, &data.log->apply);
        if (!log_fit_entry(data.log, data.log->apply, entry)) {
            /* Not enough place for an entry (with the command) */
            data.log->apply = 0;
            continue;
        }
        
        if (!IS_LEADER)
            goto apply_entry;

        /* Just the leader... */
        if ( (NOOP == entry->type) || (HEAD == entry->type) )
            goto apply_next_entry;
        
        /* CONFIG entry */
        if (CID_STABLE == entry->data.cid.state) {
            if (entry->req_id != 0) {
                /* Send reply to the client */
                rc = dare_ib_send_clt_reply(entry->clt_id, 
                            entry->req_id, CONFIG);
                if (0 != rc) {
                    error(log_fp, "Cannot send client reply\n");
                }
                if (dare_state & DIE_AF_COMMIT) {
                    info(log_fp, "I was a victim of a downsize... bye bye\n");
                    dare_server_shutdown();
                }
            }
            goto apply_next_entry;
        }
        if (data.config.cid.epoch > entry->data.cid.epoch) {
            /* The entry is from a previous configuration; 
            if unstable, ignore it */
            goto apply_next_entry;
        }
        
        /* Unstable CONFIG entry */
        dare_cid_t old_cid = data.config.cid;
        uint64_t req_id = entry->req_id;
        uint16_t clt_id = entry->clt_id;

        if (CID_EXTENDED == entry->data.cid.state) {
            /* Move configuration to transitional state */
            data.config.cid.state = CID_TRANSIT;
            if (entry->req_id != 0) {
                /* Send reply to the server so that it can join the group */
                rc = dare_ib_send_clt_reply(entry->clt_id, 
                            entry->req_id, CONFIG);
                if (0 != rc) {
                    error(log_fp, "Cannot send client reply\n");
                }
                /* Avoid sending a reply twice */
                req_id = 0;
                clt_id = 0;
            }
        }
        else if (CID_TRANSIT == entry->data.cid.state) {
            /* Move configuration to a stable state */
            uint8_t i;
            data.config.cid.state = CID_STABLE;
            /* Remove additional servers */
            for (i = data.config.cid.size[1]; 
                i < data.config.cid.size[0]; i++)
            {
                if (i == data.config.idx) {
                    /* Let the leader finish the operation first
                    Note: if the leader succeeds in committing 
                    the CONFIG entry, the servers do not have to 
                    be aware of the commit; a majority will have 
                    the new entry => the new leader will have it */
                    info_wtime(log_fp, "They gonna kill me\n");
                    dare_state |= DIE_AF_COMMIT;
                    CID_SERVER_RM(data.config.cid, i);
                    continue;
                }
                if (!CID_IS_SERVER_ON(data.config.cid, i)) {
                    continue;
                }
                CID_SERVER_RM(data.config.cid, i);
                dare_ib_disconnect_server(i);
            }
            data.config.cid.size[0] = data.config.cid.size[1];
            data.config.cid.size[1] = 0;
        }
        data.config.req_id = req_id;
        data.config.clt_id = clt_id;
        PRINT_CONF_TRANSIT(old_cid, data.config.cid);
        /* Append CONFIG entry */
        log_append_entry(data.log, SID_GET_TERM(data.ctrl_data->sid), 
                        req_id, clt_id, CONFIG, &data.config.cid);
        goto apply_next_entry;
        
apply_entry:        
        /* Apply entry */
        if (P_CONNECT == entry->type || P_SEND == entry->type || P_CLOSE == entry->type ) {
            if (!IS_LEADER) {
                if (entry->idx % 10000 == 0) {
                    info_wtime(log_fp, "APPLY LOG ENTRY: (%"PRIu64"; %"PRIu64")\n", 
                                entry->idx, entry->term);
                    INFO_PRINT_LOG(log_fp, data.log);
                }
            }
            //else {
            //    if (SID_GET_TERM(data.ctrl_data->sid) < 50) sleep(1);
            //}
            if (!IS_LEADER)
                data.apply_cmd(entry->clt_id, entry->type, entry->data.cmd.len, &entry->data.cmd.cmd, data.up_para);
            else
                data.last_cmt_csm_idx++;
            last_applied_entry.idx = entry->idx;
            last_applied_entry.term = entry->term;
            last_applied_entry.offset = data.log->apply;
            /* Needed for answering read requests */
            data.last_cmt_write_csm_idx = entry->idx;
        }
        
apply_next_entry:        
        /* Advance apply offset */
        data.log->apply += log_entry_len(entry);
    }
    
}

static void
prune_log_cb( EV_P_ ev_timer *w, int revents )
{
    int rc;
    rc = log_pruning();
    if (0 != rc) {
        error(log_fp, "Cannot prune the log\n");
        goto shutdown;
    }
    
    w->repeat = log_pruning_period;
    ev_timer_again(EV_A_ w);
    return;
    
shutdown:
    w->repeat = 0;
    ev_timer_again(EV_A_ w);
    dare_server_shutdown();     
}

static int
log_pruning()
{
    int rc;
    uint8_t i, size;
    
    if (!IS_LEADER) return 0;
    
    /* We have the requirement R1: for any server, the head offset must 
    never decrease (except for when wrapping-around the end of 
    the circular buffer).
    Periodically, the leader reads all the remote apply offsets and sets 
    its own head offset to the smallest one. For a given term, this 
    implies property P1: for any server, leader.h <= server.r. 
    The servers could update their head offset to the leader's head 
    offset; however, for R1 to hold after an election, we need 
    property P2: during all terms, for any server, server.h <= leader.h. 
    Thus, a server can be elected leader only if server.h = former_leader.h. 
    To achieve this, the leader updates the remote head offsets by 
    adding a special entry in the log; once this entry is committed, 
    the new leader must have it in its log, hence, P2 holds.
    Properties P1 and P2 ensure the correctness of log pruning.

    Also, note that the apply offset of any unresponsive server equals 
    the leader's head offset; moreover, the leader has no access to 
    the log of a server that recovers. Thus, property P3: the leader's 
    head offset remains constant during a server's recovery.
    */
    
    /* Find minimum remote apply offset */
    size = get_extended_group_size(data.config);
    uint64_t min_offset = data.log->apply;
    for (i = 0; i < size; i++) {
        if (!CID_IS_SERVER_ON(data.config.cid, i)) {
            /* Server is OFF */
            data.ctrl_data->apply_offsets[i] = data.log->apply;
        }
        if (log_is_offset_larger(data.log, min_offset, 
                    data.ctrl_data->apply_offsets[i]))
            min_offset = data.ctrl_data->apply_offsets[i];
        //info(log_fp, "   # (p%"PRIu8"): apply=%"PRIu64"\n", i, data.ctrl_data->apply_offsets[i]);
    }
    //info(log_fp, "   # min_offset = %"PRIu64"\n", min_offset);
    if (!log_offset_end_distance(data.log, min_offset)) {
        /* Need to leave an entry in the log */
        min_offset = log_get_tail(data.log);
    }
    //info(log_fp, "   # min_offset = %"PRIu64"; head = %"PRIu64"\n", min_offset, data.log->head);
    if (log_is_offset_larger(data.log, min_offset, data.log->head) &&
            !prev_log_entry_head)
    {
        /* The minimum remote apply offset is larger than the head offset */
        //info_wtime(log_fp, "Advance HEAD offset: %"PRIu64"->%"PRIu64"\n", 
          //          data.log->head, min_offset);
        data.log->head = min_offset;
        
        dare_state &= ~SNAPSHOT;
        
        /* Append a HEAD log entry */
        log_append_entry(data.log, SID_GET_TERM(data.ctrl_data->sid), 
                        0, 0, HEAD, &data.log->head);
        prev_log_entry_head = 1;
    }
    
    /* Get remote apply offsets for next prunning */
    rc = dare_ib_get_remote_apply_offsets();
    if (0 != rc) {
        error_return(1, log_fp, "dare_ib_get_remote_apply_offsets\n");
    }
    
    return 0;
}

static void
force_log_pruning()
{
    uint8_t i, size, target;
    uint64_t log_size = log_offset_end_distance(data.log, data.log->head);

    if (log_size < 0.75 * data.log->len) 
        return;
    
    /* Too many entries in the log */
    info_wtime(log_fp, "FORCED LOG PRUNING\n");
    info(log_fp, "   # log_size=%"PRIu64"; threshold=%lf\n", 
                log_size, 0.75 * data.log->len);
    size = get_extended_group_size(data.config);
    target = data.config.idx;
    uint64_t min_offset = data.log->apply;
    for (i = 0; i < size; i++) {
        if (log_is_offset_larger(data.log, min_offset, 
                        data.ctrl_data->apply_offsets[i]))
        {
            min_offset = data.ctrl_data->apply_offsets[i];
            target = i;
        }
        info(log_fp, "   # (p%"PRIu8"): apply=%"PRIu64"\n", i, data.ctrl_data->apply_offsets[i]);
    }
    if (target != data.config.idx) {
        if (!CID_IS_SERVER_ON(data.config.cid, target)) {
            log_pruning();
            return;
        }
        /* Server to slow; remove it */
        dare_cid_t old_cid = data.config.cid;
        CID_SERVER_RM(data.config.cid, target);
        dare_ib_disconnect_server(target);
        info_wtime(log_fp, "REMOVE SERVER p%"PRIu8"\n", target);
        data.config.req_id = 0;
        data.config.clt_id = 0;
        PRINT_CONF_TRANSIT(old_cid, data.config.cid);
        
        /* Append CONFIG entry */
        log_append_entry(data.log, SID_GET_TERM(data.ctrl_data->sid), 
                        0, 0, CONFIG, &data.config.cid);
        
        /* Update apply offset for this target */
        data.ctrl_data->apply_offsets[i] = data.log->apply;

        /* Prune the log */
        log_pruning();
    }
    else {
        /* All apply offsets equal the leader's apply offset */
        log_pruning();
    }
}

#endif

/* ================================================================== */
/* Group reconfiguration */
#if 1

/**
 * Pool CONFIG and HEAD log entries
 */
static void 
poll_config_entries()
{
    uint64_t head_offset = data.log->head;
    uint64_t offset = data.config.cid_offset;
    uint64_t commit = data.log->commit;
    dare_log_entry_t *entry;
    while (log_offset_end_distance(data.log, offset)) {
        entry = log_get_entry(data.log, &offset);
        
        if (!log_fit_entry(data.log, offset, entry)) {
            /* Not enough place for an entry (with the command) */
            offset = 0;
            continue;
        }
        if (CONFIG == entry->type) {
            /* CONFIG entry
            Note: a follower may repeat uncommitted CONFIG entries;
            this can lower the performance until the follower sees 
            the entry as committed; however, the behavior is correct */
            if (entry->idx > data.config.cid_idx) {
                /* Update CID */
                if (0 == update_cid(entry->data.cid)) {
                    /* The update was successful; store the Request ID 
                    and the LID of the endpoint responsible for this 
                    entry; required on re-election */
                    data.config.req_id = entry->req_id;
                    data.config.clt_id = entry->clt_id;
                }
            }
        }
        else if (HEAD == entry->type) {
            /* Check only committed entries; 
            thus, the leader's head offset is always >= */
            if (!log_is_offset_larger(data.log, offset, commit)) {
                head_offset = entry->data.head;
                dare_state &= ~SNAPSHOT;
            }
        } 
        /* Advance offset */
        offset += log_entry_len(entry);
    }
    if (log_is_offset_larger(data.log, offset, commit)) {
        data.config.cid_offset = commit;
    }
    else {
        data.config.cid_offset = offset;
    }
    if (log_is_offset_larger(data.log, head_offset, data.log->head)) {
        //info_wtime(log_fp, "Advance HEAD offset: %"PRIu64"->%"PRIu64"\n", 
        //            data.log->head, head_offset);
        //info(log_fp, "My log: "); INFO_PRINT_LOG(log_fp, data.log);
        data.log->head = head_offset;
    }
}

/**
 * Update configuration 
 */
static int 
update_cid( dare_cid_t cid )
{
    if (equal_cid(data.config.cid, cid)) {
        return 1;
    }
    PRINT_CONF_TRANSIT(data.config.cid, cid);
    
    uint8_t i, size = cid.size[0];
    if (cid.size[1] > size) {
        size = cid.size[1];
    }
    
    for (i = 0; i < size; i++) {
        if ( CID_IS_SERVER_ON(cid, i) && 
            !CID_IS_SERVER_ON(data.config.cid, i) )
        {
            /* Server arrival; we connect it on request and automatically 
             * in the period RC update */
        }
        else if ( !CID_IS_SERVER_ON(cid, i) && 
                  CID_IS_SERVER_ON(data.config.cid, i) )
        {
            /* Server departure */
            if (i == data.config.idx) {
                /* Ups ... that's me */
                info(log_fp, "Somebody removed me... bye bye\n");
                dare_server_shutdown();
            }
            dare_ib_disconnect_server(i);
            info_wtime(log_fp, "REMOVE SERVER p%"PRIu8"\n", i);
        }
    }
    data.config.cid = cid;
    return 0;
}

#endif

/* ================================================================== */
/* Others */
#if 1

/**
 * Transit server to follower state 
 */
void server_to_follower()
{   
    int rc;

    /* Stop HB mechanism for the moment ... */
    hb_event.repeat = 0.;
    ev_timer_again(data.loop, &hb_event); 
    
    /* Stop log pruning */
    prune_event.repeat = 0.;
    ev_timer_again(data.loop, &prune_event);
    
    leader_failed = 0;
    if (!hb_timeout_flag) {
        /* Restart timeout adjusting mechanism */
        to_adjust_event.repeat = recomputed_hb_timeout;
        ev_timer_again(data.loop, &to_adjust_event);
    }

    /* Revoke log access */
    dare_ib_revoke_log_access();
    
    /* Populate buffer with not committed entries */
    log_entries_to_nc_buf(data.log, 
            &data.log->nc_buf[data.config.idx]);
            
    /* Restore log access according to the new term */
    dare_ib_restore_log_access();
    
    if (SID_GET_L(data.ctrl_data->sid)) {
        /* New leader; send vote ACK to notify it that the log is accessible */
        info_wtime(log_fp, "Send vote ACK to notify p%"PRIu8" that my log is accessible\n", SID_GET_IDX(data.ctrl_data->sid));
        rc = dare_ib_send_vote_ack();
        if (rc < 0) {
            /* Operation failed; start an election */
            start_election(); 
            return;
        }
        if (0 != rc) {
            /* This should never happen */
            error_exit(1, log_fp, "Cannot send vote ack\n");
        }
    }
    
    /* Restart HB mechanism in receive mode */
    ev_set_cb(&hb_event, hb_receive_cb);
    hb_event.repeat = hb_timeout();
    ev_timer_again(data.loop, &hb_event);
}

int server_update_sid( uint64_t new_sid, uint64_t old_sid )
{
    int rc;
    rc = __sync_bool_compare_and_swap(&(data.ctrl_data->sid),
                                    old_sid, new_sid);
    if (!rc) {
        error_exit(1, log_fp, "CAS failed\n");
    }
    return 0;
}

int is_leader() 
{
    return IS_LEADER;
}

static void 
int_handler(int dummy) 
{
    info_wtime(log_fp,"SIGINT detected; shutdown\n");
    //dare_server_shutdown();
    dare_state |= TERMINATE;
}

int leader_handle_submit_req(uint8_t type, ssize_t data_size, void* buf, int clt_id)
{
    sm_cmd_t *cmd = (sm_cmd_t*)malloc(sizeof(sm_cmd_t) + data_size);
    cmd->len = data_size;
    memcpy(cmd->cmd, buf, data_size);
    
    pthread_spin_lock(&data.spinlock);
    count_pair_t* pair = NULL;
    uint64_t req_id;
    switch(type) {
        case P_CONNECT:
            pair = (count_pair_t*)malloc(sizeof(count_pair_t));
            memset(pair,0,sizeof(count_pair_t));
            pair->clt_id = clt_id;
            pair->req_id = 1;
            req_id = pair->req_id;
            HASH_ADD_INT(data.hash_map, clt_id, pair);
            break;
        case P_SEND:
            HASH_FIND_INT(data.hash_map, &clt_id, pair);
            pair->req_id = ++pair->req_id;
            req_id = pair->req_id;
            count_pair_t* replaced_pair = NULL;
            HASH_REPLACE_INT(data.hash_map, clt_id, pair, replaced_pair);
            break;
        case P_CLOSE:
            HASH_FIND_INT(data.hash_map, &clt_id, pair);
            pair->req_id = ++pair->req_id;
            req_id = pair->req_id;
            HASH_DEL(data.hash_map, pair);
            break;
    }
    log_append_entry(data.log, SID_GET_TERM(data.ctrl_data->sid), req_id, clt_id, type, cmd);
    uint64_t csm_idx = ++data.last_csm_idx;
    pthread_spin_unlock(&data.spinlock);

poll_committed_entries:
    if (csm_idx < data.last_cmt_csm_idx)
        goto poll_committed_entries;
    
    free(cmd);
}

#endif
