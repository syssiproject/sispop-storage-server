#pragma once

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>
#include <sispopc/bt_serialize.h>
#include <sispopmq/sispopmq.h>

#include "../common/message.h"
#include "../snode/sn_record.h"

namespace sispop::rpc {
class RequestHandler;
class RateLimiter;
struct OnionRequestMetadata;
struct Response;
}  // namespace sispop::rpc

namespace sispop::snode {
class ServiceNode;
}

namespace sispop::server {

using namespace std::literals;

sispopc::bt_value json_to_bt(nlohmann::json j);

nlohmann::json bt_to_json(sispopc::bt_dict_consumer d);
nlohmann::json bt_to_json(sispopc::bt_list_consumer l);

struct MonitorData {
    static constexpr auto MONITOR_EXPIRY_TIME = 65min;

    std::chrono::steady_clock::time_point expiry;  // When this notify reg expires
    std::vector<namespace_id> namespaces;          // sorted namespace_ids
    sispopmq::ConnectionID push_conn;                // ConnectionID to push notifications to
    bool want_data;                                // true if the subscriber wants msg data

    MonitorData(
            std::vector<namespace_id> namespaces,
            sispopmq::ConnectionID conn,
            bool data,
            std::chrono::seconds ttl = MONITOR_EXPIRY_TIME) :
            expiry{std::chrono::steady_clock::now() + ttl},
            namespaces{std::move(namespaces)},
            push_conn{std::move(conn)},
            want_data{data} {}

    void reset_expiry(std::chrono::seconds ttl = MONITOR_EXPIRY_TIME) {
        expiry = std::chrono::steady_clock::now() + ttl;
    }
};

class OMQ {
    sispopmq::SispopMQ omq_;
    sispopmq::ConnectionID sispopd_conn_;

    // Has information about current SNs
    snode::ServiceNode* service_node_ = nullptr;

    rpc::RequestHandler* request_handler_ = nullptr;

    rpc::RateLimiter* rate_limiter_ = nullptr;

    // Tracks accounts we are monitoring for OMQ push notification messages
    std::unordered_multimap<std::string, MonitorData> monitoring_;
    mutable std::shared_mutex monitoring_mutex_;

    // Get node's address
    std::string peer_lookup(std::string_view pubkey_bin) const;

    // Handle Session data coming from peer SN
    void handle_sn_data(sispopmq::Message& message);

    // Called starting at HF18 for SS-to-SS onion requests
    void handle_onion_request(sispopmq::Message& message);

    // Handles a decoded onion request
    void handle_onion_request(
            std::string_view payload,
            rpc::OnionRequestMetadata&& data,
            sispopmq::Message::DeferredSend send);

    // sn.ping - sent by SNs to ping each other.
    void handle_ping(sispopmq::Message& message);

    // sn.storage_test
    void handle_storage_test(sispopmq::Message& message);

    /// storage.(whatever) -- client request handling.  These reply with [BODY] on success or
    /// [CODE, BODY] on failure (where BODY typically is some sort of error message).
    ///
    /// The return value is either:
    /// [VALUE] for a successful response
    /// [ERRCODE, VALUE] for a failure.
    ///
    /// Successful responses will generally return VALUE as json, if the request was json (or
    /// empty), or a bt-encoded dict if the request was bt-encoded.  Note that base64-encoded
    /// values for json responses are raw byte values (*not* base64-encoded) when returning a
    /// bt-encoded value.
    ///
    /// Failure responses are an HTTP error number and a plain text failure string.
    ///
    /// `forwarded` is set if this request was forwarded from another swarm member rather than
    /// being direct from the client; the request is handled identically except that these
    /// forwarded requests are not-reforwarded again, and the method name is prepended on the
    /// argument list.
    void handle_client_request(
            std::string_view method, sispopmq::Message& message, bool forwarded = false);

