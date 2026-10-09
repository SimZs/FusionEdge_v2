/* Bundled FusionEdge sidebar panel. No /config/www copy or Lovelace setup. */
const STRINGS = {
  hu: {
    loading: "FusionEdge eszközök betöltése…",
    empty: "Nincs hozzáadott FusionEdge rádió. Előbb add hozzá a felismert eszközt az Integrációk oldalon.",
    retry: "Frissítés",
    offline: "Nem elérhető",
    ready: "Elérhető",
    player: "Lejátszó",
    station: "Állomás",
    noTitle: "Nincs címadat",
    power: "Be-/kikapcsolás",
    previous: "Előző",
    play: "Lejátszás",
    stop: "Leállítás",
    next: "Következő",
    volume: "Hangerő",
    source: "Lejátszási mód",
    playlist: "Állomáslista",
    loadPlaylist: "Lista megnyitása",
    closePlaylist: "Lista bezárása",
    bluetooth: "Bluetooth-kimenet",
    sourceOnly: "A BT-kimenet csak kész source módban állítható.",
    clock: "Óra bemondása",
    clockNoStream: "Csak ha nincs lejátszás",
    led: "LED-szalag",
    ledScreensaver: "Képernyővédő alatt is",
    effect: "Effekt",
    timers: "Automatikus be-/kikapcsolás",
    weekdays: "Hétköznap",
    weekends: "Hétvége",
    timerStart: "Bekapcsolás",
    timerStop: "Kikapcsolás",
    timerOff: "Kikapcsolva",
    timerHelp: "Üres időpont = kikapcsolva. Az időzítést a rádió tárolja és hajtja végre.",
    firmwareRequired: "Firmware-frissítés után használható.",
    unavailable: "Nem érhető el",
    unknown: "Ismeretlen",
    error: "A művelet nem sikerült",
  },
  en: {
    loading: "Loading FusionEdge devices…",
    empty: "No FusionEdge radio has been added. Add the discovered device on the Integrations page first.",
    retry: "Refresh",
    offline: "Unavailable",
    ready: "Available",
    player: "Player",
    station: "Station",
    noTitle: "No track information",
    power: "Power",
    previous: "Previous",
    play: "Play",
    stop: "Stop",
    next: "Next",
    volume: "Volume",
    source: "Playback mode",
    playlist: "Playlist",
    loadPlaylist: "Open playlist",
    closePlaylist: "Close playlist",
    bluetooth: "Bluetooth output",
    sourceOnly: "Bluetooth output is only available when source mode is ready.",
    clock: "Clock announcement",
    clockNoStream: "Only when no audio is playing",
    led: "LED strip",
    ledScreensaver: "During screensaver too",
    effect: "Effect",
    timers: "Automatic on/off timer",
    weekdays: "Weekdays",
    weekends: "Weekend",
    timerStart: "Turn on",
    timerStop: "Turn off",
    timerOff: "Disabled",
    timerHelp: "Empty time = disabled. The radio stores and runs the schedule.",
    firmwareRequired: "Available after updating the radio firmware.",
    unavailable: "Unavailable",
    unknown: "Unknown",
    error: "The action failed",
  },
};

const ENTITY_TYPES = {
  player: "media_player",
  bt_output: "select",
  clock_tts: "switch",
  clock_tts_no_stream: "switch",
  led_strip: "light",
  led_strip_screensaver: "switch",
  timer_weekday_start: "text",
  timer_weekday_stop: "text",
  timer_weekend_start: "text",
  timer_weekend_stop: "text",
};

const TIMER_FIELDS = ["weekday_start", "weekday_stop", "weekend_start", "weekend_stop"];

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, (char) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  })[char]);
}

function isAvailable(entity) {
  return Boolean(entity && entity.state !== "unavailable" && entity.state !== "unknown");
}

