# Security

Fingerprint templates are biometric data. They cannot be changed like a password once they leak.

## Goals

1. Only nodes that know the shared key can read or write templates.
2. Templates never appear in clear on the network, in logs or in the repository.
3. A peer cannot be impersonated, and recorded traffic cannot be replayed.
4. Nothing is written to a sensor that was not authenticated and checksum verified.
5. The sensor password and system parameters are never changed.

## Mechanisms

| threat | mechanism |
|---|---|
| eavesdropping | ChaCha20 (RFC 8439) per-direction session keys |
| tampering | HMAC-SHA256 tag (128 bit) on every record; template SHA-256 check; read-back verification after `Store` |
| impersonation | mutual challenge-response with HMAC-SHA256 over both 128-bit nonces; keys derived with HKDF-SHA256 from the 256-bit PSK |
| replay / reordering | fresh nonces from both sides per session; strict per-direction sequence numbers |
| unknown hosts | optional `sync: allowed_networks` (source IPv4 allow list); without `sync:` no port is open at all |
| key leak via repo | key only in `secrets.yaml`; `.gitignore` excludes it |
| template leak via logs | templates are never logged at any level (only slot numbers); buffers are wiped after use |
| accidental mass deletion | `delete_all` needs the configured code; admin fingers survive it; local deletions via `fingerprint_grow` are *not* propagated |
| uncontrolled restore | `RESTORE` is accepted only within an `allow_restore` window opened on the node itself |
| resource exhaustion | handshake must finish within 5 s; receive buffer ≤ one record; send buffer ≤ 16 KB per session; one manifest per session, ≤ capacity entries; ≤ 4 outstanding template requests and 1 outstanding restore; job queue ≤ 300 with idempotent jobs deduplicated; at most one template (≈1.5 KB) per session in RAM |
| stuck sensor | a finger sensor reporting a finger for > 60 s is treated as stuck; a sync round waiting > 5 min for a busy sensor is given up |

### Why this construction

- **Portability.** ESPHome offers HMAC-SHA256 on ESP32 (mbedTLS/PSA) and ESP8266 (BearSSL), but
  no common cipher. ChaCha20 is small, constant-time and easy to implement correctly. It is
  tested against OpenSSL and an RFC 8439 / RFC 5869 reference in `tests/host`.
- **Standard composition.** Encrypt-then-MAC with independent keys, PSK-based mutual
  authentication and HKDF key derivation are textbook constructions. They avoid depending on
  the internals of ESPHome's Noise implementation.

## Residual risks (please read)

- **Compromised node = compromised cluster.** Every node holds the PSK in its flash (like the
  ESPHome API key) and has the templates on its sensor. Someone with physical access to a node
  can read both, especially an outdoor station. Mitigations: flash encryption / secure boot on
  ESP32, physical protection, separate keys per site.
- **No forward secrecy.** Whoever obtains the PSK and has recorded past traffic can decrypt it.
  There is no ephemeral Diffie-Hellman. Rotate the key if you suspect a leak (flash all nodes with
  a new key).
- **Shared key.** Any node can impersonate any other node. There are no per-node identities.
- **Attacker in the LAN without the key** can block or reset connections (denial of service)
  and see that nodes talk to each other, but cannot read or change templates.
- **Template format.** R503 templates are vendor-specific feature data, not images. Whether they
  can be reverse engineered into a usable fake finger is unknown; treat them as sensitive.
- **Backups** made with `tools/growplus.py` are encrypted with a key derived from the PSK. Anyone
  holding the backup file *and* the PSK can read the templates.
- **ESP8266:** the PSK sits in flash without hardware protection.
- **Sensor UART.** The cable between ESP and sensor is not protected; neither is it in a setup
  without this component.
