"""Weekday and weekend auto on/off timers for FusionEdge."""

from homeassistant.components.text import TextEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .control_entity import FusionEdgeControlMixin
from .protocol import TIMER_FIELDS, TIMER_PATTERN, valid_timer_value


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    async_add_entities([FusionEdgeAutoTimer(entry, field) for field in TIMER_FIELDS])


class FusionEdgeAutoTimer(FusionEdgeControlMixin, TextEntity):
    """Edit one persisted timer; 'off' disables that individual event."""

    _attr_icon = "mdi:clock-outline"
    _attr_native_min = 3
    _attr_native_max = 5
    _attr_pattern = TIMER_PATTERN

    def __init__(self, entry: ConfigEntry, field: str) -> None:
        if field not in TIMER_FIELDS:
            raise ValueError("Unknown auto timer field")
        super().__init__(entry, f"timer_{field}")
        self._field = field
        self._attr_translation_key = f"timer_{field}"

    @property
    def available(self) -> bool:
        value = self._radio_state.get(f"timer_{self._field}")
        return self._online and (value == "" or valid_timer_value(value))

    @property
    def native_value(self) -> str | None:
        value = self._radio_state.get(f"timer_{self._field}")
        if value == "":
            return "off"
        return value if valid_timer_value(value) else None

    async def async_set_value(self, value: str) -> None:
        if not self.available:
            raise HomeAssistantError("The radio has not reported the auto timer settings")
        if not valid_timer_value(value):
            raise HomeAssistantError("Enter HH:MM (24-hour time) or off")
        if value != self.native_value:
            await self._send_command(f"autotimer {self._field} {value}")
