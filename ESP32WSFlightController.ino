

// Import required libraries
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebSrv.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// Replace with your network credentials
const char* ssid = "SSID_HERE";
const char* password = "PASSWORD_HERE";
bool armed = false;
char* buttonData = "";
int currentButton = -1;
char* lastButton = "";
char* inputData = "";
String pressedButton;
const int ledPin = 2;
float value = 0;
float currentValue;
int speedValue = 0;
bool disconnected = false;
bool armedDebounced = false;
StaticJsonDocument<256> doc;

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

Servo ESC;
Servo AileronL;
Servo AileronR;
Servo Rudder;
Servo ElevatorL;
Servo ElevatorR;


const char index_html[] PROGMEM = R"rawliteral(

<style>
body {
    background: #444;
    color: white;
    font-family: monospace;
}
#gamepads>* {
    background: #333;
    padding: 1em;
    margin: 10px 5px 0 0;
}
#gamepads pre {
    white-space: pre-wrap;
}

.head {
  display: flex;
}
.head .id {
  flex: 1 1 auto;
}
.head .index,
.head .id {
  display: inline-block;
  background: #222;
  padding: 0.5em;
}
.head .index {
}

.info .label {
  width: 7em;
  display: inline-block;
}
.info>div {
  padding: 0.25em;
  background: #222;
  margin: 0.25em 0.25em 0 0;
}

.inputs {
  display: flex;
}
.axes {
  display: flex;
  align-items: flex-start;
}

.svg text {
  color: #CCC;
  font-family: monospace;
}

.axes svg text {
  font-size: 0.6px;
}
.buttons svg text {
  font-size: 1.2px;
}
.axes>div, .buttons>div {
  display: inline-block;
  background: #222;
}
.axes>div {
  margin: 2px 5px 0 0;
}
.buttons>div {
  margin: 2px 2px 0 0;
}
</style>
<body>

<h1>HTML5 Gamepad Terminal</h1>
<div>running: <span id="running"></span></div>
<div id="gamepads"></div>
</body>
<script>

const fudgeFactor = 2;  // because of bug in Chrome related to svg text alignment font sizes can not be < 1
const runningElem = document.querySelector('#running');
const gamepadsElem = document.querySelector('#gamepads');
const gamepadsByIndex = {};

const buttonInput = {
    button: "",
    value: 0
};

const axisInput = {
    joystick: "",
    x: 0,
    y: 0
};

const controllerTemplate = `
<div>
  <div class="head"><div class="index"></div><div class="id"></div></div>
  <div class="info"><div class="label">connected:</div><span class="connected"></span></div>
  <div class="info"><div class="label">mapping:</div><span class="mapping"></span></div>
  <div class="inputs">
    <div class="axes"></div>
    <div class="buttons"></div>
  </div>
</div>
`;
const axisTemplate = `
<svg viewBox="-2.2 -2.2 4.4 4.4" width="128" height="128">
    <circle cx="0" cy="0" r="2" fill="none" stroke="#888" stroke-width="0.04" />
    <path d="M0,-2L0,2M-2,0L2,0" stroke="#888" stroke-width="0.04" />
    <circle cx="0" cy="0" r="0.22" fill="red" class="axis" />
    <text text-anchor="middle" fill="#CCC" x="0" y="2">0</text>
</svg>
`

const buttonTemplate = `
<svg viewBox="-2.2 -2.2 4.4 4.4" width="64" height="64">
  <circle cx="0" cy="0" r="2" fill="none" stroke="#888" stroke-width="0.1" />
  <circle cx="0" cy="0" r="0" fill="none" fill="red" class="button" />
  <text class="value" dominant-baseline="middle" text-anchor="middle" fill="#CCC" x="0" y="0">0.00</text>
  <text class="index" alignment-baseline="hanging" dominant-baseline="hanging" text-anchor="start" fill="#CCC" x="-2" y="-2">0</text>
</svg>
`;

function addGamepad(gamepad) {
  console.log('add:', gamepad.index);
  const elem = document.createElement('div');
  elem.innerHTML = controllerTemplate;

  const axesElem = elem.querySelector('.axes');
  const buttonsElem = elem.querySelector('.buttons');
  
  const axes = [];
  for (let ndx = 0; ndx < gamepad.axes.length; ndx += 2) {
    const div = document.createElement('div');
    div.innerHTML = axisTemplate;
    axesElem.appendChild(div);
    axes.push({
      axis: div.querySelector('.axis'),
      value: div.querySelector('text'),
    });
  }

  const buttons = [];
  for (let ndx = 0; ndx < gamepad.buttons.length; ++ndx) {
    const div = document.createElement('div');
    div.innerHTML = buttonTemplate;
    buttonsElem.appendChild(div);
    div.querySelector('.index').textContent = ndx;
    buttons.push({
      circle: div.querySelector('.button'),
      value: div.querySelector('.value'),
    });
  }

  gamepadsByIndex[gamepad.index] = {
    gamepad,
    elem,
    axes,
    buttons,
    index: elem.querySelector('.index'),
    id: elem.querySelector('.id'),
    mapping: elem.querySelector('.mapping'),
    connected: elem.querySelector('.connected'),
  };
  gamepadsElem.appendChild(elem);
}

