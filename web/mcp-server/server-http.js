import express from "express";
import * as bodyParser from "body-parser";
import { randomBytes, timingSafeEqual } from "node:crypto";
import { v4 as uuidv4 } from "uuid";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { McpError, ErrorCode, ListResourceTemplatesRequestSchema, ReadResourceRequestSchema, ListToolsRequestSchema, CallToolRequestSchema, ListResourcesRequestSchema, ListPromptsRequestSchema, GetPromptRequestSchema, CompleteRequestSchema } from "@modelcontextprotocol/sdk/types.js";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";
import { isInitializeRequest } from "@modelcontextprotocol/sdk/types.js";

import cors from 'cors';

//import { LightUSDMCPServer } from "lightusd/LightUSDMCPServer.js";

const portno = 8085;

// --- Security configuration -------------------------------------------------
// Bind to loopback by default so the server is NOT reachable from the LAN.
// Override with MCP_HOST=0.0.0.0 only if you intentionally expose it.
const host = process.env.MCP_HOST || "127.0.0.1";

// Require a bearer token on every /mcp request. If MCP_AUTH_TOKEN is not set,
// generate an ephemeral one for this run and print it so the operator can use
// it. The server is never reachable without presenting this token, which is
// what prevents unauthenticated LAN/cross-origin callers from invoking tools.
const AUTH_TOKEN = process.env.MCP_AUTH_TOKEN || randomBytes(32).toString("hex");
if (!process.env.MCP_AUTH_TOKEN) {
  console.warn("[mcp] MCP_AUTH_TOKEN not set; generated an ephemeral token for this run:");
  console.warn("[mcp]   " + AUTH_TOKEN);
  console.warn("[mcp] Clients must send header:  Authorization: Bearer <token>");
}

// CORS: deny cross-origin browser access by default (origin:false sends no
// Access-Control-Allow-Origin, so a malicious site cannot read responses).
// Set MCP_ALLOWED_ORIGINS to a comma-separated allow-list to opt specific
// browser origins in. Non-browser clients (curl/node) are unaffected by CORS.
const allowedOrigins = (process.env.MCP_ALLOWED_ORIGINS || "")
  .split(",").map((s) => s.trim()).filter(Boolean);

// DNS-rebinding protection: only accept requests whose Host header is in this
// allow-list. Defaults to the loopback host/port the server binds to.
const allowedHosts = (process.env.MCP_ALLOWED_HOSTS ||
  `127.0.0.1:${portno},localhost:${portno},127.0.0.1,localhost`)
  .split(",").map((s) => s.trim()).filter(Boolean);

// Constant-time string comparison to avoid leaking the token via timing.
function timingSafeEqualStr(a, b) {
  const ab = Buffer.from(String(a));
  const bb = Buffer.from(String(b));
  if (ab.length !== bb.length) {
    return false;
  }
  return timingSafeEqual(ab, bb);
}

// Express middleware: reject any /mcp request without a valid bearer token.
function requireAuth(req, res, next) {
  const header = req.headers["authorization"] || "";
  const match = /^Bearer\s+(.+)$/i.exec(header);
  if (!match || !timingSafeEqualStr(match[1], AUTH_TOKEN)) {
    res.status(401).json({
      jsonrpc: "2.0",
      error: { code: -32001, message: "Unauthorized: missing or invalid bearer token" },
      id: null,
    });
    return;
  }
  next();
}

const app = express();
//app.use(express.json());

// Increase limit for larger requests(e.g. DataURI representation of USD file)
// Assume express 14.6.0+(bodyParser is now included in express.js)
app.use(express.json({limit: '50mb'})); // Increase limit for larger requests
app.use(express.urlencoded({ extended: true, limit: '50mb' })); // Increase limit for larger requests

// CORS configuration required for browser-based MCP clients. Cross-origin is
// denied by default; opt in specific origins via MCP_ALLOWED_ORIGINS.
app.use(cors({
  origin: allowedOrigins.length > 0 ? allowedOrigins : false,
  exposedHeaders: ['Mcp-Session-Id'],
  allowedHeaders: ['Content-Type', 'mcp-session-id', 'mcp-protocol-version', 'authorization'],
}));

// Authenticate every /mcp request. The cors() middleware above handles the
// preflight OPTIONS request and short-circuits it before this runs, so genuine
// preflights are not blocked by the missing Authorization header.
app.use('/mcp', requireAuth);

// Map to store transports by session ID
const transports = {};

const session = new Map();

// The MCP server product (lightusd_mcp module, next backend) is the default;
// set LIGHTUSD_MCP_BACKEND=legacy, or use a lightusd package without
// lightusd_mcp.js, to serve the legacy LightUSDLoaderNative MCP tools. Both
// expose the same mcp* session functions.
async function loadMCPBackend() {
  if (process.env.LIGHTUSD_MCP_BACKEND !== 'legacy') {
    try {
      const { default: createLightUSDMCP } = await import("lightusd/lightusd_mcp.js");
      const mcp = await createLightUSDMCP();
      console.log("LightUSD MCP backend: next (lightusd_mcp)");
      return mcp;
    } catch (e) {
      console.warn("lightusd_mcp.js unavailable; falling back to the legacy MCP backend:", e.message);
    }
  }
  const { default: initLightUSDNative } = await import("lightusd/lightusd.js");
  const LightUSD = await initLightUSDNative();
  console.log("LightUSD MCP backend: legacy (LightUSDLoaderNative)");
  return new LightUSD.LightUSDLoaderNative();
}

