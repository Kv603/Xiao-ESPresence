import assert from 'node:assert/strict';
import { createServer } from 'node:net';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { once } from 'node:events';
import { Aedes } from '../build/mqtt-broker-private/node_modules/aedes/aedes.js';
import mqtt from '../build/mqtt-broker-private/node_modules/mqtt/build/index.js';

const root = fileURLToPath(new URL('..', import.meta.url));
const compiler = process.env.CXX || 'C:/Apps/Perl/Strawberry/c/bin/g++.exe';
const executable = resolve(root, 'build/native-c6/mqtt-broker.exe');
const build = spawn(compiler, ['-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
  '-Itests/stubs', `-I${resolve(process.env.USERPROFILE, 'Documents/Arduino/libraries/ArduinoJson/src')}`,
  'tests/test-mqtt-broker.cpp', '-o', executable], { cwd: root, stdio: 'inherit', windowsHide: true });
assert.equal((await once(build, 'exit'))[0], 0, 'Broker adapter compilation failed; run tests/Run.ps1 first.');

const broker = await Aedes.createBroker();
const sockets = new Set();
const server = createServer(socket => {
  sockets.add(socket); socket.on('close', () => sockets.delete(socket)); broker.handle(socket);
});
server.listen(0, '127.0.0.1'); await once(server, 'listening');
const url = `mqtt://127.0.0.1:${server.address().port}`;
const packets = [];
broker.on('publish', packet => {
  if (packet.topic.startsWith('devices/') || packet.topic.startsWith('homeassistant/sensor/'))
    packets.push({ topic: packet.topic, payload: packet.payload.toString(), retain: packet.retain, qos: packet.qos });
});
const connect = async options => {
  const client = mqtt.connect(url, { reconnectPeriod: 0, connectTimeout: 3000, ...options });
  await once(client, 'connect'); return client;
};
const end = async (client, force = false) => { if (client) await new Promise(done => client.end(force, {}, done)); };
const observer = await connect({ clientId: 'ha-test' });
let device, inbound = [], discoveryBeforeBirth = 0;
const base = 'devices/espresense_c6-broker-test/';
const discovery = room => `homeassistant/sensor/espresense_c6-broker-test/${room}/config`;
const status = room => `${base}${room}/status`;
const sleep = ms => new Promise(done => setTimeout(done, ms));
const waitUntil = async check => {
  for (let i = 0; i < 200; ++i) { if (check()) return; await sleep(10); }
  throw Error('Timed out waiting for MQTT broker delivery');
};
const retained = async () => {
  const probe = await connect({ clientId: `probe-${Date.now()}` });
  const values = new Map();
  probe.on('message', (topic, payload, packet) => { if (packet.retain) values.set(topic, payload.toString()); });
  await new Promise((done, reject) => probe.subscribe(['homeassistant/sensor/#', `${base}#`], { qos: 1 }, e => e ? reject(e) : done()));
  await sleep(100); await end(probe); return values;
};
const handle = async request => {
  switch (request.op) {
    case 'connect':
      device = await connect({ clientId: request.clientId, will: request.will });
      inbound = []; device.on('message', (topic, payload, packet) => inbound.push({ topic, payload: payload.toString(), retain: packet.retain }));
      break;
    case 'subscribe':
      await new Promise((done, reject) => device.subscribe(request.topic, { qos: request.qos }, e => e ? reject(e) : done())); break;
    case 'publish':
      await new Promise((done, reject) => device.publish(request.topic, request.payload, { qos: request.qos, retain: request.retain }, e => e ? reject(e) : done())); break;
    case 'stop': await end(device, !device?.connected); device = undefined; break;
    case 'drop':
      device.stream.destroy();
      await waitUntil(() => packets.some(p => p.topic === status('living_room') && p.payload === 'offline'));
      break;
    case 'birth':
      discoveryBeforeBirth = packets.filter(p => p.topic === discovery('living_room') && p.payload).length;
      await new Promise((done, reject) => observer.publish('homeassistant/status', 'online', { qos: 1, retain: true }, e => e ? reject(e) : done()));
      await waitUntil(() => inbound.some(p => p.topic === 'homeassistant/status' && p.payload === 'online'));
      return { ok: true, ...inbound.find(p => p.topic === 'homeassistant/status' && p.payload === 'online') };
    case 'check': {
      const values = await retained();
      const room = request.name === 'room-change' || request.name === 'manual-offline' ? 'kitchen' : 'living_room';
      const config = JSON.parse(values.get(discovery(room)));
      assert.equal(config.unique_id, 'espresense_c6-broker-test_device_count');
      assert.equal(config.state_topic, `${base}${room}/count`);
      assert.equal(config.availability_topic, status(room));
      assert(!values.has(`${base}${room}/count`), 'Count must not be retained');
      if (request.name === 'lwt' || request.name === 'manual-offline') {
        assert.equal(values.get(status(room)), 'offline');
        const offline = packets.filter(p => p.topic === status(room) && p.payload === 'offline').at(-1);
        assert.equal(offline.qos, 1); assert.equal(offline.retain, true);
      } else {
        assert(!values.has(status(room)), 'Reconnect must erase retained offline; online remains non-retained');
        const online = packets.filter(p => p.topic === status(room) && p.payload === 'online').at(-1);
        assert.equal(online.qos, 1); assert.equal(online.retain, false);
        assert.equal(packets.filter(p => p.topic === `${base}${room}/count`).at(-1).payload, '2');
      }
      if (request.name === 'room-change') assert(!values.has(discovery('living_room')), 'Old room discovery must be removed');
      if (request.name === 'ha-restart') assert(packets.filter(p => p.topic === discovery(room) && p.payload).length > discoveryBeforeBirth);
      console.log(`PASS ${request.name}`); break;
    }
    default: throw Error(`Unknown broker adapter operation ${request.op}`);
  }
  return { ok: true };
};

const child = spawn(executable, [], { cwd: root, windowsHide: true, stdio: ['pipe', 'pipe', 'inherit'] });
let failure;
let pending = Promise.resolve();
createInterface({ input: child.stdout }).on('line', line => {
  if (!line.startsWith('WIRE ')) { console.log(line); return; }
  pending = pending.then(async () => {
    try { child.stdin.write(`${JSON.stringify(await handle(JSON.parse(line.slice(5))))}\n`); }
    catch (error) { failure = error; child.stdin.write(`${JSON.stringify({ ok: false, error: String(error) })}\n`); }
  });
});
const timeout = setTimeout(() => { failure = Error('Broker acceptance timed out'); child.kill(); }, 30000);
try {
  const [code] = await once(child, 'exit'); await pending;
  if (failure) throw failure;
  assert.equal(code, 0, 'Native broker adapter failed');
} finally {
  clearTimeout(timeout);
  await end(device, true); await end(observer);
  for (const socket of sockets) socket.destroy();
  await new Promise(done => server.close(done));
  await new Promise(done => broker.close(done));
}
