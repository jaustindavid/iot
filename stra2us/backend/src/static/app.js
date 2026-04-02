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

// 1. Dashboard / Stats
async function fetchStats() {
    const data = await fetchAPI('/stats');
    
    document.getElementById('queueCount').innerText = data.queues.length;
    document.getElementById('kvCount').innerText = data.kvs.length;

    const qList = document.getElementById('queueList');
    qList.innerHTML = data.queues.map(q => `
        <div class="data-item">
            <div><strong>${q.topic}</strong> (Msg Count: ${q.count})</div>
            <div>
                <button class="btn-sm" onclick="peekData('q', '${q.topic}')">Peek</button>
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
async function fetchKeys() {
    const clients = await fetchAPI('/keys');
    const tbody = document.getElementById('clientsTableBody');
    tbody.innerHTML = clients.map(c => `
        <tr>
            <td><strong>${c.client_id}</strong></td>
            <td><span class="badge">${c.acl.read_write === '*' ? 'Full Access' : c.acl.read_write}</span></td>
            <td>
                <button class="btn-sm danger" onclick="revokeClient('${c.client_id}')">Revoke</button>
            </td>
        </tr>
    `).join('');
}

document.getElementById('keyForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const clientId = document.getElementById('newClientId').value;
    const acl = document.getElementById('aclSelect').value;
    
    const res = await fetchAPI('/keys', 'POST', { client_id: clientId, acl_read_write: acl });
    
    const display = document.getElementById('newSecretDisplay');
    display.innerHTML = `<strong>Success!</strong> Secret key generated.<br><br>
                         Client ID: <code>${res.client_id}</code><br>
                         Key (Hex): <code>${res.secret}</code><br><br>
                         <small style="color:var(--accent-danger);">Warning: This key will not be displayed again.</small>`;
    display.classList.remove('hidden');
    
    document.getElementById('newClientId').value = '';
    fetchKeys();
});

async function revokeClient(id) {
    if(!confirm(`Revoke client ${id}? This action cannot be undone.`)) return;
    await fetchAPI(`/keys/${id}`, 'DELETE');
    fetchKeys();
}

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

// Init
fetchStats();
