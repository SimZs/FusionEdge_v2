"""Bluetooth output routing for FusionEdge."""

from homeassistant.components.select import SelectEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .control_entity import FusionEdgeControlMixin
from .protocol import BT_OUTPUT_OPTIONS


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    async_add_entities([FusionEdgeBluetoothOutput(entry)])


class FusionEdgeBluetoothOutput(FusionEdgeControlMixin, SelectEntity):
    """Select the source-mode audio route, not the module's BT role."""

    _attr_translation_key = "bluetooth_output"
    _attr_icon = "mdi:bluetooth-audio"
    _attr_options = list(BT_OUTPUT_OPTIONS)

    def __init__(self, entry: ConfigEntry) -> None:
        super().__init__(entry, "bt_output")

    @property
    def available(self) -> bool:
        return (
            self._online
            and self._radio_state.get("bt_role") == "source"
            and self._radio_state.get("bt_ready") == 1
        )

    @property
    def current_option(self) -> str | None:
        value = self._radio_state.get("bt_output")
        if type(value) is int and 0 <= value < len(BT_OUTPUT_OPTIONS):
            return BT_OUTPUT_OPTIONS[value]
        return None

    async def async_select_option(self, option: str) -> None:
        if not self.available:
            raise HomeAssistantError("Bluetooth output is not ready in source mode")
        try:
            output = BT_OUTPUT_OPTIONS.index(option)
        except ValueError as err:
            raise HomeAssistantError("Unknown Bluetooth output option") from err
        if option != self.current_option:
            await self._send_command(f"btoutput {output}")
