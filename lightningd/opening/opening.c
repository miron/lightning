/* FIXME: Handle incoming gossip messages! */
#include <bitcoin/privkey.h>
#include <bitcoin/script.h>
#include <ccan/breakpoint/breakpoint.h>
#include <ccan/crypto/hkdf_sha256/hkdf_sha256.h>
#include <ccan/crypto/shachain/shachain.h>
#include <ccan/fdpass/fdpass.h>
#include <ccan/structeq/structeq.h>
#include <errno.h>
#include <inttypes.h>
#include <lightningd/channel.h>
#include <lightningd/commit_tx.h>
#include <lightningd/crypto_sync.h>
#include <lightningd/key_derive.h>
#include <lightningd/opening/gen_opening_control_wire.h>
#include <lightningd/opening/gen_opening_status_wire.h>
#include <lightningd/peer_failed.h>
#include <secp256k1.h>
#include <signal.h>
#include <status.h>
#include <stdio.h>
#include <type_to_string.h>
#include <version.h>
#include <wire/gen_peer_wire.h>
#include <wire/wire.h>
#include <wire/wire_sync.h>

/* Stdout == status, stdin == requests, 3 == peer */
#define STATUS_FD STDOUT_FILENO
#define REQ_FD STDIN_FILENO
#define PEER_FD 3

struct points {
	struct pubkey funding_pubkey;
	struct pubkey revocation_basepoint;
	struct pubkey payment_basepoint;
	struct pubkey delayed_payment_basepoint;
};

struct secrets {
	struct privkey funding_privkey;
	struct privkey revocation_basepoint_secret;
	struct privkey payment_basepoint_secret;
	struct privkey delayed_payment_basepoint_secret;
};

struct state {
	struct crypto_state cs;
	struct pubkey next_per_commit[NUM_SIDES];

	/* Funding and feerate: set by opening peer. */
	u64 funding_satoshis, push_msat;
	u32 feerate_per_kw;
	struct sha256_double funding_txid;
	u8 funding_txout;

	/* Secret keys and basepoint secrets. */
	struct secrets our_secrets;

	/* Our shaseed for generating per-commitment-secrets. */
	struct sha256 shaseed;
	struct channel_config localconf, *remoteconf;

	/* Limits on what remote config we accept */
	u32 max_to_self_delay;
	u64 min_effective_htlc_capacity_msat;

	struct channel *channel;
};

static void derive_our_basepoints(const struct privkey *seed,
				  struct points *points,
				  struct secrets *secrets,
				  struct sha256 *shaseed,
				  struct pubkey *first_per_commit)
{
	struct sha256 per_commit_secret;
	struct keys {
		struct privkey f, r, p, d;
		struct sha256 shaseed;
	} keys;

	hkdf_sha256(&keys, sizeof(keys), NULL, 0, seed, sizeof(*seed),
		    "c-lightning", strlen("c-lightning"));

	secrets->funding_privkey = keys.f;
	secrets->revocation_basepoint_secret = keys.r;
	secrets->payment_basepoint_secret = keys.p;
	secrets->delayed_payment_basepoint_secret = keys.d;

	if (!pubkey_from_privkey(&keys.f, &points->funding_pubkey)
	    || !pubkey_from_privkey(&keys.r, &points->revocation_basepoint)
	    || !pubkey_from_privkey(&keys.p, &points->payment_basepoint)
	    || !pubkey_from_privkey(&keys.d, &points->delayed_payment_basepoint))
		status_failed(WIRE_OPENING_KEY_DERIVATION_FAILED,
			      "seed = %s",
			      type_to_string(trc, struct privkey, seed));

	/* BOLT #3:
	 *
	 * A node MUST select an unguessable 256-bit seed for each connection,
	 * and MUST NOT reveal the seed.
	 */
	*shaseed = keys.shaseed;

	/* BOLT #3:
	 *
	 * the first secret used MUST be index 281474976710655, and then the
	 * index decremented. */
	shachain_from_seed(shaseed, 281474976710655ULL, &per_commit_secret);

