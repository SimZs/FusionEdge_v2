"""Shared MQTT state handling for FusionEdge control entities."""

from typing import Any

from homeassistant.components import mqtt
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import callback
from homeassistant.helpers.device_registry import DeviceInfo

from .const import CONF_DEVICE_ID, CONF_ROOT_TOPIC, CONF_SOFTWARE_VERSION, DOMAIN
from .protocol import parse_json_object


class FusionEdgeControlMixin:
    """Keep each control confirmed by the radio's retained state topic."""

    _attr_has_entity_name = True
    _attr_should_poll = False

    def __init__(self, entry: ConfigEntry, suffix: str) -> None:
        self._root = entry.data[CONF_ROOT_TOPIC]
        self._attr_unique_id = f"{entry.data[CONF_DEVICE_ID]}_{suffix}"
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, entry.data[CONF_DEVICE_ID])},
            name=entry.title,
            manufacturer="FusionEdge",
            model="FusionEdge radio",
            sw_version=entry.data.get(CONF_SOFTWARE_VERSION) or None,
        )
        self._online = False
        self._radio_state: dict[str, Any] = {}

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()

        @callback
        def handle_availability(message: Any) -> None:
            self._online = message.payload == "online"
            self.async_write_ha_state()

        @callback
        def handle_state(message: Any) -> None:
            state = parse_json_object(message.payload)
            if state is not None:
                self._radio_state = state
                self.async_write_ha_state()

        for suffix, handler in (
            ("availability", handle_availability),
            ("state", handle_state),
        ):
            unsubscribe = await mqtt.async_subscribe(
                self.hass, f"{self._root}{suffix}", handler
            )
            self.async_on_remove(unsubscribe)

    @property
    def available(self) -> bool:
        return self._online

    async def _send_command(self, command: str) -> None:
        """Send a one-shot command; retained commands could replay on reboot."""
        await mqtt.async_publish(
            self.hass, f"{self._root}command", command, qos=0, retain=False
        )
