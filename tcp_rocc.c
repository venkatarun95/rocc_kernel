/* RoCC (Robust Congestion Control)
 */

#include <net/tcp.h>

#undef ROCC_DEBUG

// Should be a power of two so rocc_num_intervals_mask can be set
static const u16 rocc_num_intervals = 16;
// rocc_num_intervals expressed as a mask. It is always equal to
// rocc_num_intervals-1
static const u16 rocc_num_intervals_mask = 15;
static const u32 rocc_min_cwnd = 2;
// Maximum tolerable loss rate, expressed as `loss_thresh / 1024`. Calculations
// are faster if things are powers of 2
static const u64 rocc_loss_thresh = 64;
static const u32 rocc_alpha = 2;

// To keep track of the number of packets acked over a short period of time
struct rocc_interval {
	// Starting time of this interval
	u64 start_us;
	u32 pkts_acked;
	u32 pkts_lost;
	bool app_limited;
};

static u32 id = 0;
struct rocc_data {
	// Circular queue of intervals
	struct rocc_interval *intervals;
	// Index of the last interval to be added
	u16 intervals_head;

	u32 min_rtt_us;

	// debug helper
	u32 id;
};

static void rocc_init(struct sock *sk)
{
	struct rocc_data *rocc = inet_csk_ca(sk);
	u16 i;

	rocc->intervals = kzalloc(sizeof(struct rocc_interval) * rocc_num_intervals,
				  GFP_KERNEL);
	for (i = 0; i < rocc_num_intervals; ++i) {
		rocc->intervals[i].start_us = 0;
		rocc->intervals[i].pkts_acked = 0;
		rocc->intervals[i].pkts_lost = 0;
		rocc->intervals[i].app_limited = false;
	}
	rocc->intervals_head = 0;

	rocc->min_rtt_us = U32_MAX;
	++id;
	rocc->id = id;

	cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
}

static u32 rocc_get_mss(struct tcp_sock *tsk)
{
	// TODO: Figure out if mss_cache is the one to use
	return tsk->mss_cache;
}

/* was the rocc struct fully inited */
static bool rocc_valid(struct rocc_data *rocc)
{
	return (rocc && rocc->intervals);
}