	/* BOLT #3:
	 *
	 * The `per-commitment-point` is generated using EC multiplication:
	 *
	 * 	per-commitment-point = per-commitment-secret * G
	 */
	if (secp256k1_ec_pubkey_create(secp256k1_ctx,
				       &first_per_commit->pubkey,
				       per_commit_secret.u.u8) != 1)
		status_failed(WIRE_OPENING_KEY_DERIVATION_FAILED,
			      "first_per_commit create failed, secret = %s",
			      type_to_string(trc, struct sha256,
					     &per_commit_secret));
}

static void check_config_bounds(struct state *state,
				const struct channel_config *remoteconf)
{
	u64 capacity_msat;
	u64 reserve_msat;

	/* BOLT #2:
	 *
	 * The receiving node MUST fail the channel if `to-self-delay` is
	 * unreasonably large.
	 */
	if (remoteconf->to_self_delay > state->max_to_self_delay)
		peer_failed(PEER_FD, &state->cs, NULL,
			    WIRE_OPENING_PEER_BAD_CONFIG,
			    "to_self_delay %u larger than %u",
			    remoteconf->to_self_delay, state->max_to_self_delay);

	/* BOLT #2:
	 *
	 * The receiver MAY fail the channel if `funding-satoshis` is too
	 * small, and MUST fail the channel if `push-msat` is greater than
	 * `funding-amount` * 1000.  The receiving node MAY fail the channel
	 * if it considers `htlc-minimum-msat` too large,
	 * `max-htlc-value-in-flight` too small, `channel-reserve-satoshis`
	 * too large, or `max-accepted-htlcs` too small.
	 */
	/* We accumulate this into an effective bandwidth minimum. */

	/* Overflow check before capacity calc. */
	if (remoteconf->channel_reserve_satoshis > state->funding_satoshis)
		peer_failed(PEER_FD, &state->cs, NULL,
			    WIRE_OPENING_PEER_BAD_CONFIG,
			    "Invalid channel_reserve_satoshis %"PRIu64
			    " for funding_satoshis %"PRIu64,
			    remoteconf->channel_reserve_satoshis,
			    state->funding_satoshis);

	/* Consider highest reserve. */
	reserve_msat = remoteconf->channel_reserve_satoshis * 1000;
	if (state->localconf.channel_reserve_satoshis * 1000 > reserve_msat)
		reserve_msat = state->localconf.channel_reserve_satoshis * 1000;

	capacity_msat = state->funding_satoshis * 1000 - reserve_msat;

	if (remoteconf->max_htlc_value_in_flight_msat < capacity_msat)
		capacity_msat = remoteconf->max_htlc_value_in_flight_msat;

	if (remoteconf->htlc_minimum_msat * (u64)1000 > capacity_msat)
		peer_failed(PEER_FD, &state->cs, NULL,
			    WIRE_OPENING_PEER_BAD_CONFIG,
			    "Invalid htlc_minimum_msat %u"
			    " for funding_satoshis %"PRIu64
			    " capacity_msat %"PRIu64,
			    remoteconf->htlc_minimum_msat,
			    state->funding_satoshis,
			    capacity_msat);

	if (capacity_msat < state->min_effective_htlc_capacity_msat)
		peer_failed(PEER_FD, &state->cs, NULL,
			    WIRE_OPENING_PEER_BAD_CONFIG,
			    "Channel capacity with funding %"PRIu64" msat,"
			    " reserves %"PRIu64"/%"PRIu64" msat,"
			    " max_htlc_value_in_flight_msat %"PRIu64
			    " is %"PRIu64" msat, which is below %"PRIu64" msat",
			    state->funding_satoshis * 1000,
			    remoteconf->channel_reserve_satoshis * 1000,
			    state->localconf.channel_reserve_satoshis * 1000,
			    remoteconf->max_htlc_value_in_flight_msat,
			    capacity_msat,
			    state->min_effective_htlc_capacity_msat);

	/* We don't worry about how many HTLCs they accept, as long as > 0! */
	if (remoteconf->max_accepted_htlcs == 0)
		peer_failed(PEER_FD, &state->cs, NULL,
			    WIRE_OPENING_PEER_BAD_CONFIG,
			    "max_accepted_htlcs %u invalid",
			    remoteconf->max_accepted_htlcs);

	/* BOLT #2:
	 *
	 * It MUST fail the channel if `max-accepted-htlcs` is greater
	 * than 511.
	 */
	if (remoteconf->max_accepted_htlcs > 511)
		peer_failed(PEER_FD, &state->cs, NULL,
			    WIRE_OPENING_PEER_BAD_CONFIG,
			    "max_accepted_htlcs %u too large",
			    remoteconf->max_accepted_htlcs);
}

