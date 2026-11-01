#!/usr/bin/env python3
"""Check the Home Assistant package against the entities the firmware publishes.

A dashboard that references `sensor.filament_dryer_remaining` when the firmware
calls it `remaining_weight` fails silently: Home Assistant shows "Entity not
available" and nothing explains why. This works out the entity ids Home
Assistant will derive from the discovery messages in HaMqtt.cpp and compares
them against every id the YAML mentions.

It also parses the YAML and checks for the automation syntax Home Assistant
deprecated in 2024.10, which fails in the other direction: the automation loads
and then never fires.

    python3 tools/check_ha_package.py

Needs PyYAML. Jinja2 is used to check the templates if it happens to be
installed, and skipped if not. Exits non-zero on a mismatch, so it can guard a
commit.
"""

import re
import sys
from pathlib import Path

import yaml

try:
    import jinja2
except ImportError:
    jinja2 = None

ROOT = Path(__file__).resolve().parent.parent
HA_MQTT = ROOT / "firmware" / "OpenPrintTagDryer" / "src" / "HaMqtt.cpp"
CONFIG = ROOT / "firmware" / "OpenPrintTagDryer" / "config.h"
YAML_FILES = [
    ROOT / "homeassistant" / "package.yaml",
    ROOT / "homeassistant" / "dashboard.yaml",
]
BLUEPRINTS = sorted(
    (ROOT / "homeassistant" / "blueprints").rglob("*.yaml")
)

# Entities the package defines itself, plus the helpers. These have no discovery
# message behind them, so they are legitimate references.
LOCAL_ENTITIES = {
    "sensor.filament_dryer_spool",
    "sensor.filament_dryer_dry_cycle_finishes",
    "binary_sensor.filament_dryer_filament_low",
    "input_number.filament_dryer_low_threshold",
    "input_boolean.filament_dryer_low_alerted",
    "timer.filament_dryer_dry_cycle",
    "script.filament_dryer_start_dry_cycle",
    # Documented as optional and deliberately absent on most installs.
    "switch.filament_dryer_plug",
    "climate.filament_dryer",
}


def slugify(text):
    """Home Assistant's entity id slug, near enough for these names."""
    text = text.lower()
    text = text.replace("°", "")
    text = re.sub(r"[^a-z0-9]+", "_", text)
    return text.strip("_")


def device_name():
    match = re.search(r'#define DEVICE_DEFAULT_NAME\s+"([^"]+)"', CONFIG.read_text())
    if not match:
        raise SystemExit("DEVICE_DEFAULT_NAME not found in config.h")
    return match.group(1)


def firmware_entities():
    """Entity ids Home Assistant derives from the discovery table."""
    source = HA_MQTT.read_text()
    start = source.index("const HaMqtt::Entity HaMqtt::kEntities[] = {")
    end = source.index("\n};", start)
    table = source[start:end]

    # Each entry starts with {"component", "object_id", "Name", ... and the
    # entries are wrapped across lines, so strings are matched in order.
    entries = re.findall(r'\{"(\w+)",\s*"([\w]+)",\s*"([^"]+)"', table)
    if len(entries) < 20:
        raise SystemExit(f"only parsed {len(entries)} entities; the table shape changed")

    prefix = slugify(device_name())
    result = {}
    for component, object_id, name in entries:
        result[f"{component}.{prefix}_{slugify(name)}"] = object_id
    return result


def referenced_entities():
    """Every `domain.filament_dryer_*` id the YAML mentions."""
    found = {}
    for path in YAML_FILES:
        text = path.read_text()
        for match in re.finditer(
            r"\b(sensor|binary_sensor|switch|button|number|text|timer|script|"
            r"input_number|input_boolean|climate)\.(filament_dryer[\w]*)\b",
            text,
        ):
            entity = f"{match.group(1)}.{match.group(2)}"
            found.setdefault(entity, set()).add(path.name)
    return found


class HaLoader(yaml.SafeLoader):
    """Tolerates the tags Home Assistant adds to plain YAML."""


def _passthrough(loader, node):
    if isinstance(node, yaml.ScalarNode):
        return loader.construct_scalar(node)
    if isinstance(node, yaml.SequenceNode):
        return loader.construct_sequence(node, deep=True)
    return loader.construct_mapping(node, deep=True)


