"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const vm = require("node:vm");

const source = fs.readFileSync(
  path.join(__dirname, "../custom_components/fusionedge/frontend/panel.js"),
  "utf8"
);

function createPanel() {
  const elements = new Map();
  class FakeElement {
    attachShadow() {
      this.shadowRoot = {
        innerHTML: "",
        addEventListener() {},
        querySelector() { return null; },
      };
    }
  }
  vm.runInNewContext(source, {
    HTMLElement: FakeElement,
    customElements: {
      get: (name) => elements.get(name),
      define: (name, element) => elements.set(name, element),
    },
  });
  const Panel = elements.get("fusionedge-panel");
  const panel = new Panel();
  panel.isConnected = true;
  return panel;
}

test("discovers renamed entities and groups controls for each radio", async () => {
  const panel = createPanel();
  const services = [];
  const registry = [
    ["media_player.user_renamed", "A1_player"],
    ["select.my_bt_route", "A1_bt_output"],
    ["switch.clock", "A1_clock_tts"],
    ["switch.clock_no_stream", "A1_clock_tts_no_stream"],
    ["light.strip", "A1_led_strip"],
    ["switch.led_screensaver", "A1_led_strip_screensaver"],
    ["text.weekday_on", "A1_timer_weekday_start"],
    ["text.weekday_off", "A1_timer_weekday_stop"],
    ["text.weekend_on", "A1_timer_weekend_start"],
    ["text.weekend_off", "A1_timer_weekend_stop"],
    ["media_player.second", "B2_player"],
  ].map(([entity_id, unique_id]) => ({ entity_id, unique_id, platform: "fusionedge", disabled_by: null }));
  const states = {
    "media_player.user_renamed": {
      state: "playing",
      attributes: {
        friendly_name: "Radio <One>", media_channel: "Station & More",
        media_title: "Title <script>", volume_level: 0.5,
        source: "WEB", source_list: ["WEB", "SD"],
      },
    },
    "select.my_bt_route": { state: "Bluetooth only", attributes: { options: ["Bluetooth only", "Local only"] } },
    "switch.clock": { state: "on", attributes: {} },
    "switch.clock_no_stream": { state: "off", attributes: {} },
    "light.strip": { state: "on", attributes: { effect: "Standard Stereo", effect_list: ["Standard Stereo", "Twinkle"] } },
    "switch.led_screensaver": { state: "on", attributes: {} },
    "text.weekday_on": { state: "07:30", attributes: {} },
    "text.weekday_off": { state: "off", attributes: {} },
    "text.weekend_on": { state: "09:00", attributes: {} },
    "text.weekend_off": { state: "23:00", attributes: {} },
    "media_player.second": { state: "idle", attributes: { friendly_name: "Second", source: "SD", source_list: ["WEB", "SD"] } },
  };
  panel.hass = {
    language: "hu", states,
    callWS: async (message) => {
      if (message.type === "config/entity_registry/list") return registry;
      if (message.type === "media_player/browse_media") return {
        children: [{ title: "Station <2>", media_content_type: "channel", media_content_id: "web:2", can_play: true }],
      };
      throw Error("Unexpected WS call");
    },
    callService: async (domain, service, data) => services.push({ domain, service, data }),
  };
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(panel._devices.length, 2);
  assert.match(panel.shadowRoot.innerHTML, /Radio &lt;One&gt;/);
  assert.match(panel.shadowRoot.innerHTML, /Title &lt;script&gt;/);
  assert.doesNotMatch(panel.shadowRoot.innerHTML, /Title <script>/);
  assert.match(panel.shadowRoot.innerHTML, /data-action="power" class="power is-on"[^>]*><svg viewBox="0 0 24 24"/);
  assert.doesNotMatch(panel.shadowRoot.innerHTML, /⏻/);
  panel._onClick({ target: { closest: () => ({ dataset: { action: "stop" }, disabled: false }) } });
  assert.deepEqual(JSON.parse(JSON.stringify(services[0])), {
    domain: "media_player", service: "media_stop", data: { entity_id: "media_player.user_renamed" },
  });
  await panel._browse(panel._currentDevice());
  assert.match(panel.shadowRoot.innerHTML, /Station &lt;2&gt;/);
  panel._onClick({ target: { closest: () => ({ dataset: { action: "station", index: "0" }, disabled: false }) } });
  assert.deepEqual(JSON.parse(JSON.stringify(services[1])), {
    domain: "media_player", service: "play_media",
    data: { entity_id: "media_player.user_renamed", media_content_type: "channel", media_content_id: "web:2" },
  });
  panel._onClick({ target: { closest: () => ({ dataset: { action: "clock-no-stream" }, disabled: false }) } });
  panel._onClick({ target: { closest: () => ({ dataset: { action: "led-screensaver" }, disabled: false }) } });
  assert.deepEqual(JSON.parse(JSON.stringify(services.slice(2))), [
    { domain: "switch", service: "turn_on", data: { entity_id: "switch.clock_no_stream" } },
    { domain: "switch", service: "turn_off", data: { entity_id: "switch.led_screensaver" } },
  ]);
  assert.match(panel.shadowRoot.innerHTML, /Csak ha nincs lejátszás/);
  assert.match(panel.shadowRoot.innerHTML, /Képernyővédő alatt is/);
  assert.match(panel.shadowRoot.innerHTML, /id="timer-weekday_start"[^>]*value="07:30"/);
  assert.match(panel.shadowRoot.innerHTML, /id="timer-weekday_stop"[^>]*value=""/);
  panel._onChange({ target: { id: "timer-weekday_start", dataset: { timerField: "weekday_start" }, value: "06:45", disabled: false } });
  panel._onClick({ target: { closest: () => ({ dataset: { action: "timer-off", field: "weekend_start" }, disabled: false }) } });
  assert.deepEqual(JSON.parse(JSON.stringify(services.slice(4))), [
    { domain: "text", service: "set_value", data: { entity_id: "text.weekday_on", value: "06:45" } },
    { domain: "text", service: "set_value", data: { entity_id: "text.weekend_on", value: "off" } },
  ]);
  panel.hass = {
    ...panel._hass,
    states: {
      ...states,
      "switch.clock_no_stream": { state: "on", attributes: {} },
      "switch.led_screensaver": { state: "unavailable", attributes: {} },
    },
  };
  assert.match(panel.shadowRoot.innerHTML, /data-action="clock-no-stream" class="toggle active"/);
  assert.match(panel.shadowRoot.innerHTML, /data-action="led-screensaver"[^>]*disabled/);
  assert.match(panel.shadowRoot.innerHTML, /Firmware-frissítés után használható/);
  panel.hass = {
    ...panel._hass,
    states: {
      ...states,
      "media_player.user_renamed": { ...states["media_player.user_renamed"], state: "off" },
    },
  };
  assert.match(panel.shadowRoot.innerHTML, /data-action="power" class="power "/);
});