static bool check_commit_sig(const struct state *state,
			     const struct pubkey *our_funding_key,
			     const struct pubkey *their_funding_key,
			     struct bitcoin_tx *tx,
			     const secp256k1_ecdsa_signature *remotesig)
{
	u8 *wscript;
	bool ret;

	wscript = bitcoin_redeem_2of2(state,
				      our_funding_key, their_funding_key);

	ret = check_tx_sig(tx, 0, NULL, wscript, their_funding_key, remotesig);
	tal_free(wscript);
	return ret;
}

static secp256k1_ecdsa_signature
sign_remote_commit(const struct state *state,
		   const struct pubkey *our_funding_key,
		   const struct pubkey *their_funding_key,
		   struct bitcoin_tx *tx)
{
	u8 *wscript;
	secp256k1_ecdsa_signature sig;

	wscript = bitcoin_redeem_2of2(state,
				      our_funding_key, their_funding_key);

	/* Commit tx only has one input: funding tx. */
	sign_tx_input(tx, 0, NULL, wscript, &state->our_secrets.funding_privkey,
		      our_funding_key, &sig);
	tal_free(wscript);
	return sig;
}

/* We always set channel_reserve_satoshis to 1%, rounded up. */
static void set_reserve(u64 *reserve, u64 funding)
{
	*reserve = (funding + 99) / 100;
}

