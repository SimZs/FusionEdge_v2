"""Clock TTS and LED screensaver switches for FusionEdge."""

from homeassistant.components.switch import SwitchEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .control_entity import FusionEdgeControlMixin


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    async_add_entities(
        [
            FusionEdgeClockTts(entry),
            FusionEdgeClockTtsNoStream(entry),
            FusionEdgeLedStripScreensaver(entry),
        ]
    )


class FusionEdgeClockTts(FusionEdgeControlMixin, SwitchEntity):
    """Toggle the hourly clock announcement without optimistic state."""

    _attr_translation_key = "clock_tts"
    _attr_icon = "mdi:clock-voice"

    def __init__(self, entry: ConfigEntry) -> None:
        super().__init__(entry, "clock_tts")

    @property
    def is_on(self) -> bool | None:
        value = self._radio_state.get("clock_tts")
        return bool(value) if type(value) is int and value in (0, 1) else None

    async def async_turn_on(self, **kwargs: object) -> None:
        await self._send_command("clocktts 1")

    async def async_turn_off(self, **kwargs: object) -> None:
        await self._send_command("clocktts 0")


class FusionEdgeClockTtsNoStream(FusionEdgeControlMixin, SwitchEntity):
    """Announce the time only when the radio is not playing audio."""

    _attr_translation_key = "clock_tts_no_stream"
    _attr_icon = "mdi:volume-off"

    def __init__(self, entry: ConfigEntry) -> None:
        super().__init__(entry, "clock_tts_no_stream")

    @property
    def available(self) -> bool:
        value = self._radio_state.get("clock_tts_no_stream")
        return self._online and type(value) is int and value in (0, 1)

    @property
    def is_on(self) -> bool | None:
        value = self._radio_state.get("clock_tts_no_stream")
        return bool(value) if type(value) is int and value in (0, 1) else None

    async def async_turn_on(self, **kwargs: object) -> None:
        if not self.available:
            raise HomeAssistantError("The radio has not reported the TTS no-stream setting")
        await self._send_command("clockttsnostream 1")

    async def async_turn_off(self, **kwargs: object) -> None:
        if not self.available:
            raise HomeAssistantError("The radio has not reported the TTS no-stream setting")
        await self._send_command("clockttsnostream 0")


class FusionEdgeLedStripScreensaver(FusionEdgeControlMixin, SwitchEntity):
    """Keep the LED strip enabled while the screensaver is active."""

    _attr_translation_key = "led_strip_screensaver"
    _attr_icon = "mdi:monitor"

    def __init__(self, entry: ConfigEntry) -> None:
        super().__init__(entry, "led_strip_screensaver")

    @property
    def available(self) -> bool:
        value = self._radio_state.get("ledstrip_screensaver")
        return self._online and type(value) is int and value in (0, 1)

    @property
    def is_on(self) -> bool | None:
        value = self._radio_state.get("ledstrip_screensaver")
        return bool(value) if type(value) is int and value in (0, 1) else None

    async def async_turn_on(self, **kwargs: object) -> None:
        if not self.available:
            raise HomeAssistantError("The radio has not reported the LED screensaver setting")
        await self._send_command("lsssena 1")

    async def async_turn_off(self, **kwargs: object) -> None:
        if not self.available:
            raise HomeAssistantError("The radio has not reported the LED screensaver setting")
        await self._send_command("lsssena 0")
