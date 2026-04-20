document.addEventListener('DOMContentLoaded', () => {
    const wsStatusText = document.getElementById('ws-status');
    const wsDot = document.getElementById('ws-dot');
    const terminalOutput = document.getElementById('terminal-output');
    const autoScrollCheckbox = document.getElementById('autoscroll');
    const nodeGrid = document.getElementById('node-grid');
    const tabs = document.querySelectorAll('.tab');
    const networkSection = document.getElementById('network-section');
    const terminalSection = document.getElementById('terminal-section');
    const observabilitySection = document.getElementById('observability-section');
    const sessionTableBody = document.getElementById('session-table-body');
    const drawerOverlay = document.getElementById('drawer-overlay');
    const traceTimeline = document.getElementById('trace-timeline');
    const detailContent = document.getElementById('detail-content');
    const closeDrawerBtn = document.getElementById('close-drawer');

    // Trace & Log state
    const nodeState = {}; 
    const traceStore = {}; // traceId -> { id, startTime, masterAgent, status, events: [] }
    const eventIndex = {}; // eventId -> ce object

    tabs.forEach(tab => {
        tab.addEventListener('click', () => {
            tabs.forEach(t => t.classList.remove('active'));
            tab.classList.add('active');
            const filter = tab.getAttribute('data-channel');
            
            if (filter === 'network') {
                networkSection.style.display = 'flex';
                terminalSection.style.display = 'none';
                observabilitySection.style.display = 'none';
            } else if (filter === 'observability') {
                networkSection.style.display = 'none';
                terminalSection.style.display = 'none';
                observabilitySection.style.display = 'flex';
                renderSessionsTable();
            } else {
                networkSection.style.display = 'none';
                terminalSection.style.display = 'flex';
                observabilitySection.style.display = 'none';
                document.body.setAttribute('data-active-tab', filter);
                scrollToBottom();
            }
        });
    });

    closeDrawerBtn.addEventListener('click', () => {
        drawerOverlay.classList.remove('active');
    });

    drawerOverlay.addEventListener('click', (e) => {
        if (e.target === drawerOverlay) {
            drawerOverlay.classList.remove('active');
        }
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

                // Update Capabilities if they changed
                if (capabilities[nodeId]) {
                    const currentCount = card.getAttribute('data-cap-count') || "0";
                    if (String(capabilities[nodeId].length) !== currentCount) {
                        const capsList = card.querySelector('.node-caps-list');
                        if (capsList) {
                            renderCapabilities(capsList, capabilities[nodeId]);
                            card.setAttribute('data-cap-count', capabilities[nodeId].length);
                        }
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
        eventIndex[ce.id] = ce;
        
        // Grouping logic for observability
        const traceId = ce.traceparent ? ce.traceparent.split('-')[1] : null;
        if (traceId) {
            if (!traceStore[traceId]) {
                traceStore[traceId] = {
                    id: traceId,
                    startTime: ce.time,
                    masterAgent: 'pending...',
                    status: 'OK',
                    events: []
                };
            }
            traceStore[traceId].events.push(ce);
            
            // Try to identify master agent and status from spans
            if (ce.type === 'luotsi.telemetry.span') {
                if (ce.data.attributes && ce.data.attributes['gen_ai.agent.name']) {
                    traceStore[traceId].masterAgent = ce.data.attributes['gen_ai.agent.name'];
                }
                if (ce.data.status === 'ERROR') {
                    traceStore[traceId].status = 'ERROR';
                }
            }

            if (observabilitySection.style.display === 'flex') {
                renderSessionsTable();
            }
        }

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
        } else if (ce.type === 'luotsi.telemetry.span') {
            type = 'span'; badgeClass = 'span'; badge = 'SPAN';
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

    function renderSessionsTable() {
        sessionTableBody.innerHTML = '';
        const sortedTraces = Object.values(traceStore).sort((a, b) => new Date(b.startTime) - new Date(a.startTime));
        
        sortedTraces.forEach(trace => {
            const row = document.createElement('tr');
            row.className = 'session-row';
            row.onclick = () => openTraceDrawer(trace.id);
            
            const timeStr = new Date(trace.startTime).toLocaleString();
            const statusClass = trace.status === 'OK' ? 'success' : 'error';
            
            row.innerHTML = `
                <td>
                    <div class="status-cell">
                        <div class="status-indicator ${statusClass}"></div>
                        ${trace.status}
                    </div>
                </td>
                <td style="font-family:monospace; font-size:0.8rem;">${trace.id}</td>
                <td>${timeStr}</td>
                <td><span class="badge sys">${trace.masterAgent}</span></td>
                <td>${calculateTotalDelay(trace)}ms</td>
                <td>${trace.events.length} logs</td>
            `;
            sessionTableBody.appendChild(row);
        });
    }

    function calculateTotalDelay(trace) {
        if (trace.events.length < 2) return 0;
        const spans = trace.events.filter(e => e.type === 'luotsi.telemetry.span');
        return spans.reduce((acc, span) => acc + (span.data.duration_ms || 0), 0);
    }

    function openTraceDrawer(traceId) {
        const trace = traceStore[traceId];
        if (!trace) return;

        document.getElementById('drawer-trace-id').textContent = `Trace: ${traceId}`;
        document.getElementById('drawer-trace-time').textContent = new Date(trace.startTime).toLocaleString();
        
        traceTimeline.innerHTML = '';
        
        // Sort events within trace by time
        const sortedEvents = [...trace.events].sort((a, b) => new Date(a.time) - new Date(b.time));
        
        sortedEvents.forEach(ce => {
            const item = document.createElement('div');
            item.className = 'timeline-event';
            item.onclick = (e) => {
                e.stopPropagation();
                showEventDetail(ce.id);
            };
            
            let flowText = ce.type;
            if (ce.type === 'luotsi.message') {
                flowText = `${ce.luotsisource} → ${ce.luotsitarget}`;
            } else if (ce.type === 'luotsi.telemetry.span') {
                flowText = `L-SPAN: ${ce.data.name || 'route'}`;
            }

            const timeStr = new Date(ce.time).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second:'2-digit', fractionalSecondDigits: 3 });
            
            item.innerHTML = `
                <div class="event-dot"></div>
                <div class="event-content">
                    <div class="event-header">
                        <span style="color:var(--brand-primary); font-weight:700;">${ce.type.toUpperCase()}</span>
                        <span>${timeStr}</span>
                    </div>
                    <div class="event-flow">${flowText}</div>
                    ${ce.data.duration_ms ? `<div style="font-size:0.75rem; color:var(--rpc-green); margin-top:4px;">Latency: ${ce.data.duration_ms}ms</div>` : ''}
                </div>
            `;
            traceTimeline.appendChild(item);
        });

        drawerOverlay.classList.add('active');
    }

    function showEventDetail(eventId) {
        const ce = eventIndex[eventId];
        if (!ce) return;

        let metaHtml = '';
        if (ce.type === 'luotsi.telemetry.span' && ce.data.attributes) {
            metaHtml = '<div class="detail-meta-grid">';
            for (const [k, v] of Object.entries(ce.data.attributes)) {
                metaHtml += `
                    <div class="meta-item">
                        <span class="meta-label">${k}</span>
                        <span class="meta-val">${v}</span>
                    </div>
                `;
            }
            metaHtml += '</div>';
        }

        detailContent.innerHTML = `
            ${metaHtml}
            <div class="node-caps-title" style="margin-bottom:1rem;">Payload / Data</div>
            <div class="json-payload" style="max-height:600px; overflow:auto;">${syntaxHighlight(ce.data)}</div>
        `;
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