static void open_channel(struct state *state, const struct points *ours,
			 u32 max_minimum_depth)
{
	struct channel_id tmpid, tmpid2;
	u8 *msg;
	struct bitcoin_tx *tx;
	struct points theirs;
	secp256k1_ecdsa_signature sig;

	set_reserve(&state->localconf.channel_reserve_satoshis,
		    state->funding_satoshis);

	/* BOLT #2:
	 *
	 * A sending node MUST set the most significant bit in
	 * `temporary-channel-id`, and MUST ensure it is unique from any other
	 * channel id with the same peer.
	 */
	/* We don't support more than one channel, so this is easy. */
	memset(&tmpid, 0xFF, sizeof(tmpid));

	/* BOLT #2:
	 *
	 * The sender MUST set `funding-satoshis` to less than 2^24 satoshi. */
	if (state->funding_satoshis >= 1 << 24)
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_BAD_PARAM,
			      "funding_satoshis must be < 2^24");

	/* BOLT #2:
	 *
	 * The sender MUST set `push-msat` to equal or less than to 1000 *
	 * `funding-satoshis`.
	 */
	if (state->push_msat > 1000 * state->funding_satoshis)
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_BAD_PARAM,
			      "push-msat must be < %"PRIu64,
			      1000 * state->funding_satoshis);

	msg = towire_open_channel(state, &tmpid,
				  state->funding_satoshis, state->push_msat,
				  state->localconf.dust_limit_satoshis,
				  state->localconf.max_htlc_value_in_flight_msat,
				  state->localconf.channel_reserve_satoshis,
				  state->localconf.htlc_minimum_msat,
				  state->feerate_per_kw,
				  state->localconf.to_self_delay,
				  state->localconf.max_accepted_htlcs,
				  &ours->funding_pubkey,
				  &ours->revocation_basepoint,
				  &ours->payment_basepoint,
				  &ours->delayed_payment_basepoint,
				  &state->next_per_commit[LOCAL]);
	if (!sync_crypto_write(&state->cs, PEER_FD, msg))
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_WRITE_FAILED,
			      "Writing open_channel");

	state->remoteconf = tal(state, struct channel_config);

	msg = sync_crypto_read(state, &state->cs, PEER_FD);
	if (!msg)
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_READ_FAILED,
			      "Reading accept_channel");

	/* BOLT #2:
	 *
	 * The receiver MUST fail the channel if `funding-pubkey`,
	 * `revocation-basepoint`, `payment-basepoint` or
	 * `delayed-payment-basepoint` are not valid DER-encoded compressed
	 * secp256k1 pubkeys.
	 */
	if (!fromwire_accept_channel(msg, NULL, &tmpid2,
				     &state->remoteconf->dust_limit_satoshis,
				     &state->remoteconf
					->max_htlc_value_in_flight_msat,
				     &state->remoteconf
					->channel_reserve_satoshis,
				     &state->remoteconf->minimum_depth,
				     &state->remoteconf->htlc_minimum_msat,
				     &state->remoteconf->to_self_delay,
				     &state->remoteconf->max_accepted_htlcs,
				     &theirs.funding_pubkey,
				     &theirs.revocation_basepoint,
				     &theirs.payment_basepoint,
				     &theirs.delayed_payment_basepoint,
				     &state->next_per_commit[REMOTE]))
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_READ_FAILED,
			      "Parsing accept_channel %s", tal_hex(msg, msg));

	/* BOLT #2:
	 *
	 * The `temporary-channel-id` MUST be the same as the
	 * `temporary-channel-id` in the `open_channel` message. */
	if (!structeq(&tmpid, &tmpid2))
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_READ_FAILED,
			      "accept_channel ids don't match: sent %s got %s",
			      type_to_string(msg, struct channel_id, &tmpid),
			      type_to_string(msg, struct channel_id, &tmpid2));

	/* BOLT #2:
	 *
	 * The receiver MAY reject the `minimum-depth` if it considers it
	 * unreasonably large.
	 *
	 * Other fields have the same requirements as their counterparts in
	 * `open_channel`.
	 */
	if (state->remoteconf->minimum_depth > max_minimum_depth)
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_BAD_PARAM,
			    "minimum_depth %u larger than %u",
			    state->remoteconf->minimum_depth, max_minimum_depth);
	check_config_bounds(state, state->remoteconf);

	/* Now, ask master create a transaction to pay those two addresses. */
	msg = towire_opening_open_resp(state, &ours->funding_pubkey,
				       &theirs.funding_pubkey);
	wire_sync_write(STATUS_FD, msg);

	/* Expect funding tx. */
	msg = wire_sync_read(state, REQ_FD);
	if (!fromwire_opening_open_funding(msg, NULL,
					   &state->funding_txid,
					   &state->funding_txout))
		peer_failed(PEER_FD, &state->cs, NULL,
			    WIRE_OPENING_PEER_READ_FAILED,
			    "Expected valid opening_open_funding: %s",
			    tal_hex(trc, msg));

	state->channel = new_channel(state,
				      &state->funding_txid,
				      state->funding_txout,
				      state->funding_satoshis,
				      state->push_msat,
				      state->feerate_per_kw,
				      &state->localconf,
				      state->remoteconf,
				      &ours->revocation_basepoint,
				      &theirs.revocation_basepoint,
				      &ours->payment_basepoint,
				      &theirs.payment_basepoint,
				      &ours->delayed_payment_basepoint,
				      &theirs.delayed_payment_basepoint,
				      LOCAL);
	if (!state->channel)
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_BAD_PARAM,
			      "could not create channel with given config");

	/* BOLT #2:
	 *
	 * ### The `funding_created` message
	 *
	 * This message describes the outpoint which the funder has created
	 * for the initial commitment transactions.  After receiving the
	 * peer's signature, it will broadcast the funding transaction.
	 */
	tx = channel_tx(state, state->channel,
			&state->next_per_commit[REMOTE],
			NULL, REMOTE);
	sig = sign_remote_commit(state,
				 &ours->funding_pubkey, &theirs.funding_pubkey,
				 tx);
	msg = towire_funding_created(state, &tmpid,
				     &state->funding_txid.sha,
				     state->funding_txout,
				     &sig);
	if (!sync_crypto_write(&state->cs, PEER_FD, msg))
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_WRITE_FAILED,
			      "Writing funding_created");

	/* BOLT #2:
	 *
	 * ### The `funding_signed` message
	 *
	 * This message gives the funder the signature they need for the first
	 * commitment transaction, so they can broadcast it knowing they can
	 * redeem their funds if they need to.
	 */
	msg = sync_crypto_read(state, &state->cs, PEER_FD);
	if (!msg)
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_READ_FAILED,
			      "Reading funding_signed");

	if (!fromwire_funding_signed(msg, NULL, &tmpid2, &sig))
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_READ_FAILED,
			      "Parsing funding_signed");
	if (!structeq(&tmpid, &tmpid2))
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_READ_FAILED,
			      "funding_signed ids don't match: sent %s got %s",
			      type_to_string(msg, struct channel_id, &tmpid),
			      type_to_string(msg, struct channel_id, &tmpid2));

	/* BOLT #2:
	 *
	 * The recipient MUST fail the channel if `signature` is incorrect.
	 */
	tx = channel_tx(state, state->channel,
		       &state->next_per_commit[LOCAL], NULL, LOCAL);

	if (!check_commit_sig(state, &ours->funding_pubkey,
			      &theirs.funding_pubkey, tx, &sig))
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_READ_FAILED,
			      "Bad signature %s on tx %s using key %s",
			      type_to_string(trc, secp256k1_ecdsa_signature,
					     &sig),
			      type_to_string(trc, struct bitcoin_tx, tx),
			      type_to_string(trc, struct pubkey,
					     &theirs.funding_pubkey));

	/* BOLT #2:
	 *
	 * Once the channel funder receives the `funding_signed` message, they
	 * must broadcast the funding transaction to the Bitcoin network.
	 */
	msg = towire_opening_open_funding_resp(state,
					       state->remoteconf,
					       &sig,
					       &state->cs,
					       &theirs.revocation_basepoint,
					       &theirs.payment_basepoint,
					       &theirs.delayed_payment_basepoint,
					       &state->next_per_commit[REMOTE]);

	status_send(msg);
}

