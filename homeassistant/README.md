# Home Assistant

The device speaks MQTT discovery, so the entities appear on their own. What is
in this folder is everything discovery cannot do for you: a dashboard, the
automations worth having, and a blueprint for running a dry cycle off the tag.

| File | What it is | How to install |
| --- | --- | --- |
| `package.yaml` | Helpers, template entities and seven automations | Copy to `config/packages/filament_dryer.yaml` |
| `dashboard.yaml` | One dashboard view, core cards only | Paste into a dashboard's raw YAML editor |
| `blueprints/automation/openprinttag/dry_cycle_from_tag.yaml` | Dry a spool the way its tag asks | Copy to `config/blueprints/automation/openprinttag/` |

Requires Home Assistant 2024.10 or newer: the automations use the plural
`triggers:`/`actions:` syntax introduced there.

## 1 Broker

Discovery needs an MQTT broker. The Mosquitto add-on is the usual choice:
**Settings > Add-ons > Add-on store > Mosquitto broker**, install, start, then
add the MQTT integration when Home Assistant offers it.

Make a user for the device — Mosquitto authenticates against Home Assistant
users, so **Settings > People > Users > Add user** with a name like `dryer` is
enough. Put those credentials into the device on its **Settings** page (or into
`secrets.h` before flashing) along with the broker's IP.

