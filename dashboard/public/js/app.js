document.addEventListener('DOMContentLoaded', () => {
    const wsStatusText = document.getElementById('ws-status');
    const wsDot = document.getElementById('ws-dot');
    const terminalOutput = document.getElementById('terminal-output');
    const autoScrollCheckbox = document.getElementById('autoscroll');
    const nodeGrid = document.getElementById('node-grid');
    const tabs = document.querySelectorAll('.tab');
    const networkSection = document.getElementById('network-section');
    const terminalSection = document.getElementById('terminal-section');

    // Display state
    const nodeState = {}; 

    tabs.forEach(tab => {
        tab.addEventListener('click', () => {
            tabs.forEach(t => t.classList.remove('active'));
            tab.classList.add('active');
            const filter = tab.getAttribute('data-channel');
            
            if (filter === 'network') {
                networkSection.style.display = 'flex';
                terminalSection.style.display = 'none';
            } else {
                networkSection.style.display = 'none';
                terminalSection.style.display = 'flex';
                document.body.setAttribute('data-active-tab', filter);
                scrollToBottom();
            }
        });
    });

    let socket;
    function connect() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        socket = new WebSocket(`${protocol}//${window.location.host}`);

        socket.onopen = () => {
            wsStatusText.textContent = 'Connected';
            wsDot.classList.add('connected');
            addSystemLog('Connected to Luotsi Core Telemetry Stream');
        };

        socket.onclose = () => {
            wsStatusText.textContent = 'Disconnected - Retrying...';
            wsDot.classList.remove('connected');
            setTimeout(connect, 3000);
        };

        socket.onmessage = (event) => {
            try {
                const cloudEvent = JSON.parse(event.data);
                if (cloudEvent.type === 'luotsi.heartbeat') {
                    updateNodeNetwork(cloudEvent.data);
                } else {
                    processLogEvent(cloudEvent);
                }
            } catch (e) {
                console.error("Failed to parse event", e);
            }
        };
    }

    function updateNodeNetwork(hbData) {
        const { active_nodes, capabilities } = hbData;
        const now = Date.now();
        
        // Ensure all discovered nodes exist in dom
        for (const [nodeId, lastSeen] of Object.entries(active_nodes)) {
            if (!nodeState[nodeId]) {
                createNodeCard(nodeId);
                nodeState[nodeId] = true;
            }
            
            const card = document.getElementById(`node-card-${nodeId}`);
            if (card) {
                const statusBadgeText = card.querySelector('.status-text');
                
                // If seen in the last 5 seconds, active. Else idle.
                if (now - lastSeen < 5000) {
                    card.classList.add('active');
                    statusBadgeText.textContent = 'ACTIVE';
                } else {
                    card.classList.remove('active');
                    statusBadgeText.textContent = 'IDLE';
                }

                // Update Capabilities if they just arrived
                if (capabilities[nodeId]) {
                    const capsList = card.querySelector('.node-caps-list');
                    if (capsList && capsList.children.length === 0) {
                        renderCapabilities(capsList, capabilities[nodeId]);
                    }
                }
            }
        }
    }

    function createNodeCard(nodeId) {
        const card = document.createElement('div');
        card.className = 'node-card';
        card.id = `node-card-${nodeId}`;
        
        card.innerHTML = `
            <div class="node-header">
                <div class="node-title-group">
                    <span class="node-name">${nodeId}</span>
                    <span class="node-type">luotsi.node</span>
                </div>
                <div class="node-status-badge">
                    <div class="node-dot"></div>
                    <span class="status-text">IDLE</span>
                </div>
            </div>
            <div class="node-caps-title">Available Capabilities</div>
            <ul class="node-caps-list"></ul>
        `;
        nodeGrid.appendChild(card);
    }

    function renderCapabilities(ulElement, toolsArr) {
        ulElement.innerHTML = '';
        if (!toolsArr || toolsArr.length === 0) {
             ulElement.innerHTML = '<span class="cap-desc" style="font-style:italic">No explicit tools exposed.</span>';
             return;
        }
        toolsArr.forEach(tool => {
            const li = document.createElement('li');
            li.className = 'cap-item';
            li.innerHTML = `
                <span class="cap-name">${tool.name}</span>
                <span class="cap-desc">${tool.description || ''}</span>
            `;
            ulElement.appendChild(li);
        });
    }

    function syntaxHighlight(jsonObj) {
        if (typeof jsonObj !== 'string') jsonObj = JSON.stringify(jsonObj, undefined, 2);
        jsonObj = jsonObj.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
        return jsonObj.replace(/("(\\u[a-zA-Z0-9]{4}|\\[^u]|[^\\"])*"(\s*:)?|\b(true|false|null)\b|-?\d+(?:\.\d*)?(?:[eE][+\-]?\d+)?)/g, function (match) {
            let cls = 'number';
            if (/^"/.test(match)) {
                if (/:$/.test(match)) {
                    cls = 'key'; match = match.slice(0, -1) + '<span style="color:var(--text-main)">:</span>';
                } else { cls = 'string'; }
            } else if (/true|false/.test(match)) { cls = 'boolean'; } else if (/null/.test(match)) { cls = 'null'; }
            return '<span class="' + cls + '">' + match + '</span>';
        });
    }

    function processLogEvent(ce) {
        let type = 'system', badge = 'SYS', badgeClass = 'sys', routeHtml = '', payloadToRender = ce.data;
        const timeStr = new Date(ce.time).toLocaleTimeString([], { hour12: false, hour: '2-digit', minute: '2-digit', second:'2-digit', fractionalSecondDigits: 3 });

        if (ce.type === 'luotsi.message') {
            const payload = ce.data.payload || {};
            const src = ce.luotsisource || ce.data.source_id || 'unknown';
            const tgt = ce.luotsitarget || ce.data.target_id || 'unknown';
            routeHtml = `${src} <span style="opacity:0.5">→</span> ${tgt}`;

            const method = payload.method || '';
            const idstr = payload.id ? String(payload.id) : '';
            if (method.includes('tools/') || method.includes('resources/') || method.includes('prompts/') || method === 'initialize' || method === 'notifications/initialized' || idstr.includes('__luotsi__')) {
                type = 'mcp'; badgeClass = 'mcp'; badge = 'MCP';
            } else {
                type = 'jsonrpc'; badgeClass = 'rpc'; badge = 'RPC';
            }
        }

        const logRow = document.createElement('div');
        logRow.className = `log-row`;
        logRow.setAttribute('data-type', type);
        logRow.innerHTML = `
            <div class="log-meta">
                <span class="time">[${timeStr}]</span>
                <span class="badge ${badgeClass}">${badge}</span>
                <span class="route-info">${routeHtml}</span>
            </div>
            <div class="json-payload">${syntaxHighlight(payloadToRender)}</div>
        `;
        terminalOutput.appendChild(logRow);
        if (terminalOutput.children.length > 500) terminalOutput.removeChild(terminalOutput.firstChild);
        scrollToBottom();
    }

    function addSystemLog(msg) {
        const logRow = document.createElement('div');
        logRow.className = 'log-row system';
        logRow.setAttribute('data-type', 'system');
        const timeStr = new Date().toLocaleTimeString([], { hour12: false, hour: '2-digit', minute: '2-digit', second:'2-digit' });
        logRow.innerHTML = `<span class="time" style="margin-right:8px">[${timeStr}]</span><span class="badge sys" style="margin-right:8px">SYS</span><span class="message" style="color:var(--text-main)">${msg}</span>`;
        terminalOutput.appendChild(logRow);
        scrollToBottom();
    }

    function scrollToBottom() {
        if (autoScrollCheckbox.checked) terminalOutput.scrollTop = terminalOutput.scrollHeight;
    }

    connect();
});