function removeGamepad(gamepad) {
  const info = gamepadsByIndex[gamepad.index];
  if (info) {
    delete gamepadsByIndex[gamepad.index];
    info.elem.parentElement.removeChild(info.elem);
  }
}

function addGamepadIfNew(gamepad) {
  const info = gamepadsByIndex[gamepad.index];
  if (!info) {
    addGamepad(gamepad);
  } else {
    // This broke sometime in the past. It used to be
    // the same gamepad object was returned forever.
    // Then Chrome only changed to a new gamepad object
    // is returned every frame.
    info.gamepad = gamepad;
  }
}

function handleConnect(e) {
  console.log('connect');
  addGamepadIfNew(e.gamepad);
}

function handleDisconnect(e) {
  console.log('disconnect');
  removeGamepad(e.gamepad);
}

const t = String.fromCharCode(0x26AA);
const f = String.fromCharCode(0x26AB);
function onOff(v) {
  return v ? t : f;
}

const keys = ['index', 'id', 'connected', 'mapping', /*'timestamp'*/];
function processController(info) {
  const {elem, gamepad, axes, buttons} = info;
  const lines = [`gamepad  : ${gamepad.index}`];
  for (const key of keys) {
    info[key].textContent = gamepad[key];
  }
  axes.forEach(({axis, value}, ndx) => {
    const off = ndx * 2;
    axis.setAttributeNS(null, 'cx', gamepad.axes[off    ] * fudgeFactor);
    axis.setAttributeNS(null, 'cy', gamepad.axes[off + 1] * fudgeFactor);
    value.textContent = `${gamepad.axes[off].toFixed(2).padStart(5)},${gamepad.axes[off + 1].toFixed(2).padStart(5)}`;
    if (value != 0){
      axisInput.joystick = ndx;
      axisInput.x = gamepad.axes[off    ] * fudgeFactor;
      axisInput.y = gamepad.axes[off + 1] * fudgeFactor;
      sendAxesToWebSocket(axisInput);
    }
    
  });
    
  buttons.forEach(({circle, value}, ndx) => {
    const button = gamepad.buttons[ndx];
    circle.setAttributeNS(null, 'r', button.value * fudgeFactor);
    circle.setAttributeNS(null, 'fill', button.pressed ? 'red' : 'gray');
    if (button.value != 0){
      buttonInput.button = ndx;
      buttonInput.value = button.value;
      sendButtonToWebSocket(buttonInput);
    }
    value.textContent = `${button.value.toFixed(2)}`;
  });
  
//  lines.push(`axes     : [${gamepad.axes.map((v, ndx) => `${ndx}: ${v.toFixed(2).padStart(5)}`).join(', ')} ]`);
//  lines.push(`buttons  : [${gamepad.buttons.map((v, ndx) => `${ndx}: ${onOff(v.pressed)} ${v.value.toFixed(2)}`).join(', ')} ]`);
 // elem.textContent = lines.join('\n');
  
}

function addNewPads() {
  const gamepads = navigator.getGamepads();
  for (let i = 0; i < gamepads.length; i++) {
    const gamepad = gamepads[i]
    if (gamepad) {
      addGamepadIfNew(gamepad);
    }
  }
}

window.addEventListener("gamepadconnected", handleConnect);
window.addEventListener("gamepaddisconnected", handleDisconnect);
    
// WebSocket connection
var gateway = `ws://${window.location.hostname}/ws`;
var websocket;
window.addEventListener('load', onLoad);
  
function initWebSocket() {
    console.log('Trying to open a WebSocket connection...');
    websocket = new WebSocket(gateway);
    websocket.onopen = onOpen;
    websocket.onclose = onClose;
    websocket.onmessage = onMessage;
}
  
function onOpen(event) {
    console.log('Connection opened');
}
  
function onClose(event) {
    console.log('Connection closed');
    setTimeout(initWebSocket, 2000);
}
  
function onMessage(event) {
    var transmitButton;
    //console.log(event.data);
    if (event.data) {
    //transmitButton = pressedButton;
    //console.log(event.data);
    }
}
  
function onLoad(event) {
    initWebSocket();
}
  
      // Function to send the pressed button to the WebSocket
function sendButtonToWebSocket(buttonInputData) {
    if(buttonInputData.value != 0){
      buttonJsonData = JSON.stringify(buttonInputData);
      console.log(buttonJsonData);
      websocket.send(buttonJsonData);
      
    }
}
    
function sendAxesToWebSocket(axisInputData) {
  if(axisInputData.x != 0 || axisInputData.y != 0){
      axisJsonInput = JSON.stringify(axisInputData);
      console.log(axisJsonInput);
      websocket.send(axisJsonInput);
  }
}

function process() {
  runningElem.textContent = ((performance.now() * 0.001 * 60 | 0) % 100).toString().padStart(2, '0');
  addNewPads();  // some browsers add by polling, others by event

  Object.values(gamepadsByIndex).forEach(processController);
  requestAnimationFrame(process);
}
requestAnimationFrame(process);

