/* RoCC (Robust Congestion Control
 */

#include <linux/module.h>
#include <net/tcp.h>
#include <linux/random.h>

enum ROCC_MODE {
	ROCC_REGULAR_MODE,
	ROCC_LOSS_MODE
};

static int id = 0;
struct rocc_data {
	enum ROCC_MODE mode;

	// debug helpers
	int id;
};

/*********************
 * Getters / Setters *
 * ******************/
static u32 rocc_get_rtt(struct tcp_sock *tsk)
{
        /* Get initial RTT - as measured by SYN -> SYN-ACK.
         * If information does not exist - use 1ms as a "LAN RTT".
         */
	if (tsk->srtt_us) {
		return max(tsk->srtt_us >> 3, 1U);
	} else {
		return USEC_PER_MSEC;
	}
}

static u32 rocc_get_mss(struct tcp_sock *tsk)
{
	// TODO: Figure out if mss_cache is the one to use
	return tsk->mss_cache;
}

/* Initialize cwnd to support current pacing rate (but not less then 4 packets)
vv */
static void rocc_set_cwnd(struct sock *sk)
{
	struct tcp_sock *tsk = tcp_sk(sk);
	/* u64 cwnd = sk->sk_pacing_rate; */

	/* cwnd *= rocc_get_rtt(tcp_sk(sk)); */
	/* cwnd /= tp->mss_cache; */
	/* cwnd /= USEC_PER_SEC; */
	/* cwnd *= 2; */
	/* cwnd = max(4ULL, cwnd); */
	/* cwnd = min((u32)cwnd, tp->snd_cwnd_clamp); /\* apply cap *\/ */

	u32 cwnd = 4 * rocc_get_mss(tsk);

	tsk->snd_cwnd = cwnd;
}


/* was the rocc struct fully inited */
static bool rocc_valid(struct rocc_data *rocc)
{
	return rocc;
}

static void rocc_process_sample(struct sock *sk, const struct rate_sample *rs)
{
	struct rocc_data *rocc = inet_csk_ca(sk);
	struct tcp_sock *tsk = tcp_sk(sk);
	u32 rtt = rocc_get_rtt(tsk);

	if (!rocc_valid(rocc))
		return;

	rocc_set_cwnd(sk);

	printk(KERN_INFO "rocc cwnd %u pacing %lu rtt %u mss %u", tsk->snd_cwnd, sk->sk_pacing_rate, rtt, tsk->mss_cache);

	if (rocc->mode == ROCC_LOSS_MODE)
		goto end;

 end:;
	//pcc->lost_base = tsk->lost;
	//pcc->delivered_base = tsk->delivered;
}

static void rocc_init(struct sock *sk)
{
	struct rocc_data *rocc = inet_csk_ca(sk);

	rocc->mode = ROCC_REGULAR_MODE;
	++id;
	rocc->id = id;

	cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
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
	.set_state	= rocc_set_state,
	.cong_avoid = rocc_cong_avoid,
	.pkts_acked = rocc_pkts_acked,
	.in_ack_event= rocc_ack_event,
	.cwnd_event	= rocc_cwnd_event,
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
