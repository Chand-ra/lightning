#include "config.h"
#include <bitcoin/psbt.h>
#include <bitcoin/script.h>
#include <bitcoin/tx.h>
#include <ccan/asort/asort.h>
#include <ccan/cast/cast.h>
#include <ccan/mem/mem.h>
#include <ccan/tal/str/str.h>
#include <channeld/channeld_wiregen.h>
#include <common/utils.h>
#include <hsmd/hsmd_wiregen.h>
#include <inttypes.h>
#include <lightningd/anchorspend.h>
#include <lightningd/chaintopology.h>
#include <lightningd/channel.h>
#include <lightningd/hsm_control.h>
#include <lightningd/htlc_end.h>
#include <lightningd/lightningd.h>
#include <lightningd/log.h>
#include <lightningd/peer_control.h>
#include <wally_psbt.h>

/* This is attached to each anchor tx retransmission */
struct one_anchor {
	/* We are in adet->anchors */
	struct anchor_details *adet;

	/* Is this for our own commit tx? */
	enum side commit_side;

	/* Where the anchors are */
	struct local_anchor_info info;

	/* If we made an anchor-spend tx, what was its fee? */
	struct amount_sat anchor_spend_fee;
};

/* This is attached to the *commitment* tx retransmission */
struct anchor_details {
	/* Sorted amounts for how much we risk at each blockheight. */
	struct deadline_value *vals;

	/* Witnesscript for anchor */
	const u8 *anchor_wscript;

	/* A callback for each of these */
	struct one_anchor *anchors;
};

struct deadline_value {
	/* If false, don't stress about this target, always treat it as >= 12 blocks away */
	bool important;
	/* Target we want this in block by */
	u32 block;
	/* Amount this is worth to us */
	struct amount_msat msat;
};

static int cmp_deadline_value(const struct deadline_value *a,
			      const struct deadline_value *b,
			      void *unused)
{
	return (int)a->block - (int)b->block;
}

static bool find_anchor_output(struct channel *channel,
			       const struct bitcoin_tx *tx,
			       const u8 *anchor_wscript,
			       struct bitcoin_outpoint *out)
{
	const u8 *scriptpubkey = scriptpubkey_p2wsh(tmpctx, anchor_wscript);

	for (out->n = 0; out->n < tx->wtx->num_outputs; out->n++) {
		if (memeq(scriptpubkey, tal_bytelen(scriptpubkey),
			  tx->wtx->outputs[out->n].script,
			  tx->wtx->outputs[out->n].script_len)) {
			bitcoin_txid(tx, &out->txid);
			return true;
		}
	}
	return false;
}

/* Sorts deadlines into increasing block order, merges dups */
static void merge_deadlines(struct channel *channel, struct anchor_details *adet)
{
	size_t dst;

	/* Below requires len >= 1 */
	if (tal_count(adet->vals) == 0)
		return;

	/* Sort into block-ascending order */
	asort(adet->vals, tal_count(adet->vals), cmp_deadline_value, NULL);

	/* Merge deadlines. */
	dst = 0;
	for (size_t i = 1; i < tal_count(adet->vals); i++) {
		assert(adet->vals[i].important);
		if (adet->vals[i].block != adet->vals[dst].block) {
			dst = i;
			continue;
		}
		if (!amount_msat_accumulate(&adet->vals[dst].msat,
					    adet->vals[i].msat)) {
			log_broken(channel->log,
				   "Cannot add deadlines %s + %s!",
				   fmt_amount_msat(tmpctx, adet->vals[dst].msat),
				   fmt_amount_msat(tmpctx, adet->vals[i].msat));
		}
	}
	tal_resize(&adet->vals, dst+1);
}

static void add_one_anchor(struct anchor_details *adet,
			   const struct local_anchor_info *info,
			   enum side commit_side)
{
	struct one_anchor one;

	one.info = *info;
	one.adet = adet;
	one.commit_side = commit_side;
	one.anchor_spend_fee = AMOUNT_SAT(0);
	tal_arr_expand(&adet->anchors, one);
}