for tag in ("!input", "!include", "!include_dir_named", "!secret", "!env_var"):
    HaLoader.add_constructor(tag, _passthrough)


def walk(node):
    """Every mapping in a document, depth first."""
    if isinstance(node, dict):
        yield node
        for value in node.values():
            yield from walk(value)
    elif isinstance(node, list):
        for item in node:
            yield from walk(item)


def parse_yaml(problems):
    """Parses every file and returns the documents that loaded."""
    documents = {}
    for path in YAML_FILES + BLUEPRINTS:
        try:
            documents[path] = yaml.load(path.read_text(), Loader=HaLoader)
        except yaml.YAMLError as error:
            problems.append(f"{path.name}: will not parse: {error}")
    return documents


def check_syntax(documents, problems):
    """Looks for the automation syntax that was renamed in 2024.10.

    An automation using the old keys loads without complaint and then never
    fires, which is the worst way for a config error to present itself.
    """
    for path, document in documents.items():
        automations = []
        if isinstance(document, dict) and "automation" in document:
            automations = document["automation"]
        elif isinstance(document, dict) and "blueprint" in document:
            automations = [document]

        for automation in automations:
            if not isinstance(automation, dict):
                continue
            name = automation.get("alias") or automation.get("id") or path.name
            for old, new in (("trigger", "triggers"), ("condition", "conditions"),
                             ("action", "actions")):
                if old in automation and new not in automation:
                    problems.append(
                        f"{path.name}: {name}: top-level '{old}:' was renamed "
                        f"to '{new}:' in 2024.10")

        for mapping in walk(document):
            # `platform:` inside a trigger, as opposed to an MQTT platform block.
            if "platform" in mapping and (
                    "entity_id" in mapping or "topic" in mapping
                    or "event_type" in mapping or "at" in mapping):
                problems.append(
                    f"{path.name}: trigger uses 'platform: "
                    f"{mapping['platform']}'; that key is now 'trigger:'")
            if "service" in mapping and isinstance(mapping.get("service"), str):
                problems.append(
                    f"{path.name}: action calls 'service: {mapping['service']}'; "
                    "that key is now 'action:'")


def check_templates(documents, problems):
    """Compiles every Jinja template so a typo is found here, not at runtime."""
    if jinja2 is None:
        print("\njinja2 not installed: template syntax not checked")
        return 0

    environment = jinja2.Environment()
    checked = 0
    for path, document in documents.items():
        for mapping in walk(document):
            for key, value in mapping.items():
                if not isinstance(value, str) or "{" not in value:
                    continue
                checked += 1
                try:
                    environment.parse(value)
                except jinja2.TemplateSyntaxError as error:
                    problems.append(
                        f"{path.name}: template under '{key}': {error.message}")
    return checked


def main():
    published = firmware_entities()
    referenced = referenced_entities()
    problems = []

    documents = parse_yaml(problems)
    check_syntax(documents, problems)
    templates = check_templates(documents, problems)

    print(f"device name: {device_name()!r} -> prefix {slugify(device_name())!r}")
    print(f"{len(published)} entities published by the firmware")
    print(f"{len(referenced)} distinct ids referenced by the YAML")
    print(f"{len(documents)} YAML files parsed, {templates} templates checked")

    unknown = {
        entity: files
        for entity, files in referenced.items()
        if entity not in published and entity not in LOCAL_ENTITIES
    }
    for entity, files in sorted(unknown.items()):
        problems.append(
            f"{', '.join(sorted(files))}: {entity} is referenced but never "
            "published")

    # Not an error, but worth seeing: entities nothing uses.
    unused = sorted(set(published) - set(referenced))
    if unused:
        print("\npublished but not on the dashboard (fine, just noting):")
        for entity in unused:
            print(f"  {entity}")

    if problems:
        print()
        for problem in problems:
            print(f"FAIL {problem}")
        print(f"\n{len(problems)} problem(s)")
        return 1
    print("\nOK: every reference resolves and every file parses")
    return 0


if __name__ == "__main__":
    sys.exit(main())
