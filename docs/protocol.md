# Sync protocol (version 3)

All integers are big endian. Transport: one TCP connection per session, default port 18530.

## Record layer

Every record: `u16 length | body` (length ≤ 2560).

### Handshake (plaintext bodies)

```
C → S  C_HELLO  "FGP" 0x03 | 0x01 | client_id u32 | nonce_c[16]                  (25 bytes)
S → C  S_HELLO  "FGP" 0x03 | 0x02 | server_id u32 | nonce_s[16] | proof_s[32]    (57 bytes)
C → S  C_AUTH   "FGP" 0x03 | 0x03 | proof_c[32]                                   (37 bytes)
```

The fourth magic byte is the protocol version; nodes with different versions refuse each other
(update all nodes together).

- `node_id`: FNV-1 hash of the ESPHome node name. A node refuses a connection from or to its own ID.
- Key schedule:
  `keys = HKDF-SHA256(salt = "growplus-v1", ikm = PSK, info = nonce_c | nonce_s | client_id | server_id, L = 160)`
  This yields `k_auth | k_enc_c2s | k_mac_c2s | k_enc_s2c | k_mac_s2c` (32 bytes each).
- `proof_s = HMAC-SHA256(k_auth, "server" | C_HELLO | S_HELLO[0..25])`
- `proof_c = HMAC-SHA256(k_auth, "client" | C_HELLO | S_HELLO)`

The client verifies the server first; the server accepts application data only after a valid
`C_AUTH`.

### Data records

```
body = seq u32 | ciphertext | tag[16]
ciphertext = ChaCha20(k_enc_dir, nonce = seq u32 | 0x00 * 8, counter = 0) XOR plaintext
tag = HMAC-SHA256(k_mac_dir, u16 body_length | seq | ciphertext)[0..16]
```

`seq` starts at 0 per direction and must increase by exactly one. A replayed, reordered or
tampered record aborts the session.

## Messages (plaintext of a data record)

| type | name | payload |
|---|---|---|
| 0x01 | MANIFEST | `u8 page, u8 flags (bit0 = last), u16 capacity, u16 count, count × (u16 slot, u16 meta, u8 hash[8], u32 expires)` |
| 0x02 | TEMPLATE_REQ | `u16 slot` |
| 0x03 | TEMPLATE | `u16 slot, u16 meta, u8 hash[8], u32 expires, u16 len, u8 data[len]` |
| 0x04 | TEMPLATE_NAK | `u16 slot, u8 reason` |
| 0x05 | DONE | – |
| 0x06 | RESTORE | `u16 slot, u32 expires, u16 len, u8 data[len]` (accepted only in a restore window) |
| 0x07 | RESTORE_ACK | `u16 slot, u8 result` (0 ok, 1 failed, 2 refused) |

- `meta = state << 14 | version` with state 0 none, 1 present, 2 present-forced, 3 tombstone.
- `hash` = first 8 bytes of SHA-256(template). Zero for tombstones.
- `expires` = unix time after which a guest finger is deleted, 0 = never. It belongs to the slot
  version: a node adopting a version adopts its expiry.

Version history: 1 = initial; 2 = `expires` added to MANIFEST, TEMPLATE and RESTORE;
3 = magic `FGP` and HKDF salt `growplus-v1` (rename to `fingerprint_grow_plus`), messages unchanged.

### Session flow

1. After the handshake both sides send their manifest (64 entries per page, at least one page).
2. Each side processes the peer's manifest and requests templates it needs, **one at a time**.
   The next request is sent after the previous template has been stored, which bounds the memory
   to one template.
3. A side sends `DONE` when it has the peer's full manifest and no pull is pending. It keeps
   serving requests afterwards.
4. The client closes the connection when both sides sent `DONE`.

## Version counter reset

Versions are 14 bit (max 16383). Every enrollment, deletion and conflict resolution in the
cluster consumes one version. If the limit is ever reached:

1. Back up one node with `tools/growplus.py backup`.
2. Erase the metadata on all nodes (flash with `metadata_storage` switched or wipe preferences).
3. Restore.
