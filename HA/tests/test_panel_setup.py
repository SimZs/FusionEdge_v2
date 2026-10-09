"""Test panel lifecycle without needing a full Home Assistant installation."""

import asyncio
from importlib import import_module
from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch


COMPONENT_DIR = Path(__file__).resolve().parents[1] / "custom_components/fusionedge"


def module(name: str, **attributes):
    result = ModuleType(name)
    result.__dict__.update(attributes)
    return result


class FakeHass:
    def __init__(self):
        self.data = {}
        self.paths = []
        self.forwarded = []
        self.unloaded = []

        async def register_paths(paths):
            self.paths.extend(paths)

        async def forward(entry, platforms):
            self.forwarded.append(entry.entry_id)

        async def unload(entry, platforms):
            self.unloaded.append(entry.entry_id)
            return True

        self.http = SimpleNamespace(async_register_static_paths=register_paths)
        self.config_entries = SimpleNamespace(
            async_forward_entry_setups=forward,
            async_unload_platforms=unload,
        )


class PanelSetupTest(unittest.TestCase):
    def setUp(self):
        self.panels = {}

        def register(hass, **kwargs):
            self.panels[kwargs["frontend_url_path"]] = kwargs

        def remove(hass, path):
            del self.panels[path]

        frontend = module(
            "homeassistant.components.frontend",
            async_panel_exists=lambda hass, path: path in self.panels,
            async_register_built_in_panel=register,
            async_remove_panel=remove,
        )
        http = module(
            "homeassistant.components.http",
            StaticPathConfig=lambda url, path, cache: SimpleNamespace(
                url_path=url, path=path, cache_headers=cache
            ),
        )

        async def mqtt_ready(hass):
            return True

        mqtt = module(
            "homeassistant.components.mqtt", async_wait_for_mqtt_client=mqtt_ready
        )
        components = module(
            "homeassistant.components", mqtt=mqtt, frontend=frontend, http=http
        )
        homeassistant = module("homeassistant", components=components)
        stubs = {
            "homeassistant": homeassistant,
            "homeassistant.components": components,
            "homeassistant.components.mqtt": mqtt,
            "homeassistant.components.frontend": frontend,
            "homeassistant.components.http": http,
            "homeassistant.config_entries": module(
                "homeassistant.config_entries", ConfigEntry=object
            ),
            "homeassistant.const": module(
                "homeassistant.const",
                Platform=SimpleNamespace(
                    MEDIA_PLAYER="media_player", SELECT="select", SWITCH="switch", LIGHT="light", TEXT="text"
                ),
            ),
            "homeassistant.core": module("homeassistant.core", HomeAssistant=object),
            "homeassistant.exceptions": module(
                "homeassistant.exceptions", ConfigEntryNotReady=RuntimeError
            ),
        }
        for name in tuple(sys.modules):
            if name == "fusionedge" or name.startswith("fusionedge."):
                del sys.modules[name]
        package = module("fusionedge")
        package.__path__ = [str(COMPONENT_DIR)]
        stubs["fusionedge"] = package
        self.modules_patch = patch.dict(sys.modules, stubs)
        self.modules_patch.start()
        self.addCleanup(self.modules_patch.stop)
        self.integration = import_module("fusionedge.__init__")

    def entry(self, entry_id):
        return SimpleNamespace(
            entry_id=entry_id,
            add_update_listener=lambda listener: lambda: None,
            async_on_unload=lambda unsubscribe: None,
        )

    def test_one_panel_for_two_radios_and_remove_after_last_unload(self):
        hass = FakeHass()
        first = self.entry("radio-1")
        second = self.entry("radio-2")
        asyncio.run(self.integration.async_setup_entry(hass, first))
        asyncio.run(self.integration.async_setup_entry(hass, second))
        self.assertEqual(len(hass.paths), 1)
        self.assertIn("frontend/panel.js", hass.paths[0].path.replace("\\", "/"))
        self.assertEqual(list(self.panels), ["fusionedge"])
        panel = self.panels["fusionedge"]
        self.assertEqual(panel["config"]["_panel_custom"]["name"], "fusionedge-panel")
        asyncio.run(self.integration.async_unload_entry(hass, first))
        self.assertIn("fusionedge", self.panels)
        asyncio.run(self.integration.async_unload_entry(hass, second))
        self.assertNotIn("fusionedge", self.panels)
        asyncio.run(self.integration.async_setup_entry(hass, first))
        self.assertEqual(len(hass.paths), 1)
        self.assertIn("fusionedge", self.panels)

    def test_existing_foreign_panel_is_not_removed(self):
        hass = FakeHass()
        self.panels["fusionedge"] = {"component_name": "other"}
        entry = self.entry("radio-1")
        asyncio.run(self.integration.async_setup_entry(hass, entry))
        asyncio.run(self.integration.async_unload_entry(hass, entry))
        self.assertEqual(self.panels["fusionedge"], {"component_name": "other"})


if __name__ == "__main__":
    unittest.main()