loadMCPBackend().then(function (lightusd) {

  // Handle POST requests for client-to-server communication
  app.post('/mcp', async (req, res) => {
    // NOTE: do not log req.headers — the Authorization header carries the
    // bearer token, which must not be written to logs.
    console.log("-- post /mcp --");
    // Check for existing session ID
    const sessionId = req.headers['mcp-session-id'] || undefined;
    let transport = null;

    if (sessionId && transports[sessionId]) {
      // Reuse existing transport
      transport = transports[sessionId];
    } else if (!sessionId && isInitializeRequest(req.body)) {
      // New initialization request
      transport = new StreamableHTTPServerTransport({
        sessionIdGenerator: () => uuidv4(),
        // Reject requests whose Host header is not in the allow-list, defeating
        // DNS-rebinding attacks against this (typically loopback) server.
        enableDnsRebindingProtection: true,
        allowedHosts: allowedHosts,
        onsessioninitialized: (sessionId) => {

          // Store the transport by session ID
          transports[sessionId] = transport;

          lightusd.mcpCreateContext(sessionId);
        },
      });

      // Clean up transport when closed
      transport.onclose = () => {
        if (transport.sessionId) {
          delete transports[transport.sessionId];
        }
      };
      const server = new McpServer({
        name: "lightusd-mcp-server",
        version: "0.9.5"
      });

      server.server.registerCapabilities({
        resources: {
          listChanged: true
        },
        tools: {
          listChanged: true
        }
      });

      server.server.setRequestHandler(ListResourcesRequestSchema, async () => {
        const sessionId = server.server.transport.sessionId;
        console.log("list resources", sessionId);

        lightusd.mcpSelectContext(sessionId);
        const resources_str = lightusd.mcpResourcesList();
        console.log("resources_str", resources_str);

        const j = JSON.parse(resources_str);
        return j;
      });

      server.server.setRequestHandler(ReadResourceRequestSchema, async (request, extra) => {
        const sessionId = server.server.transport.sessionId;
        //console.log("read resource", sessionId);

        const uri = request.params.uri;
        //console.log("uri", uri);

        lightusd.mcpSelectContext(sessionId);
        const resources_str = lightusd.mcpResourcesRead(uri);
        //console.log("resources_str", resources_str);

        const j = JSON.parse(resources_str);
        return j;
      });

      server.server.setRequestHandler(ListToolsRequestSchema, async () => {
        const sessionId = server.server.transport.sessionId;

        console.log("sessId", sessionId);
        console.log("lightusd", lightusd);

        lightusd.mcpSelectContext(sessionId);
        console.log("listtools");
        const tools_str = lightusd.mcpToolsList();
        console.log("tools_str", tools_str);

        const j = JSON.parse(tools_str)
        return j;
      });

      server.server.setRequestHandler(CallToolRequestSchema, async (request, extra) => {
        console.log("request", request);

        const sessionId = server.server.transport.sessionId;

        //console.log("sessId", sessionId);
        //console.log("lightusd", lightusd);

        lightusd.mcpSelectContext(sessionId);

        const tool_name = request.params.name;
        const args = JSON.stringify(request.params.arguments);
        console.log("tool_name", tool_name);
        //console.log("args", args);

        const result_str = lightusd.mcpToolsCall(tool_name, args);
        //console.log("result_str", result_str);

        const j = JSON.parse(result_str)
        return j;
      });


      /*
      server.registerTool("get_version",
        {
          title: "Get LightUSD version",
          description: "Get LightUSD version",
          inputSchema: {}
        },
        async ({ }) => {

          return {
            content: [
              {
                type: 'text',
                text: "v0.9.0"
              }
            ],
          }
        });

      server.registerTool("load_usd_layer",
        {
          title: "Load USD as Layer from URI",
          description: "Add two numbers",
          inputSchema: { a: z.number(), b: z.number() }
        },
        async ({ a, b }) => ({
          content: [{ type: "text", text: String(a + b) }]
        })
      );
    */

      // ... set up server resources, tools, and prompts ...

      // Connect to the MCP server
      await server.connect(transport);
    } else {
      // Invalid request
      res.status(400).json({
        jsonrpc: '2.0',
        error: {
          code: -32000,
          message: 'Bad Request: No valid session ID provided',
        },
        id: null,
      });
      return;
    }

    // Handle the request
    await transport.handleRequest(req, res, req.body);
  });

  // Reusable handler for GET and DELETE requests
  const handleSessionRequest = async (req, res) => {
    const sessionId = req.headers['mcp-session-id'];
    if (!sessionId || !transports[sessionId]) {
      res.status(400).send('Invalid or missing session ID');
      return;
    }

    const transport = transports[sessionId];
    await transport.handleRequest(req, res);
  };

  // Handle GET requests for server-to-client notifications via SSE
  app.get('/mcp', handleSessionRequest);

  // Handle DELETE requests for session termination
  app.delete('/mcp', handleSessionRequest);

  console.log(`MCP server listening on http://${host}:${portno}/mcp`);
  app.listen(portno, host);

}).catch((error) => {
  console.error("Failed to initialize LightUSDLoader:", error);
});