struct anchor_details *create_anchor_details(const tal_t *ctx,
					     struct channel *channel,
					     const struct bitcoin_tx *tx)
{
	struct lightningd *ld = channel->peer->ld;
	const struct htlc_in *hin;
	struct htlc_in_map_iter ini;
	const struct htlc_out *hout;
	struct htlc_out_map_iter outi;
	struct anchor_details *adet = tal(ctx, struct anchor_details);
	struct local_anchor_info *infos, local_anchor;
	struct deadline_value v;
	u32 final_deadline;

	/* If we don't have an anchor, we can't do anything. */
	if (!channel_type_has_anchors(channel->type))
		return tal_free(adet);

	if (!hsm_capable(ld, WIRE_HSMD_SIGN_ANCHORSPEND)) {
		log_broken(ld->log, "hsm not capable of signing anchorspends!");
		return tal_free(adet);
	}

	adet->anchor_wscript
		= bitcoin_wscript_anchor(adet, &channel->local_funding_pubkey);
	adet->anchors = tal_arr(adet, struct one_anchor, 0);

	/* Look for any remote commitment tx anchors we might use */
	infos = wallet_get_local_anchors(tmpctx,
					 channel->peer->ld->wallet,
					 channel->dbid);
	for (size_t i = 0; i < tal_count(infos); i++)
		add_one_anchor(adet, &infos[i], REMOTE);

	/* Now append our own, if we have one. */
	if (find_anchor_output(channel, tx, adet->anchor_wscript,
			       &local_anchor.anchor_point)) {
		local_anchor.commitment_weight = bitcoin_tx_weight(tx);
		local_anchor.commitment_fee = bitcoin_tx_compute_fee(tx);
		add_one_anchor(adet, &local_anchor, LOCAL);
	}

	log_debug(channel->log, "We have %zu anchor points to use",
		  tal_count(adet->anchors));

	/* This happens in several cases:
	 * 1. Mutual close tx.
	 * 2. There's no to-us output and no HTLCs */
	if (tal_count(adet->anchors) == 0) {
		return tal_free(adet);
	}

	adet->vals = tal_arr(adet, struct deadline_value, 0);

	/* OK, what's it worth, at each deadline?
	 * We care about incoming HTLCs where we have the preimage, and
	 * outgoing HTLCs. */
	for (hin = htlc_in_map_first(ld->htlcs_in, &ini);
	     hin;
	     hin = htlc_in_map_next(ld->htlcs_in, &ini)) {
		if (hin->key.channel != channel)
			continue;

		if (!hin->preimage)
			continue;

		v.msat = hin->msat;
		v.block = hin->cltv_expiry;
		v.important = true;
		tal_arr_expand(&adet->vals, v);
	}

	for (hout = htlc_out_map_first(ld->htlcs_out, &outi);
	     hout;
	     hout = htlc_out_map_next(ld->htlcs_out, &outi)) {
		if (hout->key.channel != channel)
			continue;

		v.msat = hout->msat;
		/* Our real deadline here is the INCOMING htlc.  If it's us, use the default so we don't leak
		 * too much information about it. */
		if (hout->in)
			v.block = hout->in->cltv_expiry;
		else
			v.block = hout->cltv_expiry + ld->config.cltv_expiry_delta;
		v.important = true;
		tal_arr_expand(&adet->vals, v);
	}

	merge_deadlines(channel, adet);

	/* Include final "unimportant" one, to make sure we eventually boost */
	assert(channel->close_attempt_height);
	if (tal_count(adet->vals) == 0)
		final_deadline = channel->close_attempt_height;
	else
		final_deadline = adet->vals[tal_count(adet->vals) - 1].block;

	/* "Two weeks later" */
	v.block = final_deadline + ld->dev_low_prio_anchor_blocks;
	v.msat = AMOUNT_MSAT(0);
	v.important = false;
	tal_arr_expand(&adet->vals, v);

	return adet;
}

