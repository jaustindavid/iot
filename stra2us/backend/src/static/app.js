const API_BASE = '/api/admin';

// Navigation Logic
document.querySelectorAll('.nav-links a').forEach(link => {
    link.addEventListener('click', (e) => {
        e.preventDefault();
        
        // Update active class
        document.querySelectorAll('.nav-links a').forEach(l => l.classList.remove('active'));
        e.target.classList.add('active');
        
        // Show correct view
        const targetId = e.target.getAttribute('data-target');
        document.querySelectorAll('.view').forEach(v => v.classList.remove('active-view'));
        document.getElementById(targetId).classList.add('active-view');

        // Fetch specifics immediately on tab switch
        if (targetId === 'dashboard') fetchStats();
        if (targetId === 'keys') fetchKeys();
        if (targetId === 'logs') fetchLogs();
    });
});

// Helper for HTTP requests
async function fetchAPI(endpoint, method = 'GET', body = null) {
    const options = { method, headers: { 'Content-Type': 'application/json' } };
    if (body) options.body = JSON.stringify(body);
    const res = await fetch(`${API_BASE}${endpoint}`, options);
    return res.json();
}

// Format Time
function formatTime(unixTime) {
    const d = new Date(unixTime * 1000);
    return `${d.toLocaleDateString()} ${d.toLocaleTimeString()}`;
}

// 0. Redis Health Check
async function checkRedisStatus() {
    try {
        const data = await fetchAPI('/stats'); // Stats relies on Redis
        updateStatus(true);
        return data;
    } catch (e) {
        updateStatus(false);
        return null;
    }
}

function updateStatus(isOnline) {
    const dot = document.querySelector('.status-dot');
    const text = document.getElementById('redisStatus');
    if (isOnline) {
        dot.className = 'status-dot online';
        text.innerText = 'Connected';
        text.style.color = 'var(--accent-success)';
    } else {
        dot.className = 'status-dot offline';
        text.innerText = 'Offline';
        text.style.color = 'var(--accent-danger)';
    }
}

// 1. Dashboard / Stats
async function fetchStats() {
    const data = await checkRedisStatus();
    if (!data) {
        document.getElementById('queueCount').innerText = '--';
        document.getElementById('kvCount').innerText = '--';
        document.getElementById('queueList').innerHTML = '<div class="text-muted">Redis is unreachable. Please check the server logs.</div>';
        document.getElementById('kvList').innerHTML = '<div class="text-muted">Redis is unreachable.</div>';
        return;
    }
    
    document.getElementById('queueCount').innerText = data.queues.length;
    document.getElementById('kvCount').innerText = data.kvs.length;

    const qList = document.getElementById('queueList');
    qList.innerHTML = data.queues.map(q => `
        <div class="data-item">
            <div><strong>${q.topic}</strong> (Msg Count: ${q.count})</div>
            <div>
                <button class="btn-sm" onclick="peekData('q', '${q.topic}')">Peek</button>
                <button class="btn-sm" onclick="openMonitor('${q.topic}')">Monitor</button>
                <button class="btn-sm danger" onclick="deleteData('q', '${q.topic}')">Delete</button>
            </div>
        </div>
    `).join('') || '<div class="text-muted">No active queues</div>';

    const kvList = document.getElementById('kvList');
    kvList.innerHTML = data.kvs.map(k => `
        <div class="data-item">
            <div><strong>${k.key}</strong></div>
            <div>
                <button class="btn-sm" onclick="peekData('kv', '${k.key}')">Read</button>
                <button class="btn-sm danger" onclick="deleteData('kv', '${k.key}')">Delete</button>
            </div>
        </div>
    `).join('') || '<div class="text-muted">No active KV pairs</div>';
}

async function peekData(type, keyId) {
    const data = await fetchAPI(`/peek/${type}/${keyId}`);
    
    const modal = document.getElementById('peekModal');
    const title = document.getElementById('peekTitle');
    const code = document.getElementById('peekDataContent');
    
    title.innerText = `Data: ${type}/${keyId}`;
    if (data.status === 'empty') {
        code.innerText = 'Empty / Value Not Found';
    } else {
        const decoded = JSON.stringify(data.message, null, 2);
        code.innerText = `Decoded MessagePack:\n${decoded}\n\nHex Format:\n${data.hex}`;
    }
    
    modal.style.display = 'block';
}