class FusionEdgePanel extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: "open" });
    this._devices = [];
    this._selected = null;
    this._registryLoading = false;
    this._registryLoaded = false;
    this._playlist = null;
    this._playlistKey = null;
    this._error = "";
    this.shadowRoot.addEventListener("click", (event) => this._onClick(event));
    this.shadowRoot.addEventListener("change", (event) => this._onChange(event));
  }

  set hass(value) {
    const previous = this._hass;
    this._hass = value;
    if (this.isConnected && !this._registryLoaded && !this._registryLoading) {
      this._loadRegistry();
    }
    if (!previous || this._relevantStatesChanged(previous, value)) {
      this._render();
    }
  }

  set panel(value) { this._panel = value; }
  set narrow(value) { this._narrow = value; }
  set route(value) { this._route = value; }

  connectedCallback() {
    this._render();
    if (this._hass && !this._registryLoaded && !this._registryLoading) {
      this._loadRegistry();
    }
  }

  get _text() {
    return STRINGS[this._hass?.language?.startsWith("hu") ? "hu" : "en"];
  }

  _currentDevice() {
    return this._devices.find((device) => device.id === this._selected) || this._devices[0];
  }

  _relevantStatesChanged(previous, next) {
    const device = this._currentDevice();
    if (!device) return false;
    return Object.keys(ENTITY_TYPES).some((kind) => {
      const entityId = device.entities[kind];
      return entityId && previous.states[entityId] !== next.states[entityId];
    });
  }

  async _loadRegistry() {
    if (!this._hass || this._registryLoading) return;
    this._registryLoading = true;
    this._error = "";
    this._render();
    try {
      const entries = await this._hass.callWS({ type: "config/entity_registry/list" });
      const devices = new Map();
      for (const entry of entries) {
        if (entry.platform !== "fusionedge" || entry.disabled_by) continue;
        const match = /^(.*)_(player|bt_output|clock_tts|clock_tts_no_stream|led_strip|led_strip_screensaver|timer_weekday_start|timer_weekday_stop|timer_weekend_start|timer_weekend_stop)$/.exec(entry.unique_id || "");
        if (!match || !entry.entity_id.startsWith(`${ENTITY_TYPES[match[2]]}.`)) continue;
        if (!devices.has(match[1])) devices.set(match[1], { id: match[1], entities: {} });
        devices.get(match[1]).entities[match[2]] = entry.entity_id;
      }
      this._devices = [...devices.values()].filter((device) => device.entities.player);
      this._devices.sort((left, right) => this._deviceName(left).localeCompare(this._deviceName(right)));
      if (!this._devices.some((device) => device.id === this._selected)) {
        this._selected = this._devices[0]?.id || null;
      }
      this._registryLoaded = true;
    } catch (error) {
      this._error = `${this._text.error}: ${error.message || error}`;
    } finally {
      this._registryLoading = false;
      this._render();
    }
  }

  _deviceName(device) {
    const player = this._hass?.states[device.entities.player];
    return player?.attributes.friendly_name || `FusionEdge ${device.id.slice(-4)}`;
  }

  _entity(device, kind) {
    return this._hass?.states[device.entities[kind]];
  }

  async _call(domain, service, data) {
    this._error = "";
    try {
      await this._hass.callService(domain, service, data);
    } catch (error) {
      this._error = `${this._text.error}: ${error.message || error}`;
      this._render();
    }
  }

  async _browse(device) {
    const player = this._entity(device, "player");
    const key = `${device.id}:${player?.attributes.source || ""}`;
    if (this._playlist && this._playlistKey === key) {
      this._playlist = null;
      this._render();
      return;
    }
    this._playlist = null;
    this._playlistKey = key;
    this._error = "";
    this._render();
    try {
      const result = await this._hass.callWS({
        type: "media_player/browse_media",
        entity_id: device.entities.player,
      });
      if (this._playlistKey === key) this._playlist = result.children || [];
    } catch (error) {
      this._error = `${this._text.error}: ${error.message || error}`;
    }
    this._render();
  }

  _onClick(event) {
    const button = event.target.closest("button[data-action]");
    if (!button || button.disabled) return;
    const device = this._currentDevice();
    if (button.dataset.action === "refresh") {
      this._registryLoaded = false;
      this._loadRegistry();
      return;
    }
    if (!device) return;
    const playerId = device.entities.player;
    switch (button.dataset.action) {
      case "power":
        this._call("media_player", this._entity(device, "player")?.state === "off" ? "turn_on" : "turn_off", { entity_id: playerId });
        break;
      case "previous": this._call("media_player", "media_previous_track", { entity_id: playerId }); break;
      case "play": this._call("media_player", "media_play", { entity_id: playerId }); break;
      case "stop": this._call("media_player", "media_stop", { entity_id: playerId }); break;
      case "next": this._call("media_player", "media_next_track", { entity_id: playerId }); break;
      case "playlist": this._browse(device); break;
      case "station": {
        const station = this._playlist?.[Number(button.dataset.index)];
        if (station?.can_play) this._call("media_player", "play_media", {
          entity_id: playerId,
          media_content_type: station.media_content_type,
          media_content_id: station.media_content_id,
        });
        break;
      }
      case "clock": {
        const entity = this._entity(device, "clock_tts");
        this._call("switch", entity?.state === "on" ? "turn_off" : "turn_on", { entity_id: device.entities.clock_tts });
        break;
      }
      case "clock-no-stream": {
        const entity = this._entity(device, "clock_tts_no_stream");
        this._call("switch", entity?.state === "on" ? "turn_off" : "turn_on", { entity_id: device.entities.clock_tts_no_stream });
        break;
      }
      case "led": {
        const entity = this._entity(device, "led_strip");
        this._call("light", entity?.state === "on" ? "turn_off" : "turn_on", { entity_id: device.entities.led_strip });
        break;
      }
      case "led-screensaver": {
        const entity = this._entity(device, "led_strip_screensaver");
        this._call("switch", entity?.state === "on" ? "turn_off" : "turn_on", { entity_id: device.entities.led_strip_screensaver });
        break;
      }
      case "timer-off": {
        const field = button.dataset.field;
        if (TIMER_FIELDS.includes(field)) this._call("text", "set_value", {
          entity_id: device.entities[`timer_${field}`], value: "off",
        });
        break;
      }
    }
  }

  _onChange(event) {
    const target = event.target;
    const device = this._currentDevice();
    if (target.id === "device") {
      this._selected = target.value;
      this._playlist = null;
      this._playlistKey = null;
      this._render();
    } else if (!device || target.disabled) {
      return;
    } else if (target.id === "volume") {
      this._call("media_player", "volume_set", {
        entity_id: device.entities.player,
        volume_level: Number(target.value) / 100,
      });
    } else if (target.id === "source") {
      this._playlist = null;
      this._playlistKey = null;
      this._call("media_player", "select_source", { entity_id: device.entities.player, source: target.value });
    } else if (target.id === "bt-output") {
      this._call("select", "select_option", { entity_id: device.entities.bt_output, option: target.value });
    } else if (target.id === "effect") {
      this._call("light", "turn_on", { entity_id: device.entities.led_strip, effect: target.value });
    } else if (TIMER_FIELDS.includes(target.dataset.timerField)) {
      this._call("text", "set_value", {
        entity_id: device.entities[`timer_${target.dataset.timerField}`],
        value: target.value || "off",
      });
    }
  }

  _render() {
    if (!this._hass || !this.shadowRoot) return;
    const t = this._text;
    const device = this._currentDevice();
    const player = device && this._entity(device, "player");
    const bt = device && this._entity(device, "bt_output");
    const clock = device && this._entity(device, "clock_tts");
    const clockNoStream = device && this._entity(device, "clock_tts_no_stream");
    const led = device && this._entity(device, "led_strip");
    const ledScreensaver = device && this._entity(device, "led_strip_screensaver");
    const timers = Object.fromEntries(TIMER_FIELDS.map((field) => [field, device && this._entity(device, `timer_${field}`)]));
    const online = isAvailable(player);
    const currentKey = `${device?.id}:${player?.attributes.source || ""}`;
    if (this._playlistKey && this._playlistKey !== currentKey) {
      this._playlist = null;
      this._playlistKey = null;
    }
    const oldScroll = this.shadowRoot.querySelector(".station-list")?.scrollTop || 0;
    const source = player?.attributes.source;
    const volume = Math.round((player?.attributes.volume_level || 0) * 100);
    const volumeStep = Math.round(volume * 21 / 100);
    const sourceOptions = player?.attributes.source_list || [];
    const btOptions = bt?.attributes.options || [];
    const ledOptions = led?.attributes.effect_list || [];
    const timerRow = (field, label) => {
      const entity = timers[field];
      const enabled = isAvailable(entity);
      const value = enabled && entity.state !== "off" ? entity.state : "";
      return `<div class="timer-row"><label for="timer-${field}">${label}</label><input id="timer-${field}" data-timer-field="${field}" type="time" value="${escapeHtml(value)}" ${!enabled ? "disabled" : ""}><button type="button" data-action="timer-off" data-field="${field}" ${!enabled || entity.state === "off" ? "disabled" : ""}>${t.timerOff}</button></div>`;
    };
    const stationButtons = this._playlist?.map((item, index) =>
      `<button type="button" class="station" data-action="station" data-index="${index}" ${!online || !item.can_play ? "disabled" : ""}>${escapeHtml(item.title)}</button>`
    ).join("") || "";
    const devicePicker = this._devices.length > 1 ? `<select id="device" aria-label="FusionEdge device">${this._devices.map((item) =>
      `<option value="${escapeHtml(item.id)}" ${item.id === device?.id ? "selected" : ""}>${escapeHtml(this._deviceName(item))}</option>`
    ).join("")}</select>` : "";
    const body = !device ? `<section class="card empty"><p>${this._registryLoading ? t.loading : t.empty}</p><button data-action="refresh">${t.retry}</button></section>` : `
      <section class="card hero">
        <div class="card-head"><h2>${t.player}</h2><span class="badge ${online ? "online" : ""}">${online ? t.ready : t.offline}</span></div>
        <p class="station-name">${escapeHtml(player?.attributes.media_channel || t.station)}</p>
        <p class="track-name">${escapeHtml(player?.attributes.media_title || t.noTitle)}</p>
        <div class="controls">
          <button data-action="power" class="power ${player?.state !== "off" ? "is-on" : ""}" title="${t.power}" aria-label="${t.power}" ${!online ? "disabled" : ""}><svg viewBox="0 0 24 24" aria-hidden="true" focusable="false"><path d="M12 2v10M6.1 6.1a8 8 0 1 0 11.8 0" /></svg></button>
          <button data-action="previous" title="${t.previous}" aria-label="${t.previous}" ${!online ? "disabled" : ""}>⏮</button>
          <button data-action="${player?.state === "playing" ? "stop" : "play"}" title="${player?.state === "playing" ? t.stop : t.play}" aria-label="${player?.state === "playing" ? t.stop : t.play}" class="primary" ${!online ? "disabled" : ""}>${player?.state === "playing" ? "■" : "▶"}</button>
          <button data-action="next" title="${t.next}" aria-label="${t.next}" ${!online ? "disabled" : ""}>⏭</button>
        </div>
        <label for="volume">${t.volume} <strong>${volumeStep}/21</strong></label>
        <input id="volume" type="range" min="0" max="100" value="${volume}" ${!online ? "disabled" : ""}>
        <label for="source">${t.source}</label>
        <select id="source" ${!online ? "disabled" : ""}>${sourceOptions.map((option) =>
          `<option value="${escapeHtml(option)}" ${option === source ? "selected" : ""}>${escapeHtml(option)}</option>`
        ).join("")}</select>
      </section>
      ${["WEB", "SD", "DLNA"].includes(source) ? `<section class="card"><div class="card-head"><h2>${t.playlist}</h2><button data-action="playlist" ${!online ? "disabled" : ""}>${this._playlist ? t.closePlaylist : t.loadPlaylist}</button></div>${this._playlist ? `<div class="station-list">${stationButtons}</div>` : ""}</section>` : ""}
      <section class="card"><h2>${t.bluetooth}</h2><select id="bt-output" ${!isAvailable(bt) ? "disabled" : ""}>${btOptions.map((option) =>
        `<option value="${escapeHtml(option)}" ${option === bt?.state ? "selected" : ""}>${escapeHtml(option)}</option>`
      ).join("")}</select>${!isAvailable(bt) ? `<p class="hint">${t.sourceOnly}</p>` : ""}</section>
      <section class="card"><div class="card-head"><div><h2>${t.clock}</h2><span class="hint">${clock?.state === "unknown" ? t.unknown : !isAvailable(clock) ? t.unavailable : ""}</span></div><button data-action="clock" class="toggle ${clock?.state === "on" ? "active" : ""}" aria-pressed="${clock?.state === "on"}" ${!isAvailable(clock) ? "disabled" : ""}>${clock?.state === "on" ? "ON" : "OFF"}</button></div><div class="subcontrol"><div>${t.clockNoStream}${online && !isAvailable(clockNoStream) ? `<p class="hint">${t.firmwareRequired}</p>` : ""}</div><button data-action="clock-no-stream" class="toggle ${clockNoStream?.state === "on" ? "active" : ""}" aria-pressed="${clockNoStream?.state === "on"}" ${!isAvailable(clockNoStream) ? "disabled" : ""}>${clockNoStream?.state === "on" ? "ON" : "OFF"}</button></div></section>
      <section class="card"><div class="card-head"><h2>${t.led}</h2><button data-action="led" class="toggle ${led?.state === "on" ? "active" : ""}" aria-pressed="${led?.state === "on"}" ${!isAvailable(led) ? "disabled" : ""}>${led?.state === "on" ? "ON" : "OFF"}</button></div><label for="effect">${t.effect}</label><select id="effect" ${!isAvailable(led) ? "disabled" : ""}>${ledOptions.map((option) =>
        `<option value="${escapeHtml(option)}" ${option === led?.attributes.effect ? "selected" : ""}>${escapeHtml(option)}</option>`
      ).join("")}</select><div class="subcontrol"><div>${t.ledScreensaver}${online && !isAvailable(ledScreensaver) ? `<p class="hint">${t.firmwareRequired}</p>` : ""}</div><button data-action="led-screensaver" class="toggle ${ledScreensaver?.state === "on" ? "active" : ""}" aria-pressed="${ledScreensaver?.state === "on"}" ${!isAvailable(ledScreensaver) ? "disabled" : ""}>${ledScreensaver?.state === "on" ? "ON" : "OFF"}</button></div></section>
      <section class="card"><h2>${t.timers}</h2><p class="hint">${t.timerHelp}</p>${online && !TIMER_FIELDS.every((field) => isAvailable(timers[field])) ? `<p class="hint">${t.firmwareRequired}</p>` : ""}<div class="timer-grid"><div><h3>${t.weekdays}</h3>${timerRow("weekday_start", t.timerStart)}${timerRow("weekday_stop", t.timerStop)}</div><div><h3>${t.weekends}</h3>${timerRow("weekend_start", t.timerStart)}${timerRow("weekend_stop", t.timerStop)}</div></div></section>`;

    this.shadowRoot.innerHTML = `<style>
      :host { display:block; min-height:100%; color:var(--primary-text-color); background:var(--primary-background-color); font-family:var(--paper-font-body1_-_font-family, sans-serif); }
      * { box-sizing:border-box; }
      .wrap { max-width:780px; padding:24px 16px 48px; margin:auto; }
      header { display:flex; align-items:center; justify-content:space-between; gap:16px; margin-bottom:20px; }
      h1 { font-size:1.8rem; margin:0; } h2 { font-size:1.05rem; margin:0; }
      .card { background:var(--ha-card-background, var(--card-background-color)); border-radius:16px; box-shadow:var(--ha-card-box-shadow, 0 2px 8px rgba(0,0,0,.1)); padding:20px; margin:0 0 16px; }
      .card-head, .row { display:flex; align-items:center; justify-content:space-between; gap:12px; }
      .subcontrol { display:flex; align-items:center; justify-content:space-between; gap:12px; border-top:1px solid var(--divider-color); margin-top:18px; padding-top:16px; }
      .badge { border-radius:99px; padding:5px 11px; color:var(--secondary-text-color); background:var(--secondary-background-color); font-size:.8rem; }
      .badge.online { color:var(--success-color, #258a45); }
      .station-name { margin:22px 0 5px; font-weight:600; font-size:1.25rem; }
      .track-name { margin:0; min-height:2.5em; color:var(--secondary-text-color); line-height:1.4; overflow-wrap:anywhere; }
      .controls { display:flex; justify-content:center; align-items:center; gap:12px; margin:18px 0 24px; }
      button, select { font:inherit; }
      button { cursor:pointer; color:var(--primary-text-color); background:var(--secondary-background-color); border:0; border-radius:10px; padding:10px 14px; }
      button:hover:not(:disabled) { filter:brightness(.94); }
      button:disabled, select:disabled, input:disabled { cursor:not-allowed; opacity:.45; }
      button:focus-visible, select:focus-visible, input:focus-visible { outline:2px solid var(--primary-color); outline-offset:2px; }
      .controls button { display:inline-flex; align-items:center; justify-content:center; min-width:52px; min-height:48px; font-size:1.35rem; }
      .controls .power.is-on { color:var(--primary-color); }
      .controls .power svg { display:block; width:25px; height:25px; fill:none; stroke:currentColor; stroke-width:2.2; stroke-linecap:round; stroke-linejoin:round; }
      .controls .primary, .toggle.active { color:white; background:var(--primary-color); }
      label { display:block; margin:18px 0 8px; color:var(--secondary-text-color); font-size:.9rem; }
      label strong { float:right; color:var(--primary-text-color); }
      input[type=range], select { width:100%; accent-color:var(--primary-color); }
      select { background:var(--secondary-background-color); color:var(--primary-text-color); border:1px solid var(--divider-color); border-radius:8px; padding:9px; }
      header select { width:auto; min-width:170px; max-width:55%; }
      .station-list { max-height:300px; overflow-y:auto; margin-top:14px; border-top:1px solid var(--divider-color); }
      .station { display:block; width:100%; text-align:left; border-radius:0; border-bottom:1px solid var(--divider-color); background:transparent; }
      .hint { color:var(--secondary-text-color); font-size:.85rem; margin:10px 0 0; }
      .timer-grid { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:20px; margin-top:14px; }
      .timer-grid h3 { font-size:.95rem; margin:0 0 8px; }
      .timer-row { display:grid; grid-template-columns:minmax(0,1fr) 100px auto; align-items:center; gap:8px; margin:8px 0; }
      .timer-row label { margin:0; }
      .timer-row input { width:100%; min-width:0; padding:8px; color:var(--primary-text-color); background:var(--secondary-background-color); border:1px solid var(--divider-color); border-radius:8px; font:inherit; }
      .timer-row button { padding:9px; font-size:.8rem; }
      .error { padding:12px 16px; border-radius:8px; background:var(--error-color, #c62828); color:white; margin-bottom:16px; }
      .empty { text-align:center; }
      @media(max-width:600px) { .timer-grid { grid-template-columns:1fr; gap:12px; } }
      @media(max-width:480px) { .wrap { padding:16px 10px 40px; } .card { padding:16px; } .controls { gap:4px; } .controls button { min-width:42px; padding:8px; } }
    </style><div class="wrap"><header><h1>FusionEdge</h1>${devicePicker}</header>${this._error ? `<div class="error" role="alert">${escapeHtml(this._error)}</div>` : ""}${body}</div>`;
    const list = this.shadowRoot.querySelector(".station-list");
    if (list) list.scrollTop = oldScroll;
  }
}

if (!customElements.get("fusionedge-panel")) {
  customElements.define("fusionedge-panel", FusionEdgePanel);
}
