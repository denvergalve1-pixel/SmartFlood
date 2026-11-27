/* SMARTFLOOD dashboard – polls the Django API every 5 seconds. */
(function () {
  "use strict";

  const body = document.body;
  const page = body.dataset.page;
  const API = {
    latest: body.dataset.apiLatest,
    history: body.dataset.apiHistory,
    alerts: body.dataset.apiAlerts,
  };
  const DEVICE_ID = body.dataset.device || "";
  API.devices = body.dataset.apiDevices;
  const REFRESH_MS = 5000;
  const COLORS = { NORMAL: "#2ecc71", WARNING: "#f4c430", DANGER: "#ef4444" };
  const $ = (id) => document.getElementById(id);
  let chart = null;

  // ---------------------------------------------------------- helpers
  // Adds ?device=<id> (and other params) so every API call is for the selected ESP32.
  function url(base, params) {
    const q = new URLSearchParams(params || {});
    if (DEVICE_ID) q.set("device", DEVICE_ID);
    const s = q.toString();
    return s ? base + "?" + s : base;
  }
  async function getJSON(url) {
    const res = await fetch(url, { cache: "no-store" });
    if (!res.ok) throw new Error("HTTP " + res.status);
    return res.json();
  }
  const fullTime = (iso) => (iso ? new Date(iso).toLocaleString() : "—");
  const shortTime = (iso) =>
    new Date(iso).toLocaleString([], { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit" });
  const setText = (id, text) => { const el = $(id); if (el) el.textContent = text; };
  const show = (id, visible) => { const el = $(id); if (el) el.hidden = !visible; };

  function ago(seconds) {
    if (seconds == null) return "";
    if (seconds < 60) return seconds + " s ago";
    if (seconds < 3600) return Math.floor(seconds / 60) + " min ago";
    if (seconds < 86400) return Math.floor(seconds / 3600) + " h ago";
    return Math.floor(seconds / 86400) + " d ago";
  }

  function setBadge(id, text, cls) {
    const el = $(id);
    if (!el) return;
    el.textContent = text;
    el.className = "badge " + cls;
  }

  function statusNote(status) {
    return {
      NORMAL: "Water level is safe.",
      WARNING: "Water is rising – stay alert.",
      DANGER: "Dangerous water level!",
    }[status] || "";
  }

  // ---------------------------------------------------------- latest
  async function refreshLatest() {
    const pill = $("conn-pill");
    try {
      const data = await getJSON(url(API.latest));
      const r = data.reading;
      show("empty-msg", page !== "history" && page !== "alerts" && !data.has_data);
      show("sample-banner", !!(r && r.is_sample));

      pill.textContent = !data.device ? "No device" : data.connected ? "ESP32 Connected" : "ESP32 Disconnected";
      pill.className = "pill " + (data.connected ? "pill-on" : "pill-off");

      if (!r) {
        setText("val-reading", "—");
        setBadge("val-status", "—", "badge-none");
        setBadge("val-conn", "Disconnected", "badge-disconnected");
        setText("val-status-note", "Waiting for ESP32 data.");
        setText("val-conn-note", "No readings received");
        setText("val-updated", "—");
        setText("val-ago", "");
        return;
      }
      setText("val-reading", r.sensor_value);
      setBadge("val-status", r.water_status, "badge-" + r.water_status.toLowerCase());
      setText("val-status-note", statusNote(r.water_status));
      setBadge("val-conn", data.connected ? "Connected" : "Disconnected",
               data.connected ? "badge-connected" : "badge-disconnected");
      setText("val-conn-note", data.connected
        ? "Receiving data"
        : r.is_sample ? "Sample data only" : "No data for over " + data.offline_after_seconds + " s");
      setText("val-updated", fullTime(r.created_at));
      setText("val-ago", ago(data.seconds_since_last));
    } catch (err) {
      pill.textContent = "Dashboard cannot reach server";
      pill.className = "pill pill-off";
    }
  }

  // ---------------------------------------------------------- chart
  function drawChart(readings) {
    const canvas = $("chart");
    if (!canvas) return;
    if (typeof Chart === "undefined") {
      setText("chart-msg", "Chart library could not load (internet access is needed for Chart.js).");
      show("chart-msg", true);
      return;
    }
    if (!readings.length) {
      setText("chart-msg", "Waiting for ESP32 data.");
      show("chart-msg", true);
    } else {
      show("chart-msg", false);
    }
    const labels = readings.map((r) => shortTime(r.created_at));
    const values = readings.map((r) => r.sensor_value);
    const pointColors = readings.map((r) => COLORS[r.water_status] || "#8fb0d8");

    if (!chart) {
      chart = new Chart(canvas, {
        type: "line",
        data: { labels, datasets: [{
          label: "Sensor value", data: values, borderColor: "#4fc3f7", backgroundColor: "rgba(79,195,247,.15)",
          fill: true, tension: 0.25, pointRadius: 3, pointBackgroundColor: pointColors,
        }] },
        options: {
          responsive: true, maintainAspectRatio: false, animation: false,
          plugins: { legend: { labels: { color: "#8fb0d8" } } },
          scales: {
            x: { ticks: { color: "#8fb0d8", maxTicksLimit: 8, maxRotation: 0 }, grid: { color: "#1d3a6b" } },
            y: { min: 0, suggestedMax: 4095, title: { display: true, text: "Raw value (0–4095)", color: "#8fb0d8" },
                 ticks: { color: "#8fb0d8" }, grid: { color: "#1d3a6b" } },
          },
        },
      });
    } else {
      chart.data.labels = labels;
      chart.data.datasets[0].data = values;
      chart.data.datasets[0].pointBackgroundColor = pointColors;
      chart.update();
    }
  }

  async function refreshHistory() {
    const select = $("limit-select");
    const limit = select ? select.value : 60;
    try {
      const data = await getJSON(url(API.history, { limit: limit }));
      drawChart(data.readings);
      const tbody = $("history-body");
      if (tbody) {
        tbody.textContent = "";
        data.readings.slice().reverse().forEach((r) => {
          const tr = document.createElement("tr");
          [fullTime(r.created_at), r.sensor_value].forEach((v) => {
            const td = document.createElement("td"); td.textContent = v; tr.appendChild(td);
          });
          const tdS = document.createElement("td");
          const b = document.createElement("span");
          b.className = "badge badge-" + r.water_status.toLowerCase();
          b.textContent = r.water_status;
          tdS.appendChild(b); tr.appendChild(tdS);
          const tdT = document.createElement("td");
          if (r.is_sample) { tdT.className = "tag-sample"; tdT.textContent = "SAMPLE"; }
          tr.appendChild(tdT);
          tbody.appendChild(tr);
        });
        if (page === "history") show("empty-msg", data.readings.length === 0);
      }
    } catch (err) { /* keep last view; the connection pill shows server problems */ }
  }

  // ---------------------------------------------------------- alerts
  async function refreshAlerts() {
    const list = $("alert-list");
    if (!list) return;
    const limit = page === "alerts" ? 100 : 5;
    try {
      const data = await getJSON(url(API.alerts, { limit: limit }));
      list.textContent = "";
      data.alerts.forEach((a) => {
        const li = document.createElement("li");
        li.className = "level-" + a.level;
        const b = document.createElement("span");
        b.className = "badge badge-" + a.level.toLowerCase();
        b.textContent = a.level;
        const msg = document.createElement("span");
        msg.textContent = a.message;
        const when = document.createElement("span");
        when.className = "when";
        when.textContent = fullTime(a.created_at);
        li.append(b, msg, when);
        list.appendChild(li);
      });
      if (page === "dashboard" && data.alerts.length === 0) {
        const li = document.createElement("li");
        li.textContent = "No alerts yet.";
        list.appendChild(li);
      }
      if (page === "alerts") {
        show("empty-msg", data.alerts.length === 0);
        setText("count-warning", data.alerts.filter((a) => a.level === "WARNING").length);
        setText("count-danger", data.alerts.filter((a) => a.level === "DANGER").length);
        setText("latest-alert", data.alerts.length ? data.alerts[0].level + " – " + fullTime(data.alerts[0].created_at) : "None");
      }
    } catch (err) { /* ignore */ }
  }

  // ---------------------------------------------------------- all devices overview
  async function refreshDevices() {
    const grid = $("device-grid");
    if (!grid) return;
    try {
      const data = await getJSON(API.devices);
      grid.textContent = "";
      if (!data.devices.length) {
        const p = document.createElement("p");
        p.className = "muted";
        p.textContent = "No devices yet.";
        grid.appendChild(p);
        return;
      }
      data.devices.forEach((d) => {
        const a = document.createElement("a");
        a.className = "device-card" + (String(d.id) === String(DEVICE_ID) ? " selected" : "");
        a.href = "?device=" + d.id;
        const name = document.createElement("strong");
        name.textContent = d.name;
        const badges = document.createElement("div");
        const conn = document.createElement("span");
        conn.className = "badge " + (d.connected ? "badge-connected" : "badge-disconnected");
        conn.textContent = d.connected ? "Connected" : "Disconnected";
        badges.appendChild(conn);
        if (d.reading) {
          const st = document.createElement("span");
          st.className = "badge badge-" + d.reading.water_status.toLowerCase();
          st.textContent = d.reading.water_status;
          st.style.marginLeft = "6px";
          badges.appendChild(st);
        }
        const note = document.createElement("p");
        note.className = "muted";
        note.textContent = d.reading
          ? "Value " + d.reading.sensor_value + " · " + ago(d.seconds_since_last)
          : "No readings yet";
        const loc = document.createElement("p");
        loc.className = "muted";
        loc.textContent = d.location || "";
        a.append(name, badges, note, loc);
        grid.appendChild(a);
      });
    } catch (err) { /* ignore */ }
  }

  // ---------------------------------------------------------- loop
  function tick() {
    refreshLatest();
    if (page === "dashboard") refreshDevices();
    if (page === "dashboard") { refreshHistory(); refreshAlerts(); }
    if (page === "history") refreshHistory();
    if (page === "alerts") refreshAlerts();
  }

  const devSelect = $("device-select");
  if (devSelect) devSelect.addEventListener("change", () => {
    window.location.search = "?device=" + encodeURIComponent(devSelect.value);
  });

  const select = $("limit-select");
  if (select) select.addEventListener("change", refreshHistory);

  tick();
  setInterval(tick, REFRESH_MS);
})();
