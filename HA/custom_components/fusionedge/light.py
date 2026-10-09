"""LED strip power and playback effects for FusionEdge."""

from typing import Any

from homeassistant.components.light import (
    ATTR_EFFECT,
    ColorMode,
    LightEntity,
    LightEntityFeature,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .control_entity import FusionEdgeControlMixin
from .protocol import LED_EFFECTS


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    async_add_entities([FusionEdgeLedStrip(entry)])


class FusionEdgeLedStrip(FusionEdgeControlMixin, LightEntity):
    """Represent the plugin as one HA light with selectable effects."""

    _attr_translation_key = "led_strip"
    _attr_icon = "mdi:led-strip-variant"
    _attr_supported_color_modes = {ColorMode.ONOFF}
    _attr_color_mode = ColorMode.ONOFF
    _attr_supported_features = LightEntityFeature.EFFECT
    _attr_effect_list = list(LED_EFFECTS)

    def __init__(self, entry: ConfigEntry) -> None:
        super().__init__(entry, "led_strip")

    @property
    def is_on(self) -> bool | None:
        value = self._radio_state.get("ledstrip_enabled")
        return bool(value) if type(value) is int and value in (0, 1) else None

    @property
    def effect(self) -> str | None:
        value = self._radio_state.get("ledstrip_effect")
        if type(value) is int and 0 <= value < len(LED_EFFECTS):
            return LED_EFFECTS[value]
        return None

    async def async_turn_on(self, **kwargs: Any) -> None:
        effect = kwargs.get(ATTR_EFFECT)
        if effect is not None:
            try:
                index = LED_EFFECTS.index(effect)
            except ValueError as err:
                raise HomeAssistantError("Unknown LED strip effect") from err
            if effect != self.effect:
                await self._send_command(f"lsmodel {index}")
        if self.is_on is not True:
            await self._send_command("lsenabled 1")

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self._send_command("lsenabled 0")
