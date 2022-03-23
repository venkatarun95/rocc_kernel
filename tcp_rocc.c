/* RoCC (Robust Congestion Control
 * For debugging use `sudo sysctl -w net.ipv4.tcp_fin_timeout=1` so you can `rmmod` the module sooner, else it will wait for 60 seconds
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <net/tcp.h>
#include <linux/random.h>

#define ROCC_DEBUG

// Should be a power of two so rocc_num_intervals_mask can be set
static const u16 rocc_num_intervals = 16;
// rocc_num_intervals expressed as a mask
static const u16 rocc_num_intervals_mask = 15;
static const u32 rocc_min_cwnd = 2;
// Maximum tolerable loss rate, expressed as `loss_thresh / 1024`. Calculations
// are faster if things are powers of 2
static const u32 loss_thresh = 8;

enum ROCC_MODE {
	ROCC_REGULAR_MODE,
	ROCC_LOSS_MODE
};

// To keep track of the number of packets acked over a short period of time
struct rocc_interval {
	// Starting time of this interval
	u64 start_us;
	u32 pkts_acked;
	u32 pkts_lost;
};

static int id = 0;
struct rocc_data {
	struct rocc_interval *intervals;
	// Index of the last interval to be added
	u16 intervals_head;

	u32 min_rtt_us;
	enum ROCC_MODE mode;

	// debug helpers
	int id;
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
	}
	rocc->intervals_head = 0;

	rocc->min_rtt_us = U32_MAX;
	rocc->mode = ROCC_REGULAR_MODE;
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

	if (!rocc_valid(rocc))
		return;

	// Is rate sample valid?
	if (rs->delivered < 0 || rs->interval_us < 0)
		return;

	/* Get initial RTT - as measured by SYN -> SYN-ACK.
         * If information does not exist - use U32_MAX as RTT
         */
	if (tsk->srtt_us) {
		rtt_us = max(tsk->srtt_us >> 3, 1U);
	} else {
		rtt_us = U32_MAX;
	}

	if (rtt_us < rocc->min_rtt_us)
		rocc->min_rtt_us = rtt_us;

	hist_us = 2 * rocc->min_rtt_us;

	// Update intervals
	timestamp = tsk->tcp_mstamp; // Most recent send/receive
	//timestamp = rs->prior_mstamp + rs->interval_us;

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
	}
	else {
		rocc->intervals[rocc->intervals_head].pkts_acked += rs->acked_sacked;
		rocc->intervals[rocc->intervals_head].pkts_lost += rs->losses;
	}

	// Find the statistics from the last `hist` seconds
	pkts_acked = 0;
	pkts_lost = 0;
	for (i = 0; i < rocc_num_intervals; ++i) {
		id = (rocc->intervals_head + i) & rocc_num_intervals_mask;
		pkts_acked += rocc->intervals[id].pkts_acked;
		pkts_lost += rocc->intervals[id].pkts_lost;
		if (rocc->intervals[id].start_us + hist_us < timestamp) {
			break;
		}
	}

	// If the loss rate was too high, reduce the cwnd
	if (pkts_lost > (pkts_acked + pkts_lost)

	cwnd = pkts_acked;
	if (cwnd < rocc_min_cwnd) {
		cwnd = rocc_min_cwnd;
	}
	tsk->snd_cwnd = cwnd;

#ifdef ROCC_DEBUG
	printk(KERN_INFO "rocc cwnd %u pacing %lu rtt %u mss %u timestamp %llu interval %ld", tsk->snd_cwnd, sk->sk_pacing_rate, rtt_us, tsk->mss_cache, timestamp, rs->interval_us);
	printk(KERN_INFO "rocc pkts_acked %u hist_us %u", pkts_acked, hist_us);
	for (i = 0; i < rocc_num_intervals; ++i) {
		id = (rocc->intervals_head + i) & rocc_num_intervals_mask;
		printk(KERN_INFO "rocc intervals %llu acked %u lost %u i %u id %u", rocc->intervals[id].start_us, rocc->intervals[id].pkts_acked, rocc->intervals[id].pkts_lost, i, id);
	}
#endif

	if (rocc->mode == ROCC_LOSS_MODE)
		goto end;

 end:;
	//pcc->lost_base = tsk->lost;
	//pcc->delivered_base = tsk->delivered;
}

static void rocc_release(struct sock *sk)
{
	struct rocc_data *rocc = inet_csk_ca(sk);
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

static void rocc_set_state(struct sock *sk, u8 new_state)
{
	struct rocc_data *rocc = inet_csk_ca(sk);
	struct tcp_sock *tsk = tcp_sk(sk);

	if (!rocc_valid(rocc))
		return;

	/* In Loss state, the counters of sent segs versus segs recived, lost
	 * and in flight, stops being in sync. So at the end of the loss state,
	 * pcc saves the diff between the counters in order to resulves these
	 * diffs in the future
	 */
	/* if (pcc->mode == PCC_LOSS && new_state != TCP_CA_Loss) { */
	/* 	double_counted = tsk->delivered + tsk->lost+ */
	/* 			 tcp_packets_in_flight(tsk); */
	/* 	double_counted -= tsk->data_segs_out; */
	/* 	double_counted -= pcc->double_counted; */
	/* 	pcc->double_counted+= double_counted; */
	/* 	printk(KERN_INFO "%d loss ended: double_counted %d\n", */
	/* 	       pcc->id, double_counted); */

	/* 	pcc->mode = PCC_DECISION_MAKING; */
	/* 	pcc_setup_intervals(pcc); */
	/* 	pcc_start_interval(sk, pcc); */
	/* } else if (pcc->mode != PCC_LOSS && new_state  == TCP_CA_Loss) { */
	/* 	printk(KERN_INFO "%d loss: started\n", pcc->id); */
	/* 	pcc->mode = PCC_LOSS; */
	/* 	pcc->wait_mode = true; */
	/* 	pcc_start_interval(sk, pcc); */
	/* } else { */
	/* 	pcc_set_cwnd(sk); */
	/* } */
}

static void rocc_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
}

static void rocc_pkts_acked(struct sock *sk, const struct ack_sample *acks)
{
}
static void rocc_ack_event(struct sock *sk, u32 flags)
{

}


static void rocc_cwnd_event(struct sock *sk, enum tcp_ca_event event)
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
	//.set_state	= rocc_set_state,
	.cong_avoid = rocc_cong_avoid,
	//.pkts_acked = rocc_pkts_acked,
	//.in_ack_event= rocc_ack_event,
	//.cwnd_event	= rocc_cwnd_event,
};

/* Kernel module section */

static int __init rocc_register(void)
{
	BUILD_BUG_ON(sizeof(struct rocc_data) > ICSK_CA_PRIV_SIZE);
	printk(KERN_INFO "rocc init reg\n");
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