/* total_weight includes the commitment tx we're trying to push, and the anchor fee output */
static struct wally_psbt *anchor_psbt(const tal_t *ctx,
				      struct channel *channel,
				      const struct one_anchor *anch,
				      struct utxo **utxos,
				      u32 feerate_target,
				      size_t total_weight,
				      bool insufficient_funds)
{
	struct lightningd *ld = channel->peer->ld;
	struct wally_psbt *psbt;
	struct amount_sat change, fee;
	struct pubkey final_key;

	/* PSBT knows how to spend utxos. */
	psbt = psbt_using_utxos(ctx, ld->wallet, utxos,
				default_locktime(ld->topology),
				BITCOIN_TX_RBF_SEQUENCE, NULL);

	/* BOLT #3:
	 * #### `to_local_anchor` and `to_remote_anchor` Output (option_anchors)
	 *...
	 * The amount of the output is fixed at 330 sats, the default
	 * dust limit for P2WSH.
	 */
	psbt_append_input(psbt, &anch->info.anchor_point, BITCOIN_TX_RBF_SEQUENCE,
			  NULL, anch->adet->anchor_wscript, NULL);
	psbt_input_set_wit_utxo(psbt, psbt->num_inputs - 1,
				scriptpubkey_p2wsh(tmpctx, anch->adet->anchor_wscript),
				AMOUNT_SAT(330));
	psbt_input_add_pubkey(psbt, psbt->num_inputs - 1, &channel->local_funding_pubkey, false);

	/* Calculate fee we need, given rate and weight */
	fee = amount_tx_fee(feerate_target, total_weight);
	/* Some fee already paid by commitment tx */
	if (!amount_sat_sub(&fee, fee, anch->info.commitment_fee))
		fee = AMOUNT_SAT(0);

	/* How much do we have? */
	change = psbt_compute_fee(psbt);

	/* We have to pay dust, at least! */
	if (!amount_sat_sub(&change, change, fee)
	    || amount_sat_less(change, chainparams->dust_limit)) {
		/* If we didn't run out of UTXOs, this implies our estimation was wrong! */
		if (!insufficient_funds)
			log_broken(channel->log, "anchor: could not afford fee %s from change %s, reducing fee",
				   fmt_amount_sat(tmpctx, fee),
				   fmt_amount_sat(tmpctx, psbt_compute_fee(psbt)));
		change = chainparams->dust_limit;
	}

	bip32_pubkey(ld, &final_key, channel->final_key_idx);
	psbt_append_output(psbt,
			   scriptpubkey_p2tr(tmpctx, &final_key),
			   change);
	return psbt;
}

/* Get UTXOs, create a PSBT to spend this */
static struct wally_psbt *try_anchor_psbt(const tal_t *ctx,
					  struct channel *channel,
					  const struct one_anchor *anch,
					  u32 feerate_target,
					  size_t base_weight,
					  size_t *total_weight,
					  struct amount_sat *fee_spent,
					  u32 *feerate,
					  struct utxo ***utxos)
{
	struct lightningd *ld = channel->peer->ld;
	struct wally_psbt *psbt;
	struct amount_sat fee;
	bool insufficient_funds;

	/* Ask for some UTXOs which could meet this feerate, and since
	 * we need one output, meet the minumum output requirement */
	*total_weight = base_weight;
	*utxos = wallet_utxo_boost(ctx,
				   ld->wallet,
				   get_block_height(ld->topology),
				   anch->info.commitment_fee,
				   chainparams->dust_limit,
				   feerate_target,
				   total_weight, &insufficient_funds);

	/* Create a new candidate PSBT */
	psbt = anchor_psbt(ctx, channel, anch, *utxos, feerate_target,
			   *total_weight, insufficient_funds);
	*fee_spent = psbt_compute_fee(psbt);

	/* Add in base commitment fee to calculate *overall* package feerate */
	if (!amount_sat_add(&fee, *fee_spent, anch->info.commitment_fee))
		abort();
	if (!amount_feerate(feerate, fee, *total_weight))
		abort();

	return psbt;
}