    /// Handles a subscription request to monitor new messages (OMQ endpoint monitor.messages).  The
    /// message body must be bt-encoded, and can be either a dict, or a list of dicts, containing
    /// the following keys.  Note that keys are case-sensitive and, for proper bt-encoding, must be
    /// in ascii-sorted order (rather than the order described here).
    ///
    /// The list of dicts mode is primarily intended to batch multiple subscription requests
    /// together
    ///
    /// Keys are:
    /// - exactly one of:
    ///   - p -- the account public key, prefixed with the netid, in bytes (33 bytes).  This should
    ///     be used for pubkeys that are ed keys (but not 05 session ids, see the next entry)
    ///   - P -- an ed25519 pubkey underlying a session ID, in bytes (32 bytes).  The account
    ///     will be derived by converting to an x25519 pubkey and prepending the 0x05 byte.  The
    ///     signature uses *this* key, not the derived x25519 key.
    /// - S -- (optional) a 32-byte authentication subkey tag to use for authentication.  The
    ///   signature with such a subkey tag uses a derived subkey (as described in the RPC endpoint
    ///   documentation).
    /// - n -- list of namespace ids to monitor for new messages; the ids must be valid (i.e. -32768
    ///   through 32767), must be sorted in numeric order, and must contain no duplicates.
    /// - d -- set to 1 if the caller wants the full message data, 0 (or omitted) will omit the data
    ///   from notifications.
    /// - t -- signature timestamp, in integer unix seconds (*not* milliseconds), associated with
    ///   the signature.  This timestamp must be within the last 2 weeks (and no more than 1 day in
    ///   the future) for this request to be valid.
    /// - s -- the signature associated with this message.  This is an Ed25519 signature of the
    ///   value:
    ///       ( "MONITOR" || ACCOUNT || TS || D || NS[0] || "," || ... || "," || NS[n] )
    ///   signed by the account Ed25519 key or derived subkey (if using a subkey tag):
    ///   - ACCOUNT is the full account ID, expressed in hex (e.g. "0512345...").
    ///   - TS is the signature timestamp value, expressed as a base-10 string
    ///   - D is "0" or "1" depending on whether data is wanted (i.e. the "d" request parameter)
    ///   - NS[i] are the namespace values from the request expressed as base-10 strings
    ///
    /// If the request validates then the connection is subscribed (for 65 minutes) to new incoming
    /// messages in the given namespace(s).  A caller should renew subscriptions periodically by
    /// re-submitting the subscription request (with at most 1h between re-subscriptions).
    ///
    /// The reply to the subscription request is either a bencoded dict or list of dicts containing
    /// the following keys.  In the case of a list of subscriptions in the request, the returned
    /// list will be the same length with the ith element corresponding to the ith element of the
    /// input.
    /// - success -- included on successful subscription and set to the integer 1
    /// - errcode -- a numeric error value indicating the failure.  Currently implemented are:
    ///   - 1 -- invalid arguments -- called for invalid data (e.g. wrong encoding, wrong value
    ///     type, or a missing required parameter)
    ///   - 2 -- invalid pubkey -- the given pubkey/session id is not a valid pubkey.
    ///   - 3 -- invalid namespace -- the namespaces provided are invalid (e.g. invalid value, not
    ///     sorted, or contains duplicates).
    ///   - 4 -- invalid timestamp -- the timestamp is not a valid integral timestamp, is too old,
    ///     or is in the future.
    ///   - 5 -- signature failed -- the signature failed to validate.
    ///   - 6 -- wrong swarm -- the given pubkey is not stored by this service node's swarm.
    /// - error -- included whenever `errcode` is, this contains an English description of the
    ///   error.
    ///
    /// Each time a message is received the service node sends a message to the connection with a
    /// first part (i.e. endpoint) of "notify.message", and second part containing the bt-encoded
    /// message details in a dict with keys:
    ///
    /// - @ -- the account pubkey, in bytes (33).  This is the actual account value, regardless of
    ///   which of `p`/`P`/`S` was used in the request.  (The dict key is a symbol so that it sorts
    ///   first in the encoded data).
    /// - h -- the message hash
    /// - n -- the message namespace (-32768 to 32767)
    /// - t -- the message timestamp (milliseconds since unix epoch), as provided by the client who
    ///   deposited the message.
    /// - z -- the expiry (milliseconds since unix epoch) of the message.
    /// - ~ -- the message data, if requested.
    ///
    /// Note: if the same connection submits multiple simultaneous subscriptions then the subsequent
    /// subscriptions add to earlier subscriptions.  This has some implications:
    ///
    /// - The caller only receives one notification on the connection for a matching message from
    ///   any of the matching subscription requests on that same connection.
    /// - All pubkey/subkey/ed25519 pubkeys that access the same account are treated as the same
    ///   subscription.
    /// - The data key (`~`) will be present in a notification if *any* subscription request for the
    ///   same account on the same connection requested data.  Same a flag only expires when the
    ///   subscription itself expires, but, like namespaces, persists as long as the subscription is
    ///   renewed (even if the renewals no longer request data).
    /// - Subscription renewal will renew all subscribed namespaces for that connection.  This *may*
    ///   extend the subscription of some namespaces (e.g. if you subscribe to namespaces 0 and 1,
    ///   then stop subscribing to namespace 1 but keep renewing a subscription to ns 0 on the same
    ///   connection for the same account).
    ///
    /// Thus any code that is managing subscriptions for multiple end clients should take care to
    /// check the namespace/data values and only pass it on if actually desired by a client.
    ///
    /// Note that the client should accept (and ignore) unknown keys, to allow for future expansion.
    void handle_monitor_messages(sispopmq::Message& message);

