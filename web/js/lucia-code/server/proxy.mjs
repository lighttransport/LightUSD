#!/usr/bin/env node
import http from 'node:http';

const port = Number(process.env.LUCIA_PROXY_PORT || 8788);
const baseURL = (process.env.LUCIA_LLM_BASE_URL || 'https://api.openai.com/v1').replace(/\/$/, '');
const apiKey = process.env.LUCIA_LLM_API_KEY || '';
const model = process.env.LUCIA_LLM_MODEL || 'gpt-4.1-mini';

const server = http.createServer(async (request, response) => {
  response.setHeader('access-control-allow-origin', 'http://localhost:5173');
  response.setHeader('access-control-allow-headers', 'content-type');
  if (request.method === 'OPTIONS') { response.writeHead(204).end(); return; }
  if (request.method !== 'POST' || request.url !== '/api/lucia/chat') { response.writeHead(404).end('Not found'); return; }
  if (!apiKey) { response.writeHead(503, { 'content-type': 'application/json' }).end(JSON.stringify({ error: 'LUCIA_LLM_API_KEY is not configured' })); return; }
  let body = '';
  for await (const chunk of request) { body += chunk; if (body.length > 256 * 1024) { response.writeHead(413).end(); return; } }
  try {
    const payload = JSON.parse(body);
    const upstream = await fetch(`${baseURL}/chat/completions`, { method: 'POST', headers: { authorization: `Bearer ${apiKey}`, 'content-type': 'application/json' }, body: JSON.stringify({ model, messages: [{ role: 'system', content: 'You are Lucia. Use only the supplied tools. Never emit JavaScript. Ask a concise question when the request is ambiguous. Scene summary: ' + JSON.stringify(payload.sceneSummary || {}) }, ...(payload.messages || [])], tools: payload.tools, tool_choice: 'auto' }) });
    response.writeHead(upstream.status, { 'content-type': upstream.headers.get('content-type') || 'application/json' });
    response.end(Buffer.from(await upstream.arrayBuffer()));
  } catch (error) { response.writeHead(400, { 'content-type': 'application/json' }).end(JSON.stringify({ error: error.message })); }
});
server.listen(port, '127.0.0.1', () => console.log(`Lucia proxy: http://127.0.0.1:${port}/api/lucia/chat`));