/* This is handed the message the peer sent which caused gossip to stop:
 * it should be an open_channel */
static void recv_channel(struct state *state, const struct points *ours,
			 u32 min_feerate, u32 max_feerate, const u8 *peer_msg)
{
	struct channel_id tmpid, tmpid2;
	struct points theirs;
	secp256k1_ecdsa_signature theirsig, sig;
	struct bitcoin_tx *tx;
	u8 *msg;

	state->remoteconf = tal(state, struct channel_config);

	/* BOLT #2:
	 *
	 * The receiver MUST fail the channel if `funding-pubkey`,
	 * `revocation-basepoint`, `payment-basepoint` or
	 * `delayed-payment-basepoint` are not valid DER-encoded compressed
	 * secp256k1 pubkeys.
	 */
	if (!fromwire_open_channel(peer_msg, NULL, &tmpid,
				   &state->funding_satoshis, &state->push_msat,
				   &state->remoteconf->dust_limit_satoshis,
				   &state->remoteconf->max_htlc_value_in_flight_msat,
				   &state->remoteconf->channel_reserve_satoshis,
				   &state->remoteconf->htlc_minimum_msat,
				   &state->feerate_per_kw,
				   &state->remoteconf->to_self_delay,
				   &state->remoteconf->max_accepted_htlcs,
				   &theirs.funding_pubkey,
				   &theirs.revocation_basepoint,
				   &theirs.payment_basepoint,
				   &theirs.delayed_payment_basepoint,
				   &state->next_per_commit[REMOTE]))
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_BAD_INITIAL_MESSAGE,
			      "Parsing open_channel %s",
			      tal_hex(peer_msg, peer_msg));

	/* BOLT #2:
	 *
	 * The receiving node ... MUST fail the channel if `funding-satoshis`
	 * is greater than or equal to 2^24 */
	if (state->funding_satoshis >= 1 << 24)
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_BAD_FUNDING,
			      "funding_satoshis %"PRIu64" too large",
			      state->funding_satoshis);

	/* BOLT #2:
	 *
	 * The receiving node ... MUST fail the channel if `push-msat` is
	 * greater than `funding-satoshis` * 1000.
	 */
	if (state->push_msat > state->funding_satoshis * 1000)
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_BAD_FUNDING,
			      "push_msat %"PRIu64
			      " too large for funding_satoshis %"PRIu64,
			      state->push_msat, state->funding_satoshis);

	/* BOLT #3:
	 *
	 * The receiver MUST fail the channel if it considers `feerate-per-kw`
	 * too small for timely processing, or unreasonably large.
	 */
	if (state->feerate_per_kw < min_feerate)
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_BAD_FUNDING,
			    "feerate_per_kw %u below minimum %u",
			    state->feerate_per_kw, min_feerate);

	if (state->feerate_per_kw > max_feerate)
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_BAD_FUNDING,
			    "feerate_per_kw %u above maximum %u",
			    state->feerate_per_kw, max_feerate);

	set_reserve(&state->localconf.channel_reserve_satoshis,
		    state->funding_satoshis);
	check_config_bounds(state, state->remoteconf);

	msg = towire_accept_channel(state, &tmpid,
				    state->localconf.dust_limit_satoshis,
				    state->localconf
				      .max_htlc_value_in_flight_msat,
				    state->localconf.channel_reserve_satoshis,
				    state->localconf.minimum_depth,
				    state->localconf.htlc_minimum_msat,
				    state->localconf.to_self_delay,
				    state->localconf.max_accepted_htlcs,
				    &ours->funding_pubkey,
				    &ours->revocation_basepoint,
				    &ours->payment_basepoint,
				    &ours->delayed_payment_basepoint,
				    &state->next_per_commit[REMOTE]);

	if (!sync_crypto_write(&state->cs, PEER_FD, msg))
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_WRITE_FAILED,
			      "Writing accept_channel");

	msg = sync_crypto_read(state, &state->cs, PEER_FD);
	if (!msg)
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_READ_FAILED,
			      "Reading funding_created");

	if (!fromwire_funding_created(msg, NULL, &tmpid2,
				      &state->funding_txid.sha,
				      &state->funding_txout,
				      &theirsig))
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_READ_FAILED,
			      "Parsing funding_created");

	/* BOLT #2:
	 *
	 * The sender MUST set `temporary-channel-id` the same as the
	 * `temporary-channel-id` in the `open_channel` message. */
	if (!structeq(&tmpid, &tmpid2))
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_READ_FAILED,
			      "funding_created ids don't match: sent %s got %s",
			      type_to_string(msg, struct channel_id, &tmpid),
			      type_to_string(msg, struct channel_id, &tmpid2));

	state->channel = new_channel(state,
				      &state->funding_txid,
				      state->funding_txout,
				      state->funding_satoshis,
				      state->push_msat,
				      state->feerate_per_kw,
				      &state->localconf,
				      state->remoteconf,
				      &ours->revocation_basepoint,
				      &theirs.revocation_basepoint,
				      &ours->payment_basepoint,
				      &theirs.payment_basepoint,
				      &ours->delayed_payment_basepoint,
				      &theirs.delayed_payment_basepoint,
				      REMOTE);
	if (!state->channel)
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_BAD_PARAM,
			      "could not create channel with given config");

	/* BOLT #2:
	 *
	 * The recipient MUST fail the channel if `signature` is incorrect.
	 */
	tx = channel_tx(state, state->channel,
		       &state->next_per_commit[LOCAL], NULL, LOCAL);

	if (!check_commit_sig(state, &ours->funding_pubkey,
			      &theirs.funding_pubkey, tx, &theirsig))
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_READ_FAILED,
			      "Bad signature %s on tx %s using key %s",
			      type_to_string(trc, secp256k1_ecdsa_signature,
					     &sig),
			      type_to_string(trc, struct bitcoin_tx, tx),
			      type_to_string(trc, struct pubkey,
					     &theirs.funding_pubkey));

	/* BOLT #2:
	 *
	 * ### The `funding_signed` message
	 *
	 * This message gives the funder the signature they need for the first
	 * commitment transaction, so they can broadcast it knowing they can
	 * redeem their funds if they need to.
	 */
	tx = channel_tx(state, state->channel,
			&state->next_per_commit[REMOTE], NULL, REMOTE);
	sig = sign_remote_commit(state,
				 &ours->funding_pubkey, &theirs.funding_pubkey,
				 tx);

	msg = towire_funding_signed(state, &tmpid, &sig);
	if (!sync_crypto_write(&state->cs, PEER_FD, msg))
		peer_failed(PEER_FD, &state->cs, NULL, WIRE_OPENING_PEER_WRITE_FAILED,
			      "Writing funding_signed");

	msg = towire_opening_accept_resp(state,
					 &state->funding_txid,
					 state->funding_txout,
					 state->remoteconf,
					 &theirsig,
					 &state->cs,
					 &theirs.funding_pubkey,
					 &theirs.revocation_basepoint,
					 &theirs.payment_basepoint,
					 &theirs.delayed_payment_basepoint,
					 &state->next_per_commit[REMOTE]);

	status_send(msg);
}