    void handle_get_logs(sispopmq::Message& message);

    void handle_get_stats(sispopmq::Message& message);

    // Access pubkeys for the 'service' command category (for access stats & logs), in binary.
    std::unordered_set<std::string> stats_access_keys_;

    // Connects (and blocks until connected) to sispopd.  When this returns an sispopd connection
    // will be available (and sispopd_conn_ will be set to the connection id to reach it).
    void connect_sispopd(const sispopmq::address& sispopd_rpc);

  public:
    OMQ(const snode::sn_record& me,
        const crypto::x25519_seckey& privkey,
        const std::vector<crypto::x25519_pubkey>& stats_access_keys_hex);

    // Initialize sispopmq; return a future that completes once we have connected to and
    // initialized from sispopd.
    void init(
            snode::ServiceNode* sn,
            rpc::RequestHandler* rh,
            rpc::RateLimiter* rl,
            sispopmq::address sispopd_rpc);

    /// Dereferencing via * or -> accesses the contained SispopMQ instance.
    sispopmq::SispopMQ& operator*() { return omq_; }
    sispopmq::SispopMQ* operator->() { return &omq_; }

    // Returns the OMQ ConnectionID for the connection to sispopd.
    const sispopmq::ConnectionID& sispopd_conn() const { return sispopd_conn_; }

    // Invokes a request to the local sispopd; given arguments (which must contain at least the
    // request name and a callback) are forwarded as `omq.request(connid, ...)`.
    template <typename... Args>
    void sispopd_request(Args&&... args) {
        assert(sispopd_conn_);
        omq_.request(sispopd_conn(), std::forward<Args>(args)...);
    }

    // Sends a one-way message to the local sispopd; arguments are forwarded as `omq.send(connid,
    // ...)` (and must contain at least a command name).
    template <typename... Args>
    void sispopd_send(Args&&... args) {
        assert(sispopd_conn_);
        omq_.send(sispopd_conn(), std::forward<Args>(args)...);
    }

    // Encodes the onion request data that we send for internal SN-to-SN onion requests starting
    // at HF18.
    static std::string encode_onion_data(
            std::string_view payload, const rpc::OnionRequestMetadata& data);
    // Decodes onion request data; throws if invalid formatted or missing required fields.
    static std::pair<std::string_view, rpc::OnionRequestMetadata> decode_onion_data(
            std::string_view data);

    // Called during message submission to send notifications to anyone subscribed to them.
    void send_notifies(message msg);
};

}  // namespace sispop::server
