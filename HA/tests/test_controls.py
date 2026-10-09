"""Exercise HA control logic with small API stubs, without installing HA."""

import asyncio
from importlib import import_module
from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch


COMPONENT_DIR = Path(__file__).resolve().parents[1] / "custom_components/fusionedge"


class Entity:
    async def async_added_to_hass(self) -> None:
        pass

    def async_on_remove(self, callback) -> None:
        pass

    def async_write_ha_state(self) -> None:
        pass


class SelectEntity(Entity):
    pass


class SwitchEntity(Entity):
    pass


class LightEntity(Entity):
    pass


class TextEntity(Entity):
    pass


def module(name: str, **attributes):
    item = ModuleType(name)
    item.__dict__.update(attributes)
    return item


class FusionEdgeControlsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.callbacks = {}
        self.published = []

        async def subscribe(hass, topic, callback):
            self.callbacks[topic] = callback
            return lambda: None

        async def publish(hass, topic, payload, qos, retain):
            self.published.append((topic, payload, qos, retain))

        mqtt = module(
            "homeassistant.components.mqtt",
            async_subscribe=subscribe,
            async_publish=publish,
        )
        components = module("homeassistant.components", mqtt=mqtt)
        package = module("fusionedge")
        package.__path__ = [str(COMPONENT_DIR)]
        homeassistant = module("homeassistant", components=components)
        stubs = {
            "fusionedge": package,
            "homeassistant": homeassistant,
            "homeassistant.components": components,
            "homeassistant.components.mqtt": mqtt,
            "homeassistant.components.select": module(
                "homeassistant.components.select", SelectEntity=SelectEntity
            ),
            "homeassistant.components.switch": module(
                "homeassistant.components.switch", SwitchEntity=SwitchEntity
            ),
            "homeassistant.components.text": module(
                "homeassistant.components.text", TextEntity=TextEntity
            ),
            "homeassistant.components.light": module(
                "homeassistant.components.light",
                ATTR_EFFECT="effect",
                ColorMode=SimpleNamespace(ONOFF="onoff"),
                LightEntity=LightEntity,
                LightEntityFeature=SimpleNamespace(EFFECT=4),
            ),
            "homeassistant.config_entries": module(
                "homeassistant.config_entries", ConfigEntry=object
            ),
            "homeassistant.core": module(
                "homeassistant.core", HomeAssistant=object, callback=lambda value: value
            ),
            "homeassistant.exceptions": module(
                "homeassistant.exceptions", HomeAssistantError=RuntimeError
            ),
            "homeassistant.helpers": module("homeassistant.helpers"),
            "homeassistant.helpers.device_registry": module(
                "homeassistant.helpers.device_registry", DeviceInfo=dict
            ),
            "homeassistant.helpers.entity_platform": module(
                "homeassistant.helpers.entity_platform", AddEntitiesCallback=object
            ),
        }
        self.modules_patch = patch.dict(sys.modules, stubs)
        self.modules_patch.start()
        self.addCleanup(self.modules_patch.stop)
        self.bt_select = import_module("fusionedge.select")
        self.tts_switch = import_module("fusionedge.switch")
        self.led_light = import_module("fusionedge.light")
        self.timer_text = import_module("fusionedge.text")
        self.entry = SimpleNamespace(
            data={
                "device_id": "94A990D248B4",
                "root_topic": "fusionedge/radio1/",
                "sw_version": "2.0.0",
            },
            title="FusionEdge",
        )

    def _add(self, entity):
        entity.hass = object()
        asyncio.run(entity.async_added_to_hass())
        self.callbacks["fusionedge/radio1/availability"](
            SimpleNamespace(payload="online")
        )
        return entity

    def _state(self, payload: str) -> None:
        self.callbacks["fusionedge/radio1/state"](
            SimpleNamespace(payload=payload)
        )

    def test_bluetooth_route_waits_for_source_ready_and_confirmed_state(self) -> None:
        entity = self._add(self.bt_select.FusionEdgeBluetoothOutput(self.entry))
        self._state('{"bt_role":"source","bt_ready":0,"bt_output":0}')
        self.assertFalse(entity.available)
        with self.assertRaises(RuntimeError):
            asyncio.run(entity.async_select_option("Local only"))
        self._state('{"bt_role":"source","bt_ready":1,"bt_output":0}')
        self.assertEqual(entity.current_option, "Bluetooth only")
        asyncio.run(entity.async_select_option("Local only"))
        self.assertEqual(entity.current_option, "Bluetooth only")
        self.assertEqual(
            self.published[-1],
            ("fusionedge/radio1/command", "btoutput 2", 0, False),
        )

    def test_tts_switch_uses_reported_state(self) -> None:
        entity = self._add(self.tts_switch.FusionEdgeClockTts(self.entry))
        self._state('{"clock_tts":0}')
        self.assertIs(entity.is_on, False)
        asyncio.run(entity.async_turn_on())
        self.assertIs(entity.is_on, False)
        self.assertEqual(self.published[-1][1], "clocktts 1")

    def test_tts_no_stream_requires_firmware_state_and_uses_existing_command(self) -> None:
        entity = self._add(self.tts_switch.FusionEdgeClockTtsNoStream(self.entry))
        self.assertFalse(entity.available)
        with self.assertRaises(RuntimeError):
            asyncio.run(entity.async_turn_on())
        self._state('{"clock_tts_no_stream":0}')
        self.assertTrue(entity.available)
        self.assertIs(entity.is_on, False)
        asyncio.run(entity.async_turn_on())
        self.assertEqual(self.published[-1][1], "clockttsnostream 1")
        self._state('{"clock_tts_no_stream":1}')
        self.assertIs(entity.is_on, True)
        asyncio.run(entity.async_turn_off())
        self.assertEqual(self.published[-1][1], "clockttsnostream 0")

    def test_led_screensaver_requires_firmware_state_and_uses_existing_command(self) -> None:
        entity = self._add(self.tts_switch.FusionEdgeLedStripScreensaver(self.entry))
        self.assertFalse(entity.available)
        with self.assertRaises(RuntimeError):
            asyncio.run(entity.async_turn_off())
        self._state('{"ledstrip_screensaver":1}')
        self.assertTrue(entity.available)
        self.assertIs(entity.is_on, True)
        asyncio.run(entity.async_turn_off())
        self.assertEqual(self.published[-1][1], "lsssena 0")
        self._state('{"ledstrip_screensaver":0}')
        asyncio.run(entity.async_turn_on())
        self.assertEqual(self.published[-1][1], "lsssena 1")

    def test_led_light_sets_effect_and_power(self) -> None:
        entity = self._add(self.led_light.FusionEdgeLedStrip(self.entry))
        self._state('{"ledstrip_enabled":0,"ledstrip_effect":0}')
        self.assertIs(entity.is_on, False)
        asyncio.run(entity.async_turn_on(effect="Twinkle"))
        self.assertEqual(
            [item[1] for item in self.published], ["lsmodel 2", "lsenabled 1"]
        )
        self._state('{"ledstrip_enabled":1,"ledstrip_effect":2}')
        self.assertEqual(entity.effect, "Twinkle")
        asyncio.run(entity.async_turn_off())
        self.assertEqual(self.published[-1][1], "lsenabled 0")

    def test_timer_uses_confirmed_value_and_off_disables_one_event(self) -> None:
        entity = self._add(self.timer_text.FusionEdgeAutoTimer(self.entry, "weekday_start"))
        self.assertFalse(entity.available)
        with self.assertRaises(RuntimeError):
            asyncio.run(entity.async_set_value("07:30"))
        self._state('{"timer_weekday_start":""}')
        self.assertTrue(entity.available)
        self.assertEqual(entity.native_value, "off")
        asyncio.run(entity.async_set_value("07:30"))
        self.assertEqual(self.published[-1][1], "autotimer weekday_start 07:30")
        self.assertEqual(entity.native_value, "off")
        self._state('{"timer_weekday_start":"07:30"}')
        self.assertEqual(entity.native_value, "07:30")
        asyncio.run(entity.async_set_value("off"))
        self.assertEqual(self.published[-1][1], "autotimer weekday_start off")
        with self.assertRaises(RuntimeError):
            asyncio.run(entity.async_set_value("24:00"))

    def test_timer_fields_have_distinct_commands(self) -> None:
        for field in ("weekday_stop", "weekend_start", "weekend_stop"):
            entity = self._add(self.timer_text.FusionEdgeAutoTimer(self.entry, field))
            self._state('{"timer_' + field + '":"12:00"}')
            asyncio.run(entity.async_set_value("13:15"))
            self.assertEqual(self.published[-1][1], f"autotimer {field} 13:15")


if __name__ == "__main__":
    unittest.main()
