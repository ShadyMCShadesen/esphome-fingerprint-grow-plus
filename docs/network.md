# Network scenarios

The sync uses **one TCP port** (default `18530`, option `port`). Every node listens on it and
connects to the peers in its `peers` list. There is no broadcast or multicast, so the same
configuration works in one subnet and across routers.

## Peer addresses

| form | example | works |
|---|---|---|
| IPv4 | `192.168.10.23` | everywhere (needs a static IP or a DHCP reservation) |
| DNS name | `reader-garage.example.lan` | everywhere your DNS resolves it |
| mDNS | `reader-garage.local` | only in the same subnet/VLAN (multicast does not cross routers unless you run an mDNS reflector) |
| with port | `host: 10.20.0.5` + `port: 18531` | when a node listens on a non-default port or behind NAT |

mDNS resolution: ESP32 resolves `.local` via lwIP (`enable_lwip_mdns_queries`, enabled by
default in ESPHome). On ESP8266 `.local` resolution depends on the lwIP build; use IP addresses or
DNS names there.

The list may contain the node itself (handy with a shared `packages:` file); a node skips the
entry that equals its own name or `name.local`, and refuses connections from its own node ID.

## Same subnet (today)

Nothing to configure besides `peers`. Optional hardening:

```yaml
fingerprint_sync:
  allowed_networks: [192.168.10.0/24]
```

## VLANs / routed networks (later)

1. Use IP addresses or DNS names in `peers` (no `.local`).
2. Firewall rules, for every pair of nodes in **both** directions:

   ```
   allow TCP  <node subnet(s)>  ->  <node subnet(s)>  dport 18530
   ```

   Only established connections need to come back; stateful firewalls handle that.
3. Set `allowed_networks` to the node subnets.
4. NAT between nodes works if the published port is set per peer (`host` + `port`).

Nothing in the sync logic changes when you move from one subnet to routed networks. Only the
addresses in `peers` and the firewall are affected.

## Traffic

- Idle round with nothing to do: about 1–3 kB per peer every `sync_interval`.
- One template: about 830 bytes plus framing per peer that needs it.
- Connections are short-lived: one per peer per round; the node closes them when done.