#ifndef TESTING
int main(int argc, char *argv[])
{
	u8 *msg, *peer_msg;
	struct state *state = tal(NULL, struct state);
	struct privkey seed;
	struct points our_points;
	u32 max_minimum_depth;
	u32 min_feerate, max_feerate;

	if (argc == 2 && streq(argv[1], "--version")) {
		printf("%s\n", version());
		exit(0);
	}

	breakpoint();

	/* We handle write returning errors! */
	signal(SIGCHLD, SIG_IGN);
	secp256k1_ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY
						 | SECP256K1_CONTEXT_SIGN);
	status_setup(STATUS_FD);

	msg = wire_sync_read(state, REQ_FD);
	if (!msg)
		status_failed(WIRE_OPENING_BAD_COMMAND, "%s", strerror(errno));

	if (!fromwire_opening_init(msg, NULL,
				   &state->localconf,
				   &state->max_to_self_delay,
				   &state->min_effective_htlc_capacity_msat,
				   &state->cs,
				   &seed))
		status_failed(WIRE_OPENING_BAD_COMMAND, "%s", strerror(errno));
	tal_free(msg);

	/* We derive everything from the one secret seed. */
	derive_our_basepoints(&seed, &our_points, &state->our_secrets,
			      &state->shaseed, &state->next_per_commit[LOCAL]);

	msg = wire_sync_read(state, REQ_FD);
	if (fromwire_opening_open(msg, NULL,
				  &state->funding_satoshis,
				  &state->push_msat,
				  &state->feerate_per_kw, &max_minimum_depth))
		open_channel(state, &our_points, max_minimum_depth);
	else if (fromwire_opening_accept(state, msg, NULL, &min_feerate,
					 &max_feerate, &peer_msg))
		recv_channel(state, &our_points, min_feerate, max_feerate,
			     peer_msg);

	/* Hand back the fd. */
	fdpass_send(REQ_FD, PEER_FD);

	/* Wait for exit command (avoid state close being read before reqfd) */
	msg = wire_sync_read(state, REQ_FD);
	if (!msg)
		status_failed(WIRE_OPENING_BAD_COMMAND, "%s", strerror(errno));
	if (!fromwire_opening_exit_req(msg, NULL))
		status_failed(WIRE_OPENING_BAD_COMMAND, "Expected exit req not %i",
			      fromwire_peektype(msg));
	tal_free(state);
	return 0;
}
#endif /* TESTING */