static void rocc_process_sample(struct sock *sk, const struct rate_sample *rs)
{
	struct rocc_data *rocc = inet_csk_ca(sk);
	struct tcp_sock *tsk = tcp_sk(sk);
	u32 rtt_us;
	u16 i, id;
	u32 hist_us;
	u64 timestamp;
	u32 interval_length;
	// Number of packets acked and lost in the last `hist_us`
	u32 pkts_acked, pkts_lost;
	u32 cwnd;
	bool loss_mode, app_limited;

	if (!rocc_valid(rocc))
		return;

	// Is rate sample valid?
	if (rs->delivered < 0 || rs->interval_us < 0)
		return;

	// Get initial RTT - as measured by SYN -> SYN-ACK.  If information
        // does not exist - use U32_MAX as RTT
	if (tsk->srtt_us) {
		rtt_us = max(tsk->srtt_us >> 3, 1U);
	} else {
		rtt_us = U32_MAX;
	}

	if (rtt_us < rocc->min_rtt_us)
		rocc->min_rtt_us = rtt_us;

	if (rocc->min_rtt_us == U32_MAX)
		hist_us = U32_MAX;
	else
		hist_us = 2 * rocc->min_rtt_us;

	// Update intervals
	timestamp = tsk->tcp_mstamp; // Most recent send/receive

	// The factor of 2 gives some headroom so that we always have
	// sufficient history. We end up storing more history than needed, but
	// that's ok
	interval_length = 2 * hist_us / rocc_num_intervals + 1; // round up
	if (rocc->intervals[rocc->intervals_head].start_us + interval_length < timestamp) {
		// Push the buffer
		rocc->intervals_head = (rocc->intervals_head - 1) & rocc_num_intervals_mask;
		rocc->intervals[rocc->intervals_head].start_us = timestamp;
		rocc->intervals[rocc->intervals_head].pkts_acked = rs->acked_sacked;
		rocc->intervals[rocc->intervals_head].pkts_lost = rs->losses;
		rocc->intervals[rocc->intervals_head].app_limited = rs->is_app_limited;
	}
	else {
		rocc->intervals[rocc->intervals_head].pkts_acked += rs->acked_sacked;
		rocc->intervals[rocc->intervals_head].pkts_lost += rs->losses;
		rocc->intervals[rocc->intervals_head].app_limited |= rs->is_app_limited;
	}

	// Find the statistics from the last `hist` seconds
	pkts_acked = 0;
	pkts_lost = 0;
	app_limited = false;
	for (i = 0; i < rocc_num_intervals; ++i) {
		id = (rocc->intervals_head + i) & rocc_num_intervals_mask;
		pkts_acked += rocc->intervals[id].pkts_acked;
		pkts_lost += rocc->intervals[id].pkts_lost;
		app_limited |= rocc->intervals[id].app_limited;
		if (rocc->intervals[id].start_us + hist_us < timestamp) {
			break;
		}
	}

	// Set cwnd
	cwnd = pkts_acked + rocc_alpha;
	if (app_limited && cwnd < tsk->snd_cwnd) {
		// Do not decrease cwnd if app limited
		cwnd = tsk->snd_cwnd;
	}
	if (cwnd < rocc_min_cwnd) {
		cwnd = rocc_min_cwnd;
	}
	tsk->snd_cwnd = cwnd;

	// Set pacing according to cwnd and whether there was excessive
	// loss. Note, this stuff isn't CCAC approved (yet).
	loss_mode = (u64) pkts_lost * 1024 > (u64) (pkts_acked + pkts_lost) * rocc_loss_thresh;
	if (loss_mode) {
		// If the loss rate was too high, reduce the pacing rate. Do
		// division at the end to minimize error due to
		// integers. Further, do all computations in u64.
		sk->sk_pacing_rate = 1000000 * (u64) cwnd * rocc_get_mss(tsk) * (1024 + 2 * rocc_loss_thresh) / (rtt_us * 2 * 1024);
	}
	else {
		// No loss, send normal pacing rate. We use min_rtt just to be
		// pace a little extra because we want to be cwnd
		// limited. Doing that in loss_mode can be dangerous if min_rtt
		// is an underestimate
		sk->sk_pacing_rate = 1000000 * (u64) cwnd * rocc_get_mss(tsk) / rocc->min_rtt_us;
	}

#ifdef ROCC_DEBUG
	printk(KERN_INFO "rocc flow %u cwnd %u pacing %lu rtt %u mss %u timestamp %llu interval %ld", rocc->id, tsk->snd_cwnd, sk->sk_pacing_rate, rtt_us, tsk->mss_cache, timestamp, rs->interval_us);
	printk(KERN_INFO "rocc pkts_acked %u hist_us %u pacing %lu loss_mode %d app_limited %d rs_limited %d", pkts_acked, hist_us, sk->sk_pacing_rate, (int)loss_mode, (int)app_limited, (int)rs->is_app_limited);
	for (i = 0; i < rocc_num_intervals; ++i) {
		id = (rocc->intervals_head + i) & rocc_num_intervals_mask;
		printk(KERN_INFO "rocc intervals %llu acked %u lost %u app_limited %d i %u id %u", rocc->intervals[id].start_us, rocc->intervals[id].pkts_acked, rocc->intervals[id].pkts_lost, (int)rocc->intervals[id].app_limited, i, id);
	}
#endif
}

static void rocc_release(struct sock *sk)
{
	struct rocc_data *rocc = inet_csk_ca(sk);
	kfree(rocc->intervals);
}

/* ROCC does not need to undo the cwnd since it does not
 * always reduce cwnd on losses. Keep it for now.
 */
static u32 rocc_undo_cwnd(struct sock *sk)
{
	return tcp_sk(sk)->snd_cwnd;
}

static u32 rocc_ssthresh(struct sock *sk)
{
	return TCP_INFINITE_SSTHRESH; /* ROCC does not use ssthresh */
}

static void rocc_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
}

static struct tcp_congestion_ops tcp_rocc_cong_ops __read_mostly = {
	.flags = TCP_CONG_NON_RESTRICTED,
	.name = "rocc",
	.owner = THIS_MODULE,
	.init = rocc_init,
	.release	= rocc_release,
	.cong_control = rocc_process_sample,
	/* Keep the windows static */
	.undo_cwnd = rocc_undo_cwnd,
	/* Slow start threshold will not exist */
	 .ssthresh = rocc_ssthresh,
	.cong_avoid = rocc_cong_avoid,
};

/* Kernel module section */

static int __init rocc_register(void)
{
	BUILD_BUG_ON(sizeof(struct rocc_data) > ICSK_CA_PRIV_SIZE);
#ifdef ROCC_DEBUG
	printk(KERN_INFO "rocc init reg\n");
#endif
	return tcp_register_congestion_control(&tcp_rocc_cong_ops);
}

static void __exit rocc_unregister(void)
{
	tcp_unregister_congestion_control(&tcp_rocc_cong_ops);
}

module_init(rocc_register);
module_exit(rocc_unregister);

MODULE_AUTHOR("Venkat Arun <venkatarun95@gmail.com>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("TCP RoCC (Robust Congestion Control)");