async function deleteData(type, keyId) {
    if(!confirm(`Are you sure you want to delete ${type}/${keyId}?`)) return;
    await fetchAPI(`/${type}/${keyId}`, 'DELETE');
    fetchStats();
}

// Close Modal
document.querySelector('.close-btn').addEventListener('click', () => {
    document.getElementById('peekModal').style.display = 'none';
});

// 2. Key Management
let allClientsData = {};

function formatAclSummary(acl) {
    const perms = acl.permissions || [];
    if (perms.length === 0)
        return '<span class="badge" style="background:var(--accent-danger);color:#fff;">No Access</span>';
    return perms.map(p =>
        `<span class="badge" style="background:${p.access==='rw' ? 'rgba(0,240,255,0.15)' : 'rgba(138,43,226,0.15)'};
               color:${p.access==='rw' ? 'var(--accent-blue)' : 'var(--accent-purple)'};
               border:1px solid ${p.access==='rw' ? 'var(--accent-blue)' : 'var(--accent-purple)'};
               font-size:0.75rem; margin-right:4px;">
            ${p.prefix}&thinsp;:&thinsp;${p.access}</span>`
    ).join('');
}

async function fetchKeys() {
    const clients = await fetchAPI('/keys');
    allClientsData = {};
    clients.forEach(c => allClientsData[c.client_id] = c);

    const tbody = document.getElementById('clientsTableBody');
    tbody.innerHTML = clients.map(c => `
        <tr>
            <td><strong>${c.client_id}</strong></td>
            <td>${formatAclSummary(c.acl)}</td>
            <td style="white-space:nowrap;">
                <button class="btn-sm" onclick="openAclModal('${c.client_id}')">Edit ACL</button>
                <button class="btn-sm danger" onclick="revokeClient('${c.client_id}')">Revoke</button>
            </td>
        </tr>
    `).join('');
}

document.getElementById('keyForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const clientId = document.getElementById('newClientId').value.trim();
    const res = await fetchAPI('/keys', 'POST', { client_id: clientId });

    const display = document.getElementById('newSecretDisplay');
    display.innerHTML = `<strong>Success!</strong> Secret key generated.<br><br>
                         Client ID: <code>${res.client_id}</code><br>
                         Key (Hex): <code>${res.secret}</code><br><br>
                         <small style="color:var(--accent-danger);">Warning: This key will not be displayed again.</small><br><br>
                         <small style="color:var(--text-muted);">Client has <strong>no access</strong> by default. Click Edit ACL to add permissions.</small>`;
    display.classList.remove('hidden');
    document.getElementById('newClientId').value = '';
    fetchKeys();
});

async function revokeClient(id) {
    if(!confirm(`Revoke client ${id}? This action cannot be undone.`)) return;
    await fetchAPI(`/keys/${id}`, 'DELETE');
    fetchKeys();
}

// --- ACL Editor ---
let aclEditingClientId = null;
let aclCurrentPermissions = [];
let aclNewAccess = 'rw';

function openAclModal(clientId) {
    const client = allClientsData[clientId];
    aclEditingClientId = clientId;
    aclCurrentPermissions = ((client.acl || {}).permissions || []).map(p => ({...p}));
    aclNewAccess = 'rw';

    document.getElementById('aclClientName').textContent = clientId;
    document.getElementById('aclNewPrefix').value = '';
    const toggle = document.getElementById('aclNewAccessToggle');
    toggle.textContent = 'rw';
    toggle.className = 'acl-access-toggle access-rw';

    renderAclPermissions();
    document.getElementById('aclModal').style.display = 'block';
}

function closeAclModal() {
    document.getElementById('aclModal').style.display = 'none';
    aclEditingClientId = null;
    aclCurrentPermissions = [];
}

function renderAclPermissions() {
    const list = document.getElementById('aclPermissionsList');
    if (aclCurrentPermissions.length === 0) {
        list.innerHTML = '<div class="text-muted acl-empty">No rules &mdash; client has no access to anything.</div>';
        return;
    }
    list.innerHTML = aclCurrentPermissions.map((p, i) => `
        <div class="acl-rule-row">
            <span class="acl-prefix-label">${p.prefix}</span>
            <button class="acl-access-toggle ${p.access === 'rw' ? 'access-rw' : 'access-r'}"
                    onclick="aclToggleAccess(${i})">${p.access}</button>
            <button class="btn-sm danger" onclick="aclRemoveRule(${i})">&#x2715;</button>
        </div>
    `).join('');
}

