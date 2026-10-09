"""MQTT discovery and user confirmation for FusionEdge radios."""

from typing import Any

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.helpers.service_info.mqtt import MqttServiceInfo

from .const import CONF_ROOT_TOPIC, CONF_SOFTWARE_VERSION, DOMAIN
from .protocol import parse_discovery


class FusionEdgeConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Create one config entry for each discovered physical radio."""

    VERSION = 1

    def __init__(self) -> None:
        self._discovered: dict[str, str] | None = None

    async def async_step_mqtt(self, discovery_info: MqttServiceInfo):
        """Validate the retained advertisement; never install silently."""
        data = parse_discovery(discovery_info.topic, discovery_info.payload)
        if data is None:
            return self.async_abort(reason="invalid_discovery")
        await self.async_set_unique_id(data["device_id"])
        self._abort_if_unique_id_configured(
            updates={
                CONF_ROOT_TOPIC: data[CONF_ROOT_TOPIC],
                CONF_SOFTWARE_VERSION: data[CONF_SOFTWARE_VERSION],
            }
        )
        self._discovered = data
        self.context["title_placeholders"] = {"name": data["name"]}
        return await self.async_step_confirm()

    async def async_step_confirm(self, user_input: dict[str, Any] | None = None):
        """Let the user decide whether to add this radio."""
        if self._discovered is None:
            return self.async_abort(reason="invalid_discovery")
        if user_input is not None:
            return self.async_create_entry(
                title=self._discovered["name"], data=self._discovered
            )
        return self.async_show_form(
            step_id="confirm",
            data_schema=vol.Schema({}),
            description_placeholders={"name": self._discovered["name"]},
        )

    async def async_step_user(self, user_input: dict[str, Any] | None = None):
        """This first version uses the radio's retained MQTT advertisement."""
        return self.async_abort(reason="discovery_required")
