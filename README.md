# esphome-fingerprint-sync

> ⚠️ **Status: in Entwicklung – noch nicht einsatzbereit.**

Einen Fingerabdruck **einmal** an einem beliebigen ESPHome-Gerät mit GROW-R503-Sensor
anlernen und danach automatisch (oder per Knopfdruck in Home Assistant) auf **alle**
anderen Geräte übertragen – im gleichen Slot (`finger_id`), damit Automationen überall
identisch funktionieren.

Das Projekt besteht aus zwei Teilen:

| Teil | Pfad | Zweck |
|------|------|-------|
| ESPHome External Component | `components/fingerprint_sync/` | Template-Export (`LoadChar` + `UpChar`) und -Import (`DownChar` + `Store`) am R503 |
| Home-Assistant-Integration | `custom_components/fingerprint_sync/` | Zentrale, verschlüsselte Template-Datenbank, Services, Auto-Sync |

## Inhalt

- [Voraussetzungen](#voraussetzungen)
- [Installation ESPHome](#installation-esphome)
- [Installation Home Assistant (HACS)](#installation-home-assistant-hacs)
- [Services](#services)
- [Speicherverbrauch (ESP8266)](#speicherverbrauch-esp8266)
- [Sicherheit & Datenschutz](#sicherheit--datenschutz)
- [Troubleshooting](#troubleshooting)
- [Lizenz](#lizenz)

## Voraussetzungen

- ESPHome (aktuelle Version) mit `fingerprint_grow` und GROW R503 an UART (57600 Baud)
- Native API **mit Verschlüsselung** (`api: encryption: key`)
- Home Assistant (getestet mit Home Assistant OS)

## Installation ESPHome

_Folgt._ Geplante Einbindung:

```yaml
external_components:
  - source: github://ShadyMCShadesen/esphome-fingerprint-sync@main
    components: [fingerprint_sync]

packages:
  fingerprint_sync: github://ShadyMCShadesen/esphome-fingerprint-sync/packages/fingerprint_sync.yaml@main
```

## Installation Home Assistant (HACS)

_Folgt._

## Services

_Folgt._ Geplant: `export_from_device`, `push_to_device`, `sync_all`,
`delete_everywhere`, `list_fingerprints`.

## Speicherverbrauch (ESP8266)

_Folgt – wird auf echter Hardware gemessen._

## Sicherheit & Datenschutz

Fingerabdruck-Templates sind **biometrische Daten**. Dieses Projekt

- speichert Templates in Home Assistant ausschließlich verschlüsselt,
- gibt Templates niemals im Klartext in Logs aus,
- setzt eine verschlüsselte ESPHome-API voraus.

Bitte niemals Templates, `secrets.yaml` oder Sensor-Passwörter committen.

## Troubleshooting

_Folgt._

## Lizenz

[MIT](LICENSE)
