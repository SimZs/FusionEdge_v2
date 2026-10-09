"""MQTT-backed FusionEdge radio media player."""

from time import monotonic
from typing import Any

import aiohttp

from homeassistant.components import mqtt
from homeassistant.components.media_player import (
    BrowseError,
    BrowseMedia,
    MediaClass,
    MediaPlayerEntity,
    MediaPlayerEntityFeature,
    MediaPlayerState,
    MediaType,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import CONF_DEVICE_ID, CONF_ROOT_TOPIC, CONF_SOFTWARE_VERSION, DOMAIN
from .protocol import (
    MAX_VOLUME,
    MAX_PLAYLIST_BYTES,
    PLAYLIST_MODES,
    SOURCES,
    command_for_source,
    parse_json_object,
    parse_volume,
    parse_station_playlist,
    source_from_mode,
    valid_playlist_url,
    volume_to_step,
)

PLAYLIST_CACHE_SECONDS = 30


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    """Add the one player represented by this physical radio."""
    async_add_entities([FusionEdgeMediaPlayer(entry)])


class FusionEdgeMediaPlayer(MediaPlayerEntity):
    """Control the existing Edge MQTT command channel; do not poll the radio."""

    _attr_should_poll = False
    _attr_has_entity_name = True
    _attr_name = None
    _attr_icon = "mdi:radio"
    _attr_volume_step = 1 / MAX_VOLUME
    _attr_supported_features = (
        MediaPlayerEntityFeature.PLAY
        | MediaPlayerEntityFeature.STOP
        | MediaPlayerEntityFeature.NEXT_TRACK
        | MediaPlayerEntityFeature.PREVIOUS_TRACK
        | MediaPlayerEntityFeature.VOLUME_SET
        | MediaPlayerEntityFeature.VOLUME_STEP
        | MediaPlayerEntityFeature.PLAY_MEDIA
        | MediaPlayerEntityFeature.TURN_ON
        | MediaPlayerEntityFeature.TURN_OFF
        | MediaPlayerEntityFeature.SELECT_SOURCE
    )

    def __init__(self, entry: ConfigEntry) -> None:
        self._root = entry.data[CONF_ROOT_TOPIC]
        self._device_id = entry.data[CONF_DEVICE_ID]
        self._attr_unique_id = f"{self._device_id}_player"
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, self._device_id)},
            name=entry.title,
            manufacturer="FusionEdge",
            model="FusionEdge radio",
            sw_version=entry.data.get(CONF_SOFTWARE_VERSION) or None,
        )
        self._online = False
        self._status: dict[str, Any] = {}
        self._extended: dict[str, Any] = {}
        self._volume: int | None = None
        self._playlist_url: str | None = None
        self._playlist_cache: list[tuple[int, str]] | None = None
        self._playlist_cached_at = 0.0
        self._playlist_cached_mode: str | None = None

    async def async_added_to_hass(self) -> None:
        """Listen to retained snapshots and the online/offline last will."""
        await super().async_added_to_hass()
        for suffix in ("availability", "status", "state", "volume", "playlist"):
            @callback
            def handle_message(message: Any, kind: str = suffix) -> None:
                self._handle_message(kind, message.payload)

            unsubscribe = await mqtt.async_subscribe(
                self.hass,
                f"{self._root}{suffix}",
                handle_message,
            )
            self.async_on_remove(unsubscribe)

    def _handle_message(self, kind: str, payload: str) -> None:
        """Only accepted messages can change the in-memory HA entity state."""
        if kind == "availability":
            self._online = payload == "online"
        elif kind == "volume":
            volume = parse_volume(payload)
            if volume is None:
                return
            self._volume = volume
        elif kind == "playlist":
            url = payload if valid_playlist_url(payload) else None
            if url != self._playlist_url:
                self._playlist_url = url
                self._playlist_cache = None
                self._playlist_cached_mode = None
        else:
            data = parse_json_object(payload)
            if data is None:
                return
            if kind == "status":
                self._status = data
            else:
                if data.get("mode") != self._extended.get("mode"):
                    self._playlist_cache = None
                    self._playlist_cached_mode = None
                self._extended = data
        self.async_write_ha_state()

    @property
    def available(self) -> bool:
        """The retained LWT makes a disconnected radio unavailable."""
        return self._online

    @property
    def state(self) -> MediaPlayerState | None:
        """Represent radio power and actual audio activity."""
        if not self._online:
            return None
        if self._status.get("on") == 0:
            return MediaPlayerState.OFF
        playing = self._extended.get("playing", self._status.get("status"))
        return MediaPlayerState.PLAYING if playing == 1 else MediaPlayerState.IDLE

    @property
    def volume_level(self) -> float | None:
        """Map Edge's 0–21 integer steps to HA's 0–1 slider."""
        return self._volume / MAX_VOLUME if self._volume is not None else None

    @property
    def media_title(self) -> str | None:
        """Current track title supplied by the existing MQTT status."""
        title = self._status.get("title")
        return title if isinstance(title, str) and title else None

    @property
    def media_channel(self) -> str | None:
        """Current station name supplied by the existing MQTT status."""
        name = self._status.get("name")
        return name if isinstance(name, str) and name else None

    @property
    def media_content_id(self) -> str | None:
        """Numerical station ID, usable with play_media."""
        station = self._status.get("station")
        return str(station) if isinstance(station, int) and station > 0 else None

    @property
    def media_content_type(self) -> MediaType:
        return MediaType.MUSIC

    @property
    def source(self) -> str | None:
        """Show the confirmed radio mode, never an optimistic selection."""
        return source_from_mode(self._extended.get("mode"))

    @property
    def source_list(self) -> list[str]:
        """Possible modes; runtime prerequisites are checked by the radio."""
        return list(SOURCES)

    @property
    def supported_features(self) -> MediaPlayerEntityFeature:
        """Offer station browsing only for the active playlist mode."""
        features = self._attr_supported_features
        if self._extended.get("mode") in PLAYLIST_MODES and self._playlist_url:
            features |= MediaPlayerEntityFeature.BROWSE_MEDIA
        return features

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        """Keep transport details visible without creating duplicate entities."""
        keys = ("mode", "bt_role", "bt_connected", "bt_ready", "bt_output")
        return {key: self._extended[key] for key in keys if key in self._extended}

    async def _send_command(self, command: str) -> None:
        """Never retain commands: reconnects must not replay them."""
        await mqtt.async_publish(
            self.hass, f"{self._root}command", command, qos=0, retain=False
        )

    async def async_media_play(self) -> None:
        await self._send_command("start")

    async def async_media_stop(self) -> None:
        await self._send_command("stop")

    async def async_media_next_track(self) -> None:
        await self._send_command("next")

    async def async_media_previous_track(self) -> None:
        await self._send_command("prev")

    async def async_set_volume_level(self, volume: float) -> None:
        step = volume_to_step(volume)
        if step is None:
            raise HomeAssistantError("Volume must be between 0 and 1")
        await self._send_command(f"vol {step}")

    async def async_volume_up(self) -> None:
        await self._send_command("volp")

    async def async_volume_down(self) -> None:
        await self._send_command("volm")

    async def async_turn_on(self) -> None:
        await self._send_command("turnon")

    async def async_turn_off(self) -> None:
        await self._send_command("turnoff")

    async def async_select_source(self, source: str) -> None:
        """Request a playback mode and wait for the state topic to confirm it."""
        command = command_for_source(source)
        if command is None:
            raise HomeAssistantError(f"Unknown FusionEdge source: {source}")
        if source != self.source:
            await self._send_command(command)
        # Keep showing the previous source until the firmware actually changes.
        self.async_write_ha_state()

    async def _load_current_playlist(self, mode: str) -> list[tuple[int, str]]:
        """Fetch a bounded playlist on demand, never in an MQTT callback."""
        if not self._playlist_url:
            raise BrowseError("The playlist URL is not available")
        if (
            self._playlist_cache is not None
            and self._playlist_cached_mode == mode
            and monotonic() - self._playlist_cached_at < PLAYLIST_CACHE_SECONDS
        ):
            return self._playlist_cache

        url = self._playlist_url
        timeout = aiohttp.ClientTimeout(total=8, connect=3)
        session = async_get_clientsession(self.hass)
        try:
            async with session.get(
                url, timeout=timeout, allow_redirects=False
            ) as response:
                if response.status != 200:
                    raise BrowseError(f"Playlist returned HTTP {response.status}")
                chunks: list[bytes] = []
                size = 0
                async for chunk in response.content.iter_chunked(16384):
                    size += len(chunk)
                    if size > MAX_PLAYLIST_BYTES:
                        raise BrowseError("Playlist is too large")
                    chunks.append(chunk)
                content = b"".join(chunks)
        except (aiohttp.ClientError, TimeoutError) as err:
            raise BrowseError("Cannot fetch the playlist from FusionEdge") from err
        if self._extended.get("mode") != mode or self._playlist_url != url:
            raise BrowseError("Playback mode changed while loading the playlist")
        stations = parse_station_playlist(content)
        if not stations:
            raise BrowseError("Playlist has no valid stations")
        self._playlist_cache = stations
        self._playlist_cached_at = monotonic()
        self._playlist_cached_mode = mode
        return stations

    async def async_browse_media(
        self,
        media_content_type: MediaType | str | None = None,
        media_content_id: str | None = None,
    ) -> BrowseMedia:
        """Show stations/tracks with IDs matching the active firmware index."""
        mode = self._extended.get("mode")
        if mode not in PLAYLIST_MODES or not self._online:
            raise BrowseError("Switch to an online WEB, SD or DLNA mode before browsing")
        if media_content_id not in (None, "", mode):
            raise BrowseError("Unknown FusionEdge media item")
        stations = await self._load_current_playlist(mode)
        return BrowseMedia(
            title=f"{self.source} playlist",
            media_class=MediaClass.DIRECTORY,
            media_content_id=mode,
            media_content_type=MediaType.CHANNELS,
            can_play=False,
            can_expand=True,
            children_media_class=MediaClass.CHANNEL,
            children=[
                BrowseMedia(
                    title=f"{station_id}. {name}",
                    media_class=MediaClass.CHANNEL,
                    media_content_id=f"{mode}:{station_id}",
                    media_content_type=MediaType.CHANNEL,
                    can_play=True,
                    can_expand=False,
                )
                for station_id, name in stations
            ],
        )

    async def async_play_media(self, media_type: str, media_id: str, **kwargs: Any) -> None:
        """Play a station number or a direct HTTP stream URL."""
        if not isinstance(media_id, str):
            raise HomeAssistantError("Invalid FusionEdge media ID")
        kind = getattr(media_type, "value", media_type)
        playlist_mode, separator, station_id = media_id.partition(":")
        if kind == "channel" and separator and playlist_mode in PLAYLIST_MODES:
            if self._extended.get("mode") != playlist_mode:
                raise HomeAssistantError("Switch back to this playlist's mode first")
            if not station_id.isascii() or not station_id.isdigit():
                raise HomeAssistantError("Invalid playlist item number")
            number = int(station_id)
            if (
                self._playlist_cached_mode != playlist_mode
                or not self._playlist_cache
                or not 1 <= number <= len(self._playlist_cache)
            ):
                raise HomeAssistantError("Playlist item is no longer in the current list")
            await self._send_command(f"play {number}")
            return
        if kind in ("channel", "station", "music") and media_id.isascii() and media_id.isdigit():
            number = int(media_id)
            if 1 <= number <= 65535:
                await self._send_command(f"play {number}")
                return
        if kind in ("url", "music") and media_id.startswith(("http://", "https://")):
            if len(media_id.encode("utf-8")) < 512 and not any(
                char in media_id for char in ("\r", "\n", "\0")
            ):
                await self._send_command(media_id)
                return
        raise HomeAssistantError("Use a station number or an HTTP stream URL")
