const express = require('express');
const { WebSocketServer } = require('ws');
const dgram = require('dgram');
const http = require('http');
const path = require('path');

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server });
const udpServer = dgram.createSocket('udp4');

app.use(express.static(path.join(__dirname, 'public')));

const nodeCapabilities = {};
const nodeLastSeen = {};

function broadcast(messageJson) {
  const messageStr = JSON.stringify(messageJson);
  wss.clients.forEach(client => {
    if (client.readyState === 1) {
      client.send(messageStr);
    }
  });
}

// Heartbeat broadcasater via WebSocket
setInterval(() => {
  broadcast({
    specversion: "1.0",
    type: "luotsi.heartbeat",
    source: "luotsi-dashboard",
    id: "hb-" + Date.now(),
    time: new Date().toISOString(),
    datacontenttype: "application/json",
    data: {
      active_nodes: nodeLastSeen,
      capabilities: nodeCapabilities
    }
  });
}, 1000);

function trackCapabilities(cloudEvent) {
  try {
    const sourceId = cloudEvent.luotsisource || (cloudEvent.data ? cloudEvent.data.source_id : null);
    const targetId = cloudEvent.luotsitarget || (cloudEvent.data ? cloudEvent.data.target_id : null);
    
    if (sourceId) nodeLastSeen[sourceId] = Date.now();
    if (targetId) nodeLastSeen[targetId] = Date.now();

    const payload = cloudEvent.data ? cloudEvent.data.payload : null;
    if (payload && payload.id) {
       const isDiscovery = typeof payload.id === 'string' && payload.id.includes('__luotsi__tools__');
       if (isDiscovery) {
           console.log(`[Discovery] Detected tool response for ${sourceId}. ID: ${payload.id}`);
           if (payload.result && payload.result.tools) {
               console.log(`[Discovery] Successfully extracted ${payload.result.tools.length} tools for ${sourceId}`);
               nodeCapabilities[sourceId] = payload.result.tools;
           } else {
               console.log(`[Discovery] Warning: Payload for ${sourceId} missing result.tools`, JSON.stringify(payload).substring(0, 200));
           }
       }
    }
  } catch (e) {
    console.error("Error parsing for capabilities:", e);
  }
}

wss.on('connection', (ws) => {
  console.log('Client connected to Dashboard WebSocket.');
  
  // Initial state blast
  ws.send(JSON.stringify({
    specversion: "1.0",
    type: "luotsi.heartbeat",
    source: "luotsi-dashboard",
    id: "init-" + Date.now(),
    time: new Date().toISOString(),
    datacontenttype: "application/json",
    data: {
      active_nodes: nodeLastSeen,
      capabilities: nodeCapabilities
    }
  }));

  ws.on('close', () => console.log('Client disconnected.'));
});

udpServer.on('error', (err) => {
  console.error(`UDP server error:\n${err.stack}`);
  udpServer.close();
});

udpServer.on('message', (msg, rinfo) => {
  try {
    const eventRaw = msg.toString();
    const cloudEvent = JSON.parse(eventRaw);
    
    // Ensure logical source/target for the dashboard frontend
    if (!cloudEvent.luotsisource && cloudEvent.data && cloudEvent.data.source_id) {
       cloudEvent.luotsisource = cloudEvent.data.source_id;
    }
    if (!cloudEvent.luotsitarget && cloudEvent.data && cloudEvent.data.target_id) {
       cloudEvent.luotsitarget = cloudEvent.data.target_id;
    }

    trackCapabilities(cloudEvent);
    broadcast(cloudEvent);
  } catch(e) {
    console.warn("Received malformed UDP packet:", e.message);
  }
});

udpServer.on('listening', () => {
  const address = udpServer.address();
  console.log(`Luotsi Observability UDP Listener running on ${address.address}:${address.port}`);
});

udpServer.bind(9000);

const HTTP_PORT = 3000;
server.listen(HTTP_PORT, () => {
  console.log(`Luotsi Dashboard WebUI running on http://localhost:${HTTP_PORT}`);
});
