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
  - source: github://ShadyMCShadesen/esphome-fingerprint-grow-plus@main
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

**LED-Rückmeldung** kommt aus den Triggern `on_touch`, `on_gesture`, `on_waiting`, `on_cancelled`,
`on_done`, `on_hold` (im `admin:`-Block) und `on_enroll_started`. Die Komponente steuert die LEDs
nie selbst.

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
  funktionieren dort trotzdem. Beispiel: ein Gerät an einem anderen Standort teilt nur den
  Admin-Finger 99:

  ```yaml
  fingerprint_grow_plus:
    admin:
      slots: [99]
    sync:
      key: !secret fingerprint_grow_plus_key
      peers: [192.168.10.21, 192.168.10.22]   # über VPN/Router kein mDNS: IP oder DNS-Name
      include_slots: ["99"]
  ```

- Ausgefallene Geräte holen alles nach, sobald sie wieder da sind; verglichen wird nach jeder
  Änderung und alle 15 min (`interval`).
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

Linux / macOS:

```bash
export GROWPLUS_KEY='<Schlüssel>'
python3 tools/growplus.py backup  --host <geraet>.local
python3 tools/growplus.py backup  --host <geraet>.local --include-local   # auch nicht synchronisierte
python3 tools/growplus.py inspect --in <geraet>-<datum>_<uhrzeit>.fgpb
# am Gerät zuerst fingerprint_grow_plus.allow_restore auslösen, dann:
python3 tools/growplus.py restore --host <geraet>.local --in <datei>.fgpb
```

Windows (PowerShell):

```powershell
$env:GROWPLUS_KEY = '<Schlüssel>'
py tools\growplus.py backup  --host <geraet>.local
py tools\growplus.py backup  --host <geraet>.local --include-local
py tools\growplus.py inspect --in <geraet>-<datum>_<uhrzeit>.fgpb
py tools\growplus.py restore --host <geraet>.local --in <datei>.fgpb
```

- `backup` sichert die synchronisierten Slots. Mit `--include-local` (oder `--all`) zusätzlich die
  Slots, die nicht synchronisiert werden (außerhalb von `include_slots`). Die gibt das Gerät nur
  heraus, nachdem am Gerät `fingerprint_grow_plus.allow_full_backup` ausgelöst wurde (5 min).
- `restore` braucht vorher `fingerprint_grow_plus.allow_restore`. Synchronisierte Slots werden
  dabei auf allen Geräten ersetzt, lokale Slots bleiben lokal.
- Das Tool verbindet sich über den Sync-Port, das Gerät braucht also einen `sync:`-Block. Für ein
  einzelnes Gerät reicht ein `sync:`-Block nur mit `key` (ohne `peers`): Er öffnet den Port für
  das Tool und synchronisiert mit niemandem.

Ohne `--out` heißt die Datei `<geraet>-<datum>_<uhrzeit>.fgpb`. Statt der Umgebungsvariable geht
auf jedem System auch `--key-file <datei>`. Backup-Dateien enthalten biometrische Daten: nie in
ein Repository legen.

## Entitäten

Der Text-Sensor `occupied_slots` zeigt die belegten Slots als Bereiche, z. B. `0, 2-5, 99, 150-151`:

```yaml
text_sensor:
  - platform: fingerprint_grow_plus
    occupied_slots:
      name: Belegte Plätze
```

Weitere (Sync-Status, Konflikte, erreichbare Geräte, letzter Sync) und alle Optionen, Aktionen
und Trigger: siehe [README](../../README.md#configuration-reference).
