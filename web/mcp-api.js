// SPDX-License-Identifier: Apache-2.0
// LightUSD MCP server module API (post-js for LIGHTUSD_WASM_PRODUCT=mcp).
//
//   const mod = await createLightUSDMCP();
//   const server = new mod.LightUSDMCPServer();
//   server.createContext('session');
//   JSON.parse(server.toolsCall('stage_new', '{}'));
//   server.handleJsonRpc('{"jsonrpc":"2.0","id":1,"method":"tools/list"}');
//
// The module also exposes the legacy-named session functions
// (mcpCreateContext, mcpSelectContext, mcpToolsList, mcpToolsCall,
// mcpResourcesList, mcpResourcesRead) on a default server, so embedders of
// the legacy LightUSDLoaderNative MCP methods can switch modules unchanged.
(function () {
  // memory64 builds take pointer arguments as BigInt; probe once.
  let bigIntPointers = false;
  try { Module['_lightusd_mcp_free'](0); } catch (error) {
    if (!(error instanceof TypeError)) throw error;
    bigIntPointers = true;
  }
  const toPtr = (p) => (bigIntPointers ? BigInt(p) : p);
  const withString = (text, fn) => {
    const ptr = stringToNewUTF8(String(text));
    try { return fn(toPtr(ptr)); } finally { _free(toPtr(ptr)); }
  };
  const takeString = (ptr) => {
    const p = Number(ptr);
    if (!p) return '';
    try { return UTF8ToString(p); } finally { Module['_lightusd_mcp_free'](toPtr(p)); }
  };

  class LightUSDMCPServer {
    constructor() {
      this.handle = Module['_lightusd_mcp_create']();
      if (!this.handle) throw new Error('LightUSDMCPServer: allocation failed');
    }
    delete() {
      if (this.handle) Module['_lightusd_mcp_destroy'](this.handle);
      this.handle = 0;
    }
    check_() {
      if (!this.handle) throw new TypeError('LightUSDMCPServer: deleted server');
      return this.handle;
    }
    createContext(sessionId) {
      return withString(sessionId, (p) => Module['_lightusd_mcp_create_context'](this.check_(), p) !== 0);
    }
    selectContext(sessionId) {
      return withString(sessionId, (p) => Module['_lightusd_mcp_select_context'](this.check_(), p) !== 0);
    }
    destroyContext(sessionId) {
      return withString(sessionId, (p) => Module['_lightusd_mcp_destroy_context'](this.check_(), p) !== 0);
    }
    toolsList() {
      return takeString(Module['_lightusd_mcp_tools_list'](this.check_()));
    }
    toolsCall(toolName, args) {
      const argText = typeof args === 'string' ? args : JSON.stringify(args ?? {});
      return withString(toolName, (n) => withString(argText, (a) =>
        takeString(Module['_lightusd_mcp_tools_call'](this.check_(), n, a))));
    }
    resourcesList() {
      return takeString(Module['_lightusd_mcp_resources_list'](this.check_()));
    }
    resourcesRead(uri) {
      return withString(uri, (p) => takeString(Module['_lightusd_mcp_resources_read'](this.check_(), p)));
    }
    handleJsonRpc(message) {
      const text = typeof message === 'string' ? message : JSON.stringify(message);
      return withString(text, (p) => takeString(Module['_lightusd_mcp_handle_jsonrpc'](this.check_(), p)));
    }
  }
  Module['LightUSDMCPServer'] = LightUSDMCPServer;

  let defaultServer = null;
  const server = () => (defaultServer ??= new LightUSDMCPServer());
  Module['mcpCreateContext'] = (id) => server().createContext(id);
  Module['mcpSelectContext'] = (id) => server().selectContext(id);
  Module['mcpToolsList'] = () => server().toolsList();
  Module['mcpToolsCall'] = (name, args) => server().toolsCall(name, args);
  Module['mcpResourcesList'] = () => server().resourcesList();
  Module['mcpResourcesRead'] = (uri) => server().resourcesRead(uri);
})();
