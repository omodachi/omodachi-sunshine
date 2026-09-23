# Managed Sunshine v3 private IPC

This contract is unchanged in source freeze v5. v4 fixed CMake header precedence; v5 fixes Linux library linkage and restricts the private build helper to compile/link only. PLUG-4 adds `pairing.revoke` and the `certificate_revocation_supported` capability flag; nothing else changed.

The transport is a same-UID Unix stream socket at `$XDG_RUNTIME_DIR/omodachi-sunshine/pairing.sock`, activated by `SUNSHINE_MANAGED_LOCAL_PAIRING=1`. Send exactly one UTF-8 JSON object followed by one LF per connection. The request limit is 4096 bytes including LF. Response JSON is limited to 32768 bytes before its LF. There is a two-second connection budget. Duplicate JSON keys, unexpected top-level fields, and unsafe endpoints are rejected. Transport failure or a missing response is not a successful operation.

`ipc-v3.schema.json` describes the producer contract. Validate `#/$defs/request` for requests and `#/$defs/response` for responses. The schema does not replace runtime checks for UTF-8 byte length, live request identity, current ownership, equality between echoed fields, monotonic epochs, and actual session stop completion. The PIN name limit is 80 UTF-8 bytes; JSON Schema length alone counts characters. `ipc-v3.examples.json` contains synthetic examples for all operations.

## Pairing

| Operation | Request fields after `op` | Success fields after `ok` |
| --- | --- | --- |
| `pairing.list` | none | `requests` array |
| `pairing.pending` | `request_id`, `client_cert_sha256` | `request` |
| `pairing.approve` | `request_id`, `client_cert_sha256`, `pin`, `name` | `accepted:true`, `request_id`, `status:"awaiting_client_proof"` |
| `pairing.cancel` | `request_id`, `client_cert_sha256` | `request` with `status:"cancelled"` |
| `pairing.clients` | none | `clients` array, `unreadable`, `truncated` |
| `pairing.revoke` | `client_cert_sha256` | `status:"revoked"` or `status:"not_found"` |

Request metadata is `{request_id,client_cert_sha256,status,expires_in_ms}`. Status is `pending`, `awaiting_client_proof`, `paired`, `failed`, `cancelled`, or `expired`. The ID is a random UUID; the fingerprint is lowercase SHA-256 of the canonical DER certificate. Deadlines are monotonic and last 120 seconds. At most eight pending requests and 64 recent terminal results are retained. Terminal results may still have positive `expires_in_ms`; the terminal status is authoritative. Terminal results are bounded tombstones and are not persistent media identities.

The GameStream `getservercert` response remains suspended while awaiting the PIN. It does **not** expose the local request ID in response headers or a native network API. Core must use the private list operation to discover the exact unexpired pending request that matches the fingerprint supplied by the authenticated companion device for its current explicit pairing intent. Select only one exact match; never use name, IP, uniqueid, recency, array order, or unrelated list entries. A discovery result conveys neither plugin approval nor certificate authorization.

Approval submits the four-digit PIN once. Only the original successful GameStream `clientpairingsecret` hash/signature verification creates `paired` and authorizes the client certificate. Core must poll the exact request ID/fingerprint until `paired` or failure; an approval ACK is insufficient. An expired retry has a new UUID and requires a current explicit approval. The normal managed-app path can carry the ephemeral PIN through the authenticated companion channel without asking the user to copy it or open Sunshine Web.

`pairing.cancel` cancels a temporary attempt and is still never a revoke.

`pairing.revoke` (PLUG-4) is the persistent one. It is addressed by certificate alone, because the temporary request ID is long gone by the time a user revokes a device and the certificate is what still authorizes a stream. It deletes every authorized record whose stored PEM has that DER SHA-256, writes the surviving records back and reloads the verification chain, so a reconnect on the revoked leaf fails the unchanged TLS check; it cancels a temporary attempt for the same certificate, so a revoke cannot race one into a fresh authorization; and it signals a stop to the live streams that exact certificate authorized, never to another client's session and never a host quit. Matching is on the recomputed DER digest only - never the display name or the locally generated client UUID. `status:"revoked"` means an authorized record was removed, `status:"not_found"` that none carried this certificate; both mean the certificate is not authorized any more. The response is an acknowledgement of the local store, not proof that a peer has disconnected.

`pairing.clients` lists the authorized client store by the only identity a revoke can use: `{client_cert_sha256, name, enabled}` per record, at most 64 (`truncated`), plus `unreadable` for records whose stored certificate does not parse and therefore have no fingerprint to revoke by. No certificate, PEM or key material leaves the socket, and `name` is the display name the record already carried - it is not an identity and nothing may authorize on it. It exists so a caller can find the certificates it has no record of and take them back, instead of reading the fork's private state file.