function aclToggleAccess(index) {
    aclCurrentPermissions[index].access =
        aclCurrentPermissions[index].access === 'rw' ? 'r' : 'rw';
    renderAclPermissions();
}

function aclRemoveRule(index) {
    aclCurrentPermissions.splice(index, 1);
    renderAclPermissions();
}

function toggleNewAccess() {
    aclNewAccess = aclNewAccess === 'rw' ? 'r' : 'rw';
    const btn = document.getElementById('aclNewAccessToggle');
    btn.textContent = aclNewAccess;
    btn.className = `acl-access-toggle ${aclNewAccess === 'rw' ? 'access-rw' : 'access-r'}`;
}

function aclAddRule() {
    const prefix = document.getElementById('aclNewPrefix').value.trim();
    if (!prefix) { document.getElementById('aclNewPrefix').focus(); return; }
    // Prevent duplicates
    if (aclCurrentPermissions.some(p => p.prefix === prefix)) {
        document.getElementById('aclNewPrefix').select();
        return;
    }
    aclCurrentPermissions.push({ prefix, access: aclNewAccess });
    document.getElementById('aclNewPrefix').value = '';
    renderAclPermissions();
}

async function saveAcl() {
    await fetchAPI(`/keys/${aclEditingClientId}/acl`, 'PUT', {
        permissions: aclCurrentPermissions
    });
    closeAclModal();
    fetchKeys();
}

document.getElementById('aclModalClose').addEventListener('click', closeAclModal);
document.getElementById('aclNewPrefix').addEventListener('keydown', e => {
    if (e.key === 'Enter') { e.preventDefault(); aclAddRule(); }
});

// 3. Activity Logs
async function fetchLogs() {
    const logs = await fetchAPI('/logs?limit=50');
    const tbody = document.getElementById('logsTableBody');
    tbody.innerHTML = logs.map(l => `
        <tr>
            <td style="color:var(--text-muted);">${formatTime(l.timestamp)}</td>
            <td style="color:var(--accent-blue);">${l.client_id}</td>
            <td>${l.action}</td>
            <td style="color:${l.status.startsWith('Success') ? 'var(--accent-success)' : 'var(--accent-danger)'}">${l.status}</td>
        </tr>
    `).join('');
}

// Polling for real-time updates (every 5 seconds)
setInterval(() => {
    const activeViewId = document.querySelector('.active-view').id;
    if (activeViewId === 'dashboard') fetchStats();
    if (activeViewId === 'logs') fetchLogs();
}, 5000);

// 4. Backup / Restore
async function downloadBackup() {
    const res = await fetch(`${API_BASE}/keys/backup`);
    if (!res.ok) {
        alert('Backup failed. Check the server logs.');
        return;
    }
    const blob = await res.blob();
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'stra2us_backup.json';
    a.click();
    URL.revokeObjectURL(url);
}

