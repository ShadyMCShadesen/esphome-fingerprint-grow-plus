# Network scenarios

Only the `sync:` block uses the network. It needs **one TCP port** (default `18530`,
`sync: port`). Every node listens on it and connects to the peers in its `sync: peers` list. There is no broadcast or multicast, so the same
configuration works in one subnet and across routers.

## Peer addresses

| form | example | works |
|---|---|---|
| IPv4 | `192.168.10.23` | everywhere (needs a static IP or a DHCP reservation) |
| DNS name | `reader-garage.example.lan` | everywhere your DNS resolves it |
| mDNS | `reader-garage.local` | only in the same subnet/VLAN (multicast does not cross routers unless you run an mDNS reflector) |
| with port | `10.20.0.5:18531` | when a node listens on a non-default port or behind NAT |

mDNS resolution: ESP32 resolves `.local` via lwIP (`enable_lwip_mdns_queries`, enabled by
default in ESPHome). On ESP8266 `.local` resolution depends on the lwIP build; use IP addresses or
DNS names there.

The list may contain the node itself (handy with a shared `packages:` file); a node skips the
entry that equals its own name or `name.local`, and refuses connections from its own node ID.

## Same subnet

Nothing to configure besides `peers`. Optional hardening:

```yaml
fingerprint_grow_plus:
  sync:
    allowed_networks: [192.168.10.0/24]
```

## VLANs, routed networks, VPN

1. Use IP addresses or DNS names in `peers` (no `.local`).
2. Firewall rules, for every pair of nodes in **both** directions:

   ```
   allow TCP  <node subnet(s)>  ->  <node subnet(s)>  dport 18530
   ```

   Only established connections need to come back; stateful firewalls handle that.
3. Set `allowed_networks` to the node subnets.
4. NAT between nodes works if the published port is set per peer (`host:port`).
5. Over a VPN (e.g. WireGuard on a remote node) the remote node's tunnel address may arrive
   instead of an address from the local subnet; add it to `allowed_networks` on the other nodes.
   One side connecting is enough: a session syncs both directions, so the remote node may list
   the others as peers without being listed itself (it then drives the sync at its `interval`).

Nothing in the sync logic changes when you move from one subnet to routed networks. Only the
addresses in `peers` and the firewall are affected.

## Traffic

- Idle round with nothing to do: about 1–3 kB per peer every `sync: interval`.
- One template: about 1.6 kB plus framing per peer that needs it (R503: 1536-byte templates).
- Connections are short-lived: one per peer per round; the node closes them when done.
