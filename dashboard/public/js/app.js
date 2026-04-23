document.addEventListener('DOMContentLoaded', () => {
    // ─── DOM References ────────────────────────────────────────────────────────
    const wsStatusText     = document.getElementById('ws-status');
    const wsDot            = document.getElementById('ws-dot');
    const terminalOutput   = document.getElementById('terminal-output');
    const autoScrollChk    = document.getElementById('autoscroll');
    const tabs             = document.querySelectorAll('.tab');
    const networkSection   = document.getElementById('network-section');
    const terminalSection  = document.getElementById('terminal-section');
    const observSection    = document.getElementById('observability-section');
    const sessionTableBody = document.getElementById('session-table-body');
    const drawerOverlay    = document.getElementById('drawer-overlay');
    const traceTimeline    = document.getElementById('trace-timeline');
    const detailContent    = document.getElementById('detail-content');
    const closeDrawerBtn   = document.getElementById('close-drawer');
    const svgEl            = document.getElementById('node-graph');
    const viewport         = document.getElementById('viewport');
    const svgEdgesG        = document.getElementById('graph-edges');
    const svgNodesG        = document.getElementById('graph-nodes');
    const graphEmptyState  = document.getElementById('graph-empty-state');
    const nodePanelOverlay = document.getElementById('node-panel-overlay');
    const closeNodePanel   = document.getElementById('close-node-panel');
    const statNodesCount   = document.getElementById('stat-nodes-count');
    const statEdgesCount   = document.getElementById('stat-edges-count');
    const statTracesCount  = document.getElementById('stat-traces-count');

    const HUB_ID = 'luotsi-hub';
    const HUB_RADIUS = 42;

    // ─── Graph State ───────────────────────────────────────────────────────────
    const nodePhysics   = {};   // nodeId → { x, y, vx, vy, pinned, layer }
    const nodeDisplay   = {};   // nodeId → { active, lastSeen, caps }
    const edgeMap       = {};   // `${src}→${tgt}` → { src, tgt, lastSeen, active }
    const traceStore    = {};   // traceId → session object
    const eventIndex    = {};   // eventId → CloudEvent

    let selectedNodeId  = null;
    let rafId           = null;
    let svgWidth        = 800;
    let svgHeight       = 600;

    // ─── Viewport Transform (Zoom / Pan) ──────────────────────────────────────
    const vt = { x: 0, y: 0, scale: 1 };

    function applyViewport() {
        viewport.setAttribute('transform', `translate(${vt.x},${vt.y}) scale(${vt.scale})`);
    }

    // Wheel → zoom centered on cursor
    svgEl.addEventListener('wheel', e => {
        e.preventDefault();
        const rect    = svgEl.getBoundingClientRect();
        const mouseX  = e.clientX - rect.left;
        const mouseY  = e.clientY - rect.top;
        const delta   = e.deltaY < 0 ? 1.1 : 0.909;
        const newScale = Math.min(4, Math.max(0.15, vt.scale * delta));
        // Zoom toward mouse position
        vt.x = mouseX - (mouseX - vt.x) * (newScale / vt.scale);
        vt.y = mouseY - (mouseY - vt.y) * (newScale / vt.scale);
        vt.scale = newScale;
        applyViewport();
    }, { passive: false });

    // Background drag → pan
    let panning = false, panStart = { x: 0, y: 0 }, panOrigin = { x: 0, y: 0 };

    svgEl.addEventListener('mousedown', e => {
        if (e.target !== svgEl && e.target !== viewport) return;
        panning = true;
        panStart  = { x: e.clientX, y: e.clientY };
        panOrigin = { x: vt.x, y: vt.y };
        svgEl.style.cursor = 'grabbing';
    });

    window.addEventListener('mousemove', e => {
        if (!panning) return;
        vt.x = panOrigin.x + (e.clientX - panStart.x);
        vt.y = panOrigin.y + (e.clientY - panStart.y);
        applyViewport();
    });

    window.addEventListener('mouseup', () => {
        if (panning) { panning = false; svgEl.style.cursor = 'default'; }
    });

    // Convert client coords → SVG world coords (accounting for viewport transform)
    function toWorld(clientX, clientY) {
        const rect = svgEl.getBoundingClientRect();
        return {
            x: (clientX - rect.left - vt.x) / vt.scale,
            y: (clientY - rect.top  - vt.y) / vt.scale
        };
    }

    // ─── SVG Resize Observer ──────────────────────────────────────────────────
    const resizeObserver = new ResizeObserver(() => {
        const rect = svgEl.getBoundingClientRect();
        svgWidth  = rect.width  || 800;
        svgHeight = rect.height || 600;
        recomputeLayers();
    });
    resizeObserver.observe(svgEl);
    setTimeout(() => {
        const rect = svgEl.getBoundingClientRect();
        svgWidth  = rect.width  || 800;
        svgHeight = rect.height || 600;
    }, 50);

    // ─── Tab Navigation ────────────────────────────────────────────────────────
    tabs.forEach(tab => {
        tab.addEventListener('click', () => {
            tabs.forEach(t => t.classList.remove('active'));
            tab.classList.add('active');
            const filter = tab.getAttribute('data-channel');

            networkSection.style.display  = filter === 'network'       ? 'flex' : 'none';
            terminalSection.style.display  = (filter !== 'network' && filter !== 'observability') ? 'flex' : 'none';
            observSection.style.display    = filter === 'observability' ? 'flex' : 'none';

            if (filter === 'observability') {
                renderSessionsTable();
            } else if (filter !== 'network') {
                document.body.setAttribute('data-active-tab', filter);
                scrollToBottom();
            } else {
                document.body.removeAttribute('data-active-tab');
            }
        });
    });

    // ─── Drawer ────────────────────────────────────────────────────────────────
    closeDrawerBtn.addEventListener('click', () => drawerOverlay.classList.remove('active'));
    drawerOverlay.addEventListener('click', e => {
        if (e.target === drawerOverlay) drawerOverlay.classList.remove('active');
    });

    // ─── Node Panel ────────────────────────────────────────────────────────────
    closeNodePanel.addEventListener('click', closePanel);
    nodePanelOverlay.addEventListener('click', e => {
        if (e.target === nodePanelOverlay) closePanel();
    });
    function closePanel() {
        nodePanelOverlay.classList.remove('active');
        selectedNodeId = null;
    }

    // ─── Force Simulation Parameters ──────────────────────────────────────────
    const P = {
        repulsion:  5500,   // node-to-node repulsion strength
        springLen:  180,    // ideal edge resting length
        springK:    0.055,  // spring stiffness
        laneK:      0.08,   // pull toward computed x-lane strength
        centerGravY:0.018,  // vertical gravity toward center-Y
        damping:    0.78,   // velocity damping (lower = smoother settle)
        nodeRadius: 28,
        labelOffset:44,
        marginX:    120,    // left/right margin from SVG edge
        layerSpacing:200,   // horizontal distance between layers
    };

    // ─── Layer Assignment (BFS topological ordering) ──────────────────────────
    // This is the key to the left-to-right layout.
    // Nodes with no incoming edges get layer 0 (leftmost).
    // Each edge pushes the target node at least one layer to the right.

    function computeLayers() {
        const ids     = Object.keys(nodePhysics);
        if (ids.length === 0) return {};

        // Build in-degree map
        const inDeg = {};
        const outAdj = {};   // adjacency list (cycles are fine, BFS handles them)
        ids.forEach(id => { inDeg[id] = 0; outAdj[id] = []; });
        Object.values(edgeMap).forEach(e => {
            if (inDeg[e.tgt] !== undefined) inDeg[e.tgt]++;
            if (outAdj[e.src]) outAdj[e.src].push(e.tgt);
        });

        const layers  = {};
        const settled = new Set(); // Each node processed EXACTLY ONCE — prevents cycles.
        const queue   = [];

        // Seed from zero-in-degree nodes (true roots)
        ids.forEach(id => {
            if (inDeg[id] === 0) {
                layers[id] = 0;
                settled.add(id);
                queue.push(id);
            }
        });

        // Fully-cyclic graph fallback: seed all unseeded nodes at layer 0
        if (queue.length === 0) {
            ids.forEach(id => { layers[id] = 0; settled.add(id); queue.push(id); });
        }

        let head = 0;
        while (head < queue.length) {
            const cur = queue[head++];
            const curLayer = layers[cur] || 0;
            (outAdj[cur] || []).forEach(tgt => {
                if (!settled.has(tgt)) {
                    // First time we reach this node — assign its layer and mark done.
                    layers[tgt] = curLayer + 1;
                    settled.add(tgt);
                    queue.push(tgt);
                }
                // Already settled → skip. This is what breaks cycles.
            });
        }

        ids.forEach(id => { if (layers[id] === undefined) layers[id] = 0; });
        return layers;
    }

    let currentLayers = {};
    // Debounce recomputeLayers: burst edge events (discovery) would otherwise
    // trigger O(nodes × edges) BFS on every single CloudEvent.
    let _layerTimer = null;
    function scheduleRecomputeLayers() {
        if (_layerTimer) clearTimeout(_layerTimer);
        _layerTimer = setTimeout(recomputeLayers, 250);
    }

    function recomputeLayers() {
        currentLayers = computeLayers();
        const maxLayer = Object.values(currentLayers).reduce((m, v) => Math.max(m, v), 0);
        const numLayers = maxLayer + 1;

        const usableW = svgWidth  - P.marginX * 2;
        const spacing = numLayers <= 1 ? 0 : usableW / (numLayers - 1);

        Object.entries(currentLayers).forEach(([id, layer]) => {
            if (nodePhysics[id]) {
                nodePhysics[id].layer = layer;
                if (id === HUB_ID) {
                    // Hub always gravitates to horizontal center
                    nodePhysics[id].targetX = svgWidth / 2;
                } else if (!nodePhysics[id].manuallyPlaced && nodePhysics[id].targetX === undefined) {
                    // Only set initial lane for nodes that haven't been placed yet.
                    // Once a node has a targetX it has settled — don't move it on topology changes.
                    nodePhysics[id].targetX = P.marginX + layer * (spacing || usableW / 2);
                }
            }
        });
        // Ensure hub targetX stays at center even if not in layer computation
        if (nodePhysics[HUB_ID] && !nodePhysics[HUB_ID].manuallyPlaced) {
            nodePhysics[HUB_ID].targetX = svgWidth / 2;
        }
    }

    // ─── Force Simulation Step ─────────────────────────────────────────────────
    function runSimulationStep() {
        const ids = Object.keys(nodePhysics);
        if (ids.length === 0) {
            renderGraph();
            rafId = requestAnimationFrame(runSimulationStep);
            return;
        }

        const midY = svgHeight / 2;

        // Reset forces
        ids.forEach(id => { nodePhysics[id].fx = 0; nodePhysics[id].fy = 0; });

        // Node-to-node repulsion (Coulomb)
        for (let i = 0; i < ids.length; i++) {
            for (let j = i + 1; j < ids.length; j++) {
                const a  = nodePhysics[ids[i]];
                const b  = nodePhysics[ids[j]];
                const dx = b.x - a.x;
                const dy = b.y - a.y;
                const r2 = dx * dx + dy * dy + 1;
                const r  = Math.sqrt(r2);
                const f  = P.repulsion / r2;
                a.fx -= (dx / r) * f;  a.fy -= (dy / r) * f;
                b.fx += (dx / r) * f;  b.fy += (dy / r) * f;
            }
        }

        // Spring attraction along edges (Hooke)
        Object.values(edgeMap).forEach(edge => {
            const a = nodePhysics[edge.src];
            const b = nodePhysics[edge.tgt];
            if (!a || !b) return;
            const dx   = b.x - a.x;
            const dy   = b.y - a.y;
            const dist = Math.sqrt(dx * dx + dy * dy) + 0.01;
            const stretch = dist - P.springLen;
            const f = P.springK * stretch;
            a.fx += (dx / dist) * f;  a.fy += (dy / dist) * f;
            b.fx -= (dx / dist) * f;  b.fy -= (dy / dist) * f;
        });

        // Lane force: pull node toward its assigned x-column (horizontal layout)
        ids.forEach(id => {
            const n = nodePhysics[id];
            if (n.targetX !== undefined) {
                n.fx += (n.targetX - n.x) * P.laneK;
            }
        });

        // Vertical centering gravity
        ids.forEach(id => {
            const n = nodePhysics[id];
            n.fy += (midY - n.y) * P.centerGravY;
        });

        // Integrate — clamp velocity and guard against NaN from degenerate forces
        const MAX_V = 25;
        const r = P.nodeRadius + 10;
        ids.forEach(id => {
            const n = nodePhysics[id];
            if (n.pinned) { n.vx = 0; n.vy = 0; return; }
            n.vx = (n.vx + n.fx) * P.damping;
            n.vy = (n.vy + n.fy) * P.damping;
            // Clamp velocity magnitude so force explosions don't teleport nodes
            const vMag = Math.sqrt(n.vx * n.vx + n.vy * n.vy);
            if (vMag > MAX_V) { n.vx *= MAX_V / vMag; n.vy *= MAX_V / vMag; }
            n.x += n.vx;
            n.y += n.vy;
            // Clamp to canvas bounds + NaN guard
            if (!isFinite(n.x)) n.x = svgWidth  / 2;
            if (!isFinite(n.y)) n.y = svgHeight / 2;
            n.x = Math.max(r, Math.min(svgWidth  - r, n.x));
            n.y = Math.max(r + 15, Math.min(svgHeight - r - 20, n.y));
        });

        renderGraph();
        rafId = requestAnimationFrame(runSimulationStep);
    }

    // ─── DOM Node/Edge pools ───────────────────────────────────────────────────
    const domEdges = {};   // edgeKey → SVG <line>
    const domNodes = {};   // nodeId  → SVG <g>

    function renderGraph() {
        const now = Date.now();

        // ── Edges ────────────────────────────────────────────────────────────
        const activeEdgeKeys = new Set(Object.keys(edgeMap));
        Object.keys(domEdges).forEach(k => {
            if (!activeEdgeKeys.has(k)) { domEdges[k].remove(); delete domEdges[k]; }
        });

        Object.entries(edgeMap).forEach(([key, edge]) => {
            const a = nodePhysics[edge.src];
            const b = nodePhysics[edge.tgt];
            if (!a || !b) return;

            // Endpoint on circle surface
            const dx   = b.x - a.x;
            const dy   = b.y - a.y;
            const len  = Math.sqrt(dx * dx + dy * dy) + 0.001;
            const r    = P.nodeRadius + 2;
            const x1   = a.x + (dx / len) * r;
            const y1   = a.y + (dy / len) * r;
            const x2   = b.x - (dx / len) * r;
            const y2   = b.y - (dy / len) * r;

            // Age-based appearance — edges stay clearly visible for 30 s then slowly fade
            const ageMs   = now - edge.lastSeen;
            const opacity = edge.active ? 1.0 : Math.max(0.28, 0.75 * Math.exp(-ageMs / 30000));
            const strokeW = edge.active ? 2.5  : 2.0;
            const color   = edge.active
                ? `rgba(139,92,246,1.0)`
                : `rgba(139,92,246,${(0.55 * Math.exp(-ageMs / 30000) + 0.28).toFixed(2)})`;

            if (!domEdges[key]) {
                const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
                line.classList.add('graph-edge');
                svgEdgesG.appendChild(line);
                domEdges[key] = line;
            }
            const line = domEdges[key];
            line.setAttribute('x1', x1);
            line.setAttribute('y1', y1);
            line.setAttribute('x2', x2);
            line.setAttribute('y2', y2);
            line.setAttribute('stroke', color);
            line.setAttribute('stroke-width', strokeW);
            line.setAttribute('stroke-linecap', 'round');
            line.style.opacity = opacity;

            // Cool down after 3 s
            if (edge.active && ageMs > 3000) edge.active = false;
        });

        // ── Nodes ────────────────────────────────────────────────────────────
        const activeNodeIds = new Set(Object.keys(nodePhysics));
        Object.keys(domNodes).forEach(id => {
            if (!activeNodeIds.has(id)) { domNodes[id].remove(); delete domNodes[id]; }
        });

        Object.entries(nodePhysics).forEach(([id, n]) => {
            const disp   = nodeDisplay[id] || {};
            const active = disp.active || false;
            const isHub  = id === HUB_ID;

            if (!domNodes[id]) {
                const g = document.createElementNS('http://www.w3.org/2000/svg', 'g');
                g.classList.add('graph-node-group');
                if (isHub) g.classList.add('hub-node');
                g.setAttribute('data-node-id', id);
                g.style.cursor = 'pointer';

                if (isHub) {
                    // ── Hub: hexagonal shape ─────────────────────────────────
                    const ringR = HUB_RADIUS + 12;
                    const ring = document.createElementNS('http://www.w3.org/2000/svg', 'polygon');
                    ring.classList.add('graph-node-ring');
                    ring.setAttribute('points', hexPoints(ringR));
                    ring.setAttribute('fill', 'none');
                    ring.setAttribute('stroke-width', '1.5');

                    const hex = document.createElementNS('http://www.w3.org/2000/svg', 'polygon');
                    hex.classList.add('graph-node-circle'); // reuse class for state updates
                    hex.setAttribute('points', hexPoints(HUB_RADIUS));

                    // Transparent circle for hit-testing (easier than polygon pointer math)
                    const hit = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
                    hit.setAttribute('r', HUB_RADIUS + 4);
                    hit.setAttribute('fill', 'transparent');
                    hit.setAttribute('stroke', 'none');

                    const abbr = document.createElementNS('http://www.w3.org/2000/svg', 'text');
                    abbr.setAttribute('text-anchor', 'middle');
                    abbr.setAttribute('dominant-baseline', 'central');
                    abbr.setAttribute('y', '-4');
                    abbr.setAttribute('fill', 'rgba(255,255,255,0.95)');
                    abbr.setAttribute('font-size', '12');
                    abbr.setAttribute('font-weight', '800');
                    abbr.setAttribute('font-family', 'Inter, sans-serif');
                    abbr.setAttribute('pointer-events', 'none');
                    abbr.setAttribute('letter-spacing', '1');
                    abbr.textContent = 'HUB';

                    const subLabel = document.createElementNS('http://www.w3.org/2000/svg', 'text');
                    subLabel.setAttribute('text-anchor', 'middle');
                    subLabel.setAttribute('dominant-baseline', 'central');
                    subLabel.setAttribute('y', '10');
                    subLabel.setAttribute('fill', 'rgba(253,230,138,0.75)');
                    subLabel.setAttribute('font-size', '9');
                    subLabel.setAttribute('font-family', 'Fira Code, monospace');
                    subLabel.setAttribute('pointer-events', 'none');
                    subLabel.textContent = 'luotsi';

                    const label = document.createElementNS('http://www.w3.org/2000/svg', 'text');
                    label.classList.add('graph-node-label');
                    label.setAttribute('text-anchor', 'middle');
                    label.setAttribute('y', HUB_RADIUS + 18);
                    label.setAttribute('fill', '#fbbf24');
                    label.setAttribute('font-size', '11');
                    label.setAttribute('font-weight', '600');
                    label.setAttribute('font-family', 'Fira Code, monospace');
                    label.setAttribute('pointer-events', 'none');
                    label.textContent = 'luotsi-hub';

                    g.appendChild(ring);
                    g.appendChild(hex);
                    g.appendChild(hit);
                    g.appendChild(abbr);
                    g.appendChild(subLabel);
                    g.appendChild(label);
                } else {
                    // ── Regular node: circle ──────────────────────────────────
                    const ring = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
                    ring.classList.add('graph-node-ring');
                    ring.setAttribute('r', P.nodeRadius + 9);
                    ring.setAttribute('fill', 'none');
                    ring.setAttribute('stroke-width', '1');

                    const circle = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
                    circle.classList.add('graph-node-circle');
                    circle.setAttribute('r', P.nodeRadius);

                    const abbr = document.createElementNS('http://www.w3.org/2000/svg', 'text');
                    abbr.setAttribute('text-anchor', 'middle');
                    abbr.setAttribute('dominant-baseline', 'central');
                    abbr.setAttribute('fill', 'rgba(255,255,255,0.92)');
                    abbr.setAttribute('font-size', '11');
                    abbr.setAttribute('font-weight', '700');
                    abbr.setAttribute('font-family', 'Inter, sans-serif');
                    abbr.setAttribute('pointer-events', 'none');
                    abbr.textContent = makeAbbr(id);

                    const label = document.createElementNS('http://www.w3.org/2000/svg', 'text');
                    label.classList.add('graph-node-label');
                    label.setAttribute('text-anchor', 'middle');
                    label.setAttribute('y', P.labelOffset);
                    label.setAttribute('fill', '#94A3B8');
                    label.setAttribute('font-size', '11');
                    label.setAttribute('font-family', 'Fira Code, monospace');
                    label.setAttribute('pointer-events', 'none');
                    label.textContent = id.length > 20 ? id.slice(0, 18) + '\u2026' : id;

                    g.appendChild(ring);
                    g.appendChild(circle);
                    g.appendChild(abbr);
                    g.appendChild(label);
                }

                makeDraggable(g, id);
                g.addEventListener('click', e => { e.stopPropagation(); openNodePanel(id); });
                svgNodesG.appendChild(g);
                domNodes[id] = g;
            }

            const g      = domNodes[id];
            const ring   = g.querySelector('.graph-node-ring');
            const shape  = g.querySelector('.graph-node-circle'); // circle or hex polygon

            g.setAttribute('transform', `translate(${n.x.toFixed(1)},${n.y.toFixed(1)})`);

            if (isHub) {
                // Hub always uses amber gradient; pulse ring when traffic is flowing
                shape.setAttribute('fill', 'url(#hub-grad)');
                shape.setAttribute('stroke', 'rgba(251,191,36,0.9)');
                shape.setAttribute('stroke-width', '2');
                shape.setAttribute('filter', 'url(#glow-amber)');
                ring.setAttribute('stroke', active ? 'rgba(252,211,77,0.45)' : 'rgba(252,211,77,0.15)');
                ring.setAttribute('filter', 'url(#glow-amber)');
            } else if (active) {
                shape.setAttribute('fill', 'url(#node-grad-active)');
                shape.setAttribute('stroke', 'rgba(139,92,246,0.85)');
                shape.setAttribute('stroke-width', '2');
                shape.setAttribute('filter', 'url(#glow-purple)');
                ring.setAttribute('stroke', 'rgba(74,222,128,0.30)');
                ring.setAttribute('filter', 'url(#glow-green)');
            } else {
                shape.setAttribute('fill', 'url(#node-grad-idle)');
                shape.setAttribute('stroke', 'rgba(255,255,255,0.08)');
                shape.setAttribute('stroke-width', '1');
                shape.removeAttribute('filter');
                ring.setAttribute('stroke', 'rgba(255,255,255,0.04)');
                ring.removeAttribute('filter');
            }

            if (id === selectedNodeId && !isHub) {
                shape.setAttribute('stroke', '#C084FC');
                shape.setAttribute('stroke-width', '2.5');
            }
        });

        // Stats (exclude hub from node count — it's the router, not an agent)
        const agentCount = Object.keys(nodePhysics).filter(id => id !== HUB_ID).length;
        statNodesCount.textContent  = agentCount;
        statEdgesCount.textContent  = Object.keys(edgeMap).length;
        statTracesCount.textContent = Object.keys(traceStore).length;

        // Empty state
        graphEmptyState.style.display = agentCount === 0 ? 'flex' : 'none';
    }

    // Compute hexagon polygon points (flat-top) centered at 0,0
    function hexPoints(R) {
        return Array.from({ length: 6 }, (_, i) => {
            const a = (i * 60 - 30) * Math.PI / 180;
            return `${(R * Math.cos(a)).toFixed(1)},${(R * Math.sin(a)).toFixed(1)}`;
        }).join(' ');
    }

    function makeAbbr(id) {
        const parts = id.replace(/[-_]/g, ' ').split(' ').filter(Boolean);
        if (parts.length >= 2) return (parts[0][0] + parts[1][0]).toUpperCase();
        return id.slice(0, 2).toUpperCase();
    }

    // ─── Draggable Nodes ──────────────────────────────────────────────────────
    function makeDraggable(g, id) {
        let dragging  = false;
        let threshold = false; // has moved enough to count as drag (not click)
        let downWorld = { x: 0, y: 0 };
        let downNode  = { x: 0, y: 0 };

        g.addEventListener('mousedown', e => {
            if (e.button !== 0) return;
            e.stopPropagation(); // don't trigger pan
            e.preventDefault();
            dragging  = true;
            threshold = false;
            downWorld = toWorld(e.clientX, e.clientY);
            downNode  = { x: nodePhysics[id].x, y: nodePhysics[id].y };
            nodePhysics[id].pinned = true;
            g.style.cursor = 'grabbing';
        });

        window.addEventListener('mousemove', e => {
            if (!dragging) return;
            const w  = toWorld(e.clientX, e.clientY);
            const dx = w.x - downWorld.x;
            const dy = w.y - downWorld.y;
            if (Math.abs(dx) > 4 || Math.abs(dy) > 4) threshold = true;
            if (!threshold) return;

            const r = P.nodeRadius + 10;
            nodePhysics[id].x  = Math.max(r, Math.min(svgWidth  - r, downNode.x + dx));
            nodePhysics[id].y  = Math.max(r, Math.min(svgHeight - r, downNode.y + dy));
            nodePhysics[id].vx = 0;
            nodePhysics[id].vy = 0;
        });

        window.addEventListener('mouseup', e => {
            if (!dragging) return;
            dragging = false;
            g.style.cursor = 'pointer';
            if (threshold) {
                // Real drag: permanently pin at user-chosen position
                nodePhysics[id].pinned = true;
                nodePhysics[id].manuallyPlaced = true;
            } else {
                // Just a click (opens panel): only unpin if node was NOT manually placed.
                // This prevents a click from undoing a previous manual drag position.
                if (!nodePhysics[id].manuallyPlaced) {
                    nodePhysics[id].pinned = false;
                }
            }
        });
    }

    // ─── Node Network Update (heartbeat) ──────────────────────────────────────
    function updateNodeNetwork(hbData) {
        const { active_nodes, capabilities } = hbData;
        const now = Date.now();
        let topologyChanged = false;

        for (const [nodeId, lastSeen] of Object.entries(active_nodes)) {
            if (!nodePhysics[nodeId]) {
                // Spawn near center with slight random scatter
                const cx = svgWidth  / 2;
                const cy = svgHeight / 2;
                const a  = Math.random() * 2 * Math.PI;
                const d  = 60 + Math.random() * 80;
                nodePhysics[nodeId] = {
                    x: cx + Math.cos(a) * d,
                    y: cy + Math.sin(a) * d,
                    vx: 0, vy: 0, fx: 0, fy: 0,
                    pinned: false, layer: 0, targetX: cx
                };
                topologyChanged = true;
            }

            if (!nodeDisplay[nodeId]) nodeDisplay[nodeId] = { active: false, lastSeen: 0, caps: {} };
            nodeDisplay[nodeId].active   = (now - lastSeen) < 5000;
            nodeDisplay[nodeId].lastSeen = lastSeen;

            if (capabilities && capabilities[nodeId]) {
                nodeDisplay[nodeId].caps = capabilities[nodeId];
            }
        }

        if (topologyChanged) scheduleRecomputeLayers();

        // Refresh panel if open
        if (selectedNodeId && nodeDisplay[selectedNodeId]) refreshPanelCaps(selectedNodeId);
    }

    // ─── Edge Tracking ────────────────────────────────────────────────────────
    // All traffic in Luotsi is mediated by luotsi-hub.
    // We always split edges: src → hub → tgt  (two separate edges).
    // If one side is already hub, we create only one edge.

    function ensureHubNode() {
        if (!nodePhysics[HUB_ID]) {
            const cx = svgWidth  / 2 || 400;
            const cy = svgHeight / 2 || 300;
            nodePhysics[HUB_ID] = {
                x: cx, y: cy, vx: 0, vy: 0, fx: 0, fy: 0,
                pinned: false, layer: 0, targetX: cx, isHub: true
            };
            nodeDisplay[HUB_ID]  = { active: true, lastSeen: Date.now(), caps: {}, isHub: true };
        }
    }

    function addDirectedEdge(src, tgt) {
        if (!src || !tgt || src === tgt) return false;
        const key = `${src}\u2192${tgt}`;
        if (!edgeMap[key]) {
            // Structurally new edge — returns true so caller can trigger recompute
            edgeMap[key] = { src, tgt, lastSeen: Date.now(), active: true };
            return true;
        }
        // Existing edge: only update activity, NO topology change
        edgeMap[key].lastSeen = Date.now();
        edgeMap[key].active   = true;
        return false;
    }

    function trackEdge(src, tgt) {
        if (!src || !tgt || src === tgt) return;
        ensureHubNode();
        let newEdge = false;
        if (src === HUB_ID || tgt === HUB_ID) {
            newEdge = addDirectedEdge(src, tgt);
        } else {
            // Both legs must be checked independently so both run even if first is false
            const a = addDirectedEdge(src, HUB_ID);
            const b = addDirectedEdge(HUB_ID, tgt);
            newEdge = a || b;
        }
        // Only schedule BFS when the graph topology actually changed.
        // Repeated traffic on existing edges does NOT reposition settled nodes.
        if (newEdge) scheduleRecomputeLayers();
    }

    // ─── Trace / Session Grouping ─────────────────────────────────────────────
    const SESSION_WINDOW_USER      = 5_000;
    const SESSION_WINDOW_DISCOVERY = 30_000;

    // pendingIdMap: maps NAT global_id → traceId so that a response event
    // (which may lack a traceparent) can still be correlated to its trace.
    const pendingIdMap = {};  // global_id string → traceId string

    function getOrCreateSession(ce) {
        // 1. Authoritative: W3C traceparent (00-traceId-spanId-flags)
        if (ce.traceparent) {
            const matches = ce.traceparent.match(/^00-([0-9a-f]{32})-([0-9a-f]{16})-[0-9a-f]{2}$/);
            if (matches) {
                const traceId = matches[1];
                if (!traceStore[traceId]) {
                    traceStore[traceId] = {
                        id: traceId, type: 'span',
                        startTime: ce.time, primaryNode: 'pending…',
                        status: 'OK', events: [], cycleOpen: true
                    };
                }
                if (ce.type === 'luotsi.telemetry.span' && ce.data?.attributes?.['gen_ai.agent.name']) {
                    traceStore[traceId].primaryNode = ce.data.attributes['gen_ai.agent.name'];
                }
                if (ce.data?.status === 'ERROR' || ce.data?.payload?.error) {
                    traceStore[traceId].status = 'ERROR';
                }
                // A span event means the round-trip completed — mark cycle closed if trace end.
                if (ce.type === 'luotsi.telemetry.span') {
                    if (ce.data?.attributes?.['luotsi.trace_end']) {
                        traceStore[traceId].cycleOpen = false;
                        traceStore[traceId].type = 'trace';
                    }
                }
                // Track NAT ids seen in this trace for fallback correlation (convert to string)
                const payloadId = ce.data?.payload?.id;
                if (payloadId !== undefined && payloadId !== null) {
                    pendingIdMap[String(payloadId)] = traceId;
                }
                return traceStore[traceId];
            }
        }

        // 2. Fallback: correlate via NAT payload ID if traceparent is absent.
        // When a response comes back without traceparent, its payload.id may
        // have been registered when the request was first seen.
        const pId = ce.data?.payload?.id;
        const pIdStr = pId !== undefined && pId !== null ? String(pId) : null;
        if (pIdStr && pendingIdMap[pIdStr]) {
            const existingTrace = traceStore[pendingIdMap[pIdStr]];
            if (existingTrace) {
                if (ce.data?.status === 'ERROR' || ce.data?.payload?.error) {
                    existingTrace.status = 'ERROR';
                }
                if (ce.type === 'luotsi.telemetry.span' && ce.data?.attributes?.['luotsi.trace_end']) {
                    existingTrace.cycleOpen = false;
                    existingTrace.type = 'trace';
                }
                return existingTrace;
            }
        }

        // 3. Time-window bucketing (last resort for orphan events)
        const idStr      = ce.data?.payload?.id ? String(ce.data.payload.id) : '';
        const method     = ce.data?.payload?.method || '';
        const isDiscovery = idStr.includes('__luotsi__') ||
                            method.includes('notifications/') ||
                            method === 'initialize';

        const src    = ce.luotsisource || ce.data?.source_id || 'unknown';
        const nowMs  = new Date(ce.time).getTime() || Date.now();
        const window = isDiscovery ? SESSION_WINDOW_DISCOVERY : SESSION_WINDOW_USER;
        // Optimization: discovery events are unified regardless of source node 
        // to prevent fragmentation of the initial fabric discovery cycle.
        const key    = isDiscovery ? `discovery` : `user:${src}`;

        const existing = Object.values(traceStore).find(s =>
            s._key === key && (nowMs - new Date(s.startTime).getTime()) < window
        );
        if (existing) return existing;

        const sid = `syn-${Date.now().toString(16)}-${(++syntheticCounter).toString(16)}`;
        traceStore[sid] = {
            id: sid, _key: key,
            type: isDiscovery ? 'discovery' : 'span',
            startTime: ce.time, primaryNode: src,
            status: 'OK', events: [], cycleOpen: true
        };
        // Register payload id for future response correlation
        if (pIdStr) pendingIdMap[pIdStr] = sid;
        return traceStore[sid];
    }

    let syntheticCounter = 0;

    // ─── Event Processing ──────────────────────────────────────────────────────
    function processLogEvent(ce) {
        eventIndex[ce.id] = ce;

        // Edge tracking from CloudEvent extension attributes
        const src = ce.luotsisource || ce.data?.source_id;
        const tgt = ce.luotsitarget || ce.data?.target_id;
        if (src && tgt) trackEdge(src, tgt);

        // Session grouping
        if (ce.type === 'luotsi.message' || ce.type.startsWith('luotsi.telemetry.')) {
            const session = getOrCreateSession(ce);
            session.events.push(ce);
            if (observSection.style.display === 'flex') renderSessionsTable();
        }

        // ── Terminal log row ──────────────────────────────────────────────────
        let type = 'system', badge = 'SYS', badgeClass = 'sys', routeHtml = '';
        const payloadToRender = ce.data;
        const timeStr = new Date(ce.time).toLocaleTimeString([], {
            hour12: false, hour: '2-digit', minute: '2-digit',
            second: '2-digit', fractionalSecondDigits: 3
        });

        if (ce.type === 'luotsi.message') {
            const payload = ce.data?.payload || {};
            const s = src || '?';
            const t = tgt || '?';
            routeHtml = `${s} <span style="opacity:0.4">→</span> ${t}`;
            const method = payload.method || '';
            const idStr  = payload.id ? String(payload.id) : '';
            if (method.includes('tools/') || method.includes('resources/') ||
                method.includes('prompts/') || method === 'initialize' ||
                method === 'notifications/initialized' || idStr.includes('__luotsi__')) {
                type = 'mcp'; badgeClass = 'mcp'; badge = 'MCP';
            } else {
                type = 'jsonrpc'; badgeClass = 'rpc'; badge = 'RPC';
            }
        } else if (ce.type === 'luotsi.telemetry.span') {
            type = 'span'; badgeClass = 'span'; badge = 'SPAN';
        }

        const logRow = document.createElement('div');
        logRow.className = 'log-row';
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
        if (terminalOutput.children.length > 600) terminalOutput.removeChild(terminalOutput.firstChild);
        scrollToBottom();
    }

    // ─── Session Table ─────────────────────────────────────────────────────────
    function renderSessionsTable() {
        if (!sessionTableBody) return;
        sessionTableBody.innerHTML = '';
        const sorted = Object.values(traceStore).sort((a, b) =>
            new Date(b.startTime) - new Date(a.startTime)
        );
        sorted.forEach(trace => {
            const row = document.createElement('tr');
            row.className = 'session-row';
            row.onclick = () => openTraceDrawer(trace.id);

            const timeStr     = trace.startTime ? new Date(trace.startTime).toLocaleString() : 'N/A';
            const statusClass = trace.status === 'OK' ? 'success' : 'error';
            const typeLabel   = trace.type === 'discovery' ? 'DISCOVERY' : trace.type === 'trace' ? 'TRACE' : trace.type === 'span' ? 'SPAN' : 'USER';
            const typeCls     = trace.type === 'discovery' ? 'sys' : trace.type === 'trace' ? 'span' : 'mcp';
            const primary     = trace.primaryNode && trace.primaryNode !== 'pending…' ? trace.primaryNode : 'luotsi-hub';
            const latency     = calculateTotalDelay(trace);
            const isClosed    = !trace.cycleOpen;
            const cycleCls    = isClosed ? 'success' : 'warning';
            const cycleLabel  = isClosed ? '● CLOSED' : '○ OPEN';

            row.innerHTML = `
                <td><div class="status-cell"><div class="status-indicator ${statusClass}"></div>${trace.status}</div></td>
                <td style="font-family:monospace; font-size:0.78rem;">${trace.id.substring(0, 10)}…</td>
                <td><span class="badge ${typeCls}" style="font-size:0.7rem;">${typeLabel}</span></td>
                <td>${timeStr}</td>
                <td><span class="badge sys">${primary}</span></td>
                <td><span class="badge ${cycleCls}" style="font-size:0.7rem;">${cycleLabel}</span></td>
                <td>${latency > 0 ? latency + 'ms' : '—'}</td>
                <td>${trace.events.length}</td>
            `;
            sessionTableBody.appendChild(row);
        });
    }

    function calculateTotalDelay(trace) {
        return trace.events
            .filter(e => e.type === 'luotsi.telemetry.span')
            .reduce((acc, s) => acc + (s.data?.duration_ms || 0), 0);
    }

    // ─── Trace Drawer ──────────────────────────────────────────────────────────
    function openTraceDrawer(traceId) {
        const trace = traceStore[traceId];
        if (!trace) return;

        document.getElementById('drawer-trace-id').textContent   = `Trace: ${traceId}`;
        document.getElementById('drawer-trace-time').textContent  = new Date(trace.startTime).toLocaleString();
        traceTimeline.innerHTML = '';

        const sorted = [...trace.events].sort((a, b) => new Date(a.time) - new Date(b.time));
        sorted.forEach(ce => {
            const item = document.createElement('div');
            item.className = 'timeline-event';
            item.onclick   = e => { e.stopPropagation(); showEventDetail(ce.id); };

            const s = ce.luotsisource || ce.data?.source_id || '?';
            const t = ce.luotsitarget || ce.data?.target_id || '?';
            let flowText = ce.type;
            let spanBadge = '';
            if (ce.type === 'luotsi.message') {
                flowText = `${s} → ${t}`;
            } else if (ce.type === 'luotsi.telemetry.span') {
                const spanName = ce.data?.name || 'route';
                const isFanOut = spanName === 'luotsi.fan_out';
                flowText = isFanOut
                    ? `FAN-OUT: ${ce.data?.attributes?.['luotsi.fan_out.method'] || ''} (${ce.data?.attributes?.['luotsi.fan_out.targets'] || '?'} targets)`
                    : `SPAN: ${spanName}`;
                spanBadge = ce.data?.duration_ms
                    ? `<span style="font-size:0.72rem; color:var(--rpc-green); margin-left:6px;">⏱ ${ce.data.duration_ms}ms</span>`
                    : '';
            }

            const isSpan   = ce.type === 'luotsi.telemetry.span';
            const isError  = ce.data?.status === 'ERROR' || ce.data?.payload?.error;
            const dotColor = isError ? 'var(--error-red, #f87171)' : isSpan ? 'var(--rpc-green)' : 'var(--brand-primary)';

            const timeStr = new Date(ce.time).toLocaleTimeString([], {
                hour: '2-digit', minute: '2-digit', second: '2-digit', fractionalSecondDigits: 3
            });
            item.innerHTML = `
                <div class="event-dot" style="background:${dotColor}; box-shadow: 0 0 6px ${dotColor}40;"></div>
                <div class="event-content">
                    <div class="event-header">
                        <span style="color:${dotColor}; font-weight:700;">${ce.type.toUpperCase()}</span>
                        <span>${timeStr}${spanBadge}</span>
                    </div>
                    <div class="event-flow">${flowText}</div>
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
        if (ce.type === 'luotsi.telemetry.span' && ce.data?.attributes) {
            metaHtml = '<div class="detail-meta-grid">';
            for (const [k, v] of Object.entries(ce.data.attributes)) {
                metaHtml += `<div class="meta-item"><span class="meta-label">${k}</span><span class="meta-val">${v}</span></div>`;
            }
            metaHtml += '</div>';
        }
        detailContent.innerHTML = `
            ${metaHtml}
            <div class="node-caps-title" style="margin-bottom:1rem;">Payload / Data</div>
            <div class="json-payload" style="max-height:600px; overflow:auto;">${syntaxHighlight(ce.data)}</div>
        `;
    }

    // ─── Node Panel ────────────────────────────────────────────────────────────
    function openNodePanel(nodeId) {
        selectedNodeId = nodeId;
        document.getElementById('panel-node-id').textContent   = nodeId;
        document.getElementById('panel-node-meta').textContent = 'luotsi.node';

        const statusEl = document.getElementById('panel-node-status');
        const disp     = nodeDisplay[nodeId] || {};
        statusEl.querySelector('.status-text').textContent = disp.active ? 'ACTIVE' : 'IDLE';
        disp.active ? statusEl.classList.add('active-status') : statusEl.classList.remove('active-status');

        refreshPanelCaps(nodeId);
        nodePanelOverlay.classList.add('active');
    }

    function refreshPanelCaps(nodeId) {
        if (selectedNodeId !== nodeId) return;
        const body = document.getElementById('node-panel-body');
        const caps = nodeDisplay[nodeId]?.caps || {};
        const cats = [
            { key: 'tools',     label: 'Tools',     icon: '⚡' },
            { key: 'resources', label: 'Resources',  icon: '📁' },
            { key: 'templates', label: 'Templates',  icon: '🧩' },
            { key: 'prompts',   label: 'Prompts',    icon: '✉'  }
        ];

        const total = cats.reduce((n, c) => n + (caps[c.key] || []).length, 0);
        if (total === 0) {
            body.innerHTML = '<div class="panel-no-caps">No capabilities discovered yet.</div>';
            return;
        }

        body.innerHTML = '';
        cats.forEach(cat => {
            const items = caps[cat.key] || [];
            if (!items.length) return;
            const section = document.createElement('div');
            section.className = 'cap-section open';

            const header = document.createElement('div');
            header.className = 'cap-section-header';
            header.innerHTML = `
                <span class="cap-section-title"><span>${cat.icon}</span> ${cat.label}</span>
                <span style="display:flex;align-items:center;gap:0.5rem;">
                    <span class="cap-section-count">${items.length}</span>
                    <span class="cap-section-chevron">▲</span>
                </span>
            `;

            const sectionBody = document.createElement('div');
            sectionBody.className = 'cap-section-body';
            items.forEach(item => {
                const entry = document.createElement('div');
                entry.className = 'cap-entry';
                const desc = item.description || (cat.key === 'templates' ? item.uriTemplate : '') || '';
                entry.innerHTML = `
                    <div class="cap-entry-name">${item.name || item.uri || '—'}</div>
                    ${desc ? `<div class="cap-entry-desc">${desc}</div>` : ''}
                `;
                sectionBody.appendChild(entry);
            });

            header.addEventListener('click', () => section.classList.toggle('open'));
            section.appendChild(header);
            section.appendChild(sectionBody);
            body.appendChild(section);
        });
    }

    // ─── Utilities ─────────────────────────────────────────────────────────────
    function syntaxHighlight(jsonObj) {
        if (typeof jsonObj !== 'string') jsonObj = JSON.stringify(jsonObj, undefined, 2);
        jsonObj = jsonObj.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
        return jsonObj.replace(/(\"(\\u[a-zA-Z0-9]{4}|\\[^u]|[^\\"])*\"(\s*:)?|\b(true|false|null)\b|-?\d+(?:\.\d*)?(?:[eE][+\-]?\d+)?)/g, match => {
            let cls = 'number';
            if (/^"/.test(match)) {
                if (/:$/.test(match)) { cls = 'key'; match = match.slice(0, -1) + '<span style="color:var(--text-main)">:</span>'; }
                else cls = 'string';
            } else if (/true|false/.test(match)) { cls = 'boolean'; }
            else if (/null/.test(match)) { cls = 'null'; }
            return `<span class="${cls}">${match}</span>`;
        });
    }

    function addSystemLog(msg) {
        const logRow  = document.createElement('div');
        logRow.className = 'log-row system';
        logRow.setAttribute('data-type', 'system');
        const timeStr = new Date().toLocaleTimeString([], { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' });
        logRow.innerHTML = `<span class="time" style="margin-right:8px">[${timeStr}]</span><span class="badge sys" style="margin-right:8px">SYS</span><span class="message" style="color:var(--text-main)">${msg}</span>`;
        terminalOutput.appendChild(logRow);
        scrollToBottom();
    }

    function scrollToBottom() {
        if (autoScrollChk.checked) terminalOutput.scrollTop = terminalOutput.scrollHeight;
    }

    // ─── WebSocket ─────────────────────────────────────────────────────────────
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
            wsStatusText.textContent = 'Disconnected — Retrying…';
            wsDot.classList.remove('connected');
            setTimeout(connect, 3000);
        };
        socket.onmessage = event => {
            try {
                const ce = JSON.parse(event.data);
                if (ce.type === 'luotsi.heartbeat') {
                    updateNodeNetwork(ce.data);
                } else {
                    processLogEvent(ce);
                }
            } catch (e) {
                console.error('Failed to parse event', e);
            }
        };
    }

    // ─── Bootstrap ────────────────────────────────────────────────────────────
    // Always create hub node immediately so it's present even before first heartbeat
    setTimeout(() => {
        ensureHubNode();
        recomputeLayers();
    }, 100);

    connect();
    applyViewport();
    rafId = requestAnimationFrame(runSimulationStep);
});
