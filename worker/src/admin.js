/**
 * Admin page HTML — inline JS, no external dependencies.
 * Manages locations and polling frequency via POST /admin.
 */
export function adminPageHtml() {
  return `<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Weather Display Admin</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: -apple-system, BlinkMacSystemFont, sans-serif; max-width: 700px; margin: 40px auto; padding: 0 20px; color: #222; }
    h1 { margin-bottom: 24px; }
    h2 { margin: 32px 0 12px; font-size: 18px; color: #555; }

    .card { background: #f8f8f8; border: 1px solid #ddd; border-radius: 8px; padding: 16px; margin-bottom: 12px; }
    .card .label { font-weight: 600; font-size: 16px; }
    .card .detail { color: #666; font-size: 14px; margin-top: 4px; }
    .card .actions { margin-top: 8px; }

    .empty { color: #999; font-style: italic; padding: 16px 0; }

    form { display: flex; flex-direction: column; gap: 8px; }
    .row { display: flex; gap: 8px; }
    .row input { flex: 1; }
    input, select { padding: 8px 12px; border: 1px solid #ccc; border-radius: 4px; font-size: 14px; }
    button { padding: 8px 16px; border: none; border-radius: 4px; cursor: pointer; font-size: 14px; font-weight: 600; }
    .btn-add { background: #2563eb; color: white; }
    .btn-add:hover { background: #1d4ed8; }
    .btn-remove { background: #ef4444; color: white; font-size: 12px; padding: 4px 10px; }
    .btn-remove:hover { background: #dc2626; }
    .btn-save { background: #16a34a; color: white; }
    .btn-save:hover { background: #15803d; }

    .usage { background: #fffbeb; border: 1px solid #fbbf24; border-radius: 8px; padding: 16px; margin-top: 16px; }
    .usage .line { font-size: 14px; margin: 4px 0; }
    .usage .warn { color: #b45309; font-weight: 600; }
    .usage .ok { color: #15803d; }

    .error { color: #ef4444; font-size: 14px; margin-top: 8px; }
    .preview-link { font-size: 13px; color: #2563eb; }
  </style>
</head>
<body>
  <h1>Weather Display Admin</h1>

  <h2>Locations</h2>
  <div id="locations"><span class="empty">Loading...</span></div>

  <h2>Add Location</h2>
  <form id="add-form">
    <div class="row">
      <input id="add-zip" placeholder="Zip code" required>
      <input id="add-label" placeholder="Label (e.g. Manhattan)">
    </div>
    <div class="row">
      <input id="add-lat" placeholder="Latitude" required>
      <input id="add-lon" placeholder="Longitude" required>
    </div>
    <button type="submit" class="btn-add">Add Location</button>
    <div id="add-error" class="error"></div>
  </form>

  <h2>Polling Frequency</h2>
  <form id="poll-form">
    <div class="row">
      <select id="poll-interval">
        <option value="3">Every 3 minutes</option>
        <option value="5">Every 5 minutes</option>
        <option value="10">Every 10 minutes</option>
        <option value="15">Every 15 minutes</option>
        <option value="30">Every 30 minutes</option>
      </select>
      <button type="submit" class="btn-save">Save</button>
    </div>
  </form>

  <div id="usage" class="usage" style="display:none"></div>

  <script>
    let state = { locations: [], pollInterval: 5 };

    async function load() {
      const res = await fetch('/admin/data');
      state = await res.json();
      render();
    }

    function render() {
      renderLocations();
      renderPollInterval();
      renderUsage();
    }

    function renderLocations() {
      const el = document.getElementById('locations');
      if (state.locations.length === 0) {
        el.innerHTML = '<span class="empty">No locations configured. Add one below.</span>';
        return;
      }
      el.innerHTML = state.locations.map(loc => \`
        <div class="card">
          <div class="label">\${loc.label} (\${loc.zip})</div>
          <div class="detail">\${loc.lat}, \${loc.lon}</div>
          <div class="detail">
            <a class="preview-link" href="/weather/\${loc.zip}.png" target="_blank">Preview PNG</a>
          </div>
          <div class="actions">
            <button class="btn-remove" onclick="removeLocation('\${loc.zip}')">Remove</button>
          </div>
        </div>
      \`).join('');
    }

    function renderPollInterval() {
      document.getElementById('poll-interval').value = String(state.pollInterval);
    }

    function renderUsage() {
      const el = document.getElementById('usage');
      const n = state.locations.length;
      const interval = state.pollInterval;
      if (n === 0) { el.style.display = 'none'; return; }

      el.style.display = 'block';
      const fetchesPerDay = Math.ceil(24 * 60 / interval);
      const owmCalls = n * fetchesPerDay;
      const kvWrites = n * fetchesPerDay; // render_updated every cycle, plus occasional render_png + render_hash
      const owmPct = Math.round(owmCalls / 1000 * 100);
      const kvPct = Math.round(kvWrites / 1000 * 100);

      const owmClass = owmPct > 80 ? 'warn' : 'ok';
      const kvClass = kvPct > 80 ? 'warn' : 'ok';

      el.innerHTML = \`
        <div class="line"><strong>Estimated daily usage</strong></div>
        <div class="line">\${n} location(s) × \${fetchesPerDay} fetches/day = <span class="\${owmClass}">\${owmCalls} OWM API calls</span> (of 1,000 free/day)</div>
        <div class="line">~<span class="\${kvClass}">\${kvWrites} KV writes</span> (of 1,000 free / 1M paid per day)</div>
        \${owmPct > 80 ? '<div class="line warn">⚠ Approaching OWM free tier limit. Consider increasing the polling interval.</div>' : ''}
      \`;
    }

    async function post(body) {
      const res = await fetch('/admin', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      return res.json();
    }

    async function removeLocation(zip) {
      if (!confirm('Remove ' + zip + '?')) return;
      const result = await post({ action: 'remove_location', zip });
      if (result.ok) {
        state.locations = result.locations;
        render();
      }
    }

    document.getElementById('add-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const errEl = document.getElementById('add-error');
      errEl.textContent = '';

      const result = await post({
        action: 'add_location',
        zip: document.getElementById('add-zip').value.trim(),
        lat: document.getElementById('add-lat').value.trim(),
        lon: document.getElementById('add-lon').value.trim(),
        label: document.getElementById('add-label').value.trim(),
      });

      if (result.error) {
        errEl.textContent = result.error;
      } else {
        state.locations = result.locations;
        document.getElementById('add-form').reset();
        render();
      }
    });

    document.getElementById('poll-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const minutes = parseInt(document.getElementById('poll-interval').value);
      const result = await post({ action: 'set_poll_interval', minutes });
      if (result.ok) {
        state.pollInterval = result.pollInterval;
        render();
      }
    });

    load();
  </script>
</body>
</html>`;
}
