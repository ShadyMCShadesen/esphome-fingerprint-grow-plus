# Anleitung (Deutsch)

`fingerprint_grow_plus` erweitert ESPHome-`fingerprint_grow` (GROW-Sensor R503 u. a.), ganz ohne
Home Assistant:

- **Admin-Finger** bedienen den Sensor: neue Finger einlernen, einen Finger löschen oder neu
  einlernen, indem man ihn zeigt, Statusfarbe anzeigen.
- **Gästefinger** laufen nach einer festen Zeit ab und werden automatisch gelöscht.
- **Geschützte Slots**: Admin-Finger lassen sich nicht versehentlich löschen.
- **Sync** *(optional)*: mehrere ESPHome-Geräte haben dieselben Finger in denselben Slots. Einmal
  einlernen, überall öffnen. Die Geräte sprechen direkt und verschlüsselt miteinander.

Jeder Teil wird über einen eigenen Block in der YAML eingeschaltet: `admin:`, `guests:`, `sync:`.
Ohne `sync:` verlässt nichts das Gerät, und es wird kein Netzwerk-Port geöffnet.

## Ein Gerät (ohne Sync)

```yaml
external_components:
  - source: github://ShadyMCShadesen/esphome-fingerprint-grow-plus@v0.2.0   # oder @main für den neuesten Stand
    components: [fingerprint_grow_plus]

fingerprint_grow_plus:
  finger_sensor: finger_detected   # binary_sensor am Touch-Pin (Wake-up) des Sensors
  admin:                           # Admin-Finger in Slot 0
```