Within a few seconds of the device connecting, a new device called **Filament
dryer** appears under the MQTT integration with 38 entities. If it does not, jump
to [troubleshooting](#6-troubleshooting).

## 2 Package

```bash
# on the Home Assistant host
mkdir -p /config/packages
cp package.yaml /config/packages/filament_dryer.yaml
```

and in `configuration.yaml`:

```yaml
homeassistant:
  packages: !include_dir_named packages
```

Check the configuration (**Developer tools > YAML > Check configuration**), then
restart.

Two things to set afterwards:

- **Low filament threshold** starts at its minimum of 25 g, which is too low to
  be useful. 150 g is a sensible value: enough filament left to finish most
  prints, little enough that the warning means something.
- **Nothing here controls the dryer by default, and most dryers cannot be
  controlled at all.** The Creality Space Pi has no network interface, and no
  plug can turn its temperature dial. So the dry-cycle script reads the tag,
  starts a timer, and tells you what to set; the dashboard button is named for
  that rather than for commanding anything.
- A smart plug is still worth wiring up, as `dryer_switch` in the
  `filament_dryer_start_dry_cycle` script. The temperature stays manual, but the
  cycle then starts the dryer, and — the part that actually earns its keep — the
  finished-cycle automation switches it off hours later whether or not you are
  home. Set `dryer_climate` too if you own a dryer that takes temperature
  orders. Left alone, both steps are skipped silently.

### What the package adds

| Entity | Why |
| --- | --- |
| `sensor.filament_dryer_spool` | One line naming what is in the dryer, for cards and notifications |
| `binary_sensor.filament_dryer_filament_low` | The threshold comparison in one place, so the alert and the dashboard cannot disagree |
| `sensor.filament_dryer_dry_cycle_finishes` | The running cycle's end as a timestamp, so cards can show it as a relative time |
| `input_number.filament_dryer_low_threshold` | The threshold |
| `input_boolean.filament_dryer_low_alerted` | Latch, so the low-filament alert fires once per spool rather than every time the lid moves |
| `timer.filament_dryer_dry_cycle` | The running cycle |
| `script.filament_dryer_start_dry_cycle` | Times a cycle to the tag's temperature and time, and says what to dial in |

### The automations

| Automation | Fires when | Does |
| --- | --- | --- |
| Announce a new spool | The tag UID changes | Waits for the weight to settle, then notifies what went in and how much is on it |
| Low filament alert | Remaining below the threshold for 5 minutes, weight stable | Notifies once, then latches |
| Rearm the low filament alert | Remaining goes 100 g clear of the threshold | Clears the latch |
| Dry cycle finished | The timer ends | Switches the plug off if there is one, notifies |
| Tag disagrees with the scale | The problem sensor stays on for 15 minutes | Notifies with both figures and points at the tare check |
| Hardware problem | Reader or converter offline for 10 minutes | Notifies with the device's own error string |
| Spoolman's tare disagrees with the tag | A tag has been in the dryer two minutes, or either figure changes | Notifies which copy of the empty spool weight is wrong |

The last one is the only thing here that wants a HACS integration,
[spoolman-homeassistant](https://github.com/Disane87/spoolman-homeassistant). It
finds the Spoolman spool by matching the dryer's tag UID against the `nfc_uid`
extra field, so it needs no spool ids configured, and without the integration
installed it simply never fires. It is worth having because Spoolman's copy of
the empty spool weight is what its used-weight figures are computed from, and a
copy that is too light is the one mistake that corrupts data quietly — see
[../docs/spoolman.md](../docs/spoolman.md).

All of them notify through `persistent_notification.create`, which works on every
install without configuration. For phone notifications, replace
`persistent_notification.create` with your `notify.mobile_app_*` action and drop
the `notification_id` line.

## 3 Dashboard

Open a dashboard, **Edit > three dots > Raw configuration editor**, and paste the
`views:` entry from `dashboard.yaml`. It uses only core cards: sections, gauge,
glance, history graphs, and a conditional card that stays hidden until something
is actually wrong.

## 4 Blueprint

The blueprint is the automated version of the drying advice on the tag. Every
input except the three sensors is optional, so it works whether or not the dryer
can be controlled:

- With no switch and no thermostat, it tells you what to set and when it is done.
- With a smart plug, it switches the dryer on, waits out the tag's recommended
  time, and switches it off.
- With a thermostat, it sets the temperature too.

It caps whatever the tag asks for at a configurable number of hours, because a
mistyped drying time on a tag should not be able to run a heater for two days.

## 5 Entity reference

Entity ids come from the device name, so all of them start with
`filament_dryer_`. **Renaming the device in Home Assistant renames the entities
with it**, at which point the YAML here needs the same rename:

```bash
sed -i 's/filament_dryer/your_new_name/g' package.yaml dashboard.yaml
```

`tools/check_ha_package.py` in this repository verifies that every id the YAML
mentions is one the firmware actually publishes, which is worth running after any
such rename:

```bash
python3 tools/check_ha_package.py
```

### The spool

| Entity | Unit | Notes |
| --- | --- | --- |
| `sensor.filament_dryer_remaining_filament` | g | Scale minus tare minus the spool's own weight |
| `sensor.filament_dryer_remaining` | % | Of the tag's reference full weight |
| `sensor.filament_dryer_remaining_length` | m | From the tag's length, or from density and diameter |
| `sensor.filament_dryer_consumed_filament` | g | What gets written back to the tag |
| `sensor.filament_dryer_gross_weight` | g | Everything on the platform, dryer included |
| `sensor.filament_dryer_brand` | | |
| `sensor.filament_dryer_material_type` | | `PLA`, `PETG`, ... from the spec's enum |
| `sensor.filament_dryer_material_name` | | The full name, colour included |
| `sensor.filament_dryer_colour` | | `#RRGGBB` |
| `sensor.filament_dryer_spool_id` | | The Prusament spool ID from the QR code |
| `sensor.filament_dryer_print_temperature` | | The tag's range, as text |
| `sensor.filament_dryer_recommended_drying_temperature` | °C | What the automations act on |
| `sensor.filament_dryer_recommended_drying_time` | min | |
| `binary_sensor.filament_dryer_tag_present` | | On when a tag is being read *and* decoded |

An entity reads `unknown` when the tag does not carry that field, which is
deliberate: an honest gap beats a fabricated number.

### Diagnostics

| Entity | Notes |
| --- | --- |
| `sensor.filament_dryer_tag_uid` | The tag's own UID; the stable identity of a spool |
| `sensor.filament_dryer_consumed_weight_on_tag` | What the tag says, as opposed to what the scale says |
| `sensor.filament_dryer_reference_full_weight` | The per-spool measured weight Prusa published |
| `sensor.filament_dryer_empty_spool_weight` | |
| `sensor.filament_dryer_load_cell_temperature` | Drives the drift compensation |
| `sensor.filament_dryer_last_tag_read` / `_last_tag_write` | Seconds since |
| `sensor.filament_dryer_write_back_status` | Why the last write happened, or why it did not |
| `sensor.filament_dryer_tag_blocks_written` | Lifetime count, against the tag's 100k endurance |
| `sensor.filament_dryer_error`, `_last_command_result` | |
| `binary_sensor.filament_dryer_weight_stable` | |
| `binary_sensor.filament_dryer_reader_online`, `_scale_online` | |
| `binary_sensor.filament_dryer_tag_disagrees_with_scale` | |

### Controls

| Entity | Notes |
| --- | --- |
| `button.filament_dryer_tare_empty_dryer` | With the dryer empty and its lid shut |
| `number.filament_dryer_calibration_mass` + `button.filament_dryer_calibrate_with_mass` | |
| `button.filament_dryer_write_tag_now` | Ignores the interval, not the safety checks |
| `switch.filament_dryer_automatic_write_back` | |
| `switch.filament_dryer_tag_polling` | Off stops the RF field entirely |
| `button.filament_dryer_re_read_tag` | |
| `button.filament_dryer_wipe_auxiliary_region` | Resets consumed weight and workgroup on the tag |
| `text.filament_dryer_storage_location` | Written into the tag's aux region, 8 characters |

## 6 Troubleshooting

**No device appears.** Check the device's own **Status** page: it reports whether
the broker is connected and, if not, the PubSubClient error. State 5 is bad
credentials; state -2 is no route to the broker.

**The device appears but every entity is unavailable.** Availability is a retained
`online`/`offline` on `openprinttag_dryer/<device-id>/status`. If that says
`offline`, the device is not connected. If it says nothing at all, the broker
dropped its retained messages: restart the device, which republishes everything.

**Entities exist but read `unknown`.** Either no state document has arrived yet —
the device publishes on every change and otherwise every 15 seconds, so waiting
half a minute settles it — or the tag genuinely does not carry that field. The
**Status** page's publish count distinguishes the two.

**Values look stale after a Home Assistant restart.** They should not: both the
discovery messages and the state document are published retained, so a restarting
Home Assistant is brought fully up to date by the broker without the device doing
anything. If they are stale anyway, the broker is not keeping retained messages,
which for Mosquitto means its persistence is off.

**Everything is duplicated with a `_2` suffix.** Two devices with the same id, or
the same device after its efuse MAC appeared to change. Delete the MQTT device in
Home Assistant and let it rediscover.

### Talking to it without discovery

The device is plain MQTT underneath, so nothing stops you using it directly. The
device id is `dryer_` plus three bytes of the efuse MAC, shown on the portal's
status page and in the serial banner.

| Topic | Direction | Payload |
| --- | --- | --- |
| `openprinttag_dryer/<id>/status` | out, retained | `online` / `offline` |
| `openprinttag_dryer/<id>/state` | out, retained | One JSON document with every value |
| `openprinttag_dryer/<id>/cmd/tare` | in | `PRESS` |
| `openprinttag_dryer/<id>/cmd/calibrate` | in | `PRESS` |
| `openprinttag_dryer/<id>/cmd/calibration_mass` | in | grams |
| `openprinttag_dryer/<id>/cmd/write_now` | in | `PRESS` |
| `openprinttag_dryer/<id>/cmd/auto_writeback` | in | `ON` / `OFF` |
| `openprinttag_dryer/<id>/cmd/poll_enabled` | in | `ON` / `OFF` |
| `openprinttag_dryer/<id>/cmd/refresh` | in | `PRESS` |
| `openprinttag_dryer/<id>/cmd/wipe_aux` | in | `PRESS` |
| `openprinttag_dryer/<id>/cmd/storage_location` | in | up to 8 characters |

The state document's keys are the short forms the discovery templates use —
`remaining_g`, `remaining_pct`, `remaining_m`, `consumed_g`, `gross_g`,
`tag_consumed_g`, `reference_g`, `empty_g`, `brand`, `material_type`,
`material_name`, `color`, `spool_id`, `print_temp`, `drying_c`, `drying_min`,
`tag_uid`, `cell_c`, `last_read_s`, `last_write_s`, `blocks_written`,
`write_status`, `last_command`, `error`, `storage_location`, `calibration_mass`,
`tag_present`, `stable`, `reader_online`, `scale_online`, `disagrees`,
`auto_writeback`, `polling`. Keys are omitted rather than sent empty when the
underlying value is unknown.

A one-line way to watch it:

```bash
mosquitto_sub -h <broker> -u dryer -P <password> -v -t 'openprinttag_dryer/#'
```
