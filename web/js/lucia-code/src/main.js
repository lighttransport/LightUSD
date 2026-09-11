import { LuciaApp } from './lucia-app.js';

const app = new LuciaApp(document.getElementById('app'));
app.start().catch((error) => {
  console.error(error);
  document.getElementById('app').innerHTML = `<main style="padding:32px"><h1>Lucia could not start</h1><p>${error.message}</p><p>Build the LightUSD WASM module, then reload this page.</p></main>`;
});