`pairing.list` and `pairing.pending` answer `certificate_revocation_supported:true` on a build that implements it. A caller that does not see the flag must keep treating its own revoke as unresolved and must keep denying its media grant, exactly as before; cancellation must never be presented as revoking an existing authorized certificate.

## Media observation

`media.list` has no extra fields and returns `{ok:true,sessions:[...]}`. `media.get` takes `session_id` and returns `{ok:true,session:{...}}`. Sessions are actual registered stream sessions, not encoder capability probes. The bounded observer retains at most 16 entries and may evict stopped ones.

The session ID and `launch_id` identify the observation; `generation` and `capture_generation` fence actual encoder selection and display instances. `selected_output`, `output_pixels`, `logical_size`, and `output_offset` come from the selected WLR monitor. `captured_pixels` appears after matching actual capture callbacks. `encoded_pixels` appears after nonempty encoder packet production and names its source in `encoded_geometry_source`. `codec` remains the compatibility encoder-name field; `encoder_name`, `encoder_backend`, and `video_codec` distinguish the implementation from H.264/HEVC/AV1.

`video_ended` is not a complete stop. `stop_complete` follows video/audio/control teardown plus completion of the queued key/button release job. `client_presented` is always null and `bitstream_dimensions_independently_parsed` is false. The source does not claim native received/displayed geometry or actual client presentation.

## Desktop ownership and adaptive output

Every desktop request carries `protocol:"omodachi.sunshine.desktop.v1"`. Except for `desktop.status`, it carries the exact lease `{lease_id,lease_epoch,owner_device_id,client_cert_sha256}`. Success is `{protocol,ok:true,result:{...}}`; application failure is `{protocol,ok:false,error:{code}}`. A transport/internal error can lack `protocol`, so clients must fail closed on missing/invalid envelope fields.

| Operation | Fields in addition to protocol and lease | Meaning |
| --- | --- | --- |
| `desktop.status` | no lease | Returns configured `available`, `backend`, `encoder`, and `encoders` (the codecs a prepared profile may name); this is not evidence of a captured frame. |
| `desktop.claim` | `output_id` | Reserves the exact `OMODACHI-<16 lowercase hex>` output and rejects unrelated live/admitted streams. |
| `desktop.prepare` | `output_id`, `identity`, `profile` | While stopped, binds the requested transition and internal owned-output selector. |
| `desktop.session` | none | Returns the exact claim and correlated actual media/capture generations. |
| `desktop.stop` | none | Signals only the owned session and waits up to 1.4 seconds for the true stop fence. |
| `desktop.release` | none | Requires the session to be stopped, clears the runtime selector, and returns `released:true`. |

The identity is `{lease_id,lease_epoch,transition_id,geometry_epoch,connection_generation}` and must match the lease. A changed identity requires both geometry and connection generations to increase. Prepare currently accepts a headless H.264 or HEVC SDR profile with transform 0 and bounded `output_mode_pixels` / `stream_pixels`. Profile fields are configuration intent; they must not be reported as actual observed encoder or client dimensions.

`desktop.status.encoders` is the ordered list of codecs this binary will both accept in a profile and actually encode: always `"h264"`, plus `"hevc"` when the configured encoder is available and Sunshine's startup encoder probe passed HEVC (`video::active_hevc_mode >= 2`). A caller must not prepare a codec that is not in the list; a caller talking to an older binary that omits `encoders` must assume `["h264"]`. The RTSP launch must request the prepared codec (`x-nv-vqos[0].bitStreamFormat` 0 for `h264`, 1 for `hevc`); any other format is answered `409 Prepared geometry mismatch`, exactly as a mismatched size, frame rate or bitrate is.

Results echo `lease`, `identity`, `configured_output_id`, `prepared`, `session_count`, `stopped`, `media_session_id`, `media_generation`, and `capture_generation`. Core owns output creation, geometry transitions, client negotiation, and the comparison between requested and actual observations. The fork selects the exact owned WLR output and refuses fallback to a physical monitor if it is missing. Launch/resume admission binds the verified TLS leaf certificate through shared connection ownership, not endpoint addresses or query IDs.

If `desktop.stop` returns `session_not_stopped`, the caller must keep ownership and retry/resolve the pending transition; it must not treat the timeout as permission to release or recreate output. No operation edits the persistent user configuration, runs arbitrary shell commands, quits the user's apps, or terminates unrelated sessions.