/* If it's possible and worth it, return signed tx.  Otherwise NULL. */
static struct bitcoin_tx *spend_anchor(const tal_t *ctx,
				       struct channel *channel,
				       struct one_anchor *anch)
{
	struct lightningd *ld = channel->peer->ld;
	size_t base_weight, psbt_weight;
	struct amount_sat psbt_fee, diff;
	struct bitcoin_tx *tx;
	struct utxo **psbt_utxos;
	const struct hsm_utxo **hsm_utxos;
	struct wally_psbt *psbt, *signed_psbt;
	struct amount_msat total_value;
	const struct deadline_value *unimportant_deadline;
	const u8 *msg;

	/* Estimate weight of anchorspend tx plus commitment_tx (not including any UTXO we add) */
	base_weight = bitcoin_tx_core_weight(1, 1)
		+ bitcoin_tx_input_weight(false,
					  bitcoin_tx_input_sig_weight()
					  + 1 + tal_bytelen(anch->adet->anchor_wscript))
		+ change_weight()
		+ anch->info.commitment_weight;

	total_value = AMOUNT_MSAT(0);
	psbt = NULL;
	unimportant_deadline = NULL;

	/* We start with furthest feerate target, and keep going backwards
	 * until it's either not worth making an anchor at that price for all
	 * the HTLCs from that point on, or we have an anchor for the closest
	 * HTLC deadline */
	for (int i = tal_count(anch->adet->vals) - 1; i >= 0; --i) {
		const struct deadline_value *val = &anch->adet->vals[i];
		u32 feerate, feerate_target;
		size_t weight;
		struct amount_sat fee;
		struct wally_psbt *candidate_psbt;
		struct utxo **utxos;

		/* We only cover important deadlines here */
		if (!val->important) {
			unimportant_deadline = val;
			continue;
		}


		if (!amount_msat_accumulate(&total_value, val->msat))
			abort();

		feerate_target = feerate_for_target(ld->topology, val->block);

		/* If the feerate for the commitment tx is already
		 * sufficient, don't try for anchor. */
		if (amount_feerate(&feerate,
				   anch->info.commitment_fee,
				   anch->info.commitment_weight)
		    && feerate >= feerate_target) {
			continue;
		}

		candidate_psbt = try_anchor_psbt(tmpctx, channel, anch,
						 feerate_target,
						 base_weight,
						 &weight,
						 &fee,
						 &feerate,
						 &utxos);
		log_debug(channel->log, "candidate_psbt total weight = %zu (commitment weight %u, anchor %zu)",
			  weight, anch->info.commitment_weight, weight - anch->info.commitment_weight);

		/* Is it even worth spending this fee to meet the deadline? */
		if (!amount_msat_greater_sat(total_value, fee)) {
			log_debug(channel->log,
				  "Not worth fee %s for %s commit tx to get %s at block %u (%+i) at feerate %uperkw",
				  fmt_amount_sat(tmpctx, fee),
				  anch->commit_side == LOCAL ? "local" : "remote",
				  fmt_amount_msat(tmpctx, val->msat),
				  val->block, val->block - get_block_height(ld->topology), feerate_target);
			break;
		}

		if (feerate < feerate_target) {
			/* We might have had lower feerates which worked: only complain if
			 * we have *nothing* */
			if (tal_count(utxos) == 0 && !psbt) {
				log_unusual(channel->log,
					    "No utxos to bump commit_tx to feerate %uperkw!",
					    feerate_target);
				break;
			}

			log_unusual(channel->log,
				    "We want to bump commit_tx to feerate %uperkw, but can only bump to %uperkw with %zu UTXOs!",
				    feerate_target, feerate, tal_count(utxos));
			psbt = candidate_psbt;
			psbt_fee = fee;
			psbt_weight = weight;
			psbt_utxos = utxos;
			/* We don't expect to do any better at higher feerates */
			break;
		}

		log_debug(channel->log, "Worth fee %s for %s commit tx to get %s at block %u (%+i) at feerate %uperkw",
			  fmt_amount_sat(tmpctx, fee),
			  anch->commit_side == LOCAL ? "local" : "remote",
			  fmt_amount_msat(tmpctx, val->msat),
			  val->block, val->block - get_block_height(ld->topology), feerate);
		psbt = candidate_psbt;
		psbt_fee = fee;
		psbt_weight = weight;
		psbt_utxos = utxos;
	}

