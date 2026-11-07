# Kurzanleitung (Deutsch)

`fingerprint_sync` überträgt die Fingerabdruck-Templates eines GROW-Sensors (R503 u. a.) direkt
zwischen ESPHome-Geräten. Ein Finger wird einmal an einem beliebigen Gerät eingelernt und liegt
danach auf allen Geräten **im gleichen Slot**. Home Assistant, MQTT oder Cloud werden dafür
nicht benötigt.

## Einrichtung

1. Schlüssel erzeugen und in die `secrets.yaml` aller Geräte eintragen:

   ```bash
   python3 tools/fpsync.py genkey
   ```

   ```yaml
   fingerprint_sync_key: "<Ausgabe>"
   fingerprint_delete_all_code: "<langer Code>"
   ```

2. Komponente einbinden, auf eine feste Version gepinnt:

   ```yaml
   external_components:
     - source: github://ShadyMCShadesen/esphome-fingerprint-sync@v0.1.0
       components: [fingerprint_sync]

   fingerprint_sync:
     key: !secret fingerprint_sync_key
     uart_id: <gleicher UART wie fingerprint_grow>
     peers:
       - <anderes-geraet>.local
     dry_run: true
   ```

3. Erst mit `dry_run: true` flashen und die Logs prüfen. Dann von jedem Sensor ein Backup
   ziehen (siehe unten) und erst danach `dry_run: false` setzen.

## Wichtige Regeln

- Ein Slot, der nur lokal fehlt (neuer Sensor, `fingerprint_grow.delete`), wird von den anderen
  Geräten **wiederhergestellt**.
- **Dauerhaft löschen** nur mit `fingerprint_sync.delete`. Das wirkt auf allen Geräten.
- `fingerprint_sync.delete_all` braucht den Code und `confirm_cluster_wide: true`.
- Unterschiedliche Finger im selben Slot werden **nie** überschrieben, sondern als Konflikt
  gemeldet (`on_conflict`, Sensor `conflict_count`). Auflösen mit
  `fingerprint_sync.resolve_conflict`:
  - `keep_local: true`: dieser Finger gilt überall
  - `keep_local: false`: den Finger der anderen übernehmen
- Gerätebezogene Bereiche: `exclude_slots: ["180-199"]` hält diese Slots lokal.
- Das Sensor-Passwort wird nie gesetzt oder geändert.

## Einlernen ohne Home Assistant

- **Taster:** `fingerprint_sync.enroll_next_free` im `on_press` eines GPIO-Binary-Sensors.
- **Weboberfläche:** Template-Buttons im `web_server` (siehe `examples/common-fingerprint-sync.yaml`).
- **Admin-Finger:** Slot 0 zweimal innerhalb von 5 s auflegen. Danach wird der nächste freie Slot
  eingelernt.

## Backup und Restore

```bash
export FPSYNC_KEY='<Schlüssel>'
python3 tools/fpsync.py backup  --host <geraet>.local --out <geraet>.fpsb
python3 tools/fpsync.py inspect --in <geraet>.fpsb
# am Gerät zuerst fingerprint_sync.allow_restore auslösen, dann:
python3 tools/fpsync.py restore --host <geraet>.local --in <geraet>.fpsb
```

Backup-Dateien sind verschlüsselt, enthalten aber biometrische Daten: nie in ein Repository legen.

## Netzwerk

Es wird ein TCP-Port (Standard 18530) in beide Richtungen zwischen allen Geräten genutzt.

- **Gleiches Netz:** `.local`-Namen funktionieren.
- **Über VLAN/Router:** IP-Adressen oder DNS-Namen verwenden und den Port in der Firewall
  freigeben.

Details: [network.md](../network.md).

## LED-Rückmeldung

Die Komponente steuert keine LEDs. Für die eigene LED-Logik gibt es die Trigger:

- `on_sync_started`, `on_sync_done`, `on_sync_failed`
- `on_conflict`, `on_slot_updated`, `on_slot_deleted`, `on_enroll_started`