1. Den Admin-Finger einmal in Slot 0 einlernen (z. B. mit `fingerprint_grow.enroll`).
2. Zweimal auflegen, abheben, 2 s warten: Der nächste freie Slot wird eingelernt.
3. Admin-Finger aus der Tür-Aktion heraushalten (siehe [Admin-Finger](#admin-finger)).

Vollständiges Beispiel: `examples/standalone.yaml`.

## Admin-Finger

```yaml
fingerprint_grow_plus:
  finger_sensor: finger_detected
  admin:
    slots: [0, 1]        # Standard: [0]
```

Auflegen, abheben, wieder auflegen … Die Geste ist fertig, wenn 2 s (`window`) kein Finger auf
dem Sensor lag:

| Admin-Finger | Wirkung |
|---|---|
| 2× | nächsten freien Slot einlernen |
| 3×, dann Finger | dieser Finger wird gelöscht (mit Sync: auf allen Geräten) |
| 4×, dann Finger | dieser Finger wird im selben Slot neu eingelernt |
| 5× (nur mit `guests:`) | Gastfinger einlernen |
| 3 s liegen lassen | `on_hold`: Statusfarbe anzeigen, siehe `health()` |
| 1× (nur mit `access:`) | `on_access`: wie ein normaler Finger, siehe unten |

Zeigt man statt des Zielfingers einen Admin-Finger oder 10 s lang gar keinen Finger, wird die
Aktion abgebrochen.

**Halten** zeigt nur den Status: Solange der Finger aufliegt und bis er `window` lang weg ist,
werden weitere Erkennungen des Admin-Fingers ignoriert. Ein Halten endet also nie in einer Geste
oder in `on_access` (Tür). Kurze Aussetzer des Touch-Signals (< 0,5 s) zählen nicht als Abheben.

**Schutz:** Admin-Slots werden nie als freier Slot vergeben, lassen sich nicht per Geste oder
`fingerprint_grow_plus.delete` löschen und bleiben bei `delete_all` stehen. Entfernen nur mit
`fingerprint_grow_plus.delete_admin` und dem Lösch-Code. Mit Sync auf allen Geräten dieselben
Admin-Slots eintragen.

**Admin-Finger öffnen nichts**, solange man es nicht einstellt. `fingerprint_grow` meldet aber
jeden Treffer, deshalb in der Tür-Aktion ausschließen:

```yaml
fingerprint_grow:
  on_finger_scan_matched:
    - if:
        condition:
          not:
            or:
              - fingerprint_grow_plus.is_admin_slot: !lambda return finger_id;
              - fingerprint_grow_plus.is_admin_busy:
        then:
          - script.execute: tuer_oeffnen
```

**Admin-Finger, der auch öffnet** (`access:`): einmal auflegen löst `on_access` aus, zweimal oder
öfter öffnet das Admin-Menü.

```yaml
fingerprint_grow_plus:
  admin:
    access: delayed      # oder immediate
    on_access:           # Variablen: finger_id, confidence
      - script.execute: tuer_oeffnen
```

- `delayed`: öffnet, sobald klar ist, dass es bei einmal blieb (etwa 2 s nach dem Abheben). Das
  Admin-Menü öffnet nie die Tür.
- `immediate`: öffnet sofort beim ersten Auflegen. Ein zweites Auflegen öffnet dann das Menü,
  die Tür ist aber schon offen.

**LED-Rückmeldung** (optional): Mit `led_feedback:` steuert die Komponente den LED-Ring des
Sensors selbst:

| Moment | LED |
|---|---|
| Admin-Finger 1× / 2× / 3× / 4× / 5× (Vorschau der Aktion) | dauerhaft cyan / lila / rot / gelb / weiß |
| Warten auf den Finger zum Löschen / Neu-Einlernen | blinkt rot / gelb |
| Einlernen (neuer Finger / erneut / Gast) | blinkt lila / gelb / weiß |
| eingelernt, gelöscht | blinkt 1,5 s grün |
| fehlgeschlagen, abgebrochen | blinkt 1,5 s rot |
| Admin-Finger gehalten: Status | 3 s dauerhaft: grün = alles ok, blau = Sync läuft, gelb = Konflikt, rot = Fehler |

Danach geht die LED aus, oder `on_idle` stellt die eigene LED wieder her:

```yaml
fingerprint_grow_plus:
  led_feedback:
    on_idle:
      - script.execute: meine_ruhe_led
```

Ohne `led_feedback:` fasst die Komponente die LED nie an; eigene Rückmeldungen baut man dann mit
den Triggern `on_touch`, `on_gesture`, `on_waiting`, `on_cancelled`, `on_done`, `on_hold`
(im `admin:`-Block) und `on_enroll_started`.

## Gästefinger

```yaml
fingerprint_grow_plus:
  time_id: sntp_time     # Pflicht: das Ablaufdatum braucht die echte Uhrzeit
  guests: "150-179"      # 7 Tage gültig; oder {slots: "150-179", valid_for: 2d}
```

Einlernen mit der Admin-Geste 5× oder `fingerprint_grow_plus.enroll_guest`; verlängern mit
`fingerprint_grow_plus.extend_guest`. Nach Ablauf wird der Finger gelöscht, mit Sync auf allen
Geräten.

## Sync

### Einrichten

1. Schlüssel einmal erzeugen und in die `secrets.yaml` eintragen (für alle Geräte derselbe):

   ```bash
   python3 tools/growplus.py genkey
   ```

   ```yaml
   fingerprint_grow_plus_key: "<Ausgabe>"
   fingerprint_delete_all_code: "<langer Code>"
   ```

2. Auf jedem Gerät den `sync:`-Block ergänzen:

   ```yaml
   fingerprint_grow_plus:
     delete_all_code: !secret fingerprint_delete_all_code
     sync:
       key: !secret fingerprint_grow_plus_key
       peers:                 # alle Geräte, überall dieselbe Liste; das eigene wird übersprungen
         - geraet-1.local
         - geraet-2.local
       dry_run: true          # erster Lauf: nur protokollieren, nichts schreiben
   ```

3. Flashen und die Logs prüfen (`Sync with … finished`, `[dry run] would store slot …`,
   eventuell `CONFLICT in slot …`). Von jedem Sensor ein Backup ziehen, dann `dry_run: false`.

### Regeln

- Ein Finger hat auf allen Geräten **denselben Slot**.
- **Löschen** mit `fingerprint_grow_plus.delete` (oder der 3×-Geste) wirkt auf allen Geräten.
  Fehlt ein Finger nur auf einem Gerät (neuer Sensor, `fingerprint_grow.delete`), wird er von den
  anderen zurückkopiert.
- **Unterschiedliche Finger im selben Slot** werden nie überschrieben, sondern als Konflikt
  gemeldet (`on_conflict`, Sensor `conflict_count`). Auflösen mit
  `fingerprint_grow_plus.resolve_conflict`: `keep_local: true` = dieser Finger gilt überall,
  `keep_local: false` = den Finger der anderen übernehmen.
- **Nur einzelne Slots synchronisieren:** `include_slots` / `exclude_slots` bestimmen, was
  synchronisiert wird. Alle anderen Slots bleiben lokal; Admin-Gesten, Gäste und `delete_all`
  funktionieren dort trotzdem.

  ```yaml
  fingerprint_grow_plus:
    sync:
      key: !secret fingerprint_grow_plus_key
      peers: [geraet-1.local, geraet-2.local]
      exclude_slots: ["180-199"]   # z. B. eigene Finger, die auf jedem Gerät lokal bleiben
      # include_slots: ["0"]       # oder: nur Slot 0 teilen (z. B. den Admin-Finger), Rest lokal
  ```

- **Wann synchronisiert wird:** nach dem Booten (`at_boot`), etwa 2 s nach einer eigenen Änderung
  wie Einlernen oder Löschen (`after_change`), alle 15 min (`interval`) und bei
  `fingerprint_grow_plus.sync`. Ausgefallene Geräte holen alles nach, sobald sie wieder da sind.
- **Automatischen Sync abschalten:**

  ```yaml
  sync:
    interval: never      # kein regelmäßiger Abgleich
    at_boot: false       # nicht nach dem Booten
    after_change: false  # nicht nach Einlernen, Löschen, …
  ```

  Änderungen warten dann auf den nächsten manuellen Sync. Ohne regelmäßigen Abgleich weiß
  `enroll_next_free` nicht sicher, welche Plätze die anderen belegt haben, und kann einen dort
  belegten Platz wählen (das wird dann als Konflikt gemeldet).
- Das Sensor-Passwort wird nie gesetzt oder geändert.

### Erstinbetriebnahme mit bereits eingelernten Fingern

Getrennt eingelernte Finger unterscheiden sich Byte für Byte, und der R503 kann zwei gespeicherte
Templates nicht zuverlässig als „gleicher Finger“ erkennen. Der Sync meldet deshalb Konflikte,
statt zu raten. Einmalig auflösen:

1. Von jedem Sensor ein Backup ziehen.
2. Das Gerät mit der richtigen Zuordnung Finger → Slot als Referenz wählen.
3. **Nur dort** `fingerprint_grow_plus.claim_all_slots` auslösen. Mit `dry_run` zeigen die anderen
   Geräte `[dry run] would REPLACE slot …`.
4. Passt alles, `dry_run: false` setzen.

### Netzwerk

Ein TCP-Port (Standard 18530), in beide Richtungen zwischen allen Geräten.

- **Gleiches Netz:** `.local`-Namen funktionieren.
- **Über VLAN, Router oder VPN:** IP-Adressen oder DNS-Namen verwenden, den Port in der Firewall
  freigeben und am besten feste IP-Adressen vergeben.
- `allowed_networks` beschränkt, aus welchen Netzen Verbindungen angenommen werden.

Details: [network.md](../network.md).

## Backup und Restore

`tools/growplus.py` (Python 3, nur Standardbibliothek) liest und schreibt die Finger eines Geräts
über die Sync-Verbindung. Die Backup-Dateien sind mit dem Schlüssel verschlüsselt.

Vollständige Befehle (`<Schlüssel>` = Wert von `fingerprint_grow_plus_key` aus der `secrets.yaml`):

```bash
# Linux / macOS
python3 tools/growplus.py backup  --host 192.168.10.21 --port 18530 --key '<Schlüssel>' --out geraet-1.fgpb --include-local
python3 tools/growplus.py inspect --key '<Schlüssel>' --in geraet-1.fgpb
python3 tools/growplus.py restore --host 192.168.10.21 --port 18530 --key '<Schlüssel>' --in geraet-1.fgpb
```

```powershell
# Windows (PowerShell)
py tools\growplus.py backup  --host 192.168.10.21 --port 18530 --key '<Schlüssel>' --out geraet-1.fgpb --include-local
py tools\growplus.py inspect --key '<Schlüssel>' --in geraet-1.fgpb
py tools\growplus.py restore --host 192.168.10.21 --port 18530 --key '<Schlüssel>' --in geraet-1.fgpb
```

- `--host`: IP-Adresse oder DNS-Name des Geräts (`.local` nur, wenn der PC mDNS auflöst);
  `--port` nur nötig, wenn das Gerät nicht 18530 verwendet.
- `--key`: statt des Schlüssels in der Befehlszeile geht auch `--key-file <datei>` oder die
  Umgebungsvariable `GROWPLUS_KEY`.
- `--out`: Name der Backup-Datei (Pflicht).
- `--include-local` (oder `--all`): sichert zusätzlich die Slots, die nicht synchronisiert werden
  (außerhalb von `include_slots`). Die gibt das Gerät nur heraus, nachdem am Gerät
  `fingerprint_grow_plus.allow_full_backup` ausgelöst wurde (5 min). Ohne die Option werden nur die
  synchronisierten Slots gesichert.
- `--slots 0,2,10-19` beschränkt Backup oder Restore auf einzelne Slots.
- `restore` braucht vorher `fingerprint_grow_plus.allow_restore` am Gerät. Synchronisierte Slots
  werden dabei auf allen Geräten ersetzt, lokale Slots bleiben lokal.
- Das Tool verbindet sich über den Sync-Port, das Gerät braucht also einen `sync:`-Block. Für ein
  einzelnes Gerät reicht ein `sync:`-Block nur mit `key` (ohne `peers`): Er öffnet den Port für
  das Tool und synchronisiert mit niemandem.

Backup-Dateien enthalten biometrische Daten: nie in ein Repository legen.

## Entitäten

Der Text-Sensor `occupied_slots` zeigt die belegten Slots als Bereiche, z. B. `0, 2-5, 17, 150-151`:

```yaml
text_sensor:
  - platform: fingerprint_grow_plus
    occupied_slots:
      name: Belegte Plätze
```

Für den Sync gibt es außerdem `unreachable_peers` (Text: welche Geräte im letzten Abgleich nicht
erreicht wurden und warum, `-` wenn alle erreicht wurden) und `versions_left` (wie viele
synchronisierte Änderungen bis zum nötigen Versions-Reset übrig sind, maximal 16383; unter 1000
warnt das Log). Weitere (Sync-Status, Konflikte, erreichbare Geräte, letzter Sync) und alle Optionen, Aktionen
und Trigger: siehe [README](../../README.md#configuration-reference).
