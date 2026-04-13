const http = require('http');
const readline = require('readline');

const PORT = 3000;

// Global reference to the active SSE response stream
let sseResponse = null;

// Ensure we don't output random logs on stdout, because luotsi reads stdout.
// We must only output valid JSON-RPC to stdout.
console.log = function(...args) {
    process.stderr.write(args.join(' ') + '\n');
};
console.error = function(...args) {
    process.stderr.write(args.join(' ') + '\n');
};

const server = http.createServer((req, res) => {
    // CORS Headers
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

    if (req.method === 'OPTIONS') {
        res.writeHead(200);
        res.end();
        return;
    }

    if (req.method === 'GET' && req.url === '/sse') {
        res.writeHead(200, {
            'Content-Type': 'text/event-stream',
            'Cache-Control': 'no-cache',
            'Connection': 'keep-alive'
        });

        sseResponse = res;

        // Tell the client where to post messages
        res.write(`event: endpoint\ndata: /messages?sessionId=1\n\n`);
        console.error(`[Inspector Gateway] SSE Client connected.`);

        req.on('close', () => {
            console.error(`[Inspector Gateway] SSE Client disconnected.`);
            if (sseResponse === res) sseResponse = null;
        });
        return;
    }

    if (req.method === 'POST' && req.url.startsWith('/messages')) {
        let body = '';
        req.on('data', chunk => {
            body += chunk;
        });

        req.on('end', () => {
            try {
                // We received a JSON-RPC payload from the inspector.
                // Forward it exactly to Luotsi bus via stdout
                const payload = JSON.parse(body);
                console.error(`[Inspector Gateway] Inspector -> Luotsi: ${JSON.stringify(payload).substring(0, 100)}...`);
                
                // Write to Luotsi (needs to be single line, newline delimited)
                process.stdout.write(JSON.stringify(payload) + '\n');
                
                res.writeHead(202, { 'Content-Type': 'application/json' });
                res.end('Accepted');
            } catch (err) {
                console.error(`[Inspector Gateway] Failed to parse inspector message: ${err.message}`);
                res.writeHead(400);
                res.end('Bad Request');
            }
        });
        return;
    }

    res.writeHead(404);
    res.end('Not Found');
});

// Read incoming JSON-RPC from Luotsi via stdin
const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout,
    terminal: false
});

rl.on('line', (line) => {
    line = line.trim();
    if (!line) return;

    try {
        // Validate it's JSON
        JSON.parse(line);
        console.error(`[Inspector Gateway] Luotsi -> Inspector: ${line.substring(0, 100)}...`);

        // Forward to the connected SSE client
        if (sseResponse) {
            sseResponse.write(`event: message\ndata: ${line}\n\n`);
        } else {
            console.error(`[Inspector Gateway] Dropped message from Luotsi (no SSE client connected)`);
        }
    } catch (e) {
        console.error(`[Inspector Gateway] Received non-JSON from Luotsi stdin: ${line}`);
    }
});

server.listen(PORT, '127.0.0.1', () => {
    console.error(`[Inspector Gateway] Listening on http://127.0.0.1:${PORT}/sse`);
});
