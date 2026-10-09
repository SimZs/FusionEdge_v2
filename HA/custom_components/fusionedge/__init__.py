"""FusionEdge integration setup."""

import asyncio
import logging
from pathlib import Path

from homeassistant.components import mqtt
from homeassistant.components import frontend
from homeassistant.components.http import StaticPathConfig
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryNotReady

from .const import DOMAIN


PLATFORMS = [Platform.MEDIA_PLAYER, Platform.SELECT, Platform.SWITCH, Platform.LIGHT, Platform.TEXT]
PANEL_PATH = "fusionedge"
PANEL_URL = "/api/fusionedge/panel.js"
PANEL_VERSION = "0.7.0"
_LOGGER = logging.getLogger(__name__)


async def _async_setup_panel(hass: HomeAssistant, entry_id: str) -> None:
    """Serve and register one panel shared by all FusionEdge config entries."""
    panel_state = hass.data.setdefault(
        DOMAIN,
        {"lock": asyncio.Lock(), "entries": set(), "static_ready": False, "panel_owned": False},
    )
    async with panel_state["lock"]:
        if not panel_state["static_ready"]:
            await hass.http.async_register_static_paths(
                [
                    StaticPathConfig(
                        PANEL_URL,
                        str(Path(__file__).parent / "frontend" / "panel.js"),
                        False,
                    )
                ]
            )
            panel_state["static_ready"] = True
        if not panel_state["entries"]:
            if frontend.async_panel_exists(hass, PANEL_PATH):
                _LOGGER.warning("FusionEdge sidebar path is already in use")
            else:
                frontend.async_register_built_in_panel(
                    hass,
                    component_name="custom",
                    sidebar_title="FusionEdge",
                    sidebar_icon="mdi:radio",
                    frontend_url_path=PANEL_PATH,
                    config={
                        "_panel_custom": {
                            "name": "fusionedge-panel",
                            "embed_iframe": False,
                            "trust_external": False,
                            "module_url": f"{PANEL_URL}?v={PANEL_VERSION}",
                        }
                    },
                )
                panel_state["panel_owned"] = True
        panel_state["entries"].add(entry_id)


async def _async_update_listener(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Follow a changed MQTT root advertised by the same physical device."""
    await hass.config_entries.async_reload(entry.entry_id)


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Wait for MQTT, then create the player and its separate controls."""
    if not await mqtt.async_wait_for_mqtt_client(hass):
        raise ConfigEntryNotReady("MQTT client is not available")
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    try:
        await _async_setup_panel(hass, entry.entry_id)
    except (OSError, RuntimeError, ValueError):
        # A frontend problem must not take down the existing MQTT entities.
        _LOGGER.exception("Could not set up the FusionEdge sidebar panel")
    entry.async_on_unload(entry.add_update_listener(_async_update_listener))
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Remove entity subscriptions on unload."""
    if not await hass.config_entries.async_unload_platforms(entry, PLATFORMS):
        return False
    panel_state = hass.data.get(DOMAIN)
    if panel_state is not None:
        async with panel_state["lock"]:
            panel_state["entries"].discard(entry.entry_id)
            if not panel_state["entries"] and panel_state["panel_owned"]:
                frontend.async_remove_panel(hass, PANEL_PATH)
                panel_state["panel_owned"] = False
    return True