</script>
)rawliteral";

void notifyClients() {
  ws.textAll(String(inputData));
}


char* debounceInput(char* button){
  char* debouncedButton = "";
  lastButton = button;
  while (button == lastButton){
    if (button != lastButton){
      
      debouncedButton = button;
      break;
    }
  }
  
  
  //might stop code while button is being press find a way to do asynchrounously 
  return debouncedButton;
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
  
    data[len] = 0;
    if (strcmp((char*)data, "button") != 0) {
      //Serial.println((char*)data);
      // Serial.printf("data: " , data , ". len: " , len);
      inputData = (char*)data;
      Serial.println(inputData);
      notifyClients();
    }
    
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      disconnected = false;
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      disconnected = true; 
      break;
    case WS_EVT_DATA:
      handleWebSocketMessage(arg, data, len);
      break;
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      disconnected = true; 
      break;
  }
}

//IPAddress local_IP(172, 26, 192, 208);
//IPAddress gateway(172, 26, 192, 247);
//IPAddress subnet(255, 255, 255, 0);
//IPAddress primaryDNS(8, 8, 8, 8);   //optional
//IPAddress secondaryDNS(8, 8, 4, 4); //optional

void initWebSocket() {
  ws.onEvent(onEvent);
  server.addHandler(&ws);
}

String processor(const String& var){
  Serial.println(var);
  /*
  if(var == "STATE"){
    if (ledState){
      return "ON";
    }
    else{
      return "OFF";
    }
  }
  return String();
  */
}


void setup(){
  // Serial port for debugging purposes
  
  Serial.begin(115200);
  ESC.attach(21,1000,2000);
  AileronL.attach(19);
  AileronR.attach(18);
  Rudder.attach(34);
  ElevatorL.attach(33);
  ELevatorR.attach(32);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  //if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    //Serial.println("STA Failed to configure");
  //}
  
  WiFi.mode(WIFI_STA);
  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi..");
  }

  //if (!WiFi.config(localIP, localGateway, subnet)){
   // Serial.println("STA Failed to configure");
  //  return false;
  //}
  // Print ESP Local IP Address
  Serial.println(WiFi.localIP());

  initWebSocket();

  // Route for root / web page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html, processor);
  });

  // Start server
  server.begin();

}



void parseData(){

  DeserializationError error = deserializeJson(doc, inputData);
  
  int button = doc["button"]; 
  float value = doc["value"]; 
  float x = doc["x"];
  float y = doc["y"];
  int joystick = doc["joystick"];
  if(error) {
  //Serial.println("DeserializationError! deserializeJson() failed with code");
  //Serial.println(error.c_str());
  }else{
    
    currentButton = button;
    currentValue = value * 100;
    speedValue = map(currentValue, 0,100,0,180);
    Serial.println(currentButton); 
    Serial.println(value);
    //pitch
    elevatorValue = map(x * 50, 0,100, 0,180);
    //roll
    aileronValue = map(y * 50, 0,100, 0,180)
  }
}

void loop() {
  parseData();
  
  if (discconected == true){
    ESC.write(0); 
    ElevatorL.write(20);
    ElevatorR.write(-20);
  }

  
  ws.cleanupClients();
  //currentButton = debounceInput(buttonData);
  
  if (joystick == 0 && x != 0 || y != 0){
    ElevatorL.write(elevatorValue);
    ElevatorR.write(-elevatorValue);
    AileronL.write(aileronValue);
    AileronR.write(aileronValue);
  }
  
  
  //Armed/Disarm
  if (currentButton == 16 && armedDebounce == false){
    armed = !armed;
    armedDebounce = true;
    if (armed == true){
      Serial.println("!SYSTEMS ARMED!");
    }else if(armed == false){
      Serial.println("!SYSTEMS DISARMED!");
    }
  }else if(currentButton != 16){
    armedDebounce = false;
  }
  
  
  //Led On/Off
  if (currentButton == 1 && armed == true){
      digitalWrite(ledPin, HIGH);
  }else if(currentButton == 1 && armed == false){
      digitalWrite(ledPin, LOW);
      Serial.println("System is not armed!");
  }
  //Test
  if (currentButton == 2 ){
     Serial.println("test");
  }
  
  
  //Rudder(Left)
  if (currentButton == 4 && armed == true){
    Rudder.write(20);
    Serial.println("Rudder Left");
  }else if(currentButton != 4 && armed == true){
    Rudder.write(0);
  }else if(currentButton == 4 && armed == false){
    Serial.println("System is not armed!");
  }
  
  //Rudder(Right)
  if (currentButton == 5 && armed == true){
    Rudder.write(-20);
    Serial.println("Rudder Right");
  }else if(currentButton != 4 && armed == true){
    Rudder.write(0);
  }else if(currentButton == 5 && armed == false){
    Serial.println("System is not armed!");
  }
   
 
  //Throttle 
  if (currentButton == 7 && armed == true){
    ESC.write(speedValue); 
  }else if(currentButton == 7 && armed == false){
    Serial.println("System is not armed!");
  }
  delay(50);
  
  
}