	/* No psbt, but only have an unimportant deadline? */
	if (!psbt && unimportant_deadline == &anch->adet->vals[0]) {
		u32 block_target, feerate_target, feerate;

		/* We're not in a hurry.  Never aim for < 12 blocks away */
		block_target = unimportant_deadline->block;
		if (block_target < get_block_height(ld->topology) + 12)
			block_target = get_block_height(ld->topology) + 12;
		feerate_target = feerate_for_target(ld->topology, block_target);

		/* If the feerate for the commitment tx is already
		 * sufficient, don't try for anchor. */
		if (!amount_feerate(&feerate,
				    anch->info.commitment_fee,
				    anch->info.commitment_weight)
		    || feerate < feerate_target) {
			log_debug(channel->log,
				  "Low-priority anchorspend aiming for block %u (feerate %u)",
				  block_target, feerate_target);
			psbt = try_anchor_psbt(tmpctx, channel, anch,
					       feerate_target,
					       base_weight,
					       &psbt_weight,
					       &psbt_fee,
					       &feerate,
					       &psbt_utxos);
			/* Don't bother with anchor if we don't add UTXOs */
			if (tal_count(psbt_utxos) == 0) {
				if (!psbt)
					log_unusual(channel->log,
						    "No utxos to bump commit_tx to feerate %uperkw!",
						    feerate_target);
				psbt = tal_free(psbt);
			}
		} else {
			log_debug(channel->log,
				  "Avoiding anchorspend: feerate already %u for target %u",
				  feerate, feerate_target);
		}
	}

	/* No psbt was worth it? */
	if (!psbt)
		return NULL;

	/* Higher enough than previous to be valid RBF?
	 * We assume 1 sat per vbyte as minrelayfee */
	if (!amount_sat_sub(&diff, psbt_fee, anch->anchor_spend_fee)
	    || amount_sat_less(diff, amount_sat(psbt_weight / 4)))
		return NULL;

	log_debug(channel->log,
		  "Anchorspend for %s commit tx fee %s (w=%zu), commit_tx fee %s (w=%u):"
		  " package feerate %"PRIu64" perkw",
		  anch->commit_side == LOCAL ? "local" : "remote",
		  fmt_amount_sat(tmpctx, psbt_fee),
		  psbt_weight - anch->info.commitment_weight,
		  fmt_amount_sat(tmpctx, anch->info.commitment_fee),
		  anch->info.commitment_weight,
		  (psbt_fee.satoshis + anch->info.commitment_fee.satoshis) /* Raw: debug log */
		  * 1000 / psbt_weight);

	/* OK, HSM, sign it! */
	hsm_utxos = utxos_to_hsm_utxos(tmpctx, psbt_utxos);
	msg = towire_hsmd_sign_anchorspend(NULL,
					   &channel->peer->id,
					   channel->dbid,
					   hsm_utxos,
					   psbt);
	msg = hsm_sync_req(tmpctx, ld, take(msg));
	if (!fromwire_hsmd_sign_anchorspend_reply(tmpctx, msg, &signed_psbt))
		fatal("Reading sign_anchorspend_reply: %s", tal_hex(tmpctx, msg));

