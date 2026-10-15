const mqtt = require('mqtt');

// Secure WebSocket connection for EMQX Serverless
const brokerUrl = 'wss://k68f60c1.ala.asia-southeast1.emqxsl.com:8084/mqtt';

const options = {
  clientId: 'NodeJS_Client_' + Math.random().toString(16).substring(2, 8),
  username: 'Van', // Put your EMQX username here
  password: '123qweasd', // Put your EMQX password here
};

console.log('Attempting to connect to EMQX Cloud...');
const client = mqtt.connect(brokerUrl, options);

client.on('connect', () => {
  console.log('Connected successfully!');
  
  // Subscribe to your ESP32's telemetry topic
  client.subscribe('elective3/gas_node/data', (err) => {
    if (!err) {
      console.log('Subscribed to gas sensor data. Waiting for messages...\n');
    } else {
      console.error('Subscription error:', err);
    }
  });
});

// Fire this event every time a new message arrives
client.on('message', (topic, message) => {
  console.log(`[${topic}]`);
  
  // Parse the JSON string back into a JavaScript object
  try {
    const data = JSON.parse(message.toString());
    console.log(`MQ-2 Avg: ${data.mq2.avg} | Alarm Status: ${data.alarm}`);
    console.log(data); // Print the full object
    console.log('-----------------------------------');
  } catch (e) {
    console.log('Raw text:', message.toString());
  }
});

client.on('error', (err) => {
  console.error('Connection error:', err);
});