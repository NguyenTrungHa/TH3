#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_arduino_version.h>

// ===== project_config.h merged here =====
namespace config {
constexpr int kLedPins[2] = {13, 12};
constexpr uint32_t kLedPwmHz = 1000;
constexpr uint8_t kLedPwmBits = 8;
constexpr int kMotor1In1 = 5;
constexpr int kMotor1In2 = 17;
constexpr uint32_t kMotorPwmHz = 1000;
constexpr int kSdaPin = 33;
constexpr int kSclPin = 32;
constexpr uint8_t kUnoAddress = 0x08;
constexpr size_t kPacketSize = 4;
constexpr uint32_t kPollIntervalMs = 2000;
constexpr uint32_t kI2cFrequencyHz = 100000;
constexpr uint16_t kI2cTimeoutMs = 50;
constexpr uint32_t kSerialBaud = 115200;
}  // namespace config

// MOTOR2 is strictly out of scope. Never configure or access its pins.
// LED outputs are active HIGH, as confirmed by the user.

// ===== controller.h merged here =====
// Pure logic only: this file has no GPIO or PWM operations.
enum class State { Stopped, Running, Fault };
struct Channel {
  const char* name;
  State state = State::Stopped;
  uint32_t started = 0;
  uint32_t revision = 0;
  unsigned duty = 50;
  const char* reason = "BOOT";
};
constexpr uint32_t kRunLimitMs = 20000;
// ---- Explicit prototypes (helps Arduino .ino preprocessing) ----
void stopChannel(Channel& c, const char* reason);
void faultChannel(Channel& c, const char* reason);
void updateChannel(Channel& c, uint32_t now, bool sensorValid);
bool startChannel(Channel& c, uint32_t now, bool sensorValid, uint32_t expectedRevision);

inline void stopChannel(Channel& c, const char* reason) {
  c.state = State::Stopped;
  c.reason = reason;
  ++c.revision;
}
inline void faultChannel(Channel& c, const char* reason) {
  c.state = State::Fault;
  c.reason = reason;
  ++c.revision;
}
inline void updateChannel(Channel& c, uint32_t now, bool sensorValid) {
  if (c.state == State::Fault) {
    stopChannel(c, c.reason);
    return;
  }
  if (c.state != State::Running) return;
  if (!sensorValid) faultChannel(c, "SENSOR_ERROR");
  else if (uint32_t(now - c.started) >= kRunLimitMs)
    faultChannel(c, "TIMEOUT");
}
inline bool startChannel(Channel& c, uint32_t now, bool sensorValid,
                         uint32_t expectedRevision) {
  if (!sensorValid || c.state == State::Fault ||
      expectedRevision != c.revision) return false;
  if (c.state == State::Running) return true; // Never extend run time.
  c.state = State::Running;
  c.started = now;
  c.reason = "START";
  ++c.revision;
  return true;
}