	if (!psbt_finalize(signed_psbt)) {
		/* Lots of logging, to try to figure out why! */
		log_broken(channel->log, "Non-final PSBT from hsm: %s",
			   fmt_wally_psbt(tmpctx, signed_psbt));
		log_broken(channel->log, "Before signing PSBT was %s",
			   fmt_wally_psbt(tmpctx, psbt));
		for (size_t i = 0; i < tal_count(psbt_utxos); i++) {
			const struct unilateral_close_info *ci = psbt_utxos[i]->close_info;

			log_broken(channel->log, "UTXO %zu: %s amt=%s keyidx=%u",
				   i,
				   fmt_bitcoin_outpoint(tmpctx, &psbt_utxos[i]->outpoint),
				   fmt_amount_sat(tmpctx, psbt_utxos[i]->amount),
				   psbt_utxos[i]->keyindex);
			if (ci) {
				log_broken(channel->log,
					   "... close from channel %"PRIu64" peer %s (%s) commitment %s csv %u",
					   ci->channel_id,
					   fmt_node_id(tmpctx, &ci->peer_id),
					   ci->option_anchors ? "anchor" : "non-anchor",
					   ci->commitment_point ? fmt_pubkey(tmpctx, ci->commitment_point) : "none",
					   ci->csv);
			}
		}
		return NULL;
	}

	/* Update fee so we know for next time */
	anch->anchor_spend_fee = psbt_fee;

	tx = tal(ctx, struct bitcoin_tx);
	tx->chainparams = chainparams;
	tx->wtx = psbt_final_tx(tx, signed_psbt);
	assert(tx->wtx);
	tx->psbt = tal_steal(tx, signed_psbt);
	log_debug(channel->log, "anchor actual weight: %zu", bitcoin_tx_weight(tx));

	return tx;
}

static bool refresh_anchor_spend(struct channel *channel,
				 const struct bitcoin_tx **tx,
				 struct one_anchor *anch)
{
	struct bitcoin_tx *replace;
	struct amount_sat old_fee = anch->anchor_spend_fee;

	replace = spend_anchor(tal_parent(*tx), channel, anch);
	if (replace) {
		struct bitcoin_txid txid;

		bitcoin_txid(replace, &txid);
		log_info(channel->log, "RBF anchor %s commit tx spend %s: fee was %s now %s",
			 anch->commit_side == LOCAL ? "local" : "remote",
			 fmt_bitcoin_txid(tmpctx, &txid),
			 fmt_amount_sat(tmpctx, old_fee),
			 fmt_amount_sat(tmpctx, anch->anchor_spend_fee));
		log_debug(channel->log, "RBF anchor spend: Old tx %s new %s",
			  fmt_bitcoin_tx(tmpctx, *tx),
			  fmt_bitcoin_tx(tmpctx, replace));
		tal_free(*tx);
		*tx = replace;
	}
	return true;
}

static void create_and_broadcast_anchor(struct channel *channel,
					struct one_anchor *anch)
{
	struct bitcoin_tx *newtx;
	struct bitcoin_txid txid;
	struct lightningd *ld = channel->peer->ld;

	/* Do we want to spend the anchor to boost channel? */
	newtx = spend_anchor(tmpctx, channel, anch);
	if (!newtx) {
		return;
	}

	bitcoin_txid(newtx, &txid);
	log_info(channel->log, "Creating anchor spend for %s commit tx %s: we're paying fee %s",
		 anch->commit_side == LOCAL ? "local" : "remote",
		 fmt_bitcoin_txid(tmpctx, &txid),
		 fmt_amount_sat(tmpctx, anch->anchor_spend_fee));

	/* Send it! */
	broadcast_tx(anch->adet, ld->topology, channel, take(newtx), NULL, true, 0, NULL,
		     refresh_anchor_spend, anch);
}

void commit_tx_boost(struct channel *channel,
		     struct anchor_details *adet,
		     bool success)
{
	enum side side;

	if (!adet)
		return;

	/* If it's in our mempool, we should consider boosting it.
	 * Otherwise, try boosting peers' commitment txs. */
	if (success)
		side = LOCAL;
	else
		side = REMOTE;

	/* Ones we've already launched will use refresh_anchor_spend */
	for (size_t i = 0; i < tal_count(adet->anchors); i++) {
		if (adet->anchors[i].commit_side != side)
			continue;
		if (amount_sat_eq(adet->anchors[i].anchor_spend_fee,
				   AMOUNT_SAT(0))) {
			create_and_broadcast_anchor(channel, &adet->anchors[i]);
		}
	}
}