async function uploadRestore() {
    const fileInput = document.getElementById('restoreFile');
    const force = document.getElementById('forceRestore').checked;
    const resultDiv = document.getElementById('restoreResult');

    if (!fileInput.files.length) {
        alert('Please select a backup file first.');
        return;
    }

    const text = await fileInput.files[0].text();
    let payload;
    try {
        payload = JSON.parse(text);
    } catch (e) {
        alert('Invalid JSON file. Please select a valid backup.');
        return;
    }

    const res = await fetch(`${API_BASE}/keys/restore?force=${force}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
    });

    const data = await res.json();
    resultDiv.classList.remove('hidden');
    resultDiv.innerHTML = `
        <strong>Restore complete!</strong><br><br>
        ✅ Restored: <strong>${data.restored.length}</strong> clients<br>
        ⏭ Skipped (already exist): <strong>${data.skipped.length}</strong> clients<br>
        ♻️ Overwritten: <strong>${data.overwritten.length}</strong> clients
        ${data.restored.length ? `<br><br><small>New: ${data.restored.join(', ')}</small>` : ''}
        ${data.overwritten.length ? `<br><small>Overwritten: ${data.overwritten.join(', ')}</small>` : ''}
    `;
}

// 5. Topic Monitor
const MONITOR_COLORS = [
    '#6c63ff','#00c2ff','#f5a623','#43e97b','#ff6b6b',
    '#a18cd1','#fda085','#84fab0','#f093fb','#4facfe',
];
const monitorClientColors = {};
let monitorInterval = null;
let monitorSeenIds = new Set();
let monitorActive = false;

function monitorClientColor(clientId) {
    if (!monitorClientColors[clientId]) {
        const idx = Object.keys(monitorClientColors).length % MONITOR_COLORS.length;
        monitorClientColors[clientId] = MONITOR_COLORS[idx];
    }
    return monitorClientColors[clientId];
}

function monitorFormatData(data) {
    if (data === null || data === undefined) return '<em>null</em>';
    if (typeof data === 'object') return JSON.stringify(data);
    return String(data);
}

async function monitorPoll() {
    const topic = document.getElementById('monitorTopic').value.trim();
    if (!topic) return;

    const res = await fetch(`${API_BASE}/stream/q/${topic}`);
    if (!res.ok) return;
    const messages = await res.json();

    const feed = document.getElementById('monitorFeed');
    let addedAny = false;

    // messages arrive newest-first from XREVRANGE; reverse so we prepend
    // oldest-first, ending with the newest entry at the top of the feed.
    for (const msg of [...messages].reverse()) {
        if (monitorSeenIds.has(msg.id)) continue;
        monitorSeenIds.add(msg.id);
        addedAny = true;

        const color = monitorClientColor(msg.client_id);
        const d = new Date(msg.received_at * 1000);
        const ts = d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
        const dataStr = monitorFormatData(msg.data);

        const row = document.createElement('div');
        row.className = 'monitor-row monitor-row-new';
        row.innerHTML = `
            <span class="monitor-ts">${ts}</span>
            <span class="monitor-badge" style="background:${color}22; color:${color}; border-color:${color}44;">${msg.client_id}</span>
            <span class="monitor-data">${dataStr}</span>
        `;
        // Prepend so newest is at top
        feed.insertBefore(row, feed.firstChild);

        // Remove animation class after it plays
        setTimeout(() => row.classList.remove('monitor-row-new'), 600);
    }

    // Cap feed at 200 rows
    while (feed.children.length > 200) {
        feed.removeChild(feed.lastChild);
    }
}

function openMonitor(topic) {
    // Navigate to the monitor view
    document.querySelectorAll('.nav-links a').forEach(a => a.classList.remove('active'));
    document.querySelectorAll('.view').forEach(v => v.classList.remove('active-view'));
    const monitorLink = document.querySelector('.nav-links a[data-target="monitor"]');
    if (monitorLink) monitorLink.classList.add('active');
    document.getElementById('monitor').classList.add('active-view');

    // Stop any existing session, pre-fill topic, start fresh
    if (monitorActive) monitorStop();
    monitorClear();
    document.getElementById('monitorTopic').value = topic;
    monitorStart();
}

function monitorStart() {
    const topic = document.getElementById('monitorTopic').value.trim();
    if (!topic) { document.getElementById('monitorTopic').focus(); return; }

    monitorActive = true;
    document.getElementById('monitorFeedTitle').textContent = `q/${topic}`;
    document.getElementById('monitorStartBtn').style.display = 'none';
    document.getElementById('monitorStopBtn').style.display = '';
    const status = document.getElementById('monitorStatus');
    status.textContent = 'Live';
    status.className = 'monitor-status-live';

    monitorPoll();
    monitorInterval = setInterval(monitorPoll, 2000);
}

function monitorStop() {
    clearInterval(monitorInterval);
    monitorInterval = null;
    monitorActive = false;
    document.getElementById('monitorStartBtn').style.display = '';
    document.getElementById('monitorStopBtn').style.display = 'none';
    const status = document.getElementById('monitorStatus');
    status.textContent = 'Stopped';
    status.className = 'monitor-status-off';
}

function monitorClear() {
    document.getElementById('monitorFeed').innerHTML = '';
    monitorSeenIds.clear();
}

// Stop monitor when navigating away
document.querySelectorAll('.nav-links a').forEach(link => {
    link.addEventListener('click', () => {
        if (monitorActive && link.dataset.target !== 'monitor') {
            monitorStop();
        }
    });
});

// Init
fetchStats();