// ===== web_page.h merged here =====
const char kWebPage[] PROGMEM = R"HTML(
<!doctype html><html lang="vi"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TH03 · Nhà kính</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#eef3f2;color:#183c35;font:16px system-ui,sans-serif}
main{max-width:1050px;margin:auto;padding:32px 20px}h1{font-size:32px;margin:10px 0}small{color:#53736b}
.notice{padding:16px;background:#fff1ca;border-radius:12px;margin:22px 0}.sensors,.cards{display:grid;gap:16px;grid-template-columns:repeat(3,1fr)}
article,.sensors div{background:white;border:1px solid #d8e3df;border-radius:16px;padding:22px}.value{font-size:28px;display:block;margin-top:8px}
.cards{margin:24px 0}h2{margin:0 0 16px}.state{font-weight:bold}.muted{color:#637a74}button,select{font:inherit;padding:11px;border-radius:8px;border:1px solid #b4c9c2}
button{cursor:pointer;background:#166953;color:white}button.stop{background:#9d3838}button:disabled{opacity:.4;cursor:not-allowed}.actions{display:flex;gap:10px;margin-top:20px}select{width:100%;margin-top:8px}#message{min-height:26px}progress{width:100%;accent-color:#16866b}
@media(max-width:700px){.cards,.sensors{grid-template-columns:1fr}}
</style><main><small>CSE3069 / TH03</small><h1>Điều khiển nhà kính</h1>
<div class="notice"><strong>LED1 · LED2 · MOTOR1 — ĐIỀU KHIỂN THẬT</strong><br>LED dùng PWM, HIGH bật / LOW tắt. Trạng thái hiển thị là lệnh xuất, không phải phản hồi đo từ tải. MOTOR2 ngoài phạm vi.</div>
<p id="connection" role="status">Đang kết nối…</p>
<section class="sensors"><div>Nhiệt độ <b class="value" id="temperature">—</b></div><div>Độ ẩm <b class="value" id="humidity">—</b></div><div>Dữ liệu UNO <b id="sensor" class="value">—</b></div></section>
<p class="muted">Giải mã cảm biến theo giả định đã chọn. Chỉ cho chạy logic khi có mẫu hợp lệ; dữ liệu quá 6 giây được coi là lỗi.</p>
<section class="cards" id="cards"></section><button class="stop" id="stopAll">DỪNG MOTOR1 VÀ HAI KÊNH LED</button>
<p id="message" role="status"></p><small>Mỗi kênh tự dừng sau 20 giây. Đóng trang không làm mất bộ đếm trên ESP32.</small></main>
<script>
const names=['LED1','LED2','MOTOR1'];let snapshot=null,online=false,busy=false;
const reasons={BOOT:'Khởi động',START:'Lệnh bật',STOP:'Lệnh tắt',TIMEOUT:'Hết 20 giây',SENSOR_ERROR:'Dữ liệu UNO lỗi / quá cũ',PWM_ERROR:'PWM error',INVALID_COMMAND:'Lệnh không hợp lệ'};
document.querySelector('#cards').innerHTML=names.map((n,i)=>`<article><h2>${n}</h2><p class="state" id="state${i}">CHƯA XÁC NHẬN</p><p id="reason${i}">—</p><progress id="progress${i}" max="20" value="0"></progress><p id="time${i}">—</p>${i<2?`<label>Mức PWM dự kiến<select id="duty${i}" aria-label="PWM ${n}"><option>25</option><option selected>50</option><option>90</option></select></label>`:`<label>Pump PWM (%)<select id="duty${i}" aria-label="Pump PWM"><option>25</option><option>50</option><option>75</option><option selected>100</option></select></label><p>Duty %, not measured flow.</p>`}<div class="actions"><button id="start${i}">BẬT</button><button class="stop" id="stop${i}">TẮT</button></div></article>`).join('');
function controls(){names.forEach((_,i)=>{document.querySelector('#start'+i).disabled=!online||busy||!snapshot?.sensorValid||!snapshot?.channels[i]?.hardwareEnabled;document.querySelector('#stop'+i).disabled=!online;document.querySelector('#duty'+i).disabled=!online||busy;});document.querySelector('#stopAll').disabled=!online;}
async function request(url,options={}){const r=await fetch(url,{...options,signal:AbortSignal.timeout(3500),cache:'no-store'});const data=await r.json();if(!r.ok)throw new Error(data.error||'Lệnh bị từ chối');return data;}
function lost(){online=false;document.querySelector('#connection').textContent='Mất kết nối — trạng thái chưa xác nhận';names.forEach((_,i)=>{document.querySelector('#state'+i).textContent='CHƯA XÁC NHẬN';document.querySelector('#time'+i).textContent='—';});controls();}
function render(s){snapshot=s;online=true;document.querySelector('#connection').textContent='ESP32: LED1 / LED2 / MOTOR1 hardware';document.querySelector('#temperature').textContent=s.sensorValid?s.temperature.toFixed(1)+' °C':'—';document.querySelector('#humidity').textContent=s.sensorValid?s.humidity.toFixed(1)+' %':'—';document.querySelector('#sensor').textContent=s.sensorValid?'HỢP LỆ':'CHƯA HỢP LỆ';s.channels.forEach((c,i)=>{document.querySelector('#state'+i).textContent=({STOPPED:'DỪNG',RUNNING:c.hardwareEnabled?'RUNNING (OUTPUT ON)':'OUTPUT ERROR',FAULT:'LỖI'})[c.state];document.querySelector('#reason'+i).textContent=reasons[c.reason]||c.reason;document.querySelector('#time'+i).textContent='Còn '+Math.ceil(c.remainingMs/1000)+' giây';document.querySelector('#progress'+i).value=c.remainingMs/1000;document.querySelector('#duty'+i).value=c.duty;});controls();}
async function command(channel,action,duty){if(action!=='STOP'&&busy)return;const revision=snapshot?.channels[channel]?.revision;busy=true;controls();try{const body=new URLSearchParams({channel,action});if(revision!==undefined)body.set('revision',revision);if(duty!==undefined)body.set('duty',duty);await request('/api/control',{method:'POST',body});document.querySelector('#message').textContent='Command processed by ESP32.';}catch(e){document.querySelector('#message').textContent=e.message;}finally{busy=false;try{render(await request('/api/status'));}catch{lost();}controls();}}
names.forEach((_,i)=>{document.querySelector('#start'+i).onclick=()=>command(i,'START');document.querySelector('#stop'+i).onclick=()=>command(i,'STOP');document.querySelector('#duty'+i).onchange=e=>command(i,'PWM',e.target.value);});document.querySelector('#stopAll').onclick=()=>command('all','STOP');
async function poll(){try{render(await request('/api/status'));}catch{lost();}setTimeout(poll,1000);}controls();poll();
</script></html>
)HTML";

// ===== original main.cpp implementation =====
namespace {
WebServer server(80);
Channel channels[] = {{"LED1"}, {"LED2"}, {"MOTOR1"}};
bool i2cReady = false, sampleValid = false, haveSample = false;
uint32_t lastPollMs = 0, lastSampleMs = 0;
float temperature = 0, humidity = 0;
constexpr uint32_t kStaleMs = 6000;

hw_timer_t* periodicTimer = nullptr;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t timerTicks = 0;
uint32_t reportedTicks = 0;
bool ledReady[2] = {false, false};
uint32_t ledApplied[2] = {UINT32_MAX, UINT32_MAX};
bool motorReady = false;
uint32_t motorApplied = UINT32_MAX;

void ARDUINO_ISR_ATTR onTimer() {
  portENTER_CRITICAL_ISR(&timerMux);
  timerTicks = timerTicks + 1;
  portEXIT_CRITICAL_ISR(&timerMux);
}
uint32_t getTimerTicks() {
  portENTER_CRITICAL(&timerMux);
  const uint32_t ticks = timerTicks;
  portEXIT_CRITICAL(&timerMux);
  return ticks;
}
void beginPeriodicTimer() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  periodicTimer = timerBegin(1000000);
  if (!periodicTimer) return;
  timerAttachInterrupt(periodicTimer, &onTimer);
  timerAlarm(periodicTimer, 1000000, true, 0);
#else
  periodicTimer = timerBegin(0, 80, true);
  if (!periodicTimer) return;
  timerAttachInterrupt(periodicTimer, &onTimer, true);
  timerAlarmWrite(periodicTimer, 1000000, true);
  timerAlarmEnable(periodicTimer);
#endif
}
void applyOutputs() {
  // Explicitly scoped to two LEDs and MOTOR1; never access MOTOR2.
  digitalWrite(config::kMotor1In2, LOW);
  const uint32_t motorDuty = channels[2].state == State::Running
      ? (channels[2].duty * 255U + 50U) / 100U : 0;
  if (motorReady && motorDuty != motorApplied) {
    if (!ledcWrite(config::kMotor1In1, motorDuty)) {
      ledcDetach(config::kMotor1In1);
      digitalWrite(config::kMotor1In1, LOW);
      pinMode(config::kMotor1In1, OUTPUT);
      digitalWrite(config::kMotor1In1, LOW);
      motorReady = false;
      faultChannel(channels[2], "PWM_ERROR");
    } else motorApplied = motorDuty;
  }
  for (unsigned i=0;i<2;++i) {
    if (!ledReady[i]) continue;
    const uint32_t duty = channels[i].state == State::Running
        ? (channels[i].duty * 255U + 50U) / 100U : 0;
    if (duty == ledApplied[i]) continue;
    if (!ledcWrite(config::kLedPins[i], duty)) {
      ledcDetach(config::kLedPins[i]);
      digitalWrite(config::kLedPins[i], LOW);
      pinMode(config::kLedPins[i], OUTPUT);
      digitalWrite(config::kLedPins[i], LOW);
      ledReady[i] = false;
      faultChannel(channels[i], "PWM_ERROR");
    } else ledApplied[i] = duty;
  }
}
void beginLeds() {
  for (unsigned i=0;i<2;++i) {
    const int pin = config::kLedPins[i];
    digitalWrite(pin, LOW);
    pinMode(pin, OUTPUT);
    ledReady[i] = ledcAttach(pin, config::kLedPwmHz, config::kLedPwmBits);
    if (!ledReady[i]) {
      digitalWrite(pin, LOW);
      faultChannel(channels[i], "PWM_ERROR");
    }
  }
  applyOutputs();
}
void beginMotor1() {
  digitalWrite(config::kMotor1In1, LOW);
  digitalWrite(config::kMotor1In2, LOW);
  pinMode(config::kMotor1In1, OUTPUT);
  pinMode(config::kMotor1In2, OUTPUT);
  channels[2].duty = 100; // Preserve the previous full-speed START default.
  motorReady = ledcAttach(config::kMotor1In1, config::kMotorPwmHz, 8);
  if (!motorReady) {
    digitalWrite(config::kMotor1In1, LOW);
    faultChannel(channels[2], "PWM_ERROR");
  }
  applyOutputs();
}

bool sensorValid(uint32_t now) {
  return i2cReady && haveSample && sampleValid && uint32_t(now-lastSampleMs)<kStaleMs;
}
const char* stateName(State s) {
  return s==State::Running ? "RUNNING" : s==State::Fault ? "FAULT" : "STOPPED";
}
void updateState() {
  const uint32_t now=millis();
  State oldStates[3];
  for(unsigned i=0;i<3;++i) {
    auto& c=channels[i];
    oldStates[i]=c.state;
    updateChannel(c,now,sensorValid(now));
  }
  applyOutputs(); // Evaluate MOTOR1 and cut drive before any channel logging.
  for(unsigned i=0;i<3;++i) {
    const auto& c=channels[i];
    if(oldStates[i]!=c.state) Serial.printf("[%lu] %s %s -> %s reason=%s\n",
      static_cast<unsigned long>(now),c.name,stateName(oldStates[i]),stateName(c.state),c.reason);
  }
}
void readUno() {
  uint8_t bytes[config::kPacketSize]={};
  const size_t received=Wire.requestFrom(config::kUnoAddress,config::kPacketSize,true);
  size_t count=0;
  while(Wire.available() && count<sizeof(bytes)) bytes[count++]=Wire.read();
  while(Wire.available()) Wire.read();
  if(received!=sizeof(bytes)||count!=sizeof(bytes)) {
    sampleValid=false;
    updateState();
    Serial.println("I2C_ERROR: incomplete packet");
    return;
  }
  temperature=((uint16_t(bytes[0])<<8)|bytes[1])/10.0f;
  humidity=((uint16_t(bytes[2])<<8)|bytes[3])/10.0f;
  // Provisional plausibility limits, not validation of UNO's unknown protocol.
  sampleValid=temperature<=80.0f && humidity<=100.0f;
  if(sampleValid) {haveSample=true;lastSampleMs=millis();}
  updateState();
  Serial.printf("RAW=%02X %02X %02X %02X ASSUMED T=%.1f RH=%.1f valid=%d\n",
    unsigned(bytes[0]),unsigned(bytes[1]),unsigned(bytes[2]),unsigned(bytes[3]),
    temperature,humidity,sampleValid);
}
void sendStatus() {
  const uint32_t now=millis();
  String json="{\"hardwareEnabled\":true,\"sensorValid\":";
  json+=sensorValid(now)?"true":"false";
  json+=",\"temperature\":"+String(temperature,1)+",\"humidity\":"+String(humidity,1);
  json+=",\"timerReady\":"+String(periodicTimer ? "true" : "false");
  json+=",\"timerTicks\":"+String(getTimerTicks());
  json+=",\"channels\":[";
  for(unsigned i=0;i<3;++i) {
    const auto& c=channels[i];
    const uint32_t elapsed=now-c.started;
    const uint32_t remaining=c.state==State::Running && elapsed<kRunLimitMs?kRunLimitMs-elapsed:0;
    if(i)json+=',';
    json+="{\"name\":\""+String(c.name)+"\",\"state\":\""+stateName(c.state);
    json+="\",\"reason\":\""+String(c.reason)+"\",\"revision\":"+String(c.revision);
    json+=",\"hardwareEnabled\":"+String((i==2 ? motorReady : ledReady[i]) ? "true" : "false");
    json+=",\"duty\":"+String(c.duty)+",\"remainingMs\":"+String(remaining)+"}";
  }
  json+="]}";
  server.sendHeader("Cache-Control","no-store");
  server.send(200,"application/json",json);
}
bool parseRevision(const String& s,uint32_t& result) {
  if(s.length()==0 || s.length()>10)return false;
  uint64_t n=0;
  for(unsigned i=0;i<s.length();++i){if(s[i]<'0'||s[i]>'9')return false;n=n*10+(s[i]-'0');}
  if(n>UINT32_MAX)return false;
  result=static_cast<uint32_t>(n);return true;
}
void invalidCommand() {
  for(auto& c:channels)faultChannel(c,"INVALID_COMMAND");
  applyOutputs();
  Serial.println("INVALID_COMMAND: MOTOR1 off; all channels FAULT -> STOPPED");
  server.send(400,"application/json","{\"error\":\"Lenh sai; da dung 3 kenh\"}");
}
void control() {
  // Timeout and sensor errors take priority over requests, including queued STARTs.
  updateState();
  const String target=server.arg("channel"), action=server.arg("action");
  if(target=="all" && action=="STOP") {
    for(auto& c:channels)stopChannel(c,"STOP");
    applyOutputs();sendStatus();return;
  }
  int index=-1;
  if(target=="0")index=0;else if(target=="1")index=1;else if(target=="2")index=2;
  if(index<0){invalidCommand();return;}
  Channel& c=channels[index];
  if(action=="STOP") {stopChannel(c,"STOP");applyOutputs();sendStatus();return;}
  if(!(index==2 ? motorReady : ledReady[index])) {
    server.send(503,"application/json","{\"error\":\"PWM initialization failed\"}");return;
  }
  if(action!="START" && action!="PWM"){invalidCommand();return;}
  uint32_t revision;
  if(!parseRevision(server.arg("revision"),revision)){invalidCommand();return;}
  if(action=="PWM") {
    const String duty=server.arg("duty");
    const bool validDuty = index==2
        ? (duty=="25" || duty=="50" || duty=="75" || duty=="100")
        : (duty=="25" || duty=="50" || duty=="90");
    if(!validDuty){invalidCommand();return;}
    if(revision!=c.revision || c.state==State::Fault) {
      server.send(409,"application/json","{\"error\":\"Trang thai da doi; thu lai\"}");return;
    }
    c.duty=duty.toInt();++c.revision;
  } else if(!startChannel(c,millis(),sensorValid(millis()),revision)) {
    server.send(409,"application/json","{\"error\":\"Du lieu UNO loi hoac trang thai da doi\"}");return;
  }
  applyOutputs();
  Serial.printf("%s %s hardware=1\n",c.name,action.c_str());
  sendStatus();
}
}
void setup() {
  beginMotor1(); // Off before Serial, I2C, timer, or Wi-Fi initialization.
  beginLeds();
  Serial.begin(config::kSerialBaud);
  Serial.println("TH03: LED1/LED2 active HIGH PWM; MOTOR1 hardware enabled.");
  beginPeriodicTimer();
  if (!periodicTimer) Serial.println("TIMER_INIT_ERROR");
  i2cReady=Wire.begin(config::kSdaPin,config::kSclPin,config::kI2cFrequencyHz);
  if(i2cReady)Wire.setTimeOut(config::kI2cTimeoutMs);
  else Serial.println("I2C_INIT_ERROR");
  lastPollMs=millis();
  WiFi.mode(WIFI_AP);
  if(!WiFi.softAP("TH03-Greenhouse","th03demo123"))Serial.println("AP_INIT_ERROR");
  Serial.print("Web: http://");Serial.println(WiFi.softAPIP());
  server.on("/",HTTP_GET,[]{server.send_P(200,"text/html; charset=utf-8",kWebPage);});
  server.on("/api/status",HTTP_GET,[]{updateState();sendStatus();});
  server.on("/api/control",HTTP_POST,control);
  server.onNotFound([]{server.send(404,"application/json","{\"error\":\"Not found\"}");});
  server.begin();
}
void loop() {
  updateState();
  const uint32_t now=millis();
  if(i2cReady && uint32_t(now-lastPollMs)>=config::kPollIntervalMs) {
    lastPollMs=now;readUno();updateState();
  }
  const uint32_t ticks = getTimerTicks();
  if (ticks != reportedTicks) {
    reportedTicks = ticks;
    Serial.printf("TIMER ticks=%lu handled_at_ms=%lu (not ISR timestamp)\n",
      static_cast<unsigned long>(ticks), static_cast<unsigned long>(millis()));
  }
  server.handleClient();
  updateState();
  yield();
}
