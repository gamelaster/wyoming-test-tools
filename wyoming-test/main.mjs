import net from 'net';
import fs from 'fs/promises';
import MyStreamHandler from './MyStreamHandler.mjs';

const sleep = ms => new Promise(r => setTimeout(r, ms));

const client = new net.Socket();

/**
 * @type MyStreamHandler
 */
let msh;

function writePacket(header, data = null, payload = null) {
  let headerPacket = {...header};
  let dataBuf, payloadBuf;
  if (data != null) {
    dataBuf = Buffer.from(JSON.stringify(data), 'utf8');
    headerPacket.data_length = dataBuf.length;
  }
  if (payload != null) {
    headerPacket.payload_length = payload.length;
  }
  client.write(JSON.stringify(headerPacket) + '\n');
  if (data != null) client.write(dataBuf);
  if (payload != null) client.write(payload);
  console.log('send', {header, data, payload});
}

client.connect(10700, '127.0.0.1', async () => {
	console.log('Connected');
  msh = new MyStreamHandler(client);
  parsePackets();
	writePacket({
    "type": "describe",
    "version": "1.5.2"
  });

  await sleep(6000);
  writePacket(
    {"type": "detection", "version": "1.5.2"},
    {"name": "alexa_v0.1", "timestamp": 2370} // TODO: from start / 1000 (ns)
  );
  writePacket(
    {"type": "transcribe", "version": "1.5.2"},
    {"language": "en"}
  );
  await sleep(1000);
  writePacket(
    {"type": "voice-started", "version": "1.5.2"},
    {"timestamp": 2970}
  );
  await sleep(4000);
  writePacket(
    {"type": "voice-stopped", "version": "1.5.2"},
    {"timestamp": 3525}
  );
  await sleep(1000);
  writePacket(
    {"type": "transcript", "version": "1.5.2",},
    {"text": " Turn on the light."}
  );
  writePacket(
    {"type": "synthesize", "version": "1.5.2"},
    {"text": "Turned on the lights", "voice": {"name": "en_US-ryan-low"}}
  );
  writePacket(
    {"type": "audio-start", "version": "1.5.2"},
    {"rate": 16000, "width": 2, "channels": 1, "timestamp": 0}
  );

  const sample = await fs.readFile('sample.raw')

  for (let i = 0; i < sample.length; i += 2048) {
    let size = 2048;
    if (i + 2048 > sample.length) {
      size = sample.length - i;
    }
    writePacket(
      {"type": "audio-chunk", "version": "1.5.2"},
      {"rate": 16000, "width": 2, "channels": 1, "timestamp": 0},
      sample.subarray(i, i + size)
    );
  }

  writePacket(
    {"type": "audio-stop", "version": "1.5.2", "data_length": 33},
    {"timestamp": 1.0640000000000005},
  );

  // writePacket(
  //   {"type": "audio-chunk", "version": "1.5.2"},
  //   {"rate": 16000, "width": 2, "channels": 1, "timestamp": 0},
  //   //DATA!!
  // );

  // every 2 seconds ping!!!
});

function onPacket(packet) {
  let print = true;
  if (packet.header.type === 'info') {
    writePacket({
      "type": "run-satellite",
      "version": "1.5.2"
    });
  } else if (packet.header.type === 'run-pipeline') {
    writePacket({
      "type": "detect",
      "version": "1.5.2",
      "data_length": 15
    }, {
      "names": null
    });
  } else if (packet.header.type === 'audio-chunk') {
    console.log('audio-chunk');
    print = false;
  }
  if (print) console.log('received', packet);
}

async function parsePackets() {
  while (1) {
    let header, data, payload;
    header = data = payload = null;

    const headerJson = (await msh.readUntil('\n')).toString();
    header = JSON.parse(headerJson);
    if (header.data_length ?? 0 != 0) {
      const dataJson = (await msh.read(header.data_length)).toString();
      data = JSON.parse(dataJson);
    }
    if (header.payload_length ?? 0 != 0) {
      payload = await msh.read(header.payload_length);
    }
    onPacket({
      header, data, payload
    });
  }
}

// client.on('data', data => {
//   parsePacket(data);
//   // console.log('unhandled', res);
// });

client.on('close', () => {
	console.log('Connection closed');
});